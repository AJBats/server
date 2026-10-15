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

#include <optional>
#include <string>

class CCharEntity;
struct cl_club;

// The shell's bank (OPEN_ISSUES #471; ROADMAP N, "The shell's bank, shared
// and transactional"): one per linkshell, of CL_BANK_SIZE slots of items --
// any item, Ex and Rare included -- and a balance of gil, shared by everyone
// wearing a linkshell item of the shell, which only the addon's Linkshell
// page reaches, over the Link's BANK, BANK_HOLDER, BANK_DEPOSIT,
// BANK_WITHDRAW, BANK_GIL and BANK_RIGHTS. Nothing in the game itself reads
// or writes it.
//
// It opens while the player stands by an auction counter, by a Nomad Moogle
// or in his own Mog House. What he may do follows his rank in the shell: the
// shell's holder anything; a sack holder and a pearl holder what the holder
// has set. Items and gil move between the bank and a bag of a character he
// manages: himself, or an alt of his account or a cardian it owns where the
// place he stands at reaches her, by its own rule -- in his Mog House anyone
// of his in his city, by a Nomad Moogle or an auction counter one near it
// (the user, 2026-10-14: no bags reached across the world) -- the Mog
// House's bags only while he stands in his Mog House. So every
// character a move changes is one this map server holds in memory, changed
// through the game's own item transaction; none is loaded from her rows.
//
// Every move is one database transaction: it locks the shell's bank (its head
// row, FOR UPDATE, so moves on one shell from any number of map servers take
// their turns), reads his rank's right to the move under that lock, checks
// the bank still holds what the page saw, writes the bank's rows and the
// character's item rows on the one connection, and commits; anything refused
// or failed before the commit rolls both back. The item transaction is let go
// only once the database has committed.
//
// The bank's stacks are kept in `cardian_ls_bank` (a row a stack: the shell,
// its slot in the bank's order, the item, the quantity and the item's extra
// data), merged and sorted on every change by ls_bank_math.h; its gil, the
// holder's settings and a version that changes with every move of its stacks in
// `cardian_ls_bank_head`. An item comes out built as the game builds one it
// loads from a character's rows, so its charges, augments, a linkpearl's
// shell and a signature come back with it.
namespace pawn::lsbank
{
    // Its tables, made where they are missing: the pawn module's init
    void ensureTable();

    // Its messages on the Cardian Link: the pawn module's init
    void registerHandlers();

    // Whether the bank opens where the player stands now: within reach of an
    // auction counter, of a Nomad Moogle, or in his own Mog House. The
    // Linkshell page's Bank row lights by it (CLUB's bank)
    auto opens(const CCharEntity* PPlayer) -> bool;

    // A stack of the bank, whole or in part, taken out into the player's own
    // inventory for the Sell page to list (pawn/ah_sell.h): as BANK_WITHDRAW
    // takes one for him -- the bank open to him, his rank's right to take,
    // the stack the page saw in that bank slot (its item, the bank's
    // version), a slot free -- so whatever holds of a move holds of this one.
    // CL_S_OK once it is in his inventory, `landed` its slot there
    auto takeForSale(CCharEntity* PChar, uint8 bankSlot, uint16 itemId, uint16 version, uint32 qty, uint8& landed) -> uint16;

    // The shell a character wears -- her link slot 1, else 2 -- and what she
    // wears of it (CL_RANK_*); none for no linkshell item, a broken pearl or
    // a new linkshell not yet made
    struct Shell
    {
        uint32      lsid  = 0;
        uint8       rank  = 0;
        std::string name;
        uint16      color = 0; // the game's own: four bits each of red, green, blue and alpha, red lowest
    };
    auto shellOf(CCharEntity* PChar) -> std::optional<Shell>;

    // CLUB's shell and bank: the shell he wears, his rank in it, and its
    // bank's stacks and gil
    void fillClub(cl_club& club, CCharEntity* PPlayer);
} // namespace pawn::lsbank
