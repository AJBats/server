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
#include "common/types/position.h"
#include "data/enums/zone.h"

#include <optional>

// World travel for pawns: a walkable zone graph built from the per-zone
// zoneline datasets (each line carries the trigger-box centre in its own
// zone and the arrival box in the destination), routed with BFS. Transport
// links (boats, airships) are not zonelines and are naturally absent;
// mog-house doors and intra-zone teleport pads are excluded as self-edges.
namespace pawn
{
    struct TravelHop
    {
        xi::ZoneId destinationZone{};
        position_t walkTo{};   // zone-line trigger centre in the current zone
        position_t arriveAt{}; // arrival box centre in the destination zone
    };

    namespace travel
    {
        // First hop on a walkable route from one zone toward another;
        // nullopt when no walkable route exists.
        auto nextHop(xi::ZoneId from, xi::ZoneId to) -> std::optional<TravelHop>;
    } // namespace travel
} // namespace pawn
