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

// The fight log's arithmetic (RESEARCH §12.5, §12.12): what one record
// counts, how a spot's averages fold, what a cure is really worth and what
// a debuff stopped. Pure and entity-free, so cardian_tactics_tests.cpp can
// hold it to account without a map server.

#include "common/cbasetypes.h"

#include <fmt/format.h>

#include <algorithm>
#include <deque>
#include <iterator>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace cardian::tactics
{
    // A running figure: a plain mean over the first few samples, then the
    // recent ones weigh more, so a change in the party shows within a
    // handful of fights
    struct Running
    {
        double mean = 0.0;
        uint32 n    = 0;

        void fold(const double v, const double alpha = 0.3)
        {
            ++n;
            const double w = std::max(alpha, 1.0 / n);
            mean += w * (v - mean);
        }
    };

    // The cure tally per caster and spell: MP spent against HP landed. The
    // truth, and it shows the waste of curing near full.
    struct CureTally
    {
        uint32 casts    = 0;
        int32  mpSpent  = 0;
        int32  hpLanded = 0;
        uint32 toppedUp = 0; // casts that filled the target: some of the cure was wasted

        auto hpPerMp() const -> double
        {
            return mpSpent > 0 ? static_cast<double>(hpLanded) / mpSpent : 0.0;
        }
    };

    // The raw-cure estimate per caster and spell: what she really heals
    // for. A cast that did not top the target up is exact; until one is
    // seen, the biggest topped-up cure is the floor (the tier's minimum
    // cure at first). The prediction is the larger of the two, so a cure
    // that grew with a ding still shows.
    struct CureEstimate
    {
        int32 floor = 0;
        int32 exact = 0;

        void note(const int32 landed, const bool toppedUp)
        {
            if (toppedUp)
            {
                floor = std::max(floor, landed);
            }
            else
            {
                // An exact measurement beats any floor, the tier's minimum
                // included; a bigger topped-up cure after it means she grew
                exact = landed;
                floor = landed;
            }
        }

        auto predict() const -> int32
        {
            return floor;
        }

        auto known() const -> bool
        {
            return exact > 0;
        }
    };

    // A debuff's worth against a mob type: casts, resists, lands, and the
    // HP its procs stopped. Value per cast counts the resists and the
    // zero-proc lands, honestly.
    struct DebuffValue
    {
        uint32 casts   = 0;
        uint32 resists = 0;
        uint32 lands   = 0;

        void note(const bool tookEffect)
        {
            ++casts;
            if (tookEffect)
            {
                ++lands;
            }
            else
            {
                ++resists;
            }
        }
    };

    // Paralysis procs against a mob type, and what they stopped: a melee
    // round at the mob's measured hit, or a spell (an interrupt avoided,
    // unpriced yet)
    struct ProcValue
    {
        uint32 melee     = 0;
        uint32 spells    = 0;
        double hpStopped = 0.0;
    };

    // What a debuff on the mob dealt for us, the exact number for Dia
    // (RESEARCH §12.13), booked by the bank: its share of the extra our
    // physical hits dealt for the defence it took, and its share of the
    // ticks the mob lost to damage over time
    struct EffectCredit
    {
        std::string effect;
        double      hp    = 0.0; // from the defence it took
        double      ticks = 0.0; // from its damage over time
    };

    struct MemberFigures
    {
        uint32      id = 0;
        std::string name;
        int32       damageTaken  = 0;
        int32       biggestHit   = 0; // the raw hit, past her HP if it went there
        uint32      hits         = 0;
        int32       tpMoveDamage = 0;
        int32       damageDealt  = 0;
        int32       wsDamage     = 0;
        uint32      wsCount      = 0;
        uint32      casts        = 0;
        uint32      interrupted  = 0;
        int32       mpSpent      = 0;
        int32       hpCured      = 0; // what her cures landed
        int32       overcure     = 0; // estimated: the raw cure past the target's missing HP
        uint32      targeted     = 0; // the mob turned onto her this many times
        uint32      deaths       = 0;
        uint32      paralysed    = 0; // her swings and casts a paralysis proc stopped

        auto meanHit() const -> double
        {
            return hits > 0 ? static_cast<double>(damageTaken) / hits : 0.0;
        }
    };

    struct CastNote
    {
        uint32      caster = 0;
        uint32      target = 0;
        uint16      spell  = 0;
        std::string spellName;
        int32       mp          = 0;
        int32       landed      = 0;
        bool        cure        = false;
        bool        debuff      = false;
        bool        tookEffect  = false;
        bool        toppedUp    = false;
    };

    // One fight: a mob from the first engagement to its death, a wipe, or
    // the party walking away. Per member, the player included.
    struct FightRecord
    {
        uint32      mobId = 0;
        std::string mobName;
        uint16      zone = 0;
        std::string zoneName;
        int32       mobMaxHp  = 0;
        int32       mobDamage = 0;   // everything the mob took, whoever or whatever dealt it
        int32       mobHpLeft = -1;  // at a close that was not a kill: where it was left
        double      openedAt  = 0.0; // seconds
        double      closedAt  = 0.0;
        double      idleSince = 0.0; // nobody of ours on it, and it on nobody of ours, since
        double      deadSince = 0.0; // read as dead with no death event yet, since
        double      diedAt    = 0.0; // the death event, or the first read as dead
        double      settlingSince = 0.0; // dead, and the cures that follow still count, since
        std::string closeWhy;
        bool        overlapping = false; // another record was open at the same time: a link
        uint32      hitting     = 0;     // whom the mob is on
        uint32      switches    = 0;
        uint32      procs       = 0;

        std::vector<std::pair<std::string, int32>> tpMoveNames; // each TP move by name, with what it dealt us

        // What the debuffs on the mob dealt for us, per effect: the exact
        // number, what our physical hits dealt past what they would have at
        // the mob's base defence, and the ticks
        std::vector<EffectCredit> credits;
        double                    defDownExtra = 0.0;
        uint32                    defDownHits  = 0;
        double                    dotDealt     = 0.0;

        auto creditFor(const std::string_view effect) -> EffectCredit&
        {
            for (auto& c : credits)
            {
                if (c.effect == effect)
                {
                    return c;
                }
            }
            credits.push_back(EffectCredit{ .effect = std::string(effect) });
            return credits.back();
        }

        auto settling() const -> bool
        {
            return settlingSince > 0.0;
        }

        std::deque<MemberFigures>  members; // a deque: a member's figures stay put while others are added
        std::vector<CastNote>      casts;

        // The bank's own state for the fight (spell_bank.cpp): the land
        // chances it sampled, the price list it printed, and the members
        // it has priced
        struct LandChance
        {
            double chance = 0.0; // of landing
            double rate   = 0.0; // the mean resist rate when it lands: the duration's factor
        };
        std::map<std::pair<uint32, uint16>, LandChance> landCache; // caster, spell
        // What the formulas say one entity's melee does to another, before
        // a fight has measured it (RESEARCH §12.2 item 5, perfect knowledge)
        struct MeleeGuess
        {
            double perRound  = 0.0; // expected damage a round: swings x hit rate x base x mean pDIF
            double perSecond = 0.0;
            double biggest   = 0.0; // one swing at the biggest pDIF sampled
        };
        std::map<std::pair<uint32, uint32>, std::optional<MeleeGuess>> meleeCache; // actor, target; a miss is remembered too
        double priorTaken = -1.0; // the formulas' taken/s and dealt/s, as the bank first priced the fight
        double priorDealt = -1.0;
        std::vector<std::string>                        priceList;
        std::set<uint32>                                priced;

        auto member(const uint32 id, const std::string_view name) -> MemberFigures&
        {
            for (auto& m : members)
            {
                if (m.id == id)
                {
                    return m;
                }
            }
            members.push_back(MemberFigures{ .id = id, .name = std::string(name) });
            return members.back();
        }

        auto find(const uint32 id) const -> const MemberFigures*
        {
            for (const auto& m : members)
            {
                if (m.id == id)
                {
                    return &m;
                }
            }
            return nullptr;
        }

        auto seconds(const double now) const -> double
        {
            return std::max(0.0, (closedAt > 0.0 ? closedAt : now) - openedAt);
        }

        // The fight's own rates, the spot's averages and the bank's live
        // figures alike
        auto dealtPerSecond(const double now) const -> double
        {
            const double secs = seconds(now);
            return secs > 0.0 ? dealt() / secs : 0.0;
        }

        auto takenPerSecond(const double now) const -> double
        {
            const double secs = seconds(now);
            return secs > 0.0 ? taken() / secs : 0.0;
        }

        auto taken() const -> int32
        {
            int32 sum = 0;
            for (const auto& m : members)
            {
                sum += m.damageTaken;
            }
            return sum;
        }

        auto dealt() const -> int32
        {
            int32 sum = 0;
            for (const auto& m : members)
            {
                sum += m.damageDealt;
            }
            return sum;
        }

        auto wsDamage() const -> std::pair<uint32, int32>
        {
            uint32 count = 0;
            int32  sum   = 0;
            for (const auto& m : members)
            {
                count += m.wsCount;
                sum += m.wsDamage;
            }
            return { count, sum };
        }

        auto cureMp() const -> int32
        {
            int32 sum = 0;
            for (const auto& c : casts)
            {
                if (c.cure)
                {
                    sum += c.mp;
                }
            }
            return sum;
        }

        auto cureCasts() const -> uint32
        {
            uint32 n = 0;
            for (const auto& c : casts)
            {
                if (c.cure)
                {
                    ++n;
                }
            }
            return n;
        }

        auto cureHp() const -> int32
        {
            int32 sum = 0;
            for (const auto& m : members)
            {
                sum += m.hpCured;
            }
            return sum;
        }

        auto overcure() const -> int32
        {
            int32 sum = 0;
            for (const auto& m : members)
            {
                sum += m.overcure;
            }
            return sum;
        }

        auto biggest() const -> const MemberFigures*
        {
            const MemberFigures* top = nullptr;
            for (const auto& m : members)
            {
                if (m.biggestHit > 0 && (top == nullptr || m.biggestHit > top->biggestHit))
                {
                    top = &m;
                }
            }
            return top;
        }
    };

    // The one line the map log gets when a record closes
    inline auto summary(const FightRecord& r) -> std::string
    {
        const double secs = r.seconds(r.closedAt);
        const double rate = secs > 0.0 ? r.taken() / secs : 0.0;

        std::string line = fmt::format("tactics: {} in {}, {:.0f} s, {}{}: took {} ({:.1f}/s", r.mobName, r.zoneName, secs, r.closeWhy, r.overlapping ? ", linked" : "", r.taken(), rate);
        if (const auto* top = r.biggest(); top != nullptr)
        {
            line += fmt::format(", biggest {} on {}", top->biggestHit, top->name);
        }
        if (!r.tpMoveNames.empty())
        {
            line += ", TP moves:";
            for (std::size_t i = 0; i < r.tpMoveNames.size(); ++i)
            {
                line += fmt::format("{} {} {}", i == 0 ? "" : ",", r.tpMoveNames[i].first, r.tpMoveNames[i].second);
            }
        }
        line += fmt::format("), dealt {}", r.dealt());
        if (r.mobDamage > r.dealt())
        {
            line += fmt::format(" of {} the mob lost", r.mobDamage);
        }
        if (r.mobHpLeft >= 0 && r.closeWhy != "killed" && r.mobMaxHp > 0)
        {
            line += fmt::format(", left it at {} HP ({}%)", r.mobHpLeft, r.mobHpLeft * 100 / r.mobMaxHp);
        }
        if (const auto [wsCount, wsSum] = r.wsDamage(); wsCount > 0)
        {
            line += fmt::format(" ({} weapon skill{} for {})", wsCount, wsCount == 1 ? "" : "s", wsSum);
        }
        if (const auto cures = r.cureCasts(); cures > 0)
        {
            line += fmt::format("; cures {} for {} MP, {} HP landed", cures, r.cureMp(), r.cureHp());
            if (r.overcure() > 0)
            {
                line += fmt::format(", ~{} over", r.overcure());
            }
        }

        // Debuffs by name, in the order first cast
        std::vector<std::pair<std::string, std::pair<uint32, uint32>>> debuffs; // name -> casts, landed
        for (const auto& c : r.casts)
        {
            if (!c.debuff)
            {
                continue;
            }
            auto it = std::find_if(debuffs.begin(), debuffs.end(), [&](const auto& d)
                                   {
                                       return d.first == c.spellName;
                                   });
            if (it == debuffs.end())
            {
                debuffs.emplace_back(c.spellName, std::pair<uint32, uint32>{ 0, 0 });
                it = std::prev(debuffs.end());
            }
            ++it->second.first;
            if (c.tookEffect)
            {
                ++it->second.second;
            }
        }
        for (const auto& [name, counts] : debuffs)
        {
            line += fmt::format("; {} {} cast, {} landed", name, counts.first, counts.second);
        }
        if (r.procs > 0)
        {
            line += fmt::format(", {} proc{}", r.procs, r.procs == 1 ? "" : "s");
        }
        if (r.switches > 0)
        {
            line += fmt::format("; the mob switched {} time{}", r.switches, r.switches == 1 ? "" : "s");
        }
        if (r.priorTaken >= 0.0 || r.priorDealt >= 0.0)
        {
            line += "; the formulas said";
            if (r.priorTaken >= 0.0)
            {
                line += fmt::format(" {:.1f} taken/s", r.priorTaken);
            }
            if (r.priorDealt >= 0.0)
            {
                line += fmt::format("{} {:.1f} dealt/s", r.priorTaken >= 0.0 ? " and" : "", r.priorDealt);
            }
        }
        std::string dealt;
        for (const auto& c : r.credits)
        {
            if (c.hp < 0.5 && c.ticks < 0.5)
            {
                continue;
            }
            dealt += fmt::format("{} {}", dealt.empty() ? "" : ",", c.effect);
            if (c.hp >= 0.5)
            {
                dealt += fmt::format(" +{:.0f} by defence over {} hit{}", c.hp, r.defDownHits, r.defDownHits == 1 ? "" : "s");
            }
            if (c.ticks >= 0.5)
            {
                dealt += fmt::format("{} +{:.0f} by ticks", c.hp >= 0.5 ? "," : "", c.ticks);
            }
        }
        if (!dealt.empty())
        {
            line += "; debuffs dealt:" + dealt;
        }
        std::string paralysed;
        for (const auto& m : r.members)
        {
            if (m.paralysed > 0)
            {
                paralysed += fmt::format("{}{} {}", paralysed.empty() ? "" : ", ", m.name, m.paralysed);
            }
        }
        if (!paralysed.empty())
        {
            line += "; paralysed: " + paralysed;
        }
        return line;
    }

    // One spot's memory of a mob type: what the next such fight will cost
    struct SpotAverages
    {
        uint32  fights = 0;
        Running seconds;
        Running cureMp;
        Running biggestHit;
        Running takenPerSecond;
        Running dealtPerSecond;

        void fold(const FightRecord& r)
        {
            const double secs = r.seconds(r.closedAt);
            ++fights;
            seconds.fold(secs);
            cureMp.fold(r.cureMp());
            const auto* top = r.biggest();
            biggestHit.fold(top != nullptr ? top->biggestHit : 0);
            takenPerSecond.fold(r.takenPerSecond(r.closedAt));
            dealtPerSecond.fold(r.dealtPerSecond(r.closedAt));
        }

        auto line(const std::string_view mob) const -> std::string
        {
            return fmt::format("{} x{}: {:.0f} s, {:.0f} MP of cures, biggest hit {:.0f}, {:.1f} taken/s, {:.1f} dealt/s",
                               mob, fights, seconds.mean, cureMp.mean, biggestHit.mean, takenPerSecond.mean, dealtPerSecond.mean);
        }
    };
} // namespace cardian::tactics
