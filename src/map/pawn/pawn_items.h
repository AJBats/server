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
#include <vector>

class CCharEntity;

// Item management for cardians: move stacks between the player's inventory
// and a live pawn's, and equip/unequip gear on the session-less character.
// Transfers ride LSB's transaction layer (claim + give/take + rollback),
// which writes char_inventory through to the database -- a pawn's despawn
// discards its position and stats, never its items. Equipping runs the same
// charutils path the client's own equip request runs, so job, level and
// slot rules hold for pawns exactly as they do for players.
//
// Every function returns an empty string on success or a short lowercase
// reason ("no space", "not equippable", ...) the command layer forwards to
// the addon verbatim.
namespace pawn::items
{
    // Move qty from the player's LOC_INVENTORY slot into the pawn's
    // inventory, or back. The item must be idle (not equipped, not in a
    // bazaar, not mid-transaction); rare/stack rules on the receiving side
    // are the transaction layer's.
    auto giveToPawn(CCharEntity* PPlayer, CCharEntity* PPawn, uint8 slot, uint32 qty) -> std::string;
    auto takeFromPawn(CCharEntity* PPlayer, CCharEntity* PPawn, uint8 slot, uint32 qty) -> std::string;

    // Equip the item in the pawn's LOC_INVENTORY invSlot into equipSlot
    // (SLOTTYPE), or clear equipSlot. Both re-run gear sets, health and
    // latents the way the 0x050 handler does for a real client.
    auto equip(CCharEntity* PPawn, uint8 invSlot, uint8 equipSlot) -> std::string;
    auto unequip(CCharEntity* PPawn, uint8 equipSlot) -> std::string;

    // Protocol chunks for the companion addon, each short enough for one
    // chat-packet reply (~140 bytes).
    //   inventory: "i <size>|<slot>:<itemId>:<qty>,..."   (used slots only)
    //   equipment: "e <equipSlot>:<itemId>:<invSlot>,..." (filled slots only)
    auto inventoryChunks(CCharEntity* PPawn) -> std::vector<std::string>;
    auto equipChunks(CCharEntity* PPawn) -> std::vector<std::string>;
} // namespace pawn::items
