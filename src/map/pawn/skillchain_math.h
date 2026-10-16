/*
===========================================================================

  Copyright (c) 2026 Cardian

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.

  This program is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program.  If not, see http://www.gnu.org/licenses/

===========================================================================
*/

#pragma once

#include "common/cbasetypes.h"

#include <fmt/format.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// Skillchains and magic bursts (ROADMAP, Road to subjob item 6, slice 3h;
// the user, 2026-10-15). The rules, pure, over plain facts, so xi_test pins
// them (cardian_skillchain_tests.cpp); the entities are read in
// skillchains.cpp and the gambit engine (CGambits) acts.
//  - options: every two-step skillchain the party can make, one member's
//    weapon skill opening and another's closing, ranked as the Shortcuts
//    menu lists them.
//  - wsCall: the tactician's Weapon skill (best) -- close what she can,
//    keep clear of what she cannot, follow the player's plan, else her best.
//  - TpClock and chainIn: when the party's melee will have the TP for the
//    plan's chain ("telepathy" of their TP), and so when the chain lands.
//  - nukeCall: the Black Mage's nuke against the burst -- cast now, burst
//    now, or hold to keep herself free and her MP for the burst.
// The game's own rules -- which properties make which chain, its tier and
// the elements that burst it -- are never copied here: every function that
// needs one takes it as a callable (battleutils::FormSkillchain,
// GetSkillchainTier, GetSkillchainMagicElement on the server).
namespace cardian::chains
{
    // The server's skillchain window (battleutils GetSkillChainEffect): a
    // weapon skill lands on an opening 3 s after it at the earliest, and an
    // opening lasts 10 s; a chain lasts a second less than what it closed,
    // which is the window a magic burst lands in
    inline constexpr double kWindowOpens = 3.0;
    inline constexpr double kOpeningLasts = 10.0;
    inline constexpr double kChainLasts   = 9.0;
    // A weapon skill from its start to its landing, near enough; and a
    // close needs that much of the window left to land inside it
    inline constexpr double kWsLands = 1.0;
    // An opener's think comes round every 2 to 3 s: half of that, on average
    inline constexpr double kThinkWait = 1.25;
    // A burst's cast must end this much before the window does: more than
    // one of the server's ticks (~400 ms), which the cast's end may fall
    // behind
    inline constexpr double kCastMargin = 0.5;

    // A weapon skill takes this much TP, and a member holds no more than this
    inline constexpr int32 kWsTp  = 1000;
    inline constexpr int32 kTpCap = 3000;
    // The plan's holds -- the closer's for the opening, the opener's for her
    // closer's TP -- end after this long at a weapon skill's TP with nothing
    // come of it, or at the cap: her TP is not kept from the fight for a plan
    // that does not come (a partner who never acts, a player who plays by
    // hand and never opens)
    inline constexpr double kPlanHoldLimit = 15.0;
    // TP is not spent without a chain on a mob under half its HP (the user,
    // 2026-09-26, once the default row's gate): an opening and her best
    // alone wait for a mob at this much or more; a close goes at any HP
    inline constexpr uint8 kSpendHpp = 50;

    // The most options the Shortcuts menu lists
    inline constexpr std::size_t kMaxOptions = 32;

    // A weapon skill's three skillchain properties, the server's
    // SKILLCHAIN_ELEMENT numbers (0: none)
    using Properties = std::array<uint8, 3>;

    // The chains by the server's numbers, for the map log (the addon words
    // its own)
    inline constexpr std::array<std::string_view, 17> kChainNames{
        "none",        "Transfixion", "Compression", "Liquefaction", "Scission", "Reverberation", "Detonation", "Induration", "Impaction",
        "Gravitation", "Distortion",  "Fusion",      "Fragmentation", "Light",   "Darkness",      "Light II",   "Darkness II",
    };

    constexpr auto chainName(const uint8 chain) -> std::string_view
    {
        return chain < kChainNames.size() ? kChainNames[chain] : std::string_view("?");
    }

    // An element's bit in a set of them (the server's ELEMENT: 1 fire to 8 dark)
    constexpr auto elementBit(const uint8 element) -> uint16
    {
        return element > 0 && element < 16 ? static_cast<uint16>(1u << element) : 0;
    }

    // --- what the party can make ---------------------------------------

    struct Skill
    {
        uint16     id = 0;
        Properties props{};
    };

    // A member as the options read her: her weapon skills usable now, and
    // the elements she can burst with (a bit each, elementBit)
    struct Member
    {
        uint32             id = 0;
        std::vector<Skill> skills;
        uint16             bursts = 0;
    };

    struct Option
    {
        uint32 opener      = 0;
        uint16 openerSkill = 0;
        uint32 closer      = 0;
        uint16 closerSkill = 0;
        uint8  chain       = 0;
        uint8  tier        = 0;
        uint16 elements    = 0; // the elements that burst it
        uint32 burster     = 0; // the first member, in the party's order, who can burst it; 0 for none
    };

    // Every two-step skillchain the party can make: each member's weapon
    // skill as the opening and each other member's as the close, the chain
    // the server would make of them (forms(opening, closing) -> chain, 0
    // for none). Ranked: a chain the party can burst first, then the higher
    // tier, then the party's order (members, then their skills); at most
    // `cap`. tierOf(chain) and elementsOf(chain) -> a set of elementBit are
    // the server's
    template <typename Forms, typename TierOf, typename ElementsOf>
    auto options(const std::vector<Member>& members, Forms&& forms, TierOf&& tierOf, ElementsOf&& elementsOf, const std::size_t cap = kMaxOptions) -> std::vector<Option>
    {
        std::vector<Option> out;
        for (const auto& opener : members)
        {
            for (const auto& closer : members)
            {
                if (opener.id == closer.id)
                {
                    continue;
                }
                for (const auto& open : opener.skills)
                {
                    for (const auto& close : closer.skills)
                    {
                        const uint8 chain = forms(open.props, close.props);
                        if (chain == 0)
                        {
                            continue;
                        }
                        Option o;
                        o.opener      = opener.id;
                        o.openerSkill = open.id;
                        o.closer      = closer.id;
                        o.closerSkill = close.id;
                        o.chain       = chain;
                        o.tier        = tierOf(chain);
                        o.elements    = elementsOf(chain);
                        for (const auto& m : members)
                        {
                            if ((m.bursts & o.elements) != 0)
                            {
                                o.burster = m.id;
                                break;
                            }
                        }
                        out.push_back(o);
                    }
                }
            }
        }
        std::ranges::stable_sort(out, [](const Option& a, const Option& b)
                                 {
                                     const bool ab = a.burster != 0;
                                     const bool bb = b.burster != 0;
                                     if (ab != bb)
                                     {
                                         return ab;
                                     }
                                     return a.tier > b.tier;
                                 });
        if (out.size() > cap)
        {
            out.resize(cap);
        }
        return out;
    }

    // --- the tactician's weapon skill ------------------------------------

    // Her place in the player's plan
    enum class Seat : uint8
    {
        None,
        Opener,
        Closer,
    };

    // What is on her mob: an opening (tier 0, the opener's three
    // properties) or a chain (its tier, the chain alone first), how long
    // since it landed and how long it has left
    struct Resonance
    {
        Properties props{};
        uint8      tier = 0;
        double     age  = 0.0;
        double     left = 0.0;
    };

    struct WsView
    {
        int32                    tp     = 0;
        uint8                    mobHpp = 100;
        std::vector<Skill>       skills;   // hers, usable now
        uint16                   best = 0; // her best alone, as Weapon skill (best) chooses it; 0 for none
        std::optional<Resonance> resonance;
        // Someone else would use what is on the mob: a member fighting it,
        // with the TP, who has a weapon skill that chains with it; and, for a
        // chain, a member who can burst its elements
        bool othersClose = false;
        bool burstComing = false;
        // Her place in the player's plan, and her partner's: he fights her
        // mob and follows the plan (a cardian's tactician's weapon skill
        // runs, the plan's weapon skill his), and has the TP for it
        Seat        seat          = Seat::None;
        uint16      planSkill     = 0; // hers in the plan
        uint8       planChain     = 0; // the chain the plan makes
        bool        partnerFights = false;
        bool        partnerReady  = false;
        std::string partner;           // the other member's name, for the words
        // Every member's knowledge of the plan, hers in it or not: the
        // opener's weapon skill's properties -- what the plan's opening
        // looks like on the mob -- and whether its closer fights the mob
        // and follows the plan (outside it, she leaves that opening to him)
        Properties  planOpening{};
        bool        planCloserFights = false;
        std::string planCloser;
        // How long she has held for the plan at a weapon skill's TP
        // (kPlanHoldLimit)
        double heldFor = 0.0;
    };

    enum class WsMove : uint8
    {
        None,  // nothing to say: no TP, no weapon skill
        Close, // `skill` closes what is on the mob, making `chain`
        Open,  // `skill` opens the plan's chain
        Free,  // `skill`, her best, alone
        Hold,  // she keeps her TP; `why` says for what
    };

    struct WsCall
    {
        WsMove      move     = WsMove::None;
        uint16      skill    = 0;
        uint8       chain    = 0;
        bool        soon     = false; // a Hold that is a close waiting for its window to open
        bool        planHold = false; // a Hold for the plan (its closer's, or its opener's for the closer's TP): bounded by kPlanHoldLimit
        std::string why;
    };

    // Her weapon skill now (ROADMAP slice 3h, the steps): outside the plan,
    // the plan's opening left to its closer; a close she can make, once the
    // window opens; a hold while someone else's opening or chain is on her
    // mob that somebody will use; the plan's closer holds for the opening and
    // its opener opens once the closer has the TP, each hold bounded by
    // kPlanHoldLimit and the TP cap; else her best alone. Spending without a
    // chain waits for a mob at kSpendHpp or more. forms(opening, closing) ->
    // chain and tierOf(chain) are the server's
    template <typename Forms, typename TierOf>
    auto wsCall(const WsView& v, Forms&& forms, TierOf&& tierOf) -> WsCall
    {
        WsCall call;
        if (v.tp < kWsTp)
        {
            return call;
        }
        const auto has = [&v](const uint16 id)
        {
            return id != 0 && std::ranges::any_of(v.skills, [id](const Skill& s) { return s.id == id; });
        };

        // 1. and 2. Something on her mob, with time left for a weapon skill
        // to land on it
        if (v.resonance.has_value() && v.resonance->left > kWsLands)
        {
            // Outside the plan, its opening is its closer's while he fights
            // the mob: nobody else closes it with a chain of her own
            if (v.seat == Seat::None && v.planCloserFights && v.resonance->tier == 0 && v.resonance->props == v.planOpening)
            {
                call.move = WsMove::Hold;
                call.why  = fmt::format("leaves the plan's opening to {}", v.planCloser);
                return call;
            }
            // The close she makes: the plan's weapon skill when she is its
            // closer, else the one making the highest-tier chain, her best
            // among equals
            const auto&  r     = *v.resonance;
            const Skill* pick  = nullptr;
            uint8        made  = 0;
            int32        score = -1;
            for (const auto& s : v.skills)
            {
                const uint8 chain = forms(r.props, s.props);
                if (chain == 0)
                {
                    continue;
                }
                const int32 mine = (v.seat == Seat::Closer && s.id == v.planSkill ? 1000 : 0) + tierOf(chain) * 10 + (s.id == v.best ? 1 : 0);
                if (mine > score)
                {
                    score = mine;
                    pick  = &s;
                    made  = chain;
                }
            }
            if (pick != nullptr)
            {
                call.skill = pick->id;
                call.chain = made;
                if (r.age <= kWindowOpens)
                {
                    call.move = WsMove::Hold;
                    call.soon = true;
                    call.why  = fmt::format("waits for the window, to close {}", chainName(made));
                    return call;
                }
                call.move = WsMove::Close;
                call.why  = fmt::format("closes {}", chainName(made));
                return call;
            }
            // One she cannot close is kept clear of while somebody will use
            // it -- a member with the TP and a weapon skill that chains with
            // it, or for a chain a member who can burst it: a weapon skill
            // that does not chain would put its own opening in its place.
            // Nobody will, and it is no reason to hold
            if (r.tier == 0 ? v.othersClose : (v.othersClose || v.burstComing))
            {
                call.move = WsMove::Hold;
                call.why  = r.tier == 0 ? "keeps clear of an opening she cannot close" : "leaves the chain's burst window be";
                return call;
            }
        }

        // The plan's holds end at the TP cap, or after kPlanHoldLimit at a
        // weapon skill's TP with nothing come of it
        const bool holdsOn = v.tp < kTpCap && v.heldFor < kPlanHoldLimit;

        // 3. The plan's closer holds her TP for the opening
        if (v.seat == Seat::Closer && v.partnerFights && has(v.planSkill) && holdsOn)
        {
            call.move     = WsMove::Hold;
            call.planHold = true;
            call.why      = fmt::format("holds her TP to close {} on {}'s opening", chainName(v.planChain), v.partner);
            return call;
        }

        // TP spent without a chain waits for a mob at half its HP or more
        const bool spends = v.mobHpp >= kSpendHpp;

        // 4. The plan's opener opens once its closer has the TP
        if (v.seat == Seat::Opener && v.partnerFights && has(v.planSkill))
        {
            if (!spends)
            {
                call.move = WsMove::Hold;
                call.why  = "the mob is under half its HP: no opening";
                return call;
            }
            if (!v.partnerReady && holdsOn)
            {
                call.move     = WsMove::Hold;
                call.planHold = true;
                call.why      = fmt::format("waits for {}'s TP to open {}", v.partner, chainName(v.planChain));
                return call;
            }
            if (!v.partnerReady)
            {
                call.move  = WsMove::Free;
                call.skill = v.planSkill;
                call.why   = fmt::format("opens with the plan's weapon skill alone ({}'s TP did not come)", v.partner);
                return call;
            }
            call.move  = WsMove::Open;
            call.skill = v.planSkill;
            call.chain = v.planChain;
            call.why   = fmt::format("opens {} for {}", chainName(v.planChain), v.partner);
            return call;
        }

        // 5. Her best alone
        if (v.best == 0)
        {
            return call;
        }
        if (!spends)
        {
            call.move = WsMove::Hold;
            call.why  = "the mob is under half its HP";
            return call;
        }
        call.move  = WsMove::Free;
        call.skill = v.best;
        call.why   = "uses her best alone";
        return call;
    }

    // --- telepathy: when the chain lands ------------------------------

    // A member's TP gain as it is seen: sampled while she fights, a rate
    // learned over the intervals her TP rose or stood below a weapon
    // skill's worth -- the only TP the rate is asked about, and above it a
    // member holding for a plan, or at the cap, would teach it nothing --
    // (a weapon skill's spend starts the next one afresh), weighted to the
    // last kTpRateSeconds or so. Out of a fight the rate is kept and the
    // baseline dropped
    inline constexpr double kTpSampleGap   = 0.5;  // samples closer than this are skipped
    inline constexpr double kTpGapTooLong  = 10.0; // an interval longer than this starts afresh
    inline constexpr double kTpRateSeconds = 15.0; // the weight's time scale
    inline constexpr double kTpTrusted     = 6.0;  // seconds of measuring before the rate is believed over the prior

    struct TpClock
    {
        double lastAt   = -1.0;
        int32  lastTp   = 0;
        double rate     = 0.0; // TP a second
        double measured = 0.0; // seconds the rate has seen

        void note(const int32 tp, const double at, const bool fighting)
        {
            if (!fighting)
            {
                lastAt = -1.0;
                return;
            }
            if (lastAt < 0.0 || at - lastAt > kTpGapTooLong || at < lastAt)
            {
                lastAt = at;
                lastTp = tp;
                return;
            }
            const double dt = at - lastAt;
            if (dt < kTpSampleGap)
            {
                return;
            }
            if (tp >= lastTp && lastTp < kWsTp)
            {
                const double seen   = static_cast<double>(tp - lastTp) / dt;
                const double weight = measured <= 0.0 ? 1.0 : dt / (dt + kTpRateSeconds);
                rate += (seen - rate) * weight;
                measured += dt;
            }
            lastAt = at;
            lastTp = tp;
        }

        // The rate believed: the measured one once it has seen enough, else
        // the prior (the server's TP for one hit by her weapon's delay)
        auto rateOr(const double prior) const -> double
        {
            return measured >= kTpTrusted ? rate : prior;
        }
    };

    // Seconds until a member with this TP has a weapon skill's worth, at
    // this rate: none when she never will
    inline auto timeToTp(const int32 tp, const double rate) -> std::optional<double>
    {
        if (tp >= kWsTp)
        {
            return 0.0;
        }
        if (rate <= 0.0)
        {
            return std::nullopt;
        }
        return static_cast<double>(kWsTp - tp) / rate;
    }

    // When the plan's chain lands, from now: both members with the TP, the
    // opener's think coming round, her weapon skill landing, the window's
    // three seconds and the closer's weapon skill landing. None when either
    // never gets the TP
    inline auto chainIn(const std::optional<double> openerReady, const std::optional<double> closerReady) -> std::optional<double>
    {
        if (!openerReady.has_value() || !closerReady.has_value())
        {
            return std::nullopt;
        }
        return std::max(*openerReady, *closerReady) + kThinkWait + kWsLands + kWindowOpens + kWsLands;
    }

    // When a chain lands on an opening already there, `age` seconds old,
    // that a member ready with a closing weapon skill closes the tick its
    // window opens
    inline auto closeIn(const double age) -> double
    {
        return std::max(0.0, kWindowOpens - age) + kWsLands;
    }

    // --- the Black Mage's nuke against the burst ------------------------

    // One nuke of hers, priced: what it deals now and burst (each capped by
    // what the mob has left), her seconds for it, its MP, its recast left
    // and its recast once cast. Every nuke she has learned, not only those
    // she can cast this instant: a burst a few seconds off may be one that
    // is on its recast now
    struct Nuke
    {
        uint16 id      = 0;
        uint8  element = 0;
        double dealt   = 0.0;
        double burst   = 0.0;
        double cast    = 0.0; // its cast alone
        double seconds = 0.0; // her time for it: the cast and the wait after
        int32  mp      = 0;
        double readyIn = 0.0;
        double recast  = 0.0;
        bool   now     = false; // she can cast it this instant (MP and recast)

        auto freeRate() const -> double
        {
            return seconds > 0.0 ? dealt / seconds : dealt;
        }
        auto burstRate() const -> double
        {
            return seconds > 0.0 ? burst / seconds : burst;
        }
    };

    struct BurstView
    {
        int32                 mp       = 0;
        uint16                elements = 0;  // the chain's burst elements
        uint8                 chain    = 0;  // for the words
        std::optional<double> open;          // a window open now: seconds it has left
        std::optional<double> chainIn;       // or a chain expected in this many seconds
        double                windowLasts = kChainLasts;
        double                mobLives    = -1.0; // seconds the mob has left at the party's rate; below 0 unknown
    };

    enum class NukeMove : uint8
    {
        Free,  // her free pick
        Burst, // `spell` bursts the open window
        Hold,  // nothing now: she keeps herself free and her MP for the burst
    };

    // `why` names the call in words that stay put while it does (a hold is
    // said as it changes); `detail` carries its numbers
    struct NukeCall
    {
        NukeMove    move  = NukeMove::Free;
        uint16      spell = 0;
        std::string why;
        std::string detail;
    };

    // The best burst among her nukes: the chain's elements, affordable with
    // `mp`, cast from `from` or once readyIn(nuke) says its recast is over,
    // the cast ending before `until`; the most it deals. nullptr for none
    template <typename ReadyIn>
    auto bestBurst(const std::vector<Nuke>& nukes, const uint16 elements, const int32 mp, const double from, const double until, ReadyIn&& readyIn) -> const Nuke*
    {
        const Nuke* best = nullptr;
        for (const auto& n : nukes)
        {
            if ((elementBit(n.element) & elements) == 0 || n.mp > mp || n.burst <= 0.0)
            {
                continue;
            }
            const double start = std::max(from, readyIn(n));
            if (start + n.cast > until - kCastMargin)
            {
                continue;
            }
            if (best == nullptr || n.burst > best->burst || (n.burst == best->burst && n.mp < best->mp))
            {
                best = &n;
            }
        }
        return best;
    }

    // Her nuke now, the burst weighed in (ROADMAP slice 3h, "The magic
    // burst"). `free` is her pick without a burst (bank_math.h pickNuke over
    // the nukes she can cast now), nullptr for none.
    //  - A window open: the burst that deals the most a second of hers, cast
    //    now and ending inside it, unless her free pick deals more a second.
    //  - A chain expected before the mob falls: her free pick when it leaves
    //    her free by then and her best burst still in reach; else she holds
    //    when that burst is worth more than the free nuke and whatever burst
    //    she could still make after it.
    //  - Nothing on the way: her free pick.
    inline auto nukeCall(const std::vector<Nuke>& nukes, const Nuke* free, const BurstView& v) -> NukeCall
    {
        NukeCall   call;
        const auto asIs = [](const Nuke& n) { return n.readyIn; };
        const auto freeCall = [&](std::string why)
        {
            call.move  = free != nullptr ? NukeMove::Free : NukeMove::Hold;
            call.spell = free != nullptr ? free->id : 0;
            call.why   = std::move(why);
            return call;
        };

        if (v.open.has_value())
        {
            const Nuke* b = nullptr;
            for (const auto& n : nukes)
            {
                if (!n.now || (elementBit(n.element) & v.elements) == 0 || n.burst <= 0.0 || n.cast > *v.open - kCastMargin)
                {
                    continue;
                }
                if (b == nullptr || n.burstRate() > b->burstRate() || (n.burstRate() == b->burstRate() && n.mp < b->mp))
                {
                    b = &n;
                }
            }
            if (b == nullptr)
            {
                return freeCall(fmt::format("no nuke of hers can burst {} in time", chainName(v.chain)));
            }
            if (free != nullptr && free->id != b->id && free->freeRate() > b->burstRate())
            {
                return freeCall(fmt::format("her free pick deals more a second than the {} burst", chainName(v.chain)));
            }
            call.move   = NukeMove::Burst;
            call.spell  = b->id;
            call.why    = fmt::format("bursts {}", chainName(v.chain));
            call.detail = fmt::format("~{:.0f} against ~{:.0f} unbursted, {:.1f} s of the window left", b->burst, b->dealt, *v.open);
            return call;
        }

        if (!v.chainIn.has_value())
        {
            return freeCall("no skillchain on the way");
        }
        const double t = *v.chainIn;
        if (v.mobLives >= 0.0 && v.mobLives < t)
        {
            return freeCall(fmt::format("the mob falls before {} lands", chainName(v.chain)));
        }
        const Nuke* b = bestBurst(nukes, v.elements, v.mp, t, t + v.windowLasts, asIs);
        if (b == nullptr)
        {
            return freeCall(fmt::format("no nuke of hers can burst {}", chainName(v.chain)));
        }
        if (free == nullptr)
        {
            call.move   = NukeMove::Hold;
            call.why    = fmt::format("saving for the {} burst", chainName(v.chain));
            call.detail = fmt::format("in ~{:.0f} s", t);
            return call;
        }

        // After her free pick: its MP spent, her seconds on it, its recast
        const auto afterFree = [free](const Nuke& n)
        {
            return n.id == free->id ? free->recast : n.readyIn;
        };
        const double freeAt = free->seconds;
        const Nuke*  after  = bestBurst(nukes, v.elements, v.mp - free->mp, std::max(t, freeAt), t + v.windowLasts, afterFree);
        if (freeAt <= t && after != nullptr && after->burst >= b->burst)
        {
            return freeCall(fmt::format("free before {} lands, the burst still in reach", chainName(v.chain)));
        }
        const double nukeNow = free->dealt + (after != nullptr ? after->burst : 0.0);
        if (b->burst > nukeNow)
        {
            call.move   = NukeMove::Hold;
            call.why    = fmt::format("saving for the {} burst", chainName(v.chain));
            call.detail = fmt::format("in ~{:.0f} s, ~{:.0f} burst against ~{:.0f} now{}", t, b->burst, free->dealt,
                                      after != nullptr ? fmt::format(" and ~{:.0f} after", after->burst) : std::string(" and no burst after"));
            return call;
        }
        return freeCall(fmt::format("her free pick and what she can burst after are worth more than saving for {}", chainName(v.chain)));
    }
} // namespace cardian::chains
