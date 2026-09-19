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

#include "common/utils.h"

#include <algorithm>
#include <optional>

namespace cardian::casting
{
    // Leave enough of the half-yalm inset to enter casting range.
    inline constexpr float kArrival = 0.2f;
    // Spell range and line of sight determine arrival.
    inline constexpr float kTolerance = 0.0f;

    // Request a casting position, or no movement when range and sight allow
    // casting. Re-evaluate each movement tick to follow the target.
    inline auto approach(const position_t& from, const position_t& target, const float reach, const bool sight) -> std::optional<position_t>
    {
        if (!sight)
        {
            return target;
        }
        const float gap = distance(from, target);
        if (gap <= reach)
        {
            return std::nullopt;
        }
        const float fraction = (gap - std::max(0.0f, reach - 0.5f)) / gap;
        return position_t(from.x + (target.x - from.x) * fraction,
                          from.y + (target.y - from.y) * fraction,
                          from.z + (target.z - from.z) * fraction, 0, 0);
    }
}
