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

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

// Regen as a healer's top-up (the user, 2026-10-27; role_support.cpp). The
// game heals a Regen's power every effect tick of the zone while it lasts
// (CStatusEffectContainer::TickRegen, every 3 s); what one would heal is its
// own script run as the healer's cast (bank::expectedRegen), her gear and
// merits counted.
//  - A member wearing one is topped up only past what its ticks left will
//    heal; first aid is apart and never waits for it.
//  - A healer puts hers where its whole will be spent before the fight
//    ends: what the member is missing and what she is expected to take in
//    the time the fight has left cover it. A fight of no known length
//    counts no time, so she waits for the HP to drop to it.
//  - Its recast keeps it on a few members: of those who qualify, the one
//    taking the most a second, the tank as a rule, then the most missing.
//    She cures nobody else with it meanwhile.
namespace cardian::regen
{
    // The zone's effect tick, seconds (zone_entities.cpp m_EffectCheckTime,
    // as skillchain_math.h kRegenTick)
    inline constexpr double kTick = 3.0;

    // A Regen as seen on a member: the HP each tick heals, and its whole run
    struct Seen
    {
        double power   = 0.0;
        double seconds = 0.0;
    };

    // What a Regen still heals: its power for each tick left
    inline auto healsLeft(const double power, const double secondsLeft) -> double
    {
        return secondsLeft <= 0.0 || power <= 0.0 ? 0.0 : power * std::floor(secondsLeft / kTick);
    }

    // What a whole one heals
    inline auto healsWhole(const Seen& seen) -> double
    {
        return healsLeft(seen.power, seen.seconds);
    }

    // The gap a routine cure answers on a member wearing a Regen: what she
    // is missing past what its ticks left will heal
    inline auto gapAfter(const int32_t missing, const double left) -> int32_t
    {
        return std::max<int32_t>(0, missing - static_cast<int32_t>(std::floor(left)));
    }

    // Whether a healer's Regen is worth putting on a member: its whole has
    // somewhere to go -- what she is missing and what she is expected to
    // take in the seconds the fight has left (none when unknown) cover it
    inline auto worthCasting(const Seen& seen, const double missing, const double takenPerSecond, const double fightLeft) -> bool
    {
        const double whole   = healsWhole(seen);
        const double horizon = std::clamp(fightLeft, 0.0, seen.seconds);
        return whole > 0.0 && missing + takenPerSecond * horizon >= whole;
    }

    // One a healer could put her Regen on
    struct Candidate
    {
        uint32_t id             = 0;
        double   takenPerSecond = 0.0;
        double   missing        = 0.0;
    };

    // The one she puts it on: who takes the most a second, then the most
    // missing; none from none
    inline auto pick(const std::span<const Candidate> candidates) -> std::optional<std::size_t>
    {
        std::optional<std::size_t> best;
        for (std::size_t i = 0; i < candidates.size(); ++i)
        {
            const auto& c = candidates[i];
            if (!best.has_value() || c.takenPerSecond > candidates[*best].takenPerSecond ||
                (c.takenPerSecond == candidates[*best].takenPerSecond && c.missing > candidates[*best].missing))
            {
                best = i;
            }
        }
        return best;
    }
} // namespace cardian::regen
