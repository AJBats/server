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
#include "common/mmo.h"

#include <optional>
#include <string>
#include <vector>

// Where a fishing venture may go (RESEARCH §11.13, "Her waters"): every
// fishing area (fishing_area) of each zone she can walk to from where she
// stands, crossing only zones whose usual monster is at her level or below
// -- the median top level of the monsters the map has loaded there,
// notorious and scripted ones left out; a zone with none, a town, always
// passable -- and the zone she stands in whatever its monsters. The zone
// lines are each zone's own (its settings file, read by upstream's loader).
// A stand-in until cardians travel (OPEN_ISSUES #431). The player picks the
// water: a zone with several lists each by its own name (the user,
// 2026-10-11).
namespace pawn::venturespots
{
    struct Spot
    {
        uint16      zone = 0;
        uint16      area = 0;
        uint8       hops = 0;   // zone lines from where she stands
        std::string name;       // the water's own name when its zone has several, else empty
        position_t  centre{};   // a point of the water: her body's place
    };

    // Her waters, nearest first
    auto spotsOf(uint32 charid) -> std::vector<Spot>;

    // One of her waters now: what a venture may be sent to
    auto reaches(uint32 charid, uint16 zone, uint16 area) -> bool;

    // A fishing area of the game, wherever she stands; none when it is not one
    auto waterOf(uint16 zone, uint16 area) -> std::optional<Spot>;
} // namespace pawn::venturespots
