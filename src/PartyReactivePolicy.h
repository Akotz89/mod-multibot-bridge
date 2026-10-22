#ifndef PARTY_REACTIVE_POLICY_H
#define PARTY_REACTIVE_POLICY_H

#include <algorithm>
#include <cstdint>
#include <limits>
#include <utility>
#include <vector>

namespace PartyReactive
{
using Guid = std::uint64_t;
enum class Kind { Gather, Craft };
enum class NeedKind { Quest, Food, Drink, HealingPotion, ManaPotion, Bandage, Reagent, Bag, Gear };
struct PartyRecipientFacts
{
    bool owner = false, human = false, controlledBot = false;
    bool currentGroupMember = false, live = false, nearby = false, securityAllowed = false;
};
inline bool IsPartyRecipient(PartyRecipientFacts const& facts)
{
    if (!facts.currentGroupMember || !facts.live || !facts.nearby)
        return false;
    return facts.owner ? facts.human : facts.controlledBot && facts.securityAllowed;
}
inline std::uint32_t SupplyDeficit(std::uint32_t consumers, std::uint32_t carried,
                                   std::uint32_t reservePerConsumer = 5)
{
    if (!consumers || !reservePerConsumer)
        return 0;
    std::uint32_t const desired = consumers > std::numeric_limits<std::uint32_t>::max() / reservePerConsumer
                                      ? std::numeric_limits<std::uint32_t>::max()
                                      : consumers * reservePerConsumer;
    return desired > carried ? desired - carried : 0;
}

inline std::uint32_t NeedPriority(NeedKind need)
{
    switch (need)
    {
        case NeedKind::Quest: return 4;
        case NeedKind::Reagent: return 3;
        case NeedKind::Food:
        case NeedKind::Drink: return 2;
        case NeedKind::HealingPotion:
        case NeedKind::ManaPotion:
        case NeedKind::Bandage: return 1;
        case NeedKind::Bag:
        case NeedKind::Gear: return 0;
    }
    return 0;
}

inline bool PreferRecipe(bool ready, std::pair<std::uint32_t, std::uint32_t> const& rank,
                         bool previousReady, std::pair<std::uint32_t, std::uint32_t> const& previousRank)
{
    return ready != previousReady ? ready : rank < previousRank;
}

inline char const* ClaimReason(Kind kind, NeedKind need)
{
    if (kind == Kind::Craft)
        return "Preparing a needed party reserve.";
    switch (need)
    {
        case NeedKind::Quest: return "Gathering a needed quest item.";
        case NeedKind::Food: return "Gathering food for the party.";
        case NeedKind::Drink: return "Gathering drinks for the party.";
        case NeedKind::HealingPotion: return "Gathering healing potions for the party.";
        case NeedKind::ManaPotion: return "Gathering mana potions for the party.";
        case NeedKind::Bandage: return "Gathering bandages for the party.";
        case NeedKind::Reagent: return "Gathering a known recipe reagent for the party.";
        case NeedKind::Bag: return "Gathering a needed party bag.";
        case NeedKind::Gear: return "Gathering a useful gear upgrade.";
    }
    return "Gathering a needed party item.";
}
struct Candidate
{
    Guid actor = 0, target = 0;
    std::uint32_t item = 0, spell = 0, have = 0, reserve = 0;
    bool eligible = false;
    Kind kind = Kind::Gather;
    NeedKind need = NeedKind::Quest;
    std::uint32_t priority = 0, coverage = 1;
    float distance = 0.0f;
};
struct Frame
{
    Guid owner = 0, group = 0;
    std::uint32_t map = 0, instance = 0;
    std::uint64_t now = 0;
    bool currentHumanLeader = false, safe = false;
    std::vector<Candidate> candidates;
};
struct Claim
{
    Guid owner = 0, group = 0, actor = 0, target = 0;
    std::uint32_t map = 0, instance = 0, item = 0, spell = 0, baseline = 0, reserve = 0;
    std::uint64_t expires = 0;
    Kind kind = Kind::Gather;
    NeedKind need = NeedKind::Quest;
};

class Controller
{
public:
    std::vector<Claim> const& Claims() const { return claims_; }

    template<class Native> void Tick(Frame const& frame, Native& native)
    {
        if (busy_) return;
        Busy scope(busy_);
        released_.clear();
        Prune(frame.now);
        for (std::size_t i = 0; i < claims_.size();)
        {
            Claim const claim = claims_[i];
            auto candidate = std::find_if(frame.candidates.begin(), frame.candidates.end(),
                [&](Candidate const& c)
                {
                    return Matches(claim, c) && c.eligible && c.have < c.reserve && c.reserve == claim.reserve;
                });
            bool release = !frame.currentHumanLeader || !frame.safe || !frame.owner || !frame.group ||
                claim.owner != frame.owner ||
                claim.group != frame.group || claim.map != frame.map || claim.instance != frame.instance ||
                frame.now >= claim.expires || Pending(claim);
            if (!release)
            {
                std::uint32_t const count = native.Count(claim);
                if (count > claim.baseline)
                {
                    native.Complete(claim, count - claim.baseline);
                    release = true;
                }
                else
                    release = candidate == frame.candidates.end() || count >= claim.reserve;
                if (!release)
                    release = !native.Continue(claim) || Pending(claim);
            }
            if (release) Release(i, frame.now, native);
            else ++i;
        }

        if (frame.currentHumanLeader && frame.safe && frame.owner && frame.group && !PendingMember(frame.owner))
        {
            auto candidates = frame.candidates;
            std::sort(candidates.begin(), candidates.end(), [](Candidate const& a, Candidate const& b)
            {
                if (a.priority != b.priority) return a.priority > b.priority;
                if (a.coverage != b.coverage) return a.coverage > b.coverage;
                if (a.distance != b.distance) return a.distance < b.distance;
                if (a.actor != b.actor) return a.actor < b.actor;
                if (a.target != b.target) return a.target < b.target;
                if (a.item != b.item) return a.item < b.item;
                if (a.spell != b.spell) return a.spell < b.spell;
                if (a.kind != b.kind) return a.kind < b.kind;
                if (a.need != b.need) return a.need < b.need;
                return a.reserve < b.reserve;
            });
            for (Candidate const& candidate : candidates)
            {
                if (!Eligible(candidate) || PendingMember(candidate.actor) ||
                    std::find(released_.begin(), released_.end(), candidate.actor) != released_.end() ||
                    Claimed(frame, candidate) || Cooling(frame, candidate)) continue;
                Claim claim{frame.owner, frame.group, candidate.actor, candidate.target, frame.map,
                    frame.instance, candidate.item, candidate.spell, candidate.have, candidate.reserve,
                    Until(frame.now, 30000), candidate.kind};
                claim.need = candidate.need;
                claim.baseline = native.Count(claim);
                if (claim.expires <= frame.now) continue;
                claims_.push_back(claim);
                if (!native.Submit(claim) || Pending(claim)) Release(claims_.size() - 1, frame.now, native);
                else native.Callout(claim);
            }
        }
        Drain(frame.now, native);
    }

    template<class Native> void Cancel(Guid member, std::uint64_t now, Native& native)
    {
        if (!member) return;
        if (busy_)
        {
            if (!PendingMember(member)) pending_.push_back(member);
            return;
        }
        Busy scope(busy_);
        Prune(now);
        pending_.push_back(member);
        Drain(now, native);
    }

private:
    struct Busy
    {
        bool& value;
        explicit Busy(bool& v) : value(v) { value = true; }
        ~Busy() { value = false; }
    };
    static std::uint64_t Until(std::uint64_t now, std::uint64_t delta)
    {
        return now > std::numeric_limits<std::uint64_t>::max() - delta
            ? std::numeric_limits<std::uint64_t>::max() : now + delta;
    }
    static bool Eligible(Candidate const& c)
    {
        return c.eligible && c.actor && c.item && c.reserve && c.have < c.reserve &&
            ((c.kind == Kind::Gather && c.target) || (c.kind == Kind::Craft && c.spell));
    }
    static bool Matches(Claim const& a, Candidate const& b)
    {
        return a.actor == b.actor && a.target == b.target && a.item == b.item &&
            a.spell == b.spell && a.kind == b.kind && a.need == b.need;
    }
    static bool Same(Claim const& a, Frame const& f, Candidate const& c)
    {
        return a.owner == f.owner && a.group == f.group && a.actor == c.actor && a.target == c.target &&
            a.map == f.map && a.instance == f.instance && a.item == c.item && a.spell == c.spell &&
            a.kind == c.kind && a.need == c.need;
    }
    static bool SameGoal(Claim const& a, Frame const& f, Candidate const& c)
    {
        if (a.owner != f.owner || a.group != f.group || a.map != f.map || a.instance != f.instance ||
            a.need != c.need)
            return false;
        switch (c.need)
        {
            case NeedKind::Food:
            case NeedKind::Drink:
            case NeedKind::HealingPotion:
            case NeedKind::ManaPotion:
            case NeedKind::Bandage:
                return true;
            case NeedKind::Quest:
                return a.actor == c.actor && a.item == c.item;
            case NeedKind::Reagent:
            case NeedKind::Bag:
            case NeedKind::Gear:
                return a.item == c.item;
        }
        return false;
    }
    bool PendingMember(Guid member) const
    {
        return std::find(pending_.begin(), pending_.end(), member) != pending_.end();
    }
    bool Pending(Claim const& c) const { return PendingMember(c.owner) || PendingMember(c.actor); }
    bool Claimed(Frame const& f, Candidate const& c) const
    {
        return std::any_of(claims_.begin(), claims_.end(), [&](Claim const& a)
        {
            return a.actor == c.actor || (c.target && a.target == c.target && a.map == f.map &&
                a.instance == f.instance) || SameGoal(a, f, c);
        });
    }
    bool Cooling(Frame const& f, Candidate const& c) const
    {
            return std::any_of(cooldowns_.begin(), cooldowns_.end(), [&](Claim const& a)
            { return Same(a, f, c) || SameGoal(a, f, c); });
    }
    void Prune(std::uint64_t now)
    {
        cooldowns_.erase(std::remove_if(cooldowns_.begin(), cooldowns_.end(),
            [now](Claim const& c) { return now >= c.expires; }), cooldowns_.end());
    }
    template<class Native> void Release(std::size_t i, std::uint64_t now, Native& native)
    {
        Claim claim = claims_[i];
        claims_.erase(claims_.begin() + i);
        released_.push_back(claim.actor);
        claim.expires = Until(now, 15000);
        cooldowns_.push_back(claim);
        native.Release(claim, true);
    }
    template<class Native> void Drain(std::uint64_t now, Native& native)
    {
        for (;;)
        {
            auto found = std::find_if(claims_.begin(), claims_.end(), [&](Claim const& c) { return Pending(c); });
            if (found == claims_.end()) break;
            Release(static_cast<std::size_t>(found - claims_.begin()), now, native);
        }
        pending_.clear();
    }
    std::vector<Claim> claims_, cooldowns_;
    std::vector<Guid> pending_, released_;
    bool busy_ = false;
};
}
#endif
