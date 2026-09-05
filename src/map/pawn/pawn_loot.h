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

namespace pawn::loot
{
    // A treasure pool holding items with no real member left in it -- the
    // player zoned, logged out or left the party while only cardians
    // stayed -- is handed out on the spot to those cardians, by the pool's
    // own rules for an unlotted item (a random member with room, never a
    // second of a rare), one slot at a time so a bag that just filled is
    // seen by the next. Lost only when no one staying can hold it. Checked
    // from the pawn zone tick, since on logout the server drops the player
    // from the party, and so from the pool, before any zone-out hook runs.
    // The pool's flush() cannot do this: it resolves only items past their
    // five minutes.
    void handOff(CCharEntity* PPawn);
} // namespace pawn::loot
