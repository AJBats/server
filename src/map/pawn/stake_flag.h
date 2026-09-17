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

// ---------------------------------------------------------------------------
// THE STAKE'S FLAG -- A TRIAL, and deliberately nothing to do with the stake
// itself.
//
// The stake (RESEARCH §12.16, stake_math.h and the orders in pawn.cpp) is a
// place the party keeps to; the player has only the log and the card to tell
// him where he put it. This file is the one thing that makes it *visible*: it
// stands his national conquest banner on the spot, facing the stake's heading.
//
// It answers a single question -- can the server draw a graphic in the world
// at a place of our choosing? -- by using the game's own props rather than
// anything client-side. See also the addon research: Ashita can project and
// draw, but a real entity occludes, lights and fogs for free.
//
// It lives in its own file, and pawn.cpp calls it in exactly TWO places
// (setStake and clearStake, each one line under a CARDIAN TRIAL comment), so
// the trial can be judged -- or lifted out whole -- without disturbing a line
// of the stake's own logic.
// ---------------------------------------------------------------------------

#include "common/cbasetypes.h"
#include "common/types/position.h"

class CCharEntity;

namespace cardian::stakeflag
{
    // Stand the owner's flag on the stake, facing the stake's heading. Safe
    // when one already stands (the stake was moved): the old one comes down
    // first, so a player never leaves a trail of banners behind him.
    void plant(CCharEntity* POwner, const position_t& at);

    // Take the owner's flag down. Silent when he has none, so every path that
    // dissolves a stake can call it without asking first.
    void dissolve(uint32 ownerCharID);

} // namespace cardian::stakeflag
