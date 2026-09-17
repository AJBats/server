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

// The stake's arithmetic (RESEARCH §12.16): the tank's tow point, and the
// rule that dissolves a stake the party has left. Pure and entity-free,
// so cardian_stake_tests.cpp can hold it to account; the controller and
// the orders read the entities and call in.

#include "common/cbasetypes.h"

#include <cmath>
#include <utility>

namespace cardian::stake
{
    // The tow point: one mob's reach past the stake, on the line from the
    // mob through the stake. A mob chasing her there stops when it is a
    // reach from her -- on the stake. A mob already on the stake has no
    // line, so the point sits `reach` along the fallback bearing (radians,
    // x = cos, z = sin)
    inline auto towPoint(const float mobX, const float mobZ, const float stakeX, const float stakeZ, const float reach, const float fallbackRadians) -> std::pair<float, float>
    {
        const float dx  = stakeX - mobX;
        const float dz  = stakeZ - mobZ;
        const float len = std::sqrt(dx * dx + dz * dz);
        if (len < 0.01f)
        {
            return { stakeX + std::cos(fallbackRadians) * reach, stakeZ + std::sin(fallbackRadians) * reach };
        }
        return { stakeX + dx / len * reach, stakeZ + dz / len * reach };
    }

    // The stake dissolves once the whole party has left its zone: nobody
    // of the party in it, and somebody of it seen in another zone. A
    // player loading between zones is seen nowhere, so a zone line alone
    // never dissolves it
    inline auto dissolves(const uint32 inZone, const uint32 elsewhere) -> bool
    {
        return inZone == 0 && elsewhere > 0;
    }

} // namespace cardian::stake
