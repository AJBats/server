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

// The MP bank's arithmetic (RESEARCH §12.6, §12.13): what a spell is worth
// in cure MP, from the figures the fight log measured and the answers the
// server's own formulas gave. Pure and entity-free, so
// cardian_tactics_tests.cpp can hold it to account; spell_bank.cpp feeds it.

#include "fight_math.h"

#include "common/cbasetypes.h"

#include <fmt/format.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace cardian::tactics
{
    // One currency: MP. HP the party will not take, or will not have to
    // cure, becomes MP at the party's own cure efficiency -- HP landed per
    // MP spent, overcure included, once three casts exist; Cure II's floor
    // (60 HP for 24 MP) before that
    struct Exchange
    {
        double hpPerMp  = 2.5;
        bool   measured = false;

        auto mp(const double hp) const -> double
        {
            return hpPerMp > 0.0 ? hp / hpPerMp : 0.0;
        }
    };

    inline auto exchange(const int32 hpLanded, const int32 mpSpent, const uint32 casts) -> Exchange
    {
        if (casts >= 3 && mpSpent > 0 && hpLanded > 0)
        {
            return { static_cast<double>(hpLanded) / mpSpent, true };
        }
        return {};
    }

    // --- cures: the smallest tier that covers the gap -------------------

    struct CureOption
    {
        std::string spell;
        int32       mp     = 0;
        int32       heals  = 0;     // the raw-cure estimate
        bool        known  = false; // exact, or a floor
        int32       lands  = 0;     // against the missing HP
        int32       over   = 0;
        bool        covers = false;

        auto hpPerMp() const -> double
        {
            return mp > 0 ? static_cast<double>(lands) / mp : 0.0;
        }
    };

    constexpr std::size_t kNoPick = static_cast<std::size_t>(-1);

    // Each option against the missing HP; the pick is the cheapest tier
    // that covers it, else the one that heals most. Nothing missing, no pick
    inline auto pickCure(std::vector<CureOption>& options, const int32 missing) -> std::size_t
    {
        const int32 gap = std::max(0, missing);
        for (auto& o : options)
        {
            o.lands  = std::clamp(o.heals, 0, gap);
            o.over   = std::max(0, o.heals - gap);
            o.covers = gap > 0 && o.heals >= gap;
        }
        if (gap == 0)
        {
            return kNoPick;
        }
        std::size_t pick = kNoPick;
        for (std::size_t i = 0; i < options.size(); ++i)
        {
            if (options[i].covers && (pick == kNoPick || options[i].mp < options[pick].mp))
            {
                pick = i;
            }
        }
        if (pick == kNoPick)
        {
            for (std::size_t i = 0; i < options.size(); ++i)
            {
                if (pick == kNoPick || options[i].heals > options[pick].heals)
                {
                    pick = i;
                }
            }
        }
        return pick;
    }

    inline auto cureLine(const std::string_view caster, const std::string_view target, const int32 missing, const std::vector<CureOption>& options, const std::size_t pick) -> std::string
    {
        std::string line = fmt::format("bank: {} cures {}, {} missing:", caster, target, missing);
        if (options.empty())
        {
            return line + " no cure tier priced";
        }
        for (std::size_t i = 0; i < options.size(); ++i)
        {
            const auto& o = options[i];
            line += fmt::format("{} {} {}{}/{} MP ({:.1f}/MP{}{})", i == 0 ? "" : ",", o.spell, o.known ? "" : "~", o.heals, o.mp, o.hpPerMp(),
                                o.over > 0 ? fmt::format(", {} over", o.over) : "", i == pick ? ", the pick" : "");
        }
        return line;
    }

    // --- the mob's life -------------------------------------------------

    // Seconds the mob has left at the party's damage rate: the live rate
    // once the fight has ten seconds of it, the spot's before, unknown
    // (negative) with neither
    inline auto remainingLife(const int32 hp, const double liveDealtPerSecond, const double liveSeconds, const double spotDealtPerSecond) -> double
    {
        if (liveSeconds >= 10.0 && liveDealtPerSecond > 0.0)
        {
            return hp / liveDealtPerSecond;
        }
        if (spotDealtPerSecond > 0.0)
        {
            return hp / spotDealtPerSecond;
        }
        return -1.0;
    }

    // The seconds a debuff matters for: its duration, capped by the mob's
    // life when that is known
    inline auto window(const double duration, const double remaining) -> double
    {
        return remaining >= 0.0 ? std::min(duration, remaining) : duration;
    }

    // --- a debuff's price -----------------------------------------------

    struct DebuffPrice
    {
        std::string spell;
        std::string target;
        int32       mp          = 0;
        double      landChance  = 1.0;
        double      duration    = 0.0;  // expected seconds it holds, given it lands
        double      window      = 0.0;  // the seconds it matters: capped by the mob's life
        double      hpSaved     = 0.0;  // expected HP the party will not take
        double      mpWorth     = 0.0;
        std::string detail;             // the working, for the log
        bool        lifeGuessed = false; // the mob's life came from the spot, or is unknown
        bool        noData      = false; // no damage rate on record: the prior is "cast it"
        double      onFor       = -1.0;  // the effect is on the mob already, this long to go
        bool        blocked     = false; // an effect on the mob nullifies this one (a Bio under a Dia)
        double      moot        = -1.0;  // the mob dies before it lands: its seconds left
        bool        priced      = true;
        std::string family;              // when unpriced: what it is

        auto verdict() const -> std::string
        {
            if (!priced)
            {
                return "unpriced";
            }
            if (onFor >= 0.0)
            {
                return std::isinf(onFor) ? "on already, for good" : fmt::format("on already, {:.0f} s to go", onFor);
            }
            if (blocked)
            {
                return "blocked by what is on it";
            }
            if (moot >= 0.0)
            {
                return fmt::format("moot, the mob has {:.0f} s left", moot);
            }
            if (noData)
            {
                return "cast, no data yet";
            }
            return mpWorth > mp ? "cast" : "skip";
        }

        // "<lead>Paralyze on Orcish_Grunt: 78% to land, ~96 s, ~2.1 rounds
        // stopped over 38 s at 22 a round, saves ~46 HP = 18 MP, costs 6 -> cast"
        auto line(const std::string_view lead) const -> std::string
        {
            std::string line = fmt::format("{}{} on {}:", lead, spell, target);
            if (!priced)
            {
                return line + fmt::format(" unpriced ({})", family);
            }
            if (onFor >= 0.0 || moot >= 0.0 || blocked)
            {
                return line + " " + verdict();
            }
            line += fmt::format(" {:.0f}% to land", landChance * 100.0);
            if (duration > 0.0)
            {
                line += fmt::format(", ~{:.0f} s", duration);
            }
            if (!detail.empty())
            {
                line += ", " + detail;
            }
            line += fmt::format(", saves ~{:.0f} HP = {:.0f} MP, costs {} -> {}", hpSaved, mpWorth, mp, verdict());
            if (lifeGuessed)
            {
                line += " (the mob's life is a guess)";
            }
            return line;
        }
    };

    // A debuff that removes a fraction of the mob's melee rounds: Paralyze
    // its proc chance, Slow the delay it adds (s / (1 + s)), Blind the hit
    // rate lost. Rounds over the window at the mob's delay, each worth what
    // its melee lands a round on record
    inline void priceRounds(DebuffPrice& p, const double roundDelay, const double fraction, const double meleePerRound, const std::string_view what)
    {
        const double n = roundDelay > 0.0 ? p.window / roundDelay * fraction : 0.0;
        p.hpSaved      = p.landChance * n * meleePerRound;
        p.detail       = fmt::format("~{:.1f} {} over {:.0f} s at {:.0f} a round", n, what, p.window, meleePerRound);
        if (meleePerRound <= 0.0)
        {
            p.noData = true;
        }
    }

    // Damage over time: power HP a tick over the window
    inline auto dotDamage(const double power, const double tick, const double window) -> double
    {
        return tick > 0.0 ? power * std::floor(window / tick) : 0.0;
    }

    // Extra damage the party deals shortens the fight, and a shorter fight
    // is HP not taken: the seconds cut are the extra at the party's rate,
    // never more than the mob has left
    inline auto secondsCut(const double extraDealt, const double dealtPerSecond, const double remaining) -> double
    {
        if (dealtPerSecond <= 0.0)
        {
            return 0.0;
        }
        const double cut = extraDealt / dealtPerSecond;
        return remaining >= 0.0 ? std::min(cut, remaining) : cut;
    }

    inline void priceExtraDamage(DebuffPrice& p, const double extraDealt, const double dealtPerSecond, const double takenPerSecond, const double remaining, const std::string_view how)
    {
        const double cut = secondsCut(extraDealt, dealtPerSecond, remaining);
        p.hpSaved        = p.landChance * cut * takenPerSecond;
        p.detail         = fmt::format("{}: ~{:.0f} extra dealt, the fight ~{:.0f} s shorter", how, extraDealt, cut);
        if (dealtPerSecond <= 0.0 || takenPerSecond <= 0.0)
        {
            p.noData = true;
        }
    }

    // What the party's melee deals over the window with the mob's defence
    // lowered: the pDIF expectation's ratio, less one
    inline auto defenceDownExtra(const double ratio, const double dealtPerSecond, const double window) -> double
    {
        return std::max(0.0, ratio - 1.0) * dealtPerSecond * window;
    }

    // What was cast and its family, so the log shows what the party leans on
    inline auto unpricedLine(const std::string_view caster, const std::string_view spell, const std::string_view target, const std::string_view family) -> std::string
    {
        return fmt::format("bank: {} casts {} on {}: unpriced ({})", caster, spell, target, family);
    }

    // --- defence down, the exact number ---------------------------------

    // A hit at the mob's defence now, as it would have landed at its base
    // defence: the pDIF expectations' ratio
    inline auto withoutDefenceDown(const int32 landed, const double pdifNow, const double pdifBase) -> double
    {
        return pdifNow > 0.0 ? landed * (pdifBase / pdifNow) : landed;
    }

    // A defence-down effect's share of the mob's percentage, and the extra
    // apportioned to it
    struct DefenceShare
    {
        std::string effect;
        int32       defp = 0; // negative
        double      hp   = 0.0;
    };

    // The extra a hit dealt for the defence loss, apportioned by each
    // effect's share of it
    inline void apportion(const double extra, std::vector<DefenceShare>& shares)
    {
        int32 sum = 0;
        for (const auto& s : shares)
        {
            sum += s.defp;
        }
        if (sum == 0)
        {
            return;
        }
        for (auto& s : shares)
        {
            s.hp += extra * s.defp / sum;
        }
    }

    // A damage-over-time tick the mob lost, apportioned by each effect's
    // share of the regen it takes (the second number is the weight)
    inline void apportionTicks(const double tick, std::vector<std::pair<std::string, int32>>& shares, std::vector<std::pair<std::string, double>>& out)
    {
        int32 sum = 0;
        for (const auto& s : shares)
        {
            sum += s.second;
        }
        if (sum <= 0)
        {
            return;
        }
        for (const auto& s : shares)
        {
            out.emplace_back(s.first, tick * s.second / sum);
        }
    }
} // namespace cardian::tactics
