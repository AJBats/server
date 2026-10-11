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
#include <cstdint>

// A venture's fight with a hooked fish, timed and not played (pawn/angler.h).
// The minigame is the client's, and a person wins it almost every time
// whatever the skill (the user, 2026-10-10), so she wins every fight but one
// she has not the skill for. The fight's length is the fish's stamina worn
// down by the rod's damage a right arrow, an arrow every arrowSeconds. The
// server's own recovery rate for the fish (fishingutils::CalculateRegen: 128
// is neutral, one more for a large catch but on Ebisu) rises once it is 24 levels or more
// over her skill, and each point past neutral slows her by `slowdown` of her
// pace; a fight that cannot end inside the fish's time limit gets away at
// that limit.
namespace cardian::fishfight
{
    // What the hook check says of the fish (fishresponse_t)
    struct Fish
    {
        uint32_t stamina   = 0;     // its stamina
        uint32_t attack    = 0;     // what a right arrow takes off it (attackdmg)
        uint32_t regen     = 128;   // its recovery rate
        uint32_t timeLimit = 0;     // seconds to land it
        bool     large     = false; // a large catch, its neutral one point higher (not on Ebisu, whose regen adds none)
    };

    struct Tuning
    {
        float arrowSeconds = 2.0f;  // a right arrow every this many seconds
        float slowdown     = 0.15f; // her pace lost for each point of recovery past neutral
    };

    struct Outcome
    {
        bool     won     = true;
        uint32_t seconds = 0; // how long the fight lasts
    };

    inline auto fight(const Fish& fish, const Tuning& tuning) -> Outcome
    {
        const uint32_t limit   = std::max<uint32_t>(fish.timeLimit, 1);
        const uint32_t neutral = 128 + (fish.large ? 1 : 0);
        const float    past    = fish.regen > neutral ? static_cast<float>(fish.regen - neutral) : 0.0f;
        const float    pace    = 1.0f - tuning.slowdown * past;
        if (fish.attack == 0 || pace <= 0.0f || tuning.arrowSeconds <= 0.0f)
        {
            return { false, limit };
        }
        const uint32_t arrows  = std::max<uint32_t>((fish.stamina + fish.attack - 1) / fish.attack, 1);
        const float    seconds = static_cast<float>(arrows) * tuning.arrowSeconds / pace;
        if (seconds > static_cast<float>(limit))
        {
            return { false, limit };
        }
        return { true, static_cast<uint32_t>(std::ceil(seconds)) };
    }
} // namespace cardian::fishfight
