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

class CCharEntity;

// Doors on a cardian's way. The collision data the navmesh is built from
// carries no door slabs, so a path runs straight through a closed door;
// the client stops a player there and opens the door for them as they
// walk up. This does the same for a cardian from the server side.
namespace pawn::doors
{
    // Open every closed generic door within `lane` yalms of the way ahead
    // of her -- the segment from where she stands to `reach` yalms along
    // her facing -- exactly as the game opens one for a player: the open
    // animation to everyone in range, closing again seven seconds later.
    // A door with a script of its own (a key, a quest, a cutscene) is left
    // to the player; a ferry gate or an elevator door keeps to its
    // timetable. Returns how many doors opened this call.
    auto openAhead(CCharEntity* PPawn, float reach, float lane) -> int;
} // namespace pawn::doors
