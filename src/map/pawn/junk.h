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

#include <cstddef>

// The junk rule (junk_math.h) over this map's own database -- its items and
// recipes, so each profile gets its own answer -- and the hand-kept
// exceptions beside it, modules/cardian/junk.yaml. Read once at boot
namespace pawn::junk
{
    void load();

    // The rule names this item
    auto isJunk(uint16 itemId) -> bool;

    // How many items it names
    auto count() -> std::size_t;
} // namespace pawn::junk
