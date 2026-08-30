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
class CZone;

// Cardian pawns: session-less CCharEntity instances loaded from real DB
// character rows and inserted into a zone with no client attached. The pawn
// module owns each entity (mirroring how MapSession owns a player's char).
// Visibility, ticking, stats and gear all ride the normal character code;
// the module drains the outbound PacketList nobody will ever read and keeps
// pawns away from the session-only zone-change paths.
// Gated behind pawn.ENABLE_PAWNS.
namespace pawn
{
    bool isEnabled();

    // Load the named offline character (same account as the summoner) and
    // insert it into the summoner's zone at the summoner's position.
    // Returns false with no side effects if the target is unknown, online,
    // already a pawn, itself, or on another account.
    bool spawn(CCharEntity* PSummoner, const std::string& targetName);

    // Remove a pawn from its zone and destroy it. No character state is
    // written back to the DB (the pawn visit leaves no trace).
    bool despawn(const std::string& targetName);

    // Per-tick maintenance for pawns in this zone (called from OnZoneTick,
    // after all charTicks): discard queued outbound packets.
    void onZoneTick(CZone* PZone);
} // namespace pawn
