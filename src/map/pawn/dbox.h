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

#include <array>
#include <string>

class CCharEntity;
class CItem;
class Scheduler;

// A cardian's delivery box, by the game's own (utils/dboxutils.h), as her
// Mog House page reaches it (the Link's DBOX). Each request opens her box as
// a client opens it, does its one thing through the game's own routine for
// it, and closes the box again, so nothing of hers is left open between the
// player's requests (an item use or a synthesis of hers holds the container
// the box opens, and the box waits for it).
// The rules are the game's: what may be sent where, a box with too much
// waiting, a Rare item she holds already, an item the auction house sent
// that has nobody to go back to. The refusals the game leaves unsaid to a
// client are told first, in the Link's codes (cardian_link_protocol.h).
// xi_test links this file without the pawn module
// (scripts/tests/cardian/dbox.lua), so it calls only the game's own code.
namespace pawn::dbox
{
    // One of a box's eight slots, as its row holds it
    struct Slot
    {
        uint16      item  = 0; // 0: empty; 65535: gil
        uint8       state = 0; // CL_DBOX_EMPTY to CL_DBOX_DELIVERED
        uint32      qty   = 0; // a stack's count, or the gil
        std::string name;      // who sent it (box 1), or whom it is for (box 2)
    };

    // One request on her box, 1 (Receive) or 2 (Send): the action
    // (CL_DBOX_*) on the box's slot; a set takes qty of the stack in her
    // inventory's invSlot, for the character `to`. The scheduler is the one
    // the game's routine for pulling in what waits wants (his session's,
    // pawn::players::schedulerOf). A CL_S_* code
    auto act(CCharEntity* PChar, uint8 box, uint8 action, uint8 slot, uint8 invSlot, uint32 qty, uint32 to, Scheduler& scheduler) -> uint16;

    // Why qty of PItem, a stack of hers in any of her bags, could not be set
    // in her outgoing box's slot for the character `to`: CL_S_OK, or the
    // refusal the set itself would give. Asked before a stack is moved out of
    // another bag for a set, so a set refused moves nothing
    auto setRefusal(CCharEntity* PChar, uint8 slot, const CItem* PItem, uint32 qty, uint32 to) -> uint16;

    // Her box's eight slots as their rows hold them
    auto slotsOf(uint32 charid, uint8 box) -> std::array<Slot, 8>;

    // What waits for her beyond her incoming box's eight slots
    auto waitingFor(uint32 charid) -> uint8;
} // namespace pawn::dbox
