#ifndef PARTY_REACTIVE_DISPATCH_H
#define PARTY_REACTIVE_DISPATCH_H

#include <cstdint>

namespace PartyReactive
{
struct DispatchFacts
{
    std::uint64_t target = 0, currentTarget = 0;
    bool distant = false, casting = false, manualBusy = false;
};

struct MemberFacts
{
    bool ownerPresent = false, ownerSession = false, ownerInWorld = false, ownerAlive = false, ownerHuman = false;
    bool groupPresent = false, ownerLeader = false;
    bool actorPresent = false, actorSession = false, actorInWorld = false, actorAlive = false;
    bool actorBot = false, actorControlledByOwner = false, actorSharesGroup = false, actorIsMember = false;
    bool ownerHasMap = false, actorSharesMap = false;
    std::uint32_t ownerMap = 0, actorMap = 0, ownerInstance = 0, actorInstance = 0;
};

struct MemberSafetyFacts { bool controlled = false, alive = false, inCombat = false, hasThreat = false; };
inline bool PartyMemberUnsafe(MemberSafetyFacts const& facts)
{
    return facts.controlled && (!facts.alive || facts.inCombat || facts.hasThreat);
}

struct OptionalSafetyFacts
{
    bool ownerCurrent = false, actorControlled = false, ownerAlive = false, actorAlive = false;
    bool combat = false, threat = false, leaderMoving = false, sameGroup = false, sameMap = false;
    bool sameInstance = false, withinDetour = false, room = false, manualBusy = false;
};
inline bool SafeOptional(OptionalSafetyFacts const& facts)
{
    return facts.ownerCurrent && facts.actorControlled && facts.ownerAlive && facts.actorAlive &&
        !facts.combat && !facts.threat && !facts.leaderMoving && facts.sameGroup && facts.sameMap &&
        facts.sameInstance && facts.withinDetour && facts.room && !facts.manualBusy;
}

struct AssistFacts
{
    bool controlled = false, alive = false, healer = false, hasVictim = false, manualBusy = false;
    bool inRange = false, following = false;
};
inline bool EligibleAssister(AssistFacts const& facts)
{
    return facts.controlled && facts.alive && !facts.healer && !facts.hasVictim &&
        !facts.manualBusy && facts.inRange && facts.following;
}

inline bool EligibleAssistTarget(bool inWorld, bool alive, bool sameMap, bool player, bool pet)
{
    return inWorld && alive && sameMap && !player && !pet;
}

inline bool EligibleGatherOutput(std::uint32_t item, bool listedForQuest, bool nativeLootPossible)
{
    return item && (listedForQuest || nativeLootPossible);
}

template<class Select, class Attack>
bool DispatchSafeAssistance(AssistFacts const& facts, bool validTarget, Select select, Attack attack)
{
    if (!EligibleAssister(facts) || !validTarget)
        return false;
    select();
    return attack();
}

inline bool WithinPartyRange(float distance, float maxDistance)
{
    return distance <= maxDistance;
}

inline bool IsCurrentMember(MemberFacts const& facts, std::uint32_t map, std::uint32_t instance)
{
    return facts.ownerPresent && facts.ownerSession && facts.ownerInWorld && facts.ownerAlive && facts.ownerHuman &&
           facts.groupPresent && facts.ownerLeader && facts.actorPresent && facts.actorSession && facts.actorInWorld &&
           facts.actorAlive && facts.actorBot && facts.actorControlledByOwner && facts.actorSharesGroup &&
           facts.actorIsMember && facts.ownerHasMap && facts.actorSharesMap && facts.ownerMap == map &&
           facts.actorMap == map && facts.ownerInstance == instance && facts.actorInstance == instance &&
           facts.ownerInstance == facts.actorInstance;
}

template<class Select, class Move, class Open>
bool DispatchGather(DispatchFacts const& facts, Select select, Move move, Open open)
{
    if (!facts.target || facts.manualBusy || (facts.currentTarget && facts.currentTarget != facts.target)) return false;
    select(facts.target);
    if (facts.distant) return move();
    if (facts.casting) return true;
    return open();
}

template<class Select, class DoSpecificAction>
bool DispatchNativeGather(DispatchFacts const& facts, Select select, DoSpecificAction doSpecificAction)
{
    return DispatchGather(facts, select,
                          [&]() { return doSpecificAction("move to loot"); },
                          [&]() { return doSpecificAction("open loot"); });
}

template<class Remove, class Reset, class Follow>
void CancelGather(std::uint64_t target, std::uint64_t currentTarget, bool regroup,
                  Remove remove, Reset reset, Follow follow)
{
    if (!target) return;
    remove(target);
    if (currentTarget && currentTarget != target) return;
    if (currentTarget == target) reset();
    if (regroup) follow();
}

template<class Remove, class Reset, class DoSpecificAction>
void CancelNativeGather(std::uint64_t target, std::uint64_t currentTarget, bool regroup,
                        Remove remove, Reset reset, DoSpecificAction doSpecificAction)
{
    CancelGather(target, currentTarget, regroup, remove, reset,
                 [&]() { doSpecificAction("follow"); });
}

template<class Send>
bool BroadcastPartyMessage(bool currentMember, std::uint64_t currentGroup,
                           std::uint64_t claimGroup, char const* message, Send send)
{
    if (!currentMember || !currentGroup || currentGroup != claimGroup || !message || !*message)
        return false;
    send(message);
    return true;
}

template<class Send>
bool BroadcastGatheringReason(bool currentMember, std::uint64_t currentGroup,
                              std::uint64_t claimGroup, Send send)
{
    return BroadcastPartyMessage(currentMember, currentGroup, claimGroup,
                                 "Gathering a needed quest item.", send);
}
}
#endif
