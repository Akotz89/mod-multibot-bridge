namespace PartyReactive
{
struct ActionIdentity
{
    std::uint64_t owner = 0, target = 0;
    std::uint32_t spell = 0;
    std::uint8_t castCount = 0;
};

inline bool MatchesObservedAction(ActionIdentity const* observed, ActionIdentity const& current)
{
    return observed && observed->owner == current.owner && observed->target == current.target &&
           observed->spell == current.spell && observed->castCount == current.castCount;
}

template<class Cancel, class Assist>
bool DispatchObservedAggro(bool validObservation, bool activeOptionalActivity, Cancel&& cancel, Assist&& assist)
{
    if (!validObservation || !activeOptionalActivity)
        return false;
    cancel();
    assist();
    return true;
}
}  // namespace PartyReactive

#ifndef PARTY_REACTIVE_POLICY_TEST
#include <mutex>
#include <unordered_map>

#include "LootMgr.h"
#include "PartyReactiveClaims.h"
#include "PartyReactiveDispatch.h"
#include "PartyReactivePolicy.h"

namespace
{
using PartyReactive::Guid;
struct PartyReactivePendingAction
{
    PartyReactive::ActionIdentity identity;
};
std::unordered_map<Guid, PartyReactive::Controller> partyReactiveControllers;
std::unordered_map<Guid, std::uint64_t> partyReactiveLastTick;
std::unordered_map<Guid, PartyReactivePendingAction> partyReactiveActions;
std::recursive_mutex partyReactiveMutex;

std::uint64_t PartyReactiveNow() { return PartyReactiveClaims::Now(); }

void PartyReactiveRecordAction(PartyReactive::Claim const& claim, Player* actor)
{
    auto const cast = actor ? actor->GetCurrentSpell(CURRENT_GENERIC_SPELL) : nullptr;
    if (!actor || !cast || !cast->GetSpellInfo())
        return;
    Guid const target = claim.kind == PartyReactive::Kind::Gather ? claim.target : claim.actor;
    uint64 const castTarget = claim.kind == PartyReactive::Kind::Gather
                                  ? cast->m_targets.GetGOTargetGUID().GetRawValue()
                                  : cast->m_targets.GetUnitTargetGUID().GetRawValue();
    if (castTarget != target || (claim.kind == PartyReactive::Kind::Craft && cast->GetSpellInfo()->Id != claim.spell))
        return;
    partyReactiveActions[claim.actor] = {
        {claim.owner, target, cast->GetSpellInfo()->Id, cast->m_cast_count}};
}

bool PartyReactiveOwnsCurrentAction(PartyReactive::Claim const& claim, Player* actor)
{
    auto found = partyReactiveActions.find(claim.actor);
    auto const cast = actor ? actor->GetCurrentSpell(CURRENT_GENERIC_SPELL) : nullptr;
    if (found == partyReactiveActions.end() || !cast || !cast->GetSpellInfo())
        return false;
    uint64 const target = claim.kind == PartyReactive::Kind::Gather
                              ? cast->m_targets.GetGOTargetGUID().GetRawValue()
                              : cast->m_targets.GetUnitTargetGUID().GetRawValue();
    return PartyReactive::MatchesObservedAction(
        &found->second.identity,
        {claim.owner, target, cast->GetSpellInfo()->Id, cast->m_cast_count});
}

Guid PartyReactiveGuid(ObjectGuid const& guid) { return guid.GetRawValue(); }

Player* PartyReactivePlayer(Guid guid) { return guid ? ObjectAccessor::FindPlayer(ObjectGuid(guid)) : nullptr; }

std::map<uint32, uint32> PartyReactiveQuestNeeds(Player* player)
{
    std::map<uint32, uint32> needs;
    if (!player)
        return needs;

    for (uint8 slot = 0; slot < MAX_QUEST_LOG_SIZE; ++slot)
    {
        uint32 const questId = player->GetQuestSlotQuestId(slot);
        if (!questId || player->GetQuestStatus(questId) != QUEST_STATUS_INCOMPLETE)
            continue;

        Quest const* const quest = sObjectMgr->GetQuestTemplate(questId);
        auto const status = player->getQuestStatusMap().find(questId);
        if (!quest || status == player->getQuestStatusMap().end())
            continue;

        for (uint8 objective = 0; objective < QUEST_ITEM_OBJECTIVES_COUNT; ++objective)
        {
            uint32 const item = quest->RequiredItemId[objective];
            uint32 const required = quest->RequiredItemCount[objective];
            uint32 const carried = player->GetItemCount(item, false);
            if (!item || required <= carried)
                continue;

            uint32 const remaining = required - carried;
            needs[item] = std::max(needs[item], remaining);
        }
    }
    return needs;
}

std::vector<Player*> PartyReactiveMembers(Player* owner)
{
    std::vector<Player*> members;
    Group* const group = owner ? owner->GetGroup() : nullptr;
    if (!owner || !group || !owner->GetMap() || GetBotAI(owner) || !IsRealPlayer(owner) ||
        group->GetLeaderGUID() != owner->GetGUID())
        return members;

    for (Group::MemberSlot const& slot : group->GetMemberSlots())
    {
        Player* const member = ObjectAccessor::FindPlayer(slot.guid);
        if (!member || !member->GetSession() || !member->IsInWorld() || !member->IsAlive() ||
            member->GetGroup() != group || !group->IsMember(member->GetGUID()) ||
            member->GetMap() != owner->GetMap() ||
            !PartyReactive::WithinPartyRange(owner->GetDistance(member), sPlayerbotAIConfig.sightDistance))
            continue;
        bool const isOwner = member == owner;
        PlayerbotAI* const ai = GetBotAI(member);
        PartyReactive::PartyRecipientFacts facts;
        facts.owner = isOwner;
        facts.human = isOwner && !ai && IsRealPlayer(member);
        facts.controlledBot = !isOwner && ai && ai->GetMaster() == owner;
        facts.currentGroupMember = true;
        facts.live = true;
        facts.nearby = true;
        facts.securityAllowed = facts.controlledBot && ai->GetSecurity() &&
            ai->GetSecurity()->CheckLevelFor(PLAYERBOT_SECURITY_ALLOW_ALL, true, owner);
        if (!PartyReactive::IsPartyRecipient(facts))
            continue;
        members.push_back(member);
    }
    std::sort(members.begin(), members.end(), [](Player* a, Player* b) { return a->GetGUID() < b->GetGUID(); });
    return members;
}

std::vector<Item*> PartyReactiveInventoryItems(Player* player)
{
    std::vector<Item*> items;
    if (!player)
        return items;
    if (PlayerbotAI* const ai = GetBotAI(player))
        return ai->GetInventoryItems();

    for (uint8 slot = INVENTORY_SLOT_ITEM_START; slot < INVENTORY_SLOT_ITEM_END; ++slot)
        if (Item* const item = player->GetItemByPos(INVENTORY_SLOT_BAG_0, slot))
            items.push_back(item);
    for (uint8 bag = INVENTORY_SLOT_BAG_START; bag < INVENTORY_SLOT_BAG_END; ++bag)
    {
        Bag* const container = static_cast<Bag*>(player->GetItemByPos(INVENTORY_SLOT_BAG_0, bag));
        if (!container)
            continue;
        for (uint8 slot = 0; slot < container->GetBagSize(); ++slot)
            if (Item* const item = container->GetItemByPos(slot))
                items.push_back(item);
    }
    return items;
}

PartyReactive::NeedKind PartyReactiveNeedKind(ItemTemplate const* proto)
{
    if (!proto)
        return PartyReactive::NeedKind::Quest;
    if (proto->Class == ITEM_CLASS_CONTAINER && proto->InventoryType == INVTYPE_BAG && proto->ContainerSlots)
        return PartyReactive::NeedKind::Bag;
    std::string const type = ItemUsageValue::GetConsumableType(proto, true);
    if (type == "food") return PartyReactive::NeedKind::Food;
    if (type == "drink") return PartyReactive::NeedKind::Drink;
    if (type == "healing potion") return PartyReactive::NeedKind::HealingPotion;
    if (type == "mana potion") return PartyReactive::NeedKind::ManaPotion;
    if (type == "bandage") return PartyReactive::NeedKind::Bandage;
    return PartyReactive::NeedKind::Quest;
}

bool PartyReactiveCanUseSupply(Player* member, ItemTemplate const* proto, PartyReactive::NeedKind need)
{
    if (!member || !proto || member->CanUseItem(proto) != EQUIP_ERR_OK)
        return false;
    return (need != PartyReactive::NeedKind::Drink && need != PartyReactive::NeedKind::ManaPotion) ||
           member->GetMaxPower(POWER_MANA) > 0;
}

uint32 PartyReactiveCount(std::vector<Player*> const& members, uint32 item)
{
    uint64 count = 0;
    for (Player* const member : members)
        count += member->GetItemCount(item, false);
    return static_cast<uint32>(std::min<uint64>(count, std::numeric_limits<uint32>::max()));
}

bool PartyReactiveSupplyDemand(std::vector<Player*> const& members, uint32 item, uint32& have, uint32& reserve,
                               PartyReactive::NeedKind& need)
{
    ItemTemplate const* const proto = sObjectMgr->GetItemTemplate(item);
    need = PartyReactiveNeedKind(proto);
    if (need == PartyReactive::NeedKind::Quest || need == PartyReactive::NeedKind::Bag)
        return false;

    uint32 consumers = 0;
    uint64 carried = 0;
    for (Player* const member : members)
    {
        if (!PartyReactiveCanUseSupply(member, proto, need))
            continue;
        ++consumers;
        for (Item* const stack : PartyReactiveInventoryItems(member))
        {
            ItemTemplate const* const stackProto = stack ? stack->GetTemplate() : nullptr;
            if (stackProto && !stack->IsInTrade() && PartyReactiveNeedKind(stackProto) == need &&
                PartyReactiveCanUseSupply(member, stackProto, need))
                carried += stack->GetCount();
        }
    }
    uint32 const deficit = PartyReactive::SupplyDeficit(consumers,
        static_cast<uint32>(std::min<uint64>(carried, std::numeric_limits<uint32>::max())));
    if (!consumers || !deficit)
        return false;

    have = PartyReactiveCount(members, item);
    uint64 const target = uint64(have) + deficit;
    reserve = static_cast<uint32>(std::min<uint64>(target, std::numeric_limits<uint32>::max()));
    return true;
}

bool PartyReactiveBagNeeded(Player* member, ItemTemplate const* proto)
{
    if (!PartyReactiveCanUseSupply(member, proto, PartyReactive::NeedKind::Bag))
        return false;
    for (Item* const item : PartyReactiveInventoryItems(member))
        if (item && item->IsBag())
            return false;
    for (uint8 slot = INVENTORY_SLOT_BAG_START; slot < INVENTORY_SLOT_BAG_END; ++slot)
        if (!member->GetItemByPos(INVENTORY_SLOT_BAG_0, slot))
            return true;
    return false;
}

bool PartyReactiveGearUpgrade(Player* bot, uint32 item)
{
    PlayerbotAI* const botAI = GetBotAI(bot);
    AiObjectContext* const context = botAI ? botAI->GetAiObjectContext() : nullptr;
    auto* const usage = context ? context->GetValue<ItemUsage>("item usage", item) : nullptr;
    return usage && (usage->Get() == ITEM_USAGE_EQUIP || usage->Get() == ITEM_USAGE_REPLACE);
}

bool PartyReactiveRecipeNeeded(std::vector<Player*> const& members, uint32 spellId)
{
    SpellInfo const* const spell = sSpellMgr->GetSpellInfo(spellId);
    uint32 const item = GetRecipeCreatedItemId(spell);
    uint32 have = 0, reserve = 0;
    PartyReactive::NeedKind need;
    return item && PartyReactiveSupplyDemand(members, item, have, reserve, need);
}

bool PartyReactiveRecipeCheck(Player* bot, SpellInfo const* spell, bool allowMissingReagents)
{
    if (!bot || !spell || spell->IsPassive() || !IsKnownActiveBotSpell(bot, spell->Id) ||
        !BotHasRecipeRequiredTools(bot, spell) || bot->IsNonMeleeSpellCast(false))
        return false;
    Spell check(bot, spell, TRIGGERED_NONE);
    check.m_targets.SetUnitTarget(bot);
    SpellCastResult const result = check.CheckCast(true);
    return result == SPELL_CAST_OK || (allowMissingReagents && result == SPELL_FAILED_REAGENTS);
}

std::map<uint32, uint32> PartyReactiveMissingReagents(Player* bot, SpellInfo const* spell)
{
    std::map<uint32, uint32> required;
    if (!bot || !spell)
        return required;
    for (uint32 index = 0; index < MAX_SPELL_REAGENTS; ++index)
    {
        if (spell->Reagent[index] <= 0 || !spell->ReagentCount[index])
            continue;
        uint32 const item = static_cast<uint32>(spell->Reagent[index]);
        uint32 const count = spell->ReagentCount[index];
        required[item] = count > std::numeric_limits<uint32>::max() - required[item]
                             ? std::numeric_limits<uint32>::max()
                             : required[item] + count;
    }
    for (auto entry = required.begin(); entry != required.end();)
    {
        uint32 const have = bot->GetItemCount(entry->first, false);
        if (have >= entry->second)
            entry = required.erase(entry);
        else
        {
            entry->second -= have;
            ++entry;
        }
    }
    return required;
}

void PartyReactiveAddGatherCandidates(Player* owner, Player* bot, PartyReactive::Frame& frame,
                                      uint32 item, uint32 spell, uint32 have, uint32 missing,
                                      PartyReactive::NeedKind need)
{
    PlayerbotAI* const botAI = GetBotAI(bot);
    AiObjectContext* const context = botAI ? botAI->GetAiObjectContext() : nullptr;
    if (!owner || !bot || !context || !item || !missing)
        return;
    GuidVector const objects = *context->GetValue<GuidVector>("nearest game objects");
    auto controller = partyReactiveControllers.find(PartyReactiveGuid(owner->GetGUID()));
    for (ObjectGuid const objectGuid : objects)
    {
        GameObject* const object = botAI->GetGameObject(objectGuid);
        if (!object || object->GetMap() != bot->GetMap())
            continue;

        bool const activeClaim = controller != partyReactiveControllers.end() &&
            std::any_of(controller->second.Claims().begin(), controller->second.Claims().end(),
                        [&](PartyReactive::Claim const& claim)
                        { return claim.actor == PartyReactiveGuid(bot->GetGUID()) &&
                                 claim.target == PartyReactiveGuid(objectGuid); });
        LootObject loot(bot, objectGuid);
        if (!activeClaim && !loot.IsLootPossible(bot))
            continue;

        GameObjectQuestItemList const* const questItems = sObjectMgr->GetGameObjectQuestItemList(object->GetEntry());
        uint32 const lootId = object->GetGOInfo()->GetLootId();
        LootTemplate const* const lootTemplate = lootId ? LootTemplates_Gameobject.GetLootFor(lootId) : nullptr;
        bool const listedForQuest = need == PartyReactive::NeedKind::Quest && questItems &&
            std::find(questItems->begin(), questItems->end(), item) != questItems->end();
        bool const nativeLootPossible = lootTemplate && lootTemplate->HasItemForPlayer(item, bot, object->GetLootMode());
        if (!PartyReactive::EligibleGatherOutput(item, listedForQuest, nativeLootPossible))
            continue;

        PartyReactive::Candidate candidate{};
        candidate.actor = PartyReactiveGuid(bot->GetGUID());
        candidate.target = PartyReactiveGuid(objectGuid);
        candidate.item = item;
        candidate.spell = spell;
        candidate.have = have;
        candidate.reserve = missing > std::numeric_limits<uint32>::max() - have
                                ? std::numeric_limits<uint32>::max() : have + missing;
        candidate.eligible = true;
        candidate.kind = PartyReactive::Kind::Gather;
        candidate.need = need;
        candidate.priority = PartyReactive::NeedPriority(need);
        candidate.distance = bot->GetDistance(object);
        frame.candidates.push_back(candidate);
    }
}

void PartyReactiveAddSupplyGatherCandidates(Player* bot, std::vector<Player*> const& members,
                                            PartyReactive::Frame& frame)
{
    PlayerbotAI* const botAI = GetBotAI(bot);
    AiObjectContext* const context = botAI ? botAI->GetAiObjectContext() : nullptr;
    if (!bot || !context)
        return;
    GuidVector const objects = *context->GetValue<GuidVector>("nearest game objects");
    for (ObjectGuid const objectGuid : objects)
    {
        GameObject* const object = botAI->GetGameObject(objectGuid);
        if (!object || object->GetMap() != bot->GetMap())
            continue;
        LootObject loot(bot, objectGuid);
        if (!loot.IsLootPossible(bot))
            continue;
        uint32 const lootId = object->GetGOInfo()->GetLootId();
        LootTemplate const* const lootTemplate = lootId ? LootTemplates_Gameobject.GetLootFor(lootId) : nullptr;
        if (!lootTemplate)
            continue;

        PartyReactive::NeedKind foundNeed = PartyReactive::NeedKind::Quest;
        uint32 have = 0, reserve = 0;
        uint32 const item = lootTemplate->FindItemForPlayer(
            [&](uint32 candidateItem)
            {
                ItemTemplate const* const proto = sObjectMgr->GetItemTemplate(candidateItem);
                if (!proto)
                    return false;
                PartyReactive::NeedKind need = PartyReactiveNeedKind(proto);
                uint32 candidateHave = 0, candidateReserve = 0;
                if (need == PartyReactive::NeedKind::Bag)
                {
                    if (!PartyReactiveBagNeeded(bot, proto))
                        return false;
                    candidateHave = bot->GetItemCount(candidateItem, false);
                    candidateReserve = candidateHave == std::numeric_limits<uint32>::max()
                                           ? candidateHave : candidateHave + 1;
                }
                else if (need != PartyReactive::NeedKind::Quest)
                {
                    if (!PartyReactiveSupplyDemand(members, candidateItem, candidateHave, candidateReserve, need))
                        return false;
                }
                else if (PartyReactiveGearUpgrade(bot, candidateItem) &&
                         bot->GetItemCount(candidateItem, false) == 0)
                {
                    need = PartyReactive::NeedKind::Gear;
                    candidateReserve = 1;
                }
                else
                    return false;

                foundNeed = need;
                have = candidateHave;
                reserve = candidateReserve;
                return have < reserve;
            }, bot, object->GetGOInfo()->GetLootId() ? object->GetLootMode() : LOOT_MODE_DEFAULT);
        if (!item || have >= reserve)
            continue;

        PartyReactive::Candidate candidate{};
        candidate.actor = PartyReactiveGuid(bot->GetGUID());
        candidate.target = PartyReactiveGuid(objectGuid);
        candidate.item = item;
        candidate.have = have;
        candidate.reserve = reserve;
        candidate.eligible = true;
        candidate.kind = PartyReactive::Kind::Gather;
        candidate.need = foundNeed;
        candidate.priority = PartyReactive::NeedPriority(foundNeed);
        candidate.distance = bot->GetDistance(object);
        frame.candidates.push_back(candidate);
    }
}

bool PartyReactiveClaimNeeded(Player* owner, std::vector<Player*> const& members,
                              PartyReactive::Claim const& claim)
{
    Player* const actor = PartyReactivePlayer(claim.actor);
    if (!owner || !actor)
        return false;
    if (claim.need == PartyReactive::NeedKind::Quest)
    {
        auto const needs = PartyReactiveQuestNeeds(actor);
        return needs.find(claim.item) != needs.end() && actor->GetItemCount(claim.item, false) < claim.reserve;
    }
    if (claim.need == PartyReactive::NeedKind::Reagent)
    {
        SpellInfo const* const spell = sSpellMgr->GetSpellInfo(claim.spell);
        uint32 const outputId = GetRecipeCreatedItemId(spell);
        ItemTemplate const* const output = sObjectMgr->GetItemTemplate(outputId);
        PartyReactive::NeedKind const outputNeed = PartyReactiveNeedKind(output);
        bool const outputNeeded = outputNeed == PartyReactive::NeedKind::Bag
                                      ? PartyReactiveBagNeeded(actor, output)
                                      : outputNeed == PartyReactive::NeedKind::Quest &&
                                                PartyReactiveGearUpgrade(actor, outputId)
                                            ? true : PartyReactiveRecipeNeeded(members, claim.spell);
        return outputNeeded && actor->GetItemCount(claim.item, false) < claim.reserve;
    }
    if (claim.need == PartyReactive::NeedKind::Bag)
        return PartyReactiveBagNeeded(actor, sObjectMgr->GetItemTemplate(claim.item)) &&
               actor->GetItemCount(claim.item, false) < claim.reserve;
    if (claim.need == PartyReactive::NeedKind::Gear)
        return PartyReactiveGearUpgrade(actor, claim.item) && actor->GetItemCount(claim.item, false) < claim.reserve;
    uint32 have = 0, reserve = 0;
    PartyReactive::NeedKind need;
    return PartyReactiveSupplyDemand(members, claim.item, have, reserve, need) && need == claim.need;
}

struct PartyReactiveNative
{
    explicit PartyReactiveNative(std::vector<Player*> memberSnapshot = {}) : members(std::move(memberSnapshot)) {}

    std::vector<Player*> members;

    bool IsCurrentMember(PartyReactive::Claim const& claim, Player*& owner, Player*& actor, Group*& group) const
    {
        owner = PartyReactivePlayer(claim.owner);
        actor = PartyReactivePlayer(claim.actor);
        group = owner ? owner->GetGroup() : nullptr;
        PlayerbotAI* const botAI = actor ? GetBotAI(actor) : nullptr;
        PartyReactive::MemberFacts facts;
        facts.ownerPresent = owner != nullptr;
        facts.ownerSession = owner && owner->GetSession();
        facts.ownerInWorld = owner && owner->IsInWorld();
        facts.ownerAlive = owner && owner->IsAlive();
        facts.ownerHuman = owner && !GetBotAI(owner) && IsRealPlayer(owner);
        facts.groupPresent = group != nullptr;
        facts.ownerLeader = group && owner && group->GetLeaderGUID() == owner->GetGUID();
        facts.actorPresent = actor != nullptr;
        facts.actorSession = actor && actor->GetSession();
        facts.actorInWorld = actor && actor->IsInWorld();
        facts.actorAlive = actor && actor->IsAlive();
        facts.actorBot = botAI != nullptr;
        facts.actorControlledByOwner = botAI && botAI->GetMaster() == owner && botAI->GetSecurity() &&
            botAI->GetSecurity()->CheckLevelFor(PLAYERBOT_SECURITY_ALLOW_ALL, true, owner);
        facts.actorSharesGroup = actor && group && actor->GetGroup() == group;
        facts.actorIsMember = group && actor && group->IsMember(actor->GetGUID());
        facts.ownerHasMap = owner && owner->GetMap();
        facts.actorSharesMap = owner && actor && owner->GetMap() && actor->GetMap() == owner->GetMap();
        facts.ownerMap = owner ? owner->GetMapId() : 0;
        facts.actorMap = actor ? actor->GetMapId() : 0;
        facts.ownerInstance = owner && owner->GetMap() ? owner->GetMap()->GetInstanceId() : 0;
        facts.actorInstance = actor && actor->GetMap() ? actor->GetMap()->GetInstanceId() : 0;
        return PartyReactive::IsCurrentMember(facts, claim.map, claim.instance);
    }

    uint32 Count(PartyReactive::Claim const& claim) const
    {
        Player* const actor = PartyReactivePlayer(claim.actor);
        if (!actor)
            return 0;
        return claim.need == PartyReactive::NeedKind::Quest || claim.need == PartyReactive::NeedKind::Reagent ||
                       claim.need == PartyReactive::NeedKind::Bag || claim.need == PartyReactive::NeedKind::Gear
                   ? actor->GetItemCount(claim.item, false)
                   : PartyReactiveCount(members, claim.item);
    }

    void Complete(PartyReactive::Claim const& claim, uint32 increased) const
    {
        Player* owner = nullptr;
        Player* actor = nullptr;
        Group* group = nullptr;
        bool const currentMember = IsCurrentMember(claim, owner, actor, group);
        std::string const message = claim.kind == PartyReactive::Kind::Craft
            ? "Preparation completed: the party gained " + std::to_string(increased) + " needed item(s)."
            : "Gathering completed: the party gained " + std::to_string(increased) + " needed item(s).";
        PartyReactive::BroadcastPartyMessage(
            currentMember, group ? PartyReactiveGuid(group->GetGUID()) : 0, claim.group, message.c_str(),
            [&](char const* text)
            {
                WorldPacket packet;
                ChatHandler::BuildChatPacket(packet, CHAT_MSG_PARTY, LANG_UNIVERSAL, actor->GetGUID(),
                                             ObjectGuid::Empty, text, CHAT_TAG_NONE, actor->GetName());
                group->BroadcastPacket(&packet, false);
            });
    }

    bool Submit(PartyReactive::Claim const& claim) const
    {
        Player* owner = nullptr;
        Player* actor = nullptr;
        Group* group = nullptr;
        if (!IsCurrentMember(claim, owner, actor, group) ||
            !PartyReactive::WithinPartyRange(owner->GetDistance(actor), sPlayerbotAIConfig.sightDistance) ||
            owner->IsInCombat() || owner->isMoving() ||
            actor->IsInCombat() || actor->isMoving() || actor->IsNonMeleeSpellCast(false))
            return false;

        PlayerbotAI* const botAI = GetBotAI(actor);
        AiObjectContext* const context = botAI ? botAI->GetAiObjectContext() : nullptr;
        if (!botAI || !PartyReactiveClaims::Register(claim.actor))
            return false;

        if (claim.kind == PartyReactive::Kind::Craft)
        {
            SpellInfo const* const spell = sSpellMgr->GetSpellInfo(claim.spell);
            ItemTemplate const* const output = sObjectMgr->GetItemTemplate(claim.item);
            ItemPosCountVec destination;
            bool const needed = claim.need == PartyReactive::NeedKind::Bag
                                    ? PartyReactiveBagNeeded(actor, output)
                                    : claim.need == PartyReactive::NeedKind::Gear
                                          ? PartyReactiveGearUpgrade(actor, claim.item)
                                          : PartyReactiveRecipeNeeded(members, claim.spell);
            if (!spell || !output || !IsKnownActiveBotSpell(actor, claim.spell) ||
                GetRecipeCreatedItemId(spell) != claim.item || !needed ||
                !PartyReactiveRecipeCheck(actor, spell, false) ||
                actor->CanStoreNewItem(NULL_BAG, NULL_SLOT, destination, claim.item, 1) != EQUIP_ERR_OK)
                return false;
            bool const submitted = botAI->CastSpell(claim.spell, actor);
            if (submitted)
                PartyReactiveRecordAction(claim, actor);
            return submitted;
        }
        if (!context)
            return false;

        LootObjectStack* const available = *context->GetValue<LootObjectStack*>("available loot");
        if (!available ||
            !PartyReactiveClaims::Acquire(claim.actor, claim.target, PartyReactiveClaims::Now() + 30000, claim.map,
                                          claim.instance))
            return false;

        ObjectGuid const target(claim.target);
        GameObject* const object = botAI->GetGameObject(target);
        if (!object || object->GetMap() != actor->GetMap())
            return false;

        LootObject loot(actor, target);
        if (!loot.IsLootPossible(actor))
            return false;

        auto* const selected = context->GetValue<LootObject>("loot target");
        if (!selected)
            return false;

        LootObject const current = *selected;
        uint64 const currentTarget = current.guid.GetRawValue();
        PartyReactive::DispatchFacts facts;
        facts.target = claim.target;
        facts.currentTarget = currentTarget;
        facts.distant = actor->GetDistance(object) > INTERACTION_DISTANCE;
        facts.casting = actor->IsNonMeleeSpellCast(false);
        facts.manualBusy = actor->IsInCombat() || actor->isMoving() || facts.casting || currentTarget;

        bool const dispatched = PartyReactive::DispatchNativeGather(
            facts,
            [&](uint64 selectedTarget)
            {
                available->Add(ObjectGuid(selectedTarget));
                selected->Set(LootObject(actor, ObjectGuid(selectedTarget)));
            },
            [&](char const* action) { return botAI->DoSpecificAction(action, Event(), true); });
        if (dispatched)
            PartyReactiveRecordAction(claim, actor);
        if (!dispatched)
            PartyReactiveClaims::Release(claim.actor, claim.target, claim.map, claim.instance);
        return dispatched;
    }

    void Callout(PartyReactive::Claim const& claim) const
    {
        Player* owner = nullptr;
        Player* actor = nullptr;
        Group* group = nullptr;
        bool const currentMember = IsCurrentMember(claim, owner, actor, group);
        if (!PartyReactive::BroadcastPartyMessage(
                currentMember, group ? PartyReactiveGuid(group->GetGUID()) : 0, claim.group,
                PartyReactive::ClaimReason(claim.kind, claim.need),
                [&](char const* message)
                {
                    WorldPacket packet;
                    ChatHandler::BuildChatPacket(packet, CHAT_MSG_PARTY, LANG_UNIVERSAL, actor->GetGUID(),
                                                 ObjectGuid::Empty, message, CHAT_TAG_NONE, actor->GetName());
                    group->BroadcastPacket(&packet, false);
                }))
            return;
    }

    bool Continue(PartyReactive::Claim const& claim) const
    {
        Player* owner = nullptr;
        Player* actor = nullptr;
        Group* group = nullptr;
        if (!IsCurrentMember(claim, owner, actor, group) ||
            !PartyReactive::WithinPartyRange(owner->GetDistance(actor), sPlayerbotAIConfig.sightDistance) ||
            owner->IsInCombat() || owner->isMoving() ||
            actor->IsInCombat() || actor->isMoving())
            return false;

        if (claim.kind == PartyReactive::Kind::Craft)
        {
            bool const casting = actor->IsNonMeleeSpellCast(false);
            return Count(claim) < claim.reserve && PartyReactiveClaimNeeded(owner, members, claim) &&
                   (!casting || PartyReactiveOwnsCurrentAction(claim, actor));
        }

        if (Count(claim) >= claim.reserve || !PartyReactiveClaimNeeded(owner, members, claim))
            return false;

        PlayerbotAI* const botAI = GetBotAI(actor);
        AiObjectContext* const context = botAI ? botAI->GetAiObjectContext() : nullptr;
        if (!context || !PartyReactiveClaims::Register(claim.actor) ||
            !PartyReactiveClaims::Acquire(claim.actor, claim.target, PartyReactiveClaims::Now() + 30000, claim.map,
                                          claim.instance))
            return false;

        GameObject* const object = botAI->GetGameObject(ObjectGuid(claim.target));
        LootObject loot(actor, ObjectGuid(claim.target));
        if (!object || object->GetMap() != actor->GetMap())
            return false;

        LootObject const current = *context->GetValue<LootObject>("loot target");
        uint64 const currentTarget = current.guid.GetRawValue();
        if ((actor->isMoving() || actor->IsNonMeleeSpellCast(false)) && currentTarget != claim.target)
            return false;
        bool const castingTarget = PartyReactiveOwnsCurrentAction(claim, actor);
        return loot.IsLootPossible(actor) || currentTarget == claim.target ||
               actor->GetLootGUID().GetRawValue() == claim.target || castingTarget;
    }

    void Release(PartyReactive::Claim const& claim, bool regroup) const
    {
        PartyReactiveClaims::Release(claim.actor, claim.target, claim.map, claim.instance);
        auto const action = partyReactiveActions.find(claim.actor);
        bool const ownsCast = action != partyReactiveActions.end() && PartyReactiveOwnsCurrentAction(claim, PartyReactivePlayer(claim.actor));
        if (action != partyReactiveActions.end())
            partyReactiveActions.erase(action);
        Player* owner = nullptr;
        Player* actor = nullptr;
        Group* group = nullptr;
        bool const member = IsCurrentMember(claim, owner, actor, group);
        if (!actor)
            return;

        PlayerbotAI* const botAI = GetBotAI(actor);
        AiObjectContext* const context = botAI ? botAI->GetAiObjectContext() : nullptr;
        if (!botAI)
            return;

        if (ownsCast && Count(claim) <= claim.baseline)
            actor->InterruptSpell(CURRENT_GENERIC_SPELL, false);

        if (claim.kind == PartyReactive::Kind::Craft)
        {
            if (regroup && member && !owner->IsInCombat() && !owner->isMoving() && !actor->IsInCombat() &&
                !actor->isMoving() && !actor->IsNonMeleeSpellCast(false))
                botAI->DoSpecificAction("follow", Event(), true);
            return;
        }
        if (!context)
            return;

        auto* const selected = context->GetValue<LootObject>("loot target");
        LootObjectStack* const available = *context->GetValue<LootObjectStack*>("available loot");
        uint64 const currentTarget = selected ? selected->Get().guid.GetRawValue() : 0;
        PartyReactive::CancelNativeGather(
            claim.target, currentTarget,
            regroup && member && !owner->IsInCombat() && !owner->isMoving() && !actor->IsInCombat() &&
                !actor->isMoving(),
            [&](uint64 target)
            {
                if (available)
                    available->Remove(ObjectGuid(target));
            },
            [&]()
            {
                if (selected)
                    selected->Set(LootObject());
            },
            [&](char const* action) { botAI->DoSpecificAction(action, Event(), true); });
    }
};

void TickPartyReactive(Player* owner)
{
    if (!owner || !owner->GetSession() || !owner->IsInWorld() || GetBotAI(owner))
        return;
    std::lock_guard<std::recursive_mutex> lock(partyReactiveMutex);

    Guid const ownerGuid = PartyReactiveGuid(owner->GetGUID());
    std::uint64_t const now = PartyReactiveNow();
    std::uint64_t& lastTick = partyReactiveLastTick[ownerGuid];
    if (now < lastTick || now - lastTick < 1000)
        return;
    lastTick = now;

    Group* const group = owner->GetGroup();
    PartyReactive::Frame frame;
    frame.owner = ownerGuid;
    frame.group = group ? PartyReactiveGuid(group->GetGUID()) : 0;
    frame.map = owner->GetMapId();
    frame.instance = owner->GetMap() ? owner->GetMap()->GetInstanceId() : 0;
    frame.now = now;
    frame.currentHumanLeader = group && IsRealPlayer(owner) && owner->IsAlive() &&
                               group->GetLeaderGUID() == owner->GetGUID();
    frame.safe = frame.currentHumanLeader && owner->GetMap() && !owner->IsInCombat() && !owner->isMoving();

    std::vector<Player*> const members = PartyReactiveMembers(owner);
    if (frame.safe)
    {
        auto controller = partyReactiveControllers.find(ownerGuid);
        for (Player* const bot : members)
        {
            PlayerbotAI* const botAI = GetBotAI(bot);
            AiObjectContext* const context = botAI ? botAI->GetAiObjectContext() : nullptr;
            if (!botAI || !context || botAI->GetMaster() != owner || bot->GetGroup() != group ||
                !group->IsMember(bot->GetGUID()) || !bot->GetSession() || !bot->IsInWorld() || !bot->IsAlive() ||
                bot->GetMap() != owner->GetMap() || bot->IsInCombat() || !bot->GetMap())
                continue;

            Guid const actorGuid = PartyReactiveGuid(bot->GetGUID());
            bool const hasClaim = controller != partyReactiveControllers.end() &&
                std::any_of(controller->second.Claims().begin(), controller->second.Claims().end(),
                            [&](PartyReactive::Claim const& claim) { return claim.actor == actorGuid; });
            LootObject currentLoot = *context->GetValue<LootObject>("loot target");
            if (!hasClaim && (bot->isMoving() || bot->IsNonMeleeSpellCast(false) || !currentLoot.IsEmpty()))
                continue;

            if (hasClaim)
            {
                for (PartyReactive::Claim const& claim : controller->second.Claims())
                {
                    if (claim.actor != actorGuid || !PartyReactiveClaimNeeded(owner, members, claim))
                        continue;
                    PartyReactive::Candidate candidate{};
                    candidate.actor = claim.actor;
                    candidate.target = claim.target;
                    candidate.item = claim.item;
                    candidate.spell = claim.spell;
                    candidate.have = claim.need == PartyReactive::NeedKind::Quest ||
                                             claim.need == PartyReactive::NeedKind::Reagent ||
                                             claim.need == PartyReactive::NeedKind::Bag ||
                                             claim.need == PartyReactive::NeedKind::Gear
                                         ? bot->GetItemCount(claim.item, false)
                                         : PartyReactiveCount(members, claim.item);
                    candidate.reserve = claim.reserve;
                    candidate.eligible = true;
                    candidate.kind = claim.kind;
                    candidate.need = claim.need;
                    candidate.priority = PartyReactive::NeedPriority(claim.need);
                    frame.candidates.push_back(candidate);
                }
                continue;
            }

            PartyReactiveAddSupplyGatherCandidates(bot, members, frame);
            for (auto const& questNeed : PartyReactiveQuestNeeds(bot))
                PartyReactiveAddGatherCandidates(owner, bot, frame, questNeed.first, 0,
                                                  bot->GetItemCount(questNeed.first, false), questNeed.second,
                                                  PartyReactive::NeedKind::Quest);

            struct RecipeCandidate
            {
                SpellInfo const* spell = nullptr;
                uint32 item = 0, have = 0, reserve = 0;
                PartyReactive::NeedKind need = PartyReactive::NeedKind::Quest;
                std::map<uint32, uint32> missing;
                bool ready = false;
                std::pair<uint32, uint32> rank;
            };
            std::map<PartyReactive::NeedKind, RecipeCandidate> selected;
            if (!hasClaim)
            {
                for (auto const& learned : bot->GetSpellMap())
                {
                    if (!IsKnownActiveBotSpell(bot, learned.first))
                        continue;
                    SpellInfo const* const spell = sSpellMgr->GetSpellInfo(learned.first);
                    uint32 const item = GetRecipeCreatedItemId(spell);
                    ItemTemplate const* const proto = sObjectMgr->GetItemTemplate(item);
                    PartyReactive::NeedKind need = PartyReactiveNeedKind(proto);
                    if (need == PartyReactive::NeedKind::Quest && PartyReactiveGearUpgrade(bot, item))
                        need = PartyReactive::NeedKind::Gear;
                    uint32 have = 0, reserve = 0;
                    bool const bagNeed = need == PartyReactive::NeedKind::Bag &&
                        PartyReactiveBagNeeded(bot, proto);
                    if (!bagNeed && need != PartyReactive::NeedKind::Gear &&
                        !PartyReactiveSupplyDemand(members, item, have, reserve, need))
                        continue;
                    if (!item || !proto || !PartyReactiveRecipeCheck(bot, spell, true))
                        continue;

                    std::map<uint32, uint32> missing = PartyReactiveMissingReagents(bot, spell);
                    bool const ready = missing.empty() && PartyReactiveRecipeCheck(bot, spell, false);
                    if (need == PartyReactive::NeedKind::Bag || need == PartyReactive::NeedKind::Gear)
                    {
                        have = bot->GetItemCount(item, false);
                        reserve = need == PartyReactive::NeedKind::Gear && have
                                      ? have
                                      : have == std::numeric_limits<uint32>::max() ? have : have + 1;
                    }
                    RecipeCandidate candidate{spell, item, have, reserve, need, std::move(missing), ready,
                                              {proto->RequiredLevel, spell->Id}};
                    auto previous = selected.find(need);
                    if (previous == selected.end() || PartyReactive::PreferRecipe(
                            candidate.ready, candidate.rank, previous->second.ready, previous->second.rank))
                        selected[need] = std::move(candidate);
                }
            }

            for (auto const& selectedRecipe : selected)
            {
                RecipeCandidate const& recipe = selectedRecipe.second;
                if (recipe.ready)
                {
                    PartyReactive::Candidate candidate{};
                    candidate.actor = actorGuid;
                    candidate.item = recipe.item;
                    candidate.spell = recipe.spell->Id;
                    candidate.have = recipe.have;
                    candidate.reserve = recipe.reserve;
                    candidate.eligible = true;
                    candidate.kind = PartyReactive::Kind::Craft;
                    candidate.need = recipe.need;
                    candidate.priority = PartyReactive::NeedPriority(recipe.need);
                    candidate.coverage = std::count_if(members.begin(), members.end(), [&](Player* member)
                    { return PartyReactiveCanUseSupply(member, recipe.spell ? sObjectMgr->GetItemTemplate(recipe.item) : nullptr,
                                                       recipe.need); });
                    frame.candidates.push_back(candidate);
                    continue;
                }
                for (auto const& reagent : recipe.missing)
                    PartyReactiveAddGatherCandidates(owner, bot, frame, reagent.first, recipe.spell->Id,
                                                     bot->GetItemCount(reagent.first, false), reagent.second,
                                                     PartyReactive::NeedKind::Reagent);
            }
        }
    }

    PartyReactiveNative native(members);
    partyReactiveControllers[ownerGuid].Tick(frame, native);
}

void CancelPartyReactiveForPlayer(Player* player, bool removeOwner)
{
    if (!player)
        return;
    std::lock_guard<std::recursive_mutex> lock(partyReactiveMutex);
    Guid const guid = PartyReactiveGuid(player->GetGUID());
    std::uint64_t const now = PartyReactiveNow();
    PartyReactiveNative native;
    for (auto& entry : partyReactiveControllers)
        entry.second.Cancel(guid, now, native);
    if (removeOwner)
    {
        partyReactiveControllers.erase(guid);
        partyReactiveLastTick.erase(guid);
    }
}

void HandlePartyReactiveCombat(Player* actor, Unit* enemy)
{
    if (!actor)
        return;
    PlayerbotAI* const botAI = GetBotAI(actor);
    Player* const owner = botAI ? botAI->GetMaster() : nullptr;
    Group* const group = owner ? owner->GetGroup() : nullptr;
    if (!actor->GetSession() || !actor->IsInWorld() || !actor->IsAlive() || !owner ||
        !owner->GetSession() || !owner->IsInWorld() || !owner->IsAlive() || GetBotAI(owner) || !group ||
        !owner->GetMap() || owner->GetMap() != actor->GetMap() ||
        !PartyReactive::WithinPartyRange(owner->GetDistance(actor), sPlayerbotAIConfig.sightDistance) ||
        group->GetLeaderGUID() != owner->GetGUID() ||
        actor->GetGroup() != group || !group->IsMember(actor->GetGUID()))
        return;

    std::lock_guard<std::recursive_mutex> lock(partyReactiveMutex);

    if (!enemy || !enemy->IsInWorld() || !enemy->IsAlive() || enemy->GetMap() != actor->GetMap() ||
        enemy->IsPlayer() || enemy->IsPet() || enemy->GetVictim() != actor || !actor->IsValidAttackTarget(enemy))
        return;

    Guid const ownerGuid = PartyReactiveGuid(owner->GetGUID());
    Guid const actorGuid = PartyReactiveGuid(actor->GetGUID());
    auto controller = partyReactiveControllers.find(ownerGuid);

    bool const activeOptionalActivity = controller != partyReactiveControllers.end() &&
        std::any_of(controller->second.Claims().begin(), controller->second.Claims().end(),
        [&](PartyReactive::Claim const& claim)
        {
            return claim.owner == ownerGuid && claim.group == PartyReactiveGuid(group->GetGUID()) &&
                   claim.actor == actorGuid && actor->GetMap() &&
                   claim.map == actor->GetMapId() && claim.instance == actor->GetMap()->GetInstanceId();
        });
    PartyReactiveNative native;
    PartyReactive::DispatchObservedAggro(true, activeOptionalActivity,
        [&]()
        {
            if (controller != partyReactiveControllers.end())
                controller->second.Cancel(actorGuid, PartyReactiveNow(), native);
        },
        [&]()
        {
            for (Player* const helper : GetBridgeVisibleBots(owner))
            {
                PlayerbotAI* const helperAI = GetBotAI(helper);
                AiObjectContext* const context = helperAI ? helperAI->GetAiObjectContext() : nullptr;
                auto* const target = context ? context->GetValue<Unit*>("current target") : nullptr;
                PartyReactive::AssistFacts facts;
                facts.controlled = helper && helperAI && helperAI->GetMaster() == owner && helper->GetGroup() == group &&
                                   group->IsMember(helper->GetGUID());
                facts.alive = helper && helper->GetSession() && helper->IsInWorld() && helper->IsAlive() &&
                              helper->GetMap() == actor->GetMap();
                facts.healer = helper && PlayerbotAI::IsHeal(helper);
                facts.hasVictim = helper && (helper->IsInCombat() || helper->GetVictim());
                facts.manualBusy = !target || target->Get() || (helper && (helper->isMoving() ||
                                                                            helper->IsNonMeleeSpellCast(false)));
                facts.inRange = helper && helper->GetDistance(enemy) <= sPlayerbotAIConfig.sightDistance;
                facts.following = helperAI && helperAI->HasStrategy("follow", BOT_STATE_NON_COMBAT);
                bool const validTarget = helper && PartyReactive::EligibleAssistTarget(
                    enemy->IsInWorld(), enemy->IsAlive(), enemy->GetMap() == helper->GetMap(),
                    enemy->IsPlayer(), enemy->IsPet()) && helper->IsValidAttackTarget(enemy);
                if (PartyReactive::DispatchSafeAssistance(
                        facts, validTarget,
                        [&]() { target->Set(enemy); },
                        [&]() { return helperAI->DoSpecificAction("attack", Event(), true); }))
                    break;
                if (target && !facts.manualBusy)
                    target->Set(nullptr);
            }
            std::string const attacker = enemy && enemy->IsInWorld() && enemy->GetMap() == actor->GetMap()
                                             ? enemy->GetName() : "an unknown attacker";
            std::string const message = "Under attack by " + attacker + "; assist!";
            WorldPacket packet;
            ChatHandler::BuildChatPacket(packet, CHAT_MSG_PARTY, LANG_UNIVERSAL, actor->GetGUID(),
                                         ObjectGuid::Empty, message, CHAT_TAG_NONE, actor->GetName());
            group->BroadcastPacket(&packet, false);
        });
}
}  // namespace
#endif  // PARTY_REACTIVE_POLICY_TEST
