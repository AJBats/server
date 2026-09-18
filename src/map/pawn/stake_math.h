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
#include <numbers>
#include <utility>

namespace cardian::stake
{
    // The tow point: one mob's reach past the stake, on the line from the
    // mob through the stake. A mob chasing her there stops when it is a
    // reach from her -- on the stake. A mob already on the stake has no
    // line, so the point uses the engine's heading: x = cos, z = -sin.
    inline auto towPoint(const float mobX, const float mobZ, const float stakeX, const float stakeZ, const float reach, const uint8 rotation) -> std::pair<float, float>
    {
        const float dx  = stakeX - mobX;
        const float dz  = stakeZ - mobZ;
        const float len = std::sqrt(dx * dx + dz * dz);
        if (len < 0.01f)
        {
            const float radians = rotation * (2.0f * std::numbers::pi_v<float> / 256.0f);
            return { stakeX + std::cos(radians) * reach, stakeZ - std::sin(radians) * reach };
        }
        return { stakeX + dx / len * reach, stakeZ + dz / len * reach };
    }

    // A new monster has not yet settled. Only a settled monster gets the
    // wider drift band before it needs towing again.
    inline auto towing(const bool newMob, const bool wasTowing, const float distance, const float tolerance) -> bool
    {
        return distance > ((newMob || wasTowing) ? tolerance : 2.0f * tolerance);
    }

    // The stake dissolves once the whole party has left its zone: nobody
    // of the party in it, and somebody of it seen in another zone. A
    // player loading between zones is seen nowhere, so a zone line alone
    // never dissolves it
    inline auto dissolves(const uint32 inZone, const uint32 elsewhere) -> bool
    {
        return inZone == 0 && elsewhere > 0;
    }

    // Other owned bodies may have waited elsewhere all session. Only the
    // owner's observed arrival elsewhere establishes departure; anybody
    // still at camp keeps it standing. Loading bodies are not observed.
    class Census
    {
    public:
        explicit Census(const uint16 zone)
        : zone_(zone)
        {
        }

        void observe(const uint16 zone, const bool owner)
        {
            inZone_         += zone == zone_;
            ownerElsewhere_ = ownerElsewhere_ || (owner && zone != zone_);
        }

        auto dissolves() const -> bool
        {
            return cardian::stake::dissolves(inZone_, ownerElsewhere_ ? 1 : 0);
        }

    private:
        uint16 zone_;
        uint32 inZone_         = 0;
        bool   ownerElsewhere_ = false;
    };

} // namespace cardian::stake
