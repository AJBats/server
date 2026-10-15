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

#include "cardian_link_protocol.h"
#include "ls_bank.h"
#include "ls_bank_math.h"

#include "common/cbasetypes.h"

#include <functional>
#include <memory>
#include <optional>
#include <vector>

class CCharEntity;
class CItem;

// The shell's bank's rows and its three moves (ls_bank.h), apart from the
// Link that asks for them, so the server's own tests reach them with a
// player of theirs and a real database (xi_test:
// scripts/tests/cardian/ls_bank_commit.lua, through cardian_pawn_stubs.cpp's
// xi.cardian.lsbank). Each move is one database transaction; its character is
// one this map server holds in memory, changed through the game's own item
// transaction. xi_test links this file without the pawn module, so it calls
// nothing in the pawn module's own sources (APP_SOURCES), or xi_test stops
// linking: only the game's code and header-only Cardian files.
namespace pawn::lsbank::moves
{
    // The shell's head row: its gil, a version every move of its stacks
    // changes, and what the holder lets a sack holder and a pearl holder do
    struct Head
    {
        uint32 gil      = 0;
        uint32 version  = 0;
        uint8  sackCan  = cardian::lsbank::kSackDefault;
        uint8  pearlCan = cardian::lsbank::kPearlDefault;
    };

    // Its tables, made where they are missing
    void ensureTables();

    // The head row, made where it is missing; false when it cannot be
    auto ensureHead(uint32 lsid) -> bool;

    // The head as it stands, unlocked: for a listing
    auto readHead(uint32 lsid) -> Head;

    // The shell's stacks as its rows hold them, in its order; none when the
    // read failed, which is never an empty bank
    auto readStacks(uint32 lsid) -> std::optional<std::vector<cardian::lsbank::Stack>>;

    // An item built as the game builds one it loads from a character's rows
    auto rebuild(uint16 itemId, uint32 qty, const cardian::lsbank::Extra& extra) -> std::unique_ptr<CItem>;

    // The moves, each CL_S_OK or why it was refused: a stack of her bag into
    // the bank (landed: the bank's slot it ended in), a stack of the bank into
    // her bag (landed: her slot), and gil either way (bankGil: the bank's
    // after it)
    auto deposit(CCharEntity* PHolder, const Shell& shell, const cl_bank_deposit& ask, uint8& landed) -> uint16;
    auto withdraw(CCharEntity* PHolder, const Shell& shell, const cl_bank_withdraw& ask, uint8& landed) -> uint16;
    auto gil(CCharEntity* PHolder, const Shell& shell, const cl_bank_gil& ask, uint32& bankGil) -> uint16;

    // Each stack in one of her bags that pick names, into the bank as
    // deposit moves one, in her bag's order (BANK_JUNK: the map's pick is the
    // junk rule). A stack worn or busy stays where it is, and a bank too full
    // for a stack goes on to the next, which may still merge. answer holds
    // what moved and whether the bank filled; look moves nothing and answers
    // what would move now, judged in deposit's own order. CL_S_OK when it all
    // went (or would); a refusal of the bank's or the database's that ended it
    // partway, what moved before it in answer; else why none moved --
    // CL_S_BANK_FULL, the first skipped stack's reason (in a trade or a
    // bazaar), or CL_S_NO_ITEM when none is named there
    auto depositEach(CCharEntity* PHolder, const Shell& shell, uint8 loc, const std::function<bool(uint16)>& pick, bool look, cl_bank_junk& answer) -> uint16;

    // xi_test's, and nothing else's: the next move failed as the wire can fail
    // one, for the cases that prove a failed move leaves the bank, her rows
    // and her character in memory agreeing (#478). A lost connection is a
    // real one: MariaDB drops it and rolls back what it held
    enum class MoveFault : uint8
    {
        None,
        RolledBack,        // the COMMIT rolled back, and told so
        Committed,         // the COMMIT went through, and its answer was lost
        LostBeforeCommit,  // the connection lost before the COMMIT
        LostAtCommit,      // the connection lost as the COMMIT was sent
        CommittedThenLost, // the COMMIT went through, then the connection was lost before its answer
        LostTwice,         // the connection lost before the COMMIT, and again while she is put back
        BankReadFails,     // the move's read of the bank's stacks failed
        LostInPutBack,     // LostTwice's second loss, still to come
    };
    void failNext(MoveFault fault);

    // The fault still armed: None once the move it was armed for has met it
    auto pendingFault() -> MoveFault;
} // namespace pawn::lsbank::moves
