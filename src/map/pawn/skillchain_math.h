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

#include "chat_codes.h"
#include "ws_phrases.h"

#include "common/cbasetypes.h"

#include <fmt/format.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <limits>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// Skillchains and magic bursts (ROADMAP, Road to subjob item 6, slice 3h;
// the user, 2026-10-15 and 2026-10-16). The rules, pure, over plain facts,
// so xi_test pins them (cardian_skillchain_tests.cpp); the entities are read
// in skillchains.cpp and the gambit engine (CGambits) acts.
//  - PlanBook: the player's plans, one each for his session, and when one
//    ends.
//  - options: every two-step skillchain the party can make, one member's
//    weapon skill opening and another's closing, ranked as the Shortcuts
//    menu lists them.
//  - wsCall: the tactician's Weapon skill (best) -- close what she can,
//    keep clear of what she cannot, follow the player's plan (its TP window,
//    a spare weapon skill alone, saving it for the next fight), else her best.
//    orderHold: a weapon skill a row orders, held as wsCall holds one of a
//    member outside the plan while its chain is under way.
//  - TpClock, startIn and spareWs: each member's TP gain in the fight, when
//    every member of the chain will be in the TP window, and whether one
//    ahead of the rest can spend a weapon skill alone without delaying it.
//  - worth: whether the plan's chain fits the mob -- its damage, a burst
//    with it, against the HP the mob will have left when it lands; wsPrice
//    and burstCounted, what its weapon skills and its burst are priced at.
//  - AskWatch and the lines: what the party's cardians say of the plan.
//  - nukeCall: the Black Mage's nuke against the burst -- cast now, burst
//    now (ahead of the chain, timed to land at burstMoment), or hold to keep
//    herself free and her MP for the burst.
// The game's own rules -- which properties make which chain, its tier, the
// elements that burst it, what a chain deals -- are never copied here: every
// function that needs one takes it as a callable or a number the server
// worked out (battleutils::FormSkillchain, GetSkillchainTier,
// GetSkillchainMagicElement, xi.combat.skillchain on the server).
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
    // closer's TP -- last while the prediction says the chain is coming (both
    // its members will have the TP at their rates, startIn) and end at once
    // when it says it never will; and never longer than this at a weapon
    // skill's TP: her TP is not kept from the fight for a plan that does not
    // come (a player who plays by hand and never opens)
    inline constexpr double kPlanHoldSafety = 30.0;
    // Spare weapon skills alone (spareWs) a member of the plan spends between
    // two of its chains on one mob: a rate read wrong cannot keep the members
    // trading spares for ever
    inline constexpr uint32 kSparesPerHold = 1;
    // TP spent alone outside the plan, or with no plan, waits for a mob at
    // this much HP or more (the user, 2026-09-26, once the default row's
    // gate: no TP on a mob about to fall); a close goes at any HP, and the
    // plan's own opening is judged by its worth (below)
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

    // --- the player's plan ---------------------------------------------

    // The skillchain his party's tacticians' weapon skills make: who opens
    // it and with what, who closes it and with what, and the chain
    struct Plan
    {
        uint32 opener      = 0;
        uint16 openerSkill = 0;
        uint32 closer      = 0;
        uint16 closerSkill = 0;
        uint8  chain       = 0;

        auto names(const uint32 member) const -> bool
        {
            return member != 0 && (opener == member || closer == member);
        }
    };

    // The plans, one per player, for his session and never saved (the user,
    // 2026-10-16: "the plan lasts the session ... dont save it, and also be
    // on the lookout for skillchain participants leaving the party too"). A
    // plan ends when its opener or its closer leaves his party, when he
    // leaves it himself, and with his session. A member who zones away or is
    // knocked out is still in his party: the plan waits for her
    class PlanBook
    {
    public:
        void set(const uint32 owner, const Plan& plan)
        {
            m_plans[owner] = plan;
        }

        // His plan taken back, or ended with his session: whether he had one
        auto clear(const uint32 owner) -> bool
        {
            return m_plans.erase(owner) != 0;
        }

        auto find(const uint32 owner) const -> std::optional<Plan>
        {
            const auto it = m_plans.find(owner);
            return it != m_plans.end() ? std::optional<Plan>(it->second) : std::nullopt;
        }

        // A member out of a party: every plan she opens or closes ends, and
        // her own as a player. The players whose plan ended
        auto memberLeft(const uint32 member) -> std::vector<uint32>
        {
            std::vector<uint32> ended;
            for (auto it = m_plans.begin(); it != m_plans.end();)
            {
                if (it->first == member || it->second.names(member))
                {
                    ended.push_back(it->first);
                    it = m_plans.erase(it);
                }
                else
                {
                    ++it;
                }
            }
            return ended;
        }

    private:
        std::map<uint32, Plan> m_plans;
    };

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

    // --- the TP window: when the chain's members are ready -------------

    // A member's TP gain as it is seen: sampled while she fights, a rate
    // learned over the intervals her TP rose or stood below a weapon
    // skill's worth -- the only TP the rate is asked about, and above it a
    // member holding for a plan, or at the cap, would teach it nothing --
    // (a weapon skill's spend starts the next one afresh), weighted to the
    // last kTpRateSeconds or so. Out of a fight the rate is kept and the
    // baseline dropped; `since` is when her sampling in this fight began
    inline constexpr double kTpSampleGap   = 0.5;  // samples closer than this are skipped
    inline constexpr double kTpGapTooLong  = 10.0; // an interval longer than this starts afresh
    inline constexpr double kTpRateSeconds = 15.0; // the weight's time scale
    inline constexpr double kTpTrusted     = 6.0;  // seconds of measuring before the rate is believed over the prior

    struct TpClock
    {
        double lastAt   = -1.0;
        int32  lastTp   = 0;
        double rate     = 0.0;  // TP a second
        double measured = 0.0;  // seconds the rate has seen
        double since    = -1.0; // when she began fighting, as sampled; below 0 out of a fight

        void note(const int32 tp, const double at, const bool fighting)
        {
            if (!fighting)
            {
                lastAt = -1.0;
                since  = -1.0;
                return;
            }
            if (lastAt < 0.0 || at - lastAt > kTpGapTooLong || at < lastAt)
            {
                lastAt = at;
                lastTp = tp;
                since  = at;
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

    // A member of a chain as the prediction reads her: her TP now and her
    // TP gain a second in this fight (TpClock's rate believed)
    struct Ready
    {
        int32  tp   = 0;
        double rate = 0.0;
    };

    // When the chain can start (the user, 2026-10-16: "predicts all SC
    // participants arriving at 1000-2000tp window"): the moment the last of
    // its members has a weapon skill's TP, in seconds from now; none when
    // one of them never will. Any number of members: two for a plan today,
    // more for a longer chain or a member in two chains
    inline auto startIn(const std::vector<Ready>& members) -> std::optional<double>
    {
        double latest = 0.0;
        for (const auto& m : members)
        {
            const auto ready = timeToTp(m.tp, m.rate);
            if (!ready.has_value())
            {
                return std::nullopt;
            }
            latest = std::max(latest, *ready);
        }
        return latest;
    }

    // A weapon skill alone takes her this long, her swings stopped, before
    // her TP starts to build again; and she is to be back in the window this
    // long before she is wanted, the prediction's own margin
    inline constexpr double kNakedBusy  = 2.0;
    inline constexpr double kSpareSlack = 2.0;

    // A weapon skill of hers alone now is spare (the user, 2026-10-16:
    // "predict when they can throw out a spare naked WS and not disrupt SC
    // timing"): she is in the window; her own opening will be gone from the
    // mob by `horizon` -- else the chain's opener would find it there; and
    // at her rate she has a weapon skill's TP again by then, with
    // kSpareSlack to spare. `horizon`: seconds until her TP is wanted -- the
    // chain's start, or the mob's end when the chain is saved for the next
    // fight. The seconds until she is back in the window; none when it is
    // not spare
    inline auto spareWs(const Ready& her, const std::optional<double> horizon) -> std::optional<double>
    {
        if (her.tp < kWsTp || !horizon.has_value() || her.rate <= 0.0 || *horizon < kWsLands + kOpeningLasts)
        {
            return std::nullopt;
        }
        const double back = kNakedBusy + static_cast<double>(kWsTp) / her.rate;
        if (back + kSpareSlack > *horizon)
        {
            return std::nullopt;
        }
        return back;
    }

    // The same while the chain is saved for the next fight, `lifeLeft`
    // seconds of the mob to go: the saving is building TP for the next pull
    // ("Let's build TP here and chain the next one"), so a weapon skill alone
    // is spare only when holding would waste TP -- she reaches the cap before
    // the mob falls -- and she is back in the window by then
    inline auto saveSpare(const Ready& her, const std::optional<double> lifeLeft) -> std::optional<double>
    {
        if (!lifeLeft.has_value() || her.rate <= 0.0 || static_cast<double>(her.tp) + her.rate * *lifeLeft <= static_cast<double>(kTpCap))
        {
            return std::nullopt;
        }
        return spareWs(her, lifeLeft);
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

    // --- whether the chain is worth it on this mob ------------------------

    // The user, 2026-10-16: "the tactician has to do is estimate the
    // skillchain total damage + mb if blm present to see if it even
    // reasonably fits". The chain fits a mob when the HP it will have left as
    // the chain lands -- the party's damage without the chain taken off
    // meanwhile -- is at least this share of the chain's damage: more of it
    // than that would be spent past the mob's death. A mob it does not fit is
    // a "TP mob": the chain is kept for the next
    inline constexpr double kFitShare = 0.75;

    // What a member's weapon skill has landed while the map server runs (a
    // miss as 0): the running mean of her last few, and how many it has seen
    struct WsLanded
    {
        double mean  = 0.0;
        uint32 count = 0;
    };

    // Landings of one weapon skill of hers that make its measured mean the
    // price: one or two can be a miss, a critical, a mob nearly dead
    inline constexpr uint32 kWsLandedTrusted = 3;

    // A weapon skill's damage as the chain's worth prices it (#496): what
    // hers of it have landed once there are kWsLandedTrusted of them --
    // measured beats the formula, which leaves her gear's procs and a
    // Sneak Attack out -- else the server's own weapon skill at its
    // expectation (`formula`, tactics_bank.lua wsExpected), else what that
    // weapon skill or any of hers has landed however few, else nothing
    inline auto wsPrice(const WsLanded& thisSkill, const std::optional<double> formula, const WsLanded& anyOfHers) -> double
    {
        if (thisSkill.count >= kWsLandedTrusted)
        {
            return thisSkill.mean;
        }
        if (formula.has_value())
        {
            return std::max(0.0, *formula);
        }
        if (thisSkill.count > 0)
        {
            return thisSkill.mean;
        }
        return anyOfHers.count > 0 ? anyOfHers.mean : 0.0;
    }

    struct Worth
    {
        double openerWs       = 0.0;  // the opener's weapon skill's damage
        double closerWs       = 0.0;  // the closer's
        double chainShare     = 0.0;  // the chain's damage for each point of the closer's weapon skill (the server's skillchain formula)
        double burst          = 0.0;  // the best burst a member in the fight would land on it; 0 for none
        int32  hp             = 0;    // the mob's HP now
        double dealtPerSecond = 0.0;  // the party's damage a second without the chain; 0 or less unknown
        double startIn        = 0.0;  // seconds until the chain can start (startIn)
    };

    struct WorthCall
    {
        bool   fits    = true;
        double total   = 0.0; // the chain's damage, its burst included
        double hpThen  = 0.0; // the HP the mob will have left as it lands, without it
        double landsIn = 0.0; // seconds until it lands
    };

    inline auto worth(const Worth& w) -> WorthCall
    {
        WorthCall out;
        out.total   = std::max(0.0, w.openerWs) + std::max(0.0, w.closerWs) * (1.0 + std::max(0.0, w.chainShare)) + std::max(0.0, w.burst);
        out.landsIn = w.startIn + kThinkWait + kWsLands + kWindowOpens + kWsLands;
        out.hpThen  = static_cast<double>(w.hp) - (w.dealtPerSecond > 0.0 ? w.dealtPerSecond * out.landsIn : 0.0);
        out.fits    = out.total > 0.0 ? out.hpThen >= kFitShare * out.total : out.hpThen > 0.0;
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
        uint8       planChain     = 0; // the chain the plan makes (every member knows it)
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
        // The plan on her mob as the tactician reads it: the chain does not
        // fit the mob, so its members keep their TP for the next fight
        // (worth); the player said Skillchain now on it; the prediction says
        // it is coming -- both its members will have the TP at their rates,
        // overdue or not (planComing); and, while it is expected, the seconds
        // until it can start (startIn). Her own weapon skill alone now is
        // spare: she is back in the window in this many seconds (spareWs;
        // none under Skillchain now, nor past kSparesPerHold)
        bool                  saving     = false;
        bool                  forced     = false;
        bool                  planComing = false;
        std::optional<double> startIn;
        std::optional<double> spareIn;
        // How long she has held for the plan at a weapon skill's TP
        // (kPlanHoldSafety)
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
        bool        planHold = false; // a Hold for the plan's chain on this mob: bounded by kPlanHoldSafety
        bool        saveHold = false; // a Hold that keeps the plan's chain for the next fight
        bool        spare    = false; // her weapon skill spends spare TP (spareWs): one of kSparesPerHold
        bool        plays    = false; // she plays her part in the plan: its opening, or its own close
        std::string why;
    };

    // A member of the plan playing her part keeps her TP for it rather than
    // close what is on her mob -- wsCall's step 1, which the others' readings
    // of her ask too (skillchains.cpp: who will close an opening, and the
    // Black Mage's forecast) -- unless it is the plan's own close (its
    // closer, with its weapon skill, on its opening) or her weapon skill is
    // spare; and only while the plan has her TP: the chain saved for the
    // next fight, or coming and her hold within its safety limit
    struct StrayView
    {
        bool   inPlan     = false;
        bool   plansOwn   = false;
        bool   spare      = false;
        bool   saving     = false;
        bool   planComing = false;
        double heldFor    = 0.0;
    };

    inline auto keepsForPlan(const StrayView& s) -> bool
    {
        return s.inPlan && !s.plansOwn && !s.spare && (s.saving || (s.planComing && s.heldFor < kPlanHoldSafety));
    }

    // Outside the plan, its chain under way on her mob is left be (the user,
    // 2026-10-17: a member who is not one of its steps stays out of its way,
    // whatever her seat): its opening while its closer plays his part, so no
    // weapon skill of hers lands between its opener and its closer; and the
    // chain it made while somebody will burst it, whose window a weapon
    // skill of hers would end or move. Why she holds; none when she need not
    inline auto leavesChainBe(const WsView& v) -> std::optional<std::string>
    {
        if (v.seat != Seat::None || !v.resonance.has_value() || v.resonance->left <= kWsLands)
        {
            return std::nullopt;
        }
        const auto& r = *v.resonance;
        if (r.tier == 0 && v.planCloserFights && r.props == v.planOpening)
        {
            return fmt::format("leaves the plan's opening to {}", v.planCloser);
        }
        if (r.tier > 0 && v.planChain != 0 && r.props[0] == v.planChain && v.burstComing)
        {
            return fmt::format("leaves the plan's {} to its burst", chainName(v.planChain));
        }
        return std::nullopt;
    }

    // Outside the plan, the mob is kept clear of an opening of hers while the
    // plan's chain is due on it within the life of one: its opener would find
    // hers there
    inline auto leavesMobClear(const WsView& v) -> std::optional<std::string>
    {
        if (v.seat == Seat::None && v.startIn.has_value() && *v.startIn < kWsLands + kOpeningLasts)
        {
            return fmt::format("leaves the mob clear: the plan's {} is due", chainName(v.planChain));
        }
        return std::nullopt;
    }

    // A weapon skill a row of hers orders -- one naming a weapon skill, any,
    // or a plain Weapon skill (best): not her tactician's, which wsCall
    // judges -- holds while the plan's chain is under way on her mob, or due
    // on it, and she is not one of its steps: wsCall's own holds for a member
    // outside the plan, the same for every seat. Why; none when it may go
    inline auto orderHold(const WsView& v) -> std::optional<std::string>
    {
        if (v.tp < kWsTp)
        {
            return std::nullopt;
        }
        if (auto why = leavesChainBe(v); why.has_value())
        {
            return why;
        }
        return leavesMobClear(v);
    }

    // Her weapon skill now (ROADMAP slice 3h, the steps): outside the plan,
    // the plan's opening left to its closer; a close she can make, once the
    // window opens -- for a member of the plan only the plan's own close, or
    // one her spare TP pays for; a hold while someone else's opening or
    // chain is on her mob that somebody will use; the plan's members save it
    // for the next fight when it does not fit the mob, spend a spare weapon
    // skill alone when the prediction allows, else the closer holds for the
    // opening and the opener opens once the closer has the TP, each hold
    // bounded by kPlanHoldSafety; outside the plan, the mob kept clear of her
    // opening while the plan's chain is due; else her best alone, on a mob at
    // kSpendHpp or more. forms(opening, closing) -> chain and tierOf(chain)
    // are the server's
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
        // Her part in the plan, played on this mob: her partner plays his,
        // and the plan's weapon skill is hers now. Her TP is the plan's
        const bool inPlan = v.seat != Seat::None && v.partnerFights && has(v.planSkill);
        // The plan's holds end at the safety limit
        const bool holdsOn = v.heldFor < kPlanHoldSafety;
        // A spare weapon skill alone: her best, with the prediction's numbers
        const auto spare = [&](const std::string_view why)
        {
            call.move  = WsMove::Free;
            call.skill = v.best;
            call.spare = true;
            call.why   = fmt::format("spends a spare weapon skill alone ({}): back at {} TP in ~{:.0f} s", why, kWsTp, v.spareIn.value_or(0.0));
            return call;
        };

        // 0. Outside the plan, its chain under way on her mob is left be: its
        // opening is its closer's while he fights the mob, and the chain it
        // made is its burster's
        if (auto why = leavesChainBe(v); why.has_value())
        {
            call.move = WsMove::Hold;
            call.why  = std::move(*why);
            return call;
        }

        // 1. and 2. Something on her mob, with time left for a weapon skill
        // to land on it
        if (v.resonance.has_value() && v.resonance->left > kWsLands)
        {
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
                // A member of the plan closes only the plan's own close --
                // its closer, with its weapon skill, on its opening -- or one
                // her spare TP pays for: any other spends TP the plan counts on
                const bool plansOwn = v.seat == Seat::Closer && r.tier == 0 && r.props == v.planOpening && pick->id == v.planSkill;
                if (keepsForPlan({ inPlan, plansOwn, v.spareIn.has_value(), v.saving, v.planComing, v.heldFor }))
                {
                    call.move     = WsMove::Hold;
                    call.chain    = 0;
                    call.skill    = 0;
                    call.planHold = !v.saving;
                    call.why      = fmt::format("keeps her TP for the plan's {}", chainName(v.planChain));
                    return call;
                }
                // Hers is the plan's own close, or one her spare TP pays for
                // (another's, with the plan coming or saved)
                call.plays = inPlan && plansOwn;
                call.spare = inPlan && !plansOwn && v.spareIn.has_value();
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

        // 3. and 4. Her part in the plan
        if (inPlan)
        {
            // The chain does not fit this mob (worth): kept for the next fight
            if (v.saving)
            {
                if (v.spareIn.has_value() && v.best != 0)
                {
                    return spare("holding would pass the TP cap before the mob falls");
                }
                call.move     = WsMove::Hold;
                call.saveHold = true;
                call.why      = fmt::format("saves {} for the next fight", chainName(v.planChain));
                return call;
            }
            const auto due = fmt::format("ahead of {}'s TP window", chainName(v.planChain));
            if (v.seat == Seat::Closer)
            {
                // The closer holds her TP for the opening while it is
                // coming; ahead of her opener, a spare weapon skill alone first
                if (v.spareIn.has_value() && v.best != 0)
                {
                    return spare(due);
                }
                if (v.planComing && holdsOn)
                {
                    call.move     = WsMove::Hold;
                    call.planHold = true;
                    call.why      = fmt::format("holds her TP to close {} on {}'s opening", chainName(v.planChain), v.partner);
                    return call;
                }
            }
            else
            {
                // The opener opens once the closer has the TP
                if (v.partnerReady)
                {
                    call.move  = WsMove::Open;
                    call.skill = v.planSkill;
                    call.chain = v.planChain;
                    call.plays = true;
                    call.why   = fmt::format("opens {} for {}", chainName(v.planChain), v.partner);
                    return call;
                }
                if (v.spareIn.has_value() && v.best != 0)
                {
                    return spare(due);
                }
                if (v.planComing && holdsOn)
                {
                    call.move     = WsMove::Hold;
                    call.planHold = true;
                    call.why      = fmt::format("waits for {}'s TP to open {}", v.partner, chainName(v.planChain));
                    return call;
                }
                // His TP is not coming: the plan's weapon skill alone, which
                // is TP spent with no chain -- never on a mob about to fall
                // (kSpendHpp)
                if (v.mobHpp < kSpendHpp)
                {
                    call.move = WsMove::Hold;
                    call.why  = "the mob is under half its HP";
                    return call;
                }
                call.move  = WsMove::Free;
                call.skill = v.planSkill;
                call.why   = fmt::format("opens with the plan's weapon skill alone ({}'s TP did not come)", v.partner);
                return call;
            }
        }

        // 5. Outside the plan while its chain is due on her mob: her own
        // opening would still be there when its opener opens
        if (auto why = leavesMobClear(v); why.has_value())
        {
            call.move = WsMove::Hold;
            call.why  = std::move(*why);
            return call;
        }

        // 6. Her best alone
        if (v.best == 0)
        {
            return call;
        }
        if (v.mobHpp < kSpendHpp)
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

    // --- what the party's cardians say of the plan ----------------------

    // With no plan picked and the party able to make a skillchain, one of
    // the player's cardians asks once as the party settles into its first
    // fights (the user, 2026-10-16)
    inline constexpr std::array<std::string_view, 5> kAskLines{
        "What skillchain are we doing?",
        "Anyone want to set up a skillchain? My TP's ready to go.",
        "Our weapon skills could link up nicely. Which chain are we going for?",
        "Should I be opening or closing? We haven't picked a skillchain yet.",
        "Our weapon skills can make a skillchain. Want to pick one?",
    };

    // The plan's members keeping their TP for the next fight (worth): one of
    // them says so, once a fight (the user, 2026-10-16: "Cardians can
    // account they're saving SC+MB for the next fight")
    inline constexpr std::array<std::string_view, 5> kSaveLines{
        "Holding the skillchain for the next one.",
        "This one's going down anyway. Saving our skillchain.",
        "Let's build TP here and chain the next one.",
        "Not worth a skillchain. Holding it for the next fight.",
        "No need on this one. Saving my TP for the next fight.",
    };

    // The plan's calls, as a player's macros make them (the user,
    // 2026-10-17, from his own party chat): its opener as she opens it,
    // "Starting {Skillchain}! {Wasp Sting}! Brutal Sheep <call12>" -- the
    // weapon skill in the game's braces when the dictionary has it
    // (ws_phrases.h), plain otherwise, and the mob where his macro has <t>
    // -- and its closer, with the TP for her close, once a fight while she
    // holds it: "{Ready!} 1240 TP <call14>". The calls are his choice
    // (Settings), none for chat::kNoCall
    inline constexpr uint8_t kSkillchainPhraseGroup = 8; // "Skillchain"
    inline constexpr uint8_t kSkillchainPhraseEntry = 45;
    inline constexpr uint8_t kReadyPhraseGroup      = 5; // "Ready!"
    inline constexpr uint8_t kReadyPhraseEntry      = 5;

    inline auto openingLine(const std::string_view weaponSkill, const std::string_view mob, const uint8_t call) -> std::string
    {
        const uint8_t entry = cardian::chat::weaponSkillEntry(weaponSkill);
        std::string   line  = "Starting " + cardian::chat::autoTranslate(kSkillchainPhraseGroup, kSkillchainPhraseEntry) + "! ";
        line += entry != 0 ? cardian::chat::autoTranslate(cardian::chat::kWeaponSkillGroup, entry) : std::string(weaponSkill);
        line += "! ";
        line += mob;
        if (const auto sound = cardian::chat::callBytes(call); !sound.empty())
        {
            line += " " + sound;
        }
        return line;
    }

    inline auto readyLine(const int32 tp, const uint8_t call) -> std::string
    {
        std::string line = cardian::chat::autoTranslate(kReadyPhraseGroup, kReadyPhraseEntry) + " " + std::to_string(tp) + " TP";
        if (const auto sound = cardian::chat::callBytes(call); !sound.empty())
        {
            line += " " + sound;
        }
        return line;
    }

    // A member of the plan not engaged yet is counted in as joining the
    // fight for this long, while she is this near the mob and on no other
    // (skillchains.cpp joining): from when the camp's tank has the mob
    // placed, or the plan first counts on her with no tank placing it. 7 s
    // to start with (the user, 2026-10-17): the map log says how long each
    // took, to tune it by
    inline constexpr double kJoinGrace = 7.0;
    inline constexpr float  kJoinReach = 30.0f;

    // The ask is said within the session's first fights, once its speaker
    // has been in the fight this long -- settled in, never on the first swing
    inline constexpr std::size_t kAskWithinFights = 3;
    inline constexpr double      kAskSettle       = 8.0;

    // When to look for a skillchain the party could make, and ask for a
    // plan: once a session, in one of its first kAskWithinFights fights,
    // kAskSettle into it, each fight looked at once
    struct AskWatch
    {
        bool                said = false;
        std::vector<uint32> fights; // the session's first fights, by their mob
        std::vector<uint32> looked; // those already looked at

        auto due(const uint32 mob, const double secondsIn) -> bool
        {
            if (said || mob == 0)
            {
                return false;
            }
            if (std::ranges::find(fights, mob) == fights.end())
            {
                if (fights.size() >= kAskWithinFights)
                {
                    return false;
                }
                fights.push_back(mob);
            }
            if (secondsIn < kAskSettle || std::ranges::find(looked, mob) != looked.end())
            {
                return false;
            }
            looked.push_back(mob);
            return true;
        }
    };

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

    // The server's regeneration tick (zone_entities.cpp, every 3 s): her
    // Refresh and the like come in that often
    inline constexpr double kRegenTick = 3.0;

    // The moment a burst is meant to land (#491; the user, 2026-10-17: she
    // "plans her cast start backwards from when she intends the magic burst
    // to land"): the middle of the window the chain opens, so a chain that
    // comes a little early or late -- a player's close by hand, a think late
    // to come round -- still finds the burst inside its window. The server
    // bursts a nuke whose damage resolves, at its cast's end, while the
    // chain is on the mob (magic_burst.lua getMagicBurstTier), and a chain
    // of two weapon skills stays kChainLasts from its landing
    // (battleutils GetSkillChainEffect). Both ways round, from one rule: the
    // burst for a chain landing in `chainIn` seconds, and the chain wanted
    // for a burst meant for `burstAt` -- what a Black Mage who starts her
    // cast first would tell the party to close by (not built)
    inline auto burstMoment(const double chainIn, const double windowLasts = kChainLasts) -> double
    {
        return chainIn + windowLasts / 2.0;
    }

    inline auto chainWantedFor(const double burstAt, const double windowLasts = kChainLasts) -> double
    {
        return burstAt - windowLasts / 2.0;
    }

    // Her cast's start that lands it at `burstAt`: its resolved cast time
    // before it
    inline auto castStartFor(const double burstAt, const double cast) -> double
    {
        return burstAt - cast;
    }

    // Who the chain's worth counts a burst from (#497; the user,
    // 2026-10-17): a member whose tactician casts her nukes -- a cardian, or
    // a played character on autopilot -- as her tactician would land it; and
    // a player bursting by hand, in full, when he knows a nuke of the
    // chain's burst elements and has the MP for one -- the Shortcuts list's
    // own test for naming him a burster -- and is at the fight
    struct BursterView
    {
        bool   tactician = false; // her tactician casts her nukes, and she is in the fight on the mob
        double tacticians = 0.0;  // what her tactician's best burst would land (CGambits::BurstEstimate)
        bool   player    = false; // a real player, playing by hand or on autopilot
        bool   atFight   = false; // near the mob, in its zone, alive
        bool   knowsNuke = false; // a nuke of the chain's burst elements learned, at his jobs and level
        bool   mpForOne  = false; // the MP for one of those
        double byHand    = 0.0;   // what his best of those would land, priced as a cardian's are
    };

    inline auto burstCounted(const BursterView& b) -> double
    {
        double best = b.tactician ? std::max(0.0, b.tacticians) : 0.0;
        if (b.player && b.atFight && b.knowsNuke && b.mpForOne)
        {
            best = std::max(best, b.byHand);
        }
        return best;
    }

    struct BurstView
    {
        int32                 mp          = 0;
        int32                 mpMax       = 0;   // her maximum, where what she regains stops; 0 for none known
        double                mpPerSecond = 0.0; // what she regains a second in the fight, her Refresh (the user, 2026-10-16)
        uint16                elements    = 0;   // the chain's burst elements
        uint8                 chain       = 0;   // for the words
        std::optional<double> open;              // a window open now: seconds it has left
        std::optional<double> chainIn;           // or a chain expected in this many seconds
        double                windowLasts = kChainLasts;
        double                mobLives    = -1.0; // seconds the mob has left at the party's rate; below 0 unknown

        // Her MP `at` seconds from now from `base`: what she regains
        // meanwhile added, up to her maximum
        auto mpAt(const int32 base, const double at) const -> int32
        {
            const double gained = mpPerSecond > 0.0 && at > 0.0 ? mpPerSecond * at : 0.0;
            const double then   = static_cast<double>(base) + gained;
            return static_cast<int32>(mpMax > 0 ? std::min(then, static_cast<double>(std::max(base, mpMax))) : then);
        }
    };

    enum class NukeMove : uint8
    {
        Free,  // her free pick
        Burst, // `spell` bursts the open window
        Hold,  // nothing now: she keeps herself free and her MP for the burst
    };

    // `why` names the call in words that stay put while it does (a hold is
    // said as it changes); `detail` carries its numbers. `ahead`: a Burst
    // started before the chain has landed, timed to land inside its window
    // (#491). `burstIn`: seconds until the cast of the burst she plans
    // starts -- 0 for one starting now; none when no burst is planned --
    // which the tick's own check waits for (CGambits::BurstNow)
    struct NukeCall
    {
        NukeMove              move  = NukeMove::Free;
        uint16                spell = 0;
        bool                  ahead = false;
        std::optional<double> burstIn;
        std::string           why;
        std::string           detail;
    };

    // A burst among her nukes, and when its cast starts
    struct PlannedBurst
    {
        const Nuke* nuke  = nullptr;
        double      start = 0.0;
    };

    // The best burst among her nukes for a chain landing at `chainAt`, its
    // window ending at `until`: the chain's elements, affordable with what
    // mpAt(start) says she has as its cast starts, started no earlier than
    // `from` (she is free) nor than readyIn(nuke) (its recast), and no
    // earlier than `aim` less its cast -- landing at the moment the burst is
    // meant for (burstMoment), later if she is busy until then; landing
    // after the chain and before the window ends, kCastMargin to spare; the
    // most it deals. No nuke for none
    template <typename MpAt, typename ReadyIn>
    auto bestBurst(const std::vector<Nuke>& nukes, const uint16 elements, MpAt&& mpAt, const double from, const double chainAt, const double aim, const double until,
                   ReadyIn&& readyIn) -> PlannedBurst
    {
        PlannedBurst best;
        for (const auto& n : nukes)
        {
            if ((elementBit(n.element) & elements) == 0 || n.burst <= 0.0)
            {
                continue;
            }
            const double start = std::max({ from, readyIn(n), castStartFor(aim, n.cast) });
            const double lands = start + n.cast;
            if (lands < chainAt || lands > until - kCastMargin || n.mp > mpAt(start))
            {
                continue;
            }
            if (best.nuke == nullptr || n.burst > best.nuke->burst || (n.burst == best.nuke->burst && n.mp < best.nuke->mp))
            {
                best = PlannedBurst{ &n, start };
            }
        }
        return best;
    }

    // How often the tick's own check asks for a burst (CGambits::BurstNow): a
    // burst whose cast is due within half of it starts now, so the cast
    // starts as near its planned moment as the checks allow
    inline constexpr double kBurstCheckEvery = 0.5;

    // Her nuke now, the burst weighed in (ROADMAP slice 3h, "The magic
    // burst"). `free` is her pick without a burst (bank_math.h pickNuke over
    // the nukes she can cast now), nullptr for none.
    //  - A window open: the burst that deals the most a second of hers, cast
    //    now and ending inside it, unless her free pick deals more a second.
    //  - A chain expected before the mob falls: her best burst on it, its
    //    cast started so it lands at burstMoment, the middle of the chain's
    //    window (#491) -- now, ahead of the chain, once that start has come.
    //    Before then, her free pick when it leaves her free by the burst's
    //    start with the burst still in reach -- the MP she regains meanwhile
    //    counted, so a chain far off leaves her nuking freely; else she holds
    //    when that burst is worth more than the free nuke and whatever burst
    //    she could still make after it. A chain that does not come leaves her
    //    that nuke cast unbursted, its MP and time spent as a burst's
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
            call.move    = NukeMove::Burst;
            call.spell   = b->id;
            call.burstIn = 0.0;
            call.why     = fmt::format("bursts {}", chainName(v.chain));
            call.detail  = fmt::format("~{:.0f} against ~{:.0f} unbursted, {:.1f} s of the window left", b->burst, b->dealt, *v.open);
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
        const double aim   = burstMoment(t, v.windowLasts);
        const double until = t + v.windowLasts;
        const auto   mpNow = [&v](const double at) { return v.mpAt(v.mp, at); };
        const auto   b     = bestBurst(nukes, v.elements, mpNow, 0.0, t, aim, until, asIs);
        if (b.nuke == nullptr)
        {
            return freeCall(fmt::format("no nuke of hers can burst {}", chainName(v.chain)));
        }
        // Her burst's cast, started now ahead of the chain to land at its
        // planned moment: the window's middle, or later where she was busy
        const auto burstNow = [&]()
        {
            call.move    = NukeMove::Burst;
            call.spell   = b.nuke->id;
            call.ahead   = true;
            call.burstIn = 0.0;
            call.why     = fmt::format("bursts {} ahead of the chain", chainName(v.chain));
            call.detail  = fmt::format("~{:.0f} against ~{:.0f} unbursted: {} in ~{:.1f} s, her {:.1f} s cast landing ~{:.1f} s into its {:.0f} s window", b.nuke->burst,
                                       b.nuke->dealt, chainName(v.chain), t, b.nuke->cast, b.start + b.nuke->cast - t, v.windowLasts);
            return call;
        };
        const bool due = b.start <= kBurstCheckEvery / 2.0 && b.nuke->now;
        // Due now and her free pick besides: cast now, it is the burst
        if (due && free != nullptr && free->id == b.nuke->id)
        {
            return burstNow();
        }
        if (free == nullptr)
        {
            if (due)
            {
                return burstNow();
            }
            call.move    = NukeMove::Hold;
            call.burstIn = b.start;
            call.why     = fmt::format("saving for the {} burst", chainName(v.chain));
            call.detail  = fmt::format("in ~{:.0f} s, her cast starting in ~{:.1f} s", t, b.start);
            return call;
        }

        // After her free pick: its MP spent, what she regains by her burst's
        // cast, her seconds on it, its recast
        const auto afterFree = [free](const Nuke& n)
        {
            return n.id == free->id ? free->recast : n.readyIn;
        };
        const auto mpAfter = [&v, free](const double at) { return v.mpAt(v.mp - free->mp, at); };
        const auto after   = bestBurst(nukes, v.elements, mpAfter, free->seconds, t, aim, until, afterFree);
        // Free in time: her free pick leaves her the burst as planned, its
        // cast starting at its moment and not pushed later by the free one
        const bool onTime = after.nuke != nullptr && after.start <= castStartFor(aim, after.nuke->cast) + kBurstCheckEvery;
        if (onTime && after.nuke->burst >= b.nuke->burst)
        {
            call.burstIn = after.start;
            return freeCall(fmt::format("free before her {} burst's cast, the burst still in reach", chainName(v.chain)));
        }
        const double nukeNow = free->dealt + (after.nuke != nullptr ? after.nuke->burst : 0.0);
        if (b.nuke->burst > nukeNow)
        {
            if (due)
            {
                return burstNow();
            }
            call.move    = NukeMove::Hold;
            call.burstIn = b.start;
            call.why     = fmt::format("saving for the {} burst", chainName(v.chain));
            call.detail  = fmt::format("in ~{:.0f} s, her cast starting in ~{:.1f} s; ~{:.0f} burst against ~{:.0f} now{}", t, b.start, b.nuke->burst, free->dealt,
                                       after.nuke != nullptr ? fmt::format(" and ~{:.0f} after", after.nuke->burst) : std::string(" and no burst after"));
            return call;
        }
        if (after.nuke != nullptr)
        {
            call.burstIn = after.start;
        }
        return freeCall(fmt::format("her free pick and what she can burst after are worth more than saving for {}", chainName(v.chain)));
    }
} // namespace cardian::chains
