#include "../src/PartyReactiveDispatch.h"
#include "../src/PartyReactivePolicy.h"
#define PARTY_REACTIVE_POLICY_TEST
#include "../src/PartyReactiveNative.inc"
#undef PARTY_REACTIVE_POLICY_TEST
#include <cassert>
#include <functional>
#include <iostream>
#include <map>
#include <string>
#include <utility>
#include <vector>
using namespace PartyReactive;

struct Native
{
    std::vector<Claim> submitted, released, callouts;
    std::vector<std::pair<Claim, std::uint32_t>> completed;
    std::map<Guid, std::uint32_t> inventory;
    std::map<std::uint32_t, std::uint32_t> partyInventory;
    bool countPartyInventory = false;
    bool accept = true;
    std::function<void(Claim const&)> onSubmit;
    bool Submit(Claim const& c) { submitted.push_back(c); if (onSubmit) onSubmit(c); return accept; }
    void Callout(Claim const& c) { callouts.push_back(c); }
    void Complete(Claim const& c, std::uint32_t count) { completed.emplace_back(c, count); }
    void Release(Claim const& c, bool regroup) { assert(regroup); released.push_back(c); }
    std::uint32_t Count(Claim const& c)
    { return countPartyInventory ? partyInventory[c.item] : inventory[c.actor]; }
    bool Continue(Claim const&) { return true; }
};
Candidate Gather(Guid actor, Guid target, bool eligible = true)
{ return {actor, target, 7, 0, 0, 3, eligible, Kind::Gather}; }
Candidate Craft(Guid actor, bool eligible = true)
{ return {actor, 0, 7, 42, 0, 3, eligible, Kind::Craft}; }
Frame Safe(std::vector<Candidate> candidates)
{ return {1, 2, 3, 4, 10, true, true, std::move(candidates)}; }

void EligibilityAndDeterminism()
{
    Controller c; Native n;
    Frame f = Safe({Gather(11, 90), Gather(10, 99), Gather(10, 88), Gather(12, 1, false)});
    c.Tick(f, n);
    assert(n.submitted.size() == 2 && n.submitted[0].actor == 10 && n.submitted[0].target == 88);
    assert(n.submitted[1].actor == 11 && n.submitted[1].target == 90);
    Controller other; Native m;
    auto reversed = Safe({Gather(10, 88), Gather(10, 99), Gather(11, 90)});
    other.Tick(reversed, m);
    assert(m.submitted.size() == 2 && m.submitted[0].target == 88 && m.submitted[1].target == 90);
    for (Candidate bad : {Gather(0, 1), Gather(1, 0), Gather(1, 1, false),
                          Candidate{1, 1, 0, 0, 0, 3, true, Kind::Gather},
                          Candidate{1, 1, 7, 0, 3, 3, true, Kind::Gather}, Craft(1, false)})
    { Controller x; Native y; x.Tick(Safe({bad}), y); assert(x.Claims().empty()); }
}
void ExclusivityAndEarnedCompletion()
{
    Controller c; Native n;
    c.Tick(Safe({Gather(10, 90), Gather(11, 90), Gather(10, 91)}), n);
    assert(c.Claims().size() == 1 && c.Claims()[0].actor == 10 && c.Claims()[0].target == 90);
    Controller craft; Native cn; craft.Tick(Safe({Craft(20), Craft(21)}), cn);
    assert(craft.Claims().size() == 2 && cn.submitted[0].kind == Kind::Craft);
    Frame f = Safe({Gather(10, 90)}); f.now++;
    c.Tick(f, n); assert(c.Claims().size() == 1 && n.submitted.size() == 1);
    n.inventory[10] = 1; ++f.now; c.Tick(f, n);
    assert(c.Claims().empty() && n.released.size() == 1 && n.completed.size() == 1 && n.completed[0].second == 1);
    ++f.now; c.Tick(f, n); assert(n.submitted.size() == 1);
}
void ContextAndCandidateCancellation()
{
    for (unsigned change = 0; change != 10; ++change)
    {
        Controller c; Native n; Frame f = Safe({Gather(10, 90)}); c.Tick(f, n); ++f.now;
        switch (change)
        {
        case 0: f.safe = false; break;
        case 1: ++f.owner; break;
        case 2: ++f.group; break;
        case 3: ++f.map; break;
        case 4: ++f.instance; break;
        case 5: f.candidates.clear(); break;
        case 6: f.candidates[0].eligible = false; break;
        case 7: ++f.candidates[0].target; break;
        case 8: f.candidates[0].have = f.candidates[0].reserve; break;
        case 9: f.currentHumanLeader = false; break;
        }
        c.Tick(f, n);
        assert(c.Claims().empty() && n.released.size() == 1 && n.submitted.size() == 1);
    }
}
void ExpiryReleasesClaim()
{
    Controller c; Native n; Frame f = Safe({Gather(10, 90)});
    c.Tick(f, n); assert(c.Claims().size() == 1 && n.submitted.size() == 1);
    f.now = c.Claims()[0].expires; c.Tick(f, n);
    assert(c.Claims().empty() && n.released.size() == 1 && n.submitted.size() == 1);
}
void DynamicMembershipAndExplicitCancel()
{
    Controller c; Native n; Frame f = Safe({});
    for (Guid actor = 10; actor != 80; ++actor) f.candidates.push_back(Gather(actor, actor + 100));
    c.Tick(f, n);
    assert(c.Claims().size() == 70 && n.submitted.size() == 70);
    c.Cancel(10, ++f.now, n);
    assert(c.Claims().size() == 69 && n.released.size() == 1);
    c.Cancel(f.owner, f.now, n);
    assert(c.Claims().empty() && n.released.size() == 70);
}
void DispatchPreservesManualTargets()
{
    assert(WithinPartyRange(0.0f, 100.0f));
    assert(WithinPartyRange(100.0f, 100.0f));
    assert(!WithinPartyRange(100.1f, 100.0f));

    unsigned selected = 0, moved = 0, opened = 0, removed = 0, reset = 0, followed = 0;
    auto select = [&](std::uint64_t id) { selected = static_cast<unsigned>(id); };
    auto move = [&] { ++moved; return true; }; auto open = [&] { ++opened; return true; };
    assert(DispatchGather({42, 0, true, false}, select, move, open));
    assert(selected == 42 && moved == 1 && opened == 0);
    assert(!DispatchGather({42, 99, true, false}, select, move, open));
    assert(selected == 42 && moved == 1 && opened == 0);
    assert(!DispatchGather({42, 0, false, false, true}, select, move, open));
    assert(selected == 42 && moved == 1 && opened == 0);
    assert(!DispatchGather({0, 0, false, false}, select, move, open));
    assert(DispatchGather({42, 42, false, true}, select, move, open) && opened == 0);
    assert(DispatchGather({42, 42, false, false}, select, move, open) && opened == 1);
    CancelGather(42, 99, true, [&](auto id) { removed = id; }, [&] { ++reset; }, [&] { ++followed; });
    assert(removed == 42 && reset == 0 && followed == 0);
    CancelGather(42, 42, true, [&](auto) {}, [&] { ++reset; }, [&] { ++followed; });
    assert(reset == 1 && followed == 1);
    CancelGather(42, 0, true, [&](auto) {}, [&] { ++reset; }, [&] { ++followed; });
    assert(reset == 1 && followed == 2);
}

void NativeAdapterActionsAndCalloutGate()
{
    assert(EligibleGatherOutput(7, true, false));
    assert(EligibleGatherOutput(7, false, true));
    assert(!EligibleGatherOutput(7, false, false));
    assert(!EligibleGatherOutput(0, true, true));

    std::vector<std::uint64_t> available;
    std::uint64_t selected = 0;
    std::vector<std::string> actions;
    auto select = [&](std::uint64_t target)
    {
        available.push_back(target);
        selected = target;
    };
    auto action = [&](char const* name)
    {
        actions.emplace_back(name);
        return true;
    };

    assert(DispatchNativeGather({42, 0, true, false, false}, select, action));
    assert(selected == 42 && available == std::vector<std::uint64_t>{42});
    assert(actions == std::vector<std::string>{"move to loot"});
    actions.clear();
    assert(DispatchNativeGather({42, 42, false, false, false}, select, action));
    assert(actions == std::vector<std::string>{"open loot"});

    std::vector<std::string> messages;
    assert(BroadcastGatheringReason(true, 2, 2, [&](char const* text) { messages.emplace_back(text); }));
    assert(messages == std::vector<std::string>{"Gathering a needed quest item."});
    assert(!BroadcastGatheringReason(false, 2, 2, [&](char const* text) { messages.emplace_back(text); }));
    assert(!BroadcastGatheringReason(true, 3, 2, [&](char const* text) { messages.emplace_back(text); }));
    assert(messages.size() == 1);
}

PartyReactive::MemberFacts CurrentMemberFacts()
{
    PartyReactive::MemberFacts facts;
    facts.ownerPresent = facts.ownerSession = facts.ownerInWorld = facts.ownerAlive = facts.ownerHuman = true;
    facts.groupPresent = facts.ownerLeader = true;
    facts.actorPresent = facts.actorSession = facts.actorInWorld = facts.actorAlive = true;
    facts.actorBot = facts.actorControlledByOwner = facts.actorSharesGroup = facts.actorIsMember = true;
    facts.ownerHasMap = facts.actorSharesMap = true;
    facts.ownerMap = facts.actorMap = 3;
    facts.ownerInstance = facts.actorInstance = 4;
    return facts;
}

void NativeAdapterRevalidatesMemberContext()
{
    auto facts = CurrentMemberFacts();
    assert(PartyReactive::IsCurrentMember(facts, 3, 4));
    for (bool PartyReactive::MemberFacts::* invalid : {
             &PartyReactive::MemberFacts::ownerPresent, &PartyReactive::MemberFacts::ownerSession,
             &PartyReactive::MemberFacts::ownerInWorld, &PartyReactive::MemberFacts::ownerAlive,
             &PartyReactive::MemberFacts::ownerHuman, &PartyReactive::MemberFacts::groupPresent,
             &PartyReactive::MemberFacts::ownerLeader, &PartyReactive::MemberFacts::actorPresent,
             &PartyReactive::MemberFacts::actorSession, &PartyReactive::MemberFacts::actorInWorld,
             &PartyReactive::MemberFacts::actorAlive, &PartyReactive::MemberFacts::actorBot,
             &PartyReactive::MemberFacts::actorControlledByOwner, &PartyReactive::MemberFacts::actorSharesGroup,
             &PartyReactive::MemberFacts::actorIsMember, &PartyReactive::MemberFacts::ownerHasMap,
             &PartyReactive::MemberFacts::actorSharesMap})
    {
        auto invalidFacts = facts;
        invalidFacts.*invalid = false;
        assert(!PartyReactive::IsCurrentMember(invalidFacts, 3, 4));
    }
    assert(!PartyReactive::IsCurrentMember(facts, 8, 4));
    assert(!PartyReactive::IsCurrentMember(facts, 3, 9));
    facts.ownerMap = 8;
    assert(!PartyReactive::IsCurrentMember(facts, 3, 4));
    facts = CurrentMemberFacts();
    facts.actorMap = 8;
    assert(!PartyReactive::IsCurrentMember(facts, 3, 4));
    facts = CurrentMemberFacts();
    facts.ownerInstance = 9;
    assert(!PartyReactive::IsCurrentMember(facts, 3, 4));
    facts = CurrentMemberFacts();
    facts.actorInstance = 9;
    assert(!PartyReactive::IsCurrentMember(facts, 3, 4));
    facts = CurrentMemberFacts();
    facts.actorInstance = 5;
    assert(facts.ownerInstance == 4 && facts.actorInstance != facts.ownerInstance);
    assert(!PartyReactive::IsCurrentMember(facts, 3, 4));
    assert(!PartyReactive::IsCurrentMember(facts, 3, 5));
}

void NativeAdapterCancellationActions()
{
    std::vector<std::uint64_t> available{42};
    std::uint64_t selected = 42;
    std::vector<std::string> actions;
    CancelNativeGather(42, 42, true,
                       [&](std::uint64_t target)
                       {
                           available.erase(std::remove(available.begin(), available.end(), target), available.end());
                       },
                       [&]() { selected = 0; },
                       [&](char const* name) { actions.emplace_back(name); });
    assert(available.empty() && selected == 0 && actions == std::vector<std::string>{"follow"});

    available.push_back(42);
    selected = 42;
    actions.clear();
    CancelNativeGather(42, 42, false,
                       [&](std::uint64_t target)
                       {
                           available.erase(std::remove(available.begin(), available.end(), target), available.end());
                       },
                       [&]() { selected = 0; },
                       [&](char const* name) { actions.emplace_back(name); });
    assert(available.empty() && selected == 0 && actions.empty());
}
void CombatInterruptsOnlyAnActiveOptionalActivity()
{
    std::vector<int> effects;
    assert(!DispatchObservedAggro(false, true, [&] { effects.push_back(1); }, [&] { effects.push_back(2); }));
    assert(effects.empty());
    assert(!DispatchObservedAggro(true, false, [&] { effects.push_back(1); }, [&] { effects.push_back(2); }));
    assert(effects.empty());
    effects.clear();
    assert(DispatchObservedAggro(true, true, [&] { effects.push_back(1); }, [&] { effects.push_back(2); }));
    assert((effects == std::vector<int>{1, 2}));
}
void GatheringReasonCalloutFollowsAcceptedSubmissionOnly()
{
    Controller accepted; Native native; Frame frame = Safe({Gather(10, 90)});
    accepted.Tick(frame, native);
    assert(native.callouts.size() == 1 && native.callouts[0].kind == Kind::Gather);
    ++frame.now; accepted.Tick(frame, native);
    assert(native.submitted.size() == 1 && native.callouts.size() == 1);

    Controller rejected; Native no; no.accept = false;
    rejected.Tick(Safe({Gather(10, 90)}), no);
    assert(no.callouts.empty());

    Controller invalid; Native bad;
    invalid.Tick(Safe({Gather(10, 0)}), bad);
    assert(bad.callouts.empty());

    Controller cancelled; Native pending;
    pending.onSubmit = [&](Claim const& claim) { cancelled.Cancel(claim.actor, 11, pending); };
    cancelled.Tick(Safe({Gather(10, 90)}), pending);
    assert(pending.callouts.empty() && pending.released.size() == 1);

    Controller craft; Native made;
    craft.Tick(Safe({Craft(10)}), made);
    assert(made.submitted.size() == 1 && made.callouts.size() == 1 &&
           made.callouts[0].kind == Kind::Craft);
}

void NeedPriorityAndSingleGoal()
{
    Controller c; Native n;
    Candidate near = Gather(10, 90); near.priority = 1; near.distance = 1.0f;
    Candidate covered = Gather(11, 91); covered.priority = 1; covered.coverage = 2; covered.distance = 50.0f;
    Candidate urgent = Gather(12, 92); urgent.priority = 2; urgent.coverage = 1; urgent.distance = 100.0f;
    c.Tick(Safe({near, covered, urgent}), n);
    assert(n.submitted.size() == 3 && n.submitted[0].actor == 12 && n.submitted[1].actor == 11 &&
           n.submitted[2].actor == 10);

    Controller food; Native f;
    Candidate first = Craft(10); first.item = 101; first.need = NeedKind::Food;
    Candidate second = Craft(11); second.item = 102; second.need = NeedKind::Food;
    food.Tick(Safe({second, first}), f);
    assert(f.submitted.size() == 1 && f.submitted[0].actor == 10);

    Controller reagents; Native r;
    Candidate herb = Gather(10, 201); herb.need = NeedKind::Reagent;
    Candidate ore = Gather(11, 202); ore.need = NeedKind::Reagent; ore.item = 8;
    reagents.Tick(Safe({herb, ore}), r);
    assert(r.submitted.size() == 2);
}

void DynamicSupplyDeficitsAndRecipeSelection()
{
    assert(SupplyDeficit(0, 0) == 0);
    assert(SupplyDeficit(2, 10) == 0);
    assert(SupplyDeficit(2, 7) == 3);
    assert(SupplyDeficit(0xffffffffu, 0) == 0xffffffffu);
    assert(SupplyDeficit(3, 14, 4) == 0);
    assert(SupplyDeficit(3, 8, 4) == 4);
    std::vector<SupplyStack> stacks = {
        {101, 4, NeedKind::Food, true, false}, {101, 5, NeedKind::Food, true, true},
        {202, 6, NeedKind::Food, true, false}, {101, 3, NeedKind::Food, false, false},
        {101, 8, NeedKind::Drink, true, false}
    };
    SupplyCounts const available = AvailableSupply(stacks, 101, NeedKind::Food);
    assert(available.item == 4 && available.category == 10);
    std::vector<SupplyStack> overflowing = {
        {101, 0xffffffffu, NeedKind::Food, true, false}, {101, 1, NeedKind::Food, true, false}
    };
    SupplyCounts const saturated = AvailableSupply(overflowing, 101, NeedKind::Food);
    assert(saturated.item == 0xffffffffu && saturated.category == 0xffffffffu);
    std::vector<SupplyStack> categoryOverflow = {
        {101, 0xfffffffeu, NeedKind::Food, true, false}, {202, 2, NeedKind::Food, true, false}
    };
    SupplyCounts const categorySaturated = AvailableSupply(categoryOverflow, 101, NeedKind::Food);
    assert(categorySaturated.item == 0xfffffffeu && categorySaturated.category == 0xffffffffu);

    assert(NeedPriority(NeedKind::Quest) > NeedPriority(NeedKind::Reagent));
    assert(NeedPriority(NeedKind::Reagent) > NeedPriority(NeedKind::Food));
    assert(NeedPriority(NeedKind::Food) > NeedPriority(NeedKind::Bandage));
    assert(NeedPriority(NeedKind::Bag) == 0 && NeedPriority(NeedKind::Gear) == 0);
    assert(PreferRecipe(true, {75, 500}, false, {1, 1}));
    assert(PreferRecipe(true, {75, 500}, true, {75, 501}));
    assert(!PreferRecipe(false, {1, 1}, true, {75, 501}));
    assert(!PreferRecipe(true, {75, 501}, true, {75, 500}));

    assert(std::string(ClaimReason(Kind::Craft, NeedKind::Food)) == "Preparing a needed party reserve.");
    assert(std::string(ClaimReason(Kind::Gather, NeedKind::Reagent)) ==
           "Gathering a known recipe reagent for the party.");
    assert(std::string(ClaimReason(Kind::Gather, NeedKind::Bag)) == "Gathering a needed party bag.");
    assert(std::string(ClaimReason(Kind::Gather, NeedKind::Gear)) == "Gathering a useful gear upgrade.");
}

void PartySuppliesExcludeUnrelatedHumanMembers()
{
    PartyRecipientFacts owner{true, true, false, true, true, true, false};
    PartyRecipientFacts controlledBot{false, false, true, true, true, true, true};
    PartyRecipientFacts unrelatedHuman{false, true, false, true, true, true, false};
    PartyRecipientFacts uncontrolledBot{false, false, true, true, true, true, false};
    assert(IsPartyRecipient(owner));
    assert(IsPartyRecipient(controlledBot));
    assert(!IsPartyRecipient(unrelatedHuman));
    assert(!IsPartyRecipient(uncontrolledBot));

    struct MemberInventory { PartyRecipientFacts facts; std::uint32_t food; };
    std::vector<MemberInventory> party{{owner, 2}, {controlledBot, 2}, {unrelatedHuman, 1000}};
    std::uint32_t consumers = 0, carried = 0;
    std::vector<Guid> recipients;
    for (std::size_t i = 0; i < party.size(); ++i)
        if (IsPartyRecipient(party[i].facts))
        {
            ++consumers;
            carried += party[i].food;
            recipients.push_back(i + 1);
        }
    assert(consumers == 2 && carried == 4 && SupplyDeficit(consumers, carried) == 6);
    assert((recipients == std::vector<Guid>{1, 2}));

    Frame frame = Safe({Gather(2, 22)});
    assert(std::none_of(frame.candidates.begin(), frame.candidates.end(),
                        [](Candidate const& candidate) { return candidate.actor == 3; }));
    Controller controller; Native native;
    controller.Tick(frame, native);
    assert(controller.Claims().size() == 1 && controller.Claims()[0].actor == 2);
}

void PartySupplyOutcomeUsesObservedInventoryDelta()
{
    Controller c; Native n; n.countPartyInventory = true;
    Candidate food = Craft(10); food.item = 710; food.need = NeedKind::Food; food.reserve = 5;
    c.Tick(Safe({food}), n);
    assert(c.Claims().size() == 1 && n.callouts.size() == 1);
    n.partyInventory[710] = 2;
    Frame next = Safe({food}); ++next.now;
    c.Tick(next, n);
    assert(c.Claims().empty() && n.completed.size() == 1 && n.completed[0].second == 2);
    assert(n.released.size() == 1);
}

void PartySafetyAndAssistanceGates()
{
    using namespace PartyReactive;
    assert(PartyMemberUnsafe({true, false, false, false}));
    assert(PartyMemberUnsafe({true, true, true, false}));
    assert(!PartyMemberUnsafe({false, false, true, true}));

    OptionalSafetyFacts safe{true, true, true, true, false, false, false, true, true, true, true, true, false};
    assert(SafeOptional(safe));
    safe.leaderMoving = true; assert(!SafeOptional(safe));
    safe.leaderMoving = false; safe.manualBusy = true; assert(!SafeOptional(safe));

    AssistFacts helper{true, true, false, false, false, true, true};
    assert(EligibleAssister(helper));
    helper.healer = true; assert(!EligibleAssister(helper));
    std::vector<std::string> actions;
    helper.healer = false;
    assert(DispatchSafeAssistance(helper, true, [&] { actions.push_back("select"); },
                                  [&] { actions.push_back("attack"); return true; }));
    assert((actions == std::vector<std::string>{"select", "attack"}));
    actions.clear();
    assert(!DispatchSafeAssistance(helper, true, [&] { actions.push_back("select"); },
                                   [&] { actions.push_back("attack"); return false; }));
    assert((actions == std::vector<std::string>{"select", "attack"}));
    actions.clear();
    assert(!DispatchSafeAssistance(helper, false, [&] { actions.push_back("select"); },
                                   [&] { actions.push_back("attack"); return true; }));
    assert(actions.empty());
    helper.manualBusy = true;
    assert(!DispatchSafeAssistance(helper, true, [&] { actions.push_back("select"); },
                                   [&] { actions.push_back("attack"); return true; }));
    assert(actions.empty());
    assert(EligibleAssistTarget(true, true, true, false, false));
    assert(!EligibleAssistTarget(true, true, true, true, false));
    assert(!EligibleAssistTarget(true, true, true, false, true));

    std::vector<std::string> messages;
    assert(BroadcastPartyMessage(true, 2, 2, "Gathering a reagent for the party.",
                                 [&](char const* message) { messages.emplace_back(message); }));
    assert(!BroadcastPartyMessage(true, 3, 2, "Wrong group.",
                                  [&](char const* message) { messages.emplace_back(message); }));
    assert(messages.size() == 1 && messages[0] == "Gathering a reagent for the party.");
}

void OptionalCastOwnershipRequiresImmediateObservation()
{
    PartyReactive::ActionIdentity const laterManualCast{1, 2, 42, 7};
    assert(!PartyReactive::MatchesObservedAction(nullptr, laterManualCast));
    assert(PartyReactive::MatchesObservedAction(&laterManualCast, laterManualCast));

    PartyReactive::ActionIdentity const replacedCast{1, 2, 42, 8};
    assert(!PartyReactive::MatchesObservedAction(&laterManualCast, replacedCast));
    PartyReactive::ActionIdentity const otherOwner{3, 2, 42, 7};
    assert(!PartyReactive::MatchesObservedAction(&laterManualCast, otherOwner));
}

int main()
{
    EligibilityAndDeterminism(); ExclusivityAndEarnedCompletion();
    ContextAndCandidateCancellation(); ExpiryReleasesClaim(); DynamicMembershipAndExplicitCancel();
    NeedPriorityAndSingleGoal(); PartySafetyAndAssistanceGates();
    DynamicSupplyDeficitsAndRecipeSelection(); PartySupplyOutcomeUsesObservedInventoryDelta();
    PartySuppliesExcludeUnrelatedHumanMembers();
    DispatchPreservesManualTargets(); CombatInterruptsOnlyAnActiveOptionalActivity();
    GatheringReasonCalloutFollowsAcceptedSubmissionOnly();
    NativeAdapterActionsAndCalloutGate(); NativeAdapterRevalidatesMemberContext();
    NativeAdapterCancellationActions();
    OptionalCastOwnershipRequiresImmediateObservation();
    std::cout << "PASS owner-party needs, safety, priority, native gather/cancel actions, callouts, and combat interruption\n";
}
