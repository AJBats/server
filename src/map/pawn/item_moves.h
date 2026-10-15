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

#include "ls_bank_math.h"

#include "common/cbasetypes.h"

#include <memory>
#include <optional>
#include <string_view>
#include <vector>

class CCharEntity;
class CItem;
class Transaction;

// A move that changes characters' items in memory and their rows in the
// database together, kept whole when the database fails partway (#478): the
// shell's bank's moves (ls_bank_moves.h) and the Sell page's listings
// (ah_sell.h).
//
// A move is one database transaction (Txn), its characters changed through
// the game's own item transaction on the same connection. It can fail before
// its COMMIT -- a statement refused, or the connection lost, which fails every
// statement after it while MariaDB rolls the move back -- or at the COMMIT, on
// the wire: rolled back and told so, or committed with its answer lost. Each
// move writes a mark of its own inside its transaction, so a COMMIT told
// failed is settled by asking whether the mark is there (commitMove): there,
// the move happened and its item transaction lets go; not there, it did not,
// and its characters are put back as their rows are (abandon). Proven by
// xi_test's scripts/tests/cardian/ls_bank_commit.lua and ah_sell.lua.
//
// xi_test links this file without the pawn module, so it calls nothing in the
// pawn module's own sources: only the game's code and header-only Cardian files.
namespace pawn::itemmoves
{
    // xi_test's, and nothing else's: the next move failed as the wire can fail
    // one, for the cases that prove a failed move leaves the database, the rows
    // and the characters in memory agreeing. A lost connection is a real one:
    // MariaDB drops it and rolls back what it held
    enum class Fault : uint8
    {
        None,
        RolledBack,        // the COMMIT rolled back, and told so
        Committed,         // the COMMIT went through, and its answer was lost
        LostBeforeCommit,  // the connection lost before the COMMIT
        LostAtCommit,      // the connection lost as the COMMIT was sent
        CommittedThenLost, // the COMMIT went through, then the connection was lost before its answer
        LostTwice,         // the connection lost before the COMMIT, and again while she is put back
        BankReadFails,     // the bank's read of its stacks failed (ls_bank_moves.h)
        LostInPutBack,     // LostTwice's second loss, still to come
    };
    void failNext(Fault fault);

    // The fault still armed: None once the move it was armed for has met it
    auto pendingFault() -> Fault;

    // Whether xi_test armed this fault, which the move then meets
    auto takeFault(Fault fault) -> bool;

    // The marks' table, made where it is missing, and marks left by a map
    // stopped mid-move cleared: the pawn module's init, and the tests'
    void ensureTable();

    // One move's database transaction, opened and closed by hand, so a refusal
    // rolls it back quietly (a throw into db::transaction is logged as a
    // critical failure). While it is open nothing may open another: a START
    // TRANSACTION commits the one open. Rolled back unless committed
    class Txn
    {
    public:
        Txn();
        ~Txn();

        Txn(const Txn&)            = delete;
        Txn& operator=(const Txn&) = delete;

        auto ok() const -> bool;

        // The COMMIT; false when it failed -- or when xi_test makes it fail as
        // the wire can
        auto commit() -> bool;
        void rollback();

    private:
        bool open_ = false;
    };

    // A slot a move changed: whose, the bag and the slot
    struct Slot
    {
        CCharEntity* who  = nullptr;
        uint8        loc  = 0;
        uint8        slot = 0;
    };

    // A new mark for a move, and the mark written inside its transaction, the
    // move's last write before its COMMIT
    auto newMark() -> uint64;
    auto writeMark(uint64 mark) -> bool;

    // A move that did not happen, once its item transaction has changed its
    // characters: the database rolled back first, then the characters put back
    // through the item transaction's undo in a scratch transaction, then
    // checked against their rows. `whole`: the slot the move took a stack of
    // whole, if it took one (the rollback put its row back already, and the
    // undo's own insert of it would fail); `touched`: the slots it changed
    void abandon(Txn& txn, Transaction& items, const std::optional<Slot>& whole, const std::vector<Slot>& touched);

    // A move's COMMIT, a failed one settled by its mark: true when the move
    // happened (its item transaction let go), false when it did not
    // (abandoned). `what` names the move in the log
    auto commitMove(Txn& txn, Transaction& items, uint64 mark, const std::optional<Slot>& whole, const std::vector<Slot>& touched, std::string_view what) -> bool;

    // An item built as charutils::LoadInventory builds one from its row: its
    // quantity and extra data, a linkshell item's kind when its data has none,
    // an enchanted item's last use and a piece of gear's augments. A copy of
    // upstream's steps: compare them at every merge from base
    auto rebuild(uint16 itemId, uint32 qty, const cardian::lsbank::Extra& extra) -> std::unique_ptr<CItem>;
} // namespace pawn::itemmoves
