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

// Re-teaching the retail client its party structure after a possession
// zone-in, inside its GAMEOK response. The client caches which party row is
// itself and re-derives it only on a structural change (join, leave,
// disband); the "you are solo" triple LSB sends on leaving a party is that
// structural change, and the party's real table follows it.
namespace partyteach
{
    // A character whose client just changed identity (possession).
    void requestReteach(uint32 charID);
} // namespace partyteach
