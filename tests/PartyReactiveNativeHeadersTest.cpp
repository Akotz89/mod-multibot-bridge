#include "Chat.h"
#include "GameObject.h"
#include "Group.h"
#include "Map.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "PlayerbotAIConfig.h"
#include "QuestDef.h"
#include "ScriptMgr.h"
#include "WorldPacket.h"
#include "AiObjectContext.h"
#include "Event.h"
#include "LootObjectStack.h"
#include "LootMgr.h"

#include "../src/PartyReactiveDispatch.h"

bool PartyReactiveNativeHeadersSmoke(Player* owner, Player* actor, Group* group, GameObject* object,
                                     Quest const* quest, PlayerbotAI* botAI)
{
    if (!owner || !actor || !group || !object || !quest || !botAI)
        return false;

    (void)PartyReactive::WithinPartyRange(owner->GetDistance(actor), sPlayerbotAIConfig.sightDistance);
    (void)(group->GetLeaderGUID() == owner->GetGUID() && group->IsMember(actor->GetGUID()));
    (void)(owner->GetMap() == actor->GetMap() && actor->GetMap() == object->GetMap());
    (void)quest->RequiredItemId[0];
    (void)owner->GetItemCount(quest->RequiredItemId[0], false);
    (void)PlayerbotAI::IsHeal(actor);
    (void)botAI->HasStrategy("follow", BOT_STATE_NON_COMBAT);
    (void)actor->GetVictim();
    (void)actor->IsValidAttackTarget(owner);

    AiObjectContext* const context = botAI->GetAiObjectContext();
    if (context)
    {
        LootObjectStack* const available = *context->GetValue<LootObjectStack*>("available loot");
        auto* const selected = context->GetValue<LootObject>("loot target");
        if (available && selected)
        {
            ObjectGuid const target = object->GetGUID();
            uint64 const currentTarget = selected->Get().guid.GetRawValue();
            (void)PartyReactive::DispatchNativeGather(
                {target.GetRawValue(), currentTarget, actor->GetDistance(object) > INTERACTION_DISTANCE,
                 actor->IsNonMeleeSpellCast(false), actor->IsInCombat() || actor->isMoving()},
                [&](uint64 id)
                {
                    available->Add(ObjectGuid(id));
                    selected->Set(LootObject(actor, ObjectGuid(id)));
                },
                [&](char const* action) { return botAI->DoSpecificAction(action, Event(), true); });
            PartyReactive::CancelNativeGather(
                target.GetRawValue(), selected->Get().guid.GetRawValue(), true,
                [&](uint64 id) { available->Remove(ObjectGuid(id)); },
                [&]() { selected->Set(LootObject()); },
                [&](char const* action) { return botAI->DoSpecificAction(action, Event(), true); });
            (void)LootObject(actor, target).IsLootPossible(actor);
            uint32 const lootId = object->GetGOInfo()->GetLootId();
            LootTemplate const* const lootTemplate = lootId ? LootTemplates_Gameobject.GetLootFor(lootId) : nullptr;
            (void)(lootTemplate && lootTemplate->HasItemForPlayer(quest->RequiredItemId[0], actor, LOOT_MODE_DEFAULT));
            (void)(lootTemplate && lootTemplate->FindItemForPlayer(
                [&](uint32 item) { return item == quest->RequiredItemId[0]; }, actor, object->GetLootMode()));
        }
    }

    (void)PartyReactive::BroadcastPartyMessage(
        group->IsMember(actor->GetGUID()), group->GetGUID().GetRawValue(), group->GetGUID().GetRawValue(), "test",
        [&](char const* message)
        {
            WorldPacket packet;
            ChatHandler::BuildChatPacket(packet, CHAT_MSG_PARTY, LANG_UNIVERSAL, actor->GetGUID(), ObjectGuid::Empty,
                                         message, CHAT_TAG_NONE, actor->GetName());
            group->BroadcastPacket(&packet, false);
        });
    return botAI->DoSpecificAction("move to loot", Event(), true);
}

bool PartyReactiveNativeAssistHeadersSmoke(Player* actor, Unit* enemy, PlayerbotAI* helperAI)
{
    if (!actor || !enemy || !helperAI || !helperAI->GetAiObjectContext())
        return false;

    auto* const target = helperAI->GetAiObjectContext()->GetValue<Unit*>("current target");
    PartyReactive::AssistFacts facts{true, actor->IsAlive(), PlayerbotAI::IsHeal(actor),
                                     actor->GetVictim() != nullptr, !target || target->Get() || actor->isMoving(),
                                     actor->GetDistance(enemy) <= sPlayerbotAIConfig.sightDistance,
                                     helperAI->HasStrategy("follow", BOT_STATE_NON_COMBAT)};
    bool const validTarget = PartyReactive::EligibleAssistTarget(
        enemy->IsInWorld(), enemy->IsAlive(), enemy->GetMap() == actor->GetMap(), enemy->IsPlayer(), enemy->IsPet());
    return PartyReactive::DispatchSafeAssistance(
        facts, validTarget, [&]() { target->Set(enemy); },
        [&]() { return helperAI->DoSpecificAction("attack", Event(), true); });
}

class PartyReactiveNativeScriptHeadersSmoke final : public PlayerScript
{
public:
    PartyReactiveNativeScriptHeadersSmoke() : PlayerScript("PartyReactiveNativeHeadersTest") {}
    void OnPlayerJustDied(Player*) override {}
    void OnPlayerEnterCombat(Player*, Unit*) override {}
    void OnPlayerUpdate(Player*, uint32) override {}
    void OnPlayerLogout(Player*) override {}
    void OnPlayerMapChanged(Player*) override {}
};
