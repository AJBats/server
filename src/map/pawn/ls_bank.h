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

// The linkshell bank (OPEN_ISSUES #471): a storage of Cardian's own, one per
// account, of CL_BANK_SIZE slots of items -- any item, Ex and Rare included,
// never gil -- which only the addon's Linkshell page reaches, over the Link's
// BANK, BANK_HOLDER, BANK_DEPOSIT and BANK_WITHDRAW. Nothing in the game
// itself reads or writes it.
//
// It opens while the player stands by an auction counter, by a Nomad Moogle
// or in his own Mog House, and every request is refused CL_S_BANK_CLOSED
// anywhere else. Items move between it and the inventory of a character he
// manages: himself, an alt of his account or a cardian it owns (the club's
// members of those kinds, club.h), wherever she is. A character in memory --
// he himself, a member standing as a cardian -- is changed there through the
// game's own item transaction, which writes her rows and tells a client; one
// out of the world is loaded for the move a moment and let go after it, as a
// venture's sale loads her (venture_sales.h). A member away on a venture, or
// held by one in no zone, is not reached until she is back.
//
// The bank's stacks are kept in its table, `cardian_bank` (one row a stack:
// the account, its slot in the bank's order, the item, the quantity and the
// item's extra data), merged and sorted on every change by the rules of
// ls_bank_math.h. An item comes out of the bank built as the game builds one
// it loads from a character's rows, so its charges, augments, a linkpearl's
// shell and a signature come back with it.
namespace pawn::lsbank
{
    // Its table, made where it is missing: the pawn module's init
    void ensureTable();

    // Its messages on the Cardian Link: the pawn module's init
    void registerHandlers();

    // Whether the bank opens where the player stands now: within reach of an
    // auction counter, of a Nomad Moogle, or in his own Mog House. The
    // Linkshell page's Bank row lights by it (CLUB's bank)
    auto opens(const CCharEntity* PPlayer) -> bool;
} // namespace pawn::lsbank
