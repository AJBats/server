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

#include <string>

class CCharEntity;

// Cardian possession: the player takes control of one of their cardians
// (or an offline character on the account), and the character they were
// playing becomes a cardian standing where it stood. Composes the pawn
// module and charswap: the client is sent a rezone so it re-learns who it
// is, but the server zones nobody -- both characters stay exactly where
// they stand, in memory and in the database, and the two live entities
// trade owners (session <-> pawn module) in place. Party membership, buffs,
// TP, timers and fights carry over on both sides.
// Gated behind cardian.ENABLE_CHARSWAP and pawn.ENABLE_PAWNS.
namespace possess
{
    bool isEnabled();

    // Stage possession of the named character by PChar's client and force
    // the rezone. A live cardian must be in PChar's zone and keeps its
    // position; an offline character is summoned to PChar's spot. Returns
    // false with no side effects when the target is unknown, itself, not
    // owned by the account, played by someone else, or out of zone.
    bool start(CCharEntity* PChar, const std::string& targetName);
} // namespace possess
