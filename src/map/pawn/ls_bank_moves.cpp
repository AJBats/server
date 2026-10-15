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

#include "ls_bank_moves.h"

#include "common/database.h"
#include "common/earth_time.h"
#include "common/logging.h"
#include "common/timer.h"
#include "entities/char_entity.h"
#include "item_container.h"
#include "items/item.h"
#include "items/item_equipment.h"
#include "items/item_linkshell.h"
#include "items/item_usable.h"
#include "items/transactions/item_claim.h"
#include "lua/luautils.h"
#include "packets/s2c/0x01d_item_same.h"
#include "packets/s2c/0x01e_item_num.h"
#include "packets/s2c/0x020_item_attr.h"
#include "utils/charutils.h"
#include "utils/itemutils.h"

#include <algorithm>
#include <cstring>
#include <limits>
#include <optional>
#include <random>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace pawn::lsbank::moves
{
    namespace
    {
        using cardian::lsbank::Extra;
        using cardian::lsbank::Stack;

        constexpr uint32 kPurseMax = 999'999'999; // the most gil a purse holds, and so the most one move carries

        MoveFault nextFault = MoveFault::None; // xi_test's (failNext)

        // Whether xi_test armed this fault, which it then meets
        auto takeFault(const MoveFault fault) -> bool
        {
            if (nextFault != fault)
            {
                return false;
            }
            nextFault = MoveFault::None;
            return true;
        }

        // xi_test's lost connection: MariaDB drops it as it drops any client
        // gone quiet, and rolls back the transaction it held
        void loseConnection()
        {
            db::preparedStmt("SET SESSION wait_timeout = 1");
            std::this_thread::sleep_for(std::chrono::milliseconds(2500));
        }

        // ---- one move's database transaction ----------------------------------

        // Opened and closed by hand, so a refusal rolls it back quietly (a throw
        // into db::transaction is logged as a critical failure). While it is open
        // nothing may open another: a START TRANSACTION commits the one open.
        // Rolled back unless committed
        class Txn
        {
        public:
            Txn()
            : open_(db::transactionStart())
            {
                if (open_)
                {
                    db::getDatabase().setInTransaction(true); // a lost connection is not retried mid-transaction
                }
            }

            ~Txn()
            {
                rollback();
            }

            Txn(const Txn&)            = delete;
            Txn& operator=(const Txn&) = delete;

            auto ok() const -> bool
            {
                return open_;
            }

            // The COMMIT; false when it failed -- or when xi_test makes it
            // fail as the wire can
            auto commit() -> bool
            {
                if (!open_)
                {
                    return false;
                }
                if (takeFault(MoveFault::LostAtCommit))
                {
                    loseConnection();
                }
                bool done = false;
                if (takeFault(MoveFault::RolledBack))
                {
                    db::transactionRollback();
                }
                else
                {
                    done = db::transactionCommit();
                    if (!done)
                    {
                        db::transactionRollback();
                    }
                    else if (takeFault(MoveFault::Committed))
                    {
                        done = false; // committed, and its answer lost
                    }
                    else if (takeFault(MoveFault::CommittedThenLost))
                    {
                        loseConnection();
                        db::transactionRollback(); // fails on the lost connection, as a failed COMMIT's does
                        done = false;
                    }
                }
                db::getDatabase().setInTransaction(false);
                open_ = false;
                return done;
            }

            void rollback()
            {
                if (open_)
                {
                    db::transactionRollback();
                    db::getDatabase().setInTransaction(false);
                    open_ = false;
                }
            }

        private:
            bool open_ = false;
        };

        // ---- the bank's rows --------------------------------------------------

        auto headFrom(const auto& rset) -> Head
        {
            Head head;
            head.gil      = rset->get<uint32>("gil");
            head.version  = rset->get<uint32>("version");
            head.sackCan  = rset->get<uint8>("sack_can");
            head.pearlCan = rset->get<uint8>("pearl_can");
            return head;
        }

        // The head locked until the open transaction ends: every move on one
        // shell waits for the one before it, on whatever map server. Made
        // before the transaction (ensureHead), never inside it: INSERT IGNORE
        // on a row that is there takes a shared lock, and two servers each
        // holding one while they wait for the exclusive lock would deadlock.
        // None when it cannot be had
        auto lockHead(const uint32 lsid) -> std::optional<Head>
        {
            if (const auto rset = db::preparedStmt("SELECT gil, version, sack_can, pearl_can FROM cardian_ls_bank_head WHERE lsid = ? FOR UPDATE", lsid); rset && rset->next())
            {
                return headFrom(rset);
            }
            return std::nullopt;
        }

        // A stack move's mark on the head: the gil it leaves, and a new version
        auto writeHead(const uint32 lsid, const uint32 gil) -> bool
        {
            return db::preparedStmt("UPDATE cardian_ls_bank_head SET gil = ?, version = version + 1 WHERE lsid = ?", gil, lsid) != nullptr;
        }

        // A gil move's mark: the gil alone. The version tells a withdrawal the
        // stacks moved, and gil moves none
        auto writeGil(const uint32 lsid, const uint32 gil) -> bool
        {
            return db::preparedStmt("UPDATE cardian_ls_bank_head SET gil = ? WHERE lsid = ?", gil, lsid) != nullptr;
        }

        // How many of an item one slot holds
        auto stackSizeOf(const uint16 itemId) -> uint32
        {
            const CItem* PItem = xi::items::lookup(itemId);
            return PItem != nullptr ? std::max<uint32>(1, PItem->getStackSize()) : 1;
        }

        // The shell's stacks written whole, a row a stack in its order. Only
        // inside a move's transaction, whose rollback undoes a half-written bank
        auto writeStacks(const uint32 lsid, const std::vector<Stack>& bank) -> bool
        {
            if (!db::preparedStmt("DELETE FROM cardian_ls_bank WHERE lsid = ?", lsid))
            {
                return false;
            }
            uint8 slot = 0;
            for (const auto& stack : bank)
            {
                ++slot;
                Extra extra = stack.extra; // the binding reads a blob from a mutable value
                if (!db::preparedStmt("INSERT INTO cardian_ls_bank (lsid, slot, itemId, quantity, extra) VALUES (?, ?, ?, ?, ?)", lsid, slot, stack.item,
                                      stack.qty, extra))
                {
                    return false;
                }
            }
            return true;
        }

        // ---- a move that fails once her items moved (#478) --------------------
        //
        // Once her item transaction has changed her, a move that fails must put
        // her back as her rows are, and her rows are what the database rolled
        // back to. It fails before the COMMIT -- a statement refused, or the
        // connection lost, which fails every statement after it while MariaDB
        // rolls the move back -- or at the COMMIT, on the wire: rolled back and
        // told so, or committed with its answer lost. Each move writes a mark of
        // its own inside its transaction, so a COMMIT told failed is settled by
        // asking whether the mark is there: there, the move happened, and its
        // item transaction lets go as after any move; not there, it did not. A
        // move that did not happen is abandoned (abandon). Proven by xi_test's
        // scripts/tests/cardian/ls_bank_commit.lua, a lost connection included.

        // A slot of hers: the bag and the slot
        using Slot = std::pair<uint8, uint8>;

        auto newMark() -> uint64
        {
            static std::mt19937_64 marks{ std::random_device{}() };
            uint64                 mark = 0;
            while (mark == 0)
            {
                mark = marks();
            }
            return mark;
        }

        auto writeMark(const uint64 mark) -> bool
        {
            if (takeFault(MoveFault::LostBeforeCommit))
            {
                loseConnection();
            }
            else if (takeFault(MoveFault::LostTwice))
            {
                nextFault = MoveFault::LostInPutBack;
                loseConnection();
            }
            return db::preparedStmt("INSERT INTO cardian_ls_bank_marks (mark) VALUES (?)", mark) != nullptr;
        }

        void forgetMark(const uint64 mark)
        {
            db::preparedStmt("DELETE FROM cardian_ls_bank_marks WHERE mark = ?", mark);
        }

        enum class Outcome : uint8
        {
            Committed,
            RolledBack,
            Unknown,
        };

        // Whether a move whose COMMIT was told failed went through after all:
        // its mark, read with a lock, so a COMMIT the database is still
        // finishing is waited for rather than read as rolled back. The read
        // reconnects a lost connection, and a database that stays gone ends
        // the map server (LandSandBoat's query layer); Unknown is a read that
        // failed otherwise
        auto outcomeOf(const uint64 mark) -> Outcome
        {
            if (const auto rset = db::preparedStmt("SELECT mark FROM cardian_ls_bank_marks WHERE mark = ? LOCK IN SHARE MODE", mark))
            {
                return rset->next() ? Outcome::Committed : Outcome::RolledBack;
            }
            return Outcome::Unknown;
        }

        // Her character in memory put back as the rollback left her rows. The
        // item transaction's undo writes her rows as it puts her back, so it
        // runs in a scratch transaction thrown away after it, whose START
        // reconnects a lost connection; and a stack the move took whole has its
        // row cleared in that scratch first -- the rollback put the row back
        // already, and the undo's own insert of it would fail and leave the
        // stack out of memory
        void putBack(ItemClaimTransaction& items, const CCharEntity* PHolder, const std::optional<Slot>& whole)
        {
            Txn scratch;
            if (scratch.ok() && whole.has_value())
            {
                db::preparedStmt("DELETE FROM char_inventory WHERE charid = ? AND location = ? AND slot = ?", PHolder->id, whole->first, whole->second);
            }
            if (takeFault(MoveFault::LostInPutBack))
            {
                loseConnection();
            }
            items.rollback();
            scratch.rollback();
        }

        // Her character in memory made to follow her rows in the slots a move
        // that did not happen touched. putBack's undo leaves memory as it was
        // wherever a write of it fails (a connection lost again), so memory is
        // checked against the rows, which are the truth after any rollback --
        // and after a COMMIT nobody can settle, whichever way it went. A slot
        // that agrees is left alone; one that does not is set from its row as
        // the game loads one, with a critical line in the log
        void followRows(CCharEntity* PHolder, const std::vector<Slot>& touched)
        {
            bool changed = false;
            for (const auto& [loc, slot] : touched)
            {
                auto* storage = PHolder->getStorage(loc);
                if (storage == nullptr)
                {
                    continue;
                }
                const auto rset = db::preparedStmt("SELECT itemId, quantity, signature, extra FROM char_inventory WHERE charid = ? AND location = ? AND slot = ?", PHolder->id,
                                                   loc, slot);
                if (rset == nullptr)
                {
                    ShowCriticalFmt("lsbank: {}'s slot {} of bag {} could not be read to check memory against it", PHolder->getName(), slot, loc);
                    continue;
                }
                CItem* mine = storage->GetItem(slot);
                if (!rset->next())
                {
                    if (mine != nullptr)
                    {
                        ShowCriticalFmt("lsbank: {} holds {} x{} in slot {} of bag {} in memory, and nothing in its row: taken out of memory", PHolder->getName(),
                                        mine->getID(), mine->getQuantity(), slot, loc);
                        storage->RemoveItem(slot);
                        PHolder->pushPacket<GP_SERV_COMMAND_ITEM_ATTR>(nullptr, static_cast<CONTAINER_ID>(loc), slot);
                        changed = true;
                    }
                    continue;
                }
                const auto itemId = rset->get<uint16>("itemId");
                const auto qty    = rset->get<uint32>("quantity");
                if (mine != nullptr && mine->getID() == itemId)
                {
                    if (mine->getQuantity() != qty)
                    {
                        ShowCriticalFmt("lsbank: {} holds {} x{} in slot {} of bag {} in memory, and x{} in its row: set from the row", PHolder->getName(), itemId,
                                        mine->getQuantity(), slot, loc, qty);
                        mine->setQuantity(qty);
                        PHolder->pushPacket<GP_SERV_COMMAND_ITEM_NUM>(static_cast<CONTAINER_ID>(loc), slot, qty);
                        changed = true;
                    }
                    continue;
                }
                Extra extra{};
                db::extractFromBlob(rset, "extra", extra);
                auto PItem = rebuild(itemId, qty, extra);
                if (PItem == nullptr)
                {
                    ShowCriticalFmt("lsbank: {}'s row in slot {} of bag {} holds item {}, which the game does not know", PHolder->getName(), slot, loc, itemId);
                    continue;
                }
                if (PItem->isType(ITEM_LINKSHELL) || PItem->hasFlag(ItemFlag::Inscribable))
                {
                    PItem->setSignature(rset->get<std::string>("signature"));
                }
                ShowCriticalFmt("lsbank: {} holds {} in slot {} of bag {} in memory, and {} x{} in its row: set from the row", PHolder->getName(),
                                mine != nullptr ? mine->getID() : 0, slot, loc, itemId, qty);
                if (mine != nullptr)
                {
                    storage->RemoveItem(slot);
                }
                if (storage->InsertItem(std::move(PItem), slot) == ERROR_SLOTID)
                {
                    ShowCriticalFmt("lsbank: {}'s slot {} of bag {} would not take its row's item back into memory", PHolder->getName(), slot, loc);
                    PHolder->pushPacket<GP_SERV_COMMAND_ITEM_ATTR>(nullptr, static_cast<CONTAINER_ID>(loc), slot);
                    changed = true;
                    continue;
                }
                PHolder->pushPacket<GP_SERV_COMMAND_ITEM_ATTR>(storage->GetItem(slot), static_cast<CONTAINER_ID>(loc), slot);
                changed = true;
            }
            if (changed)
            {
                PHolder->pushPacket<GP_SERV_COMMAND_ITEM_SAME>(PHolder);
            }
        }

        // A move that did not happen, once her item transaction has changed
        // her: the database rolled back first, then her character put back,
        // then checked against her rows (followRows). Never her undo inside the
        // move's own transaction: on a lost connection every write of it fails,
        // and she would stay as the move left her while the database rolls her
        // rows back. `whole`: her slot the move took whole, if it took one;
        // `touched`: her slots the move changed
        void abandon(Txn& txn, ItemClaimTransaction& items, CCharEntity* PHolder, const std::optional<Slot>& whole, const std::vector<Slot>& touched)
        {
            txn.rollback();
            putBack(items, PHolder, whole);
            followRows(PHolder, touched);
        }

        // A move's COMMIT, a failed one settled: true when the move happened
        // (its item transaction let go), false when it did not (abandoned)
        auto commitMove(Txn& txn, ItemClaimTransaction& items, CCharEntity* PHolder, const uint64 mark, const std::optional<Slot>& whole, const std::vector<Slot>& touched,
                        const std::string_view what) -> bool
        {
            bool happened = txn.commit();
            if (!happened)
            {
                switch (outcomeOf(mark))
                {
                    case Outcome::Committed:
                        ShowWarningFmt("lsbank: {}'s {} was told its COMMIT failed, and it went through: kept", PHolder->getName(), what);
                        happened = true;
                        break;
                    case Outcome::RolledBack:
                        ShowWarningFmt("lsbank: {}'s {} did not commit: put back", PHolder->getName(), what);
                        break;
                    case Outcome::Unknown:
                        ShowCriticalFmt("lsbank: {}'s {} failed to commit, and the database cannot say whether it went through: her character follows her rows",
                                        PHolder->getName(), what);
                        break;
                }
            }
            if (!happened)
            {
                abandon(txn, items, PHolder, whole, touched);
                return false;
            }
            if (!items.commit())
            {
                ShowErrorFmt("lsbank: {}'s item transaction would not let go after the bank committed", PHolder->getName());
            }
            forgetMark(mark);
            return true;
        }

        // ---- the moves --------------------------------------------------------
        //
        // Each is one database transaction (Txn): the shell's head locked, the
        // bank read under the lock and judged against what the page saw, the
        // character's item transaction run on the same connection, the bank's
        // rows written, the move's mark written, and the database committed
        // before the item transaction is (commitMove). A refusal before her
        // item step returns, the transaction rolled back as it goes; any
        // failure from her item step on abandons the move (abandon).

        // The common start of a move, inside its transaction: the head locked,
        // and his rank's right to the move read under that lock, since the
        // holder may have changed it after the Link's judgement read it. The
        // head; none and why when it may not go
        auto beginMove(const Shell& shell, const uint8 right, uint16& why) -> std::optional<Head>
        {
            const auto head = lockHead(shell.lsid);
            if (!head.has_value())
            {
                why = CL_S_REFUSED;
                return std::nullopt;
            }
            if ((cardian::lsbank::canOf(shell.rank, head->sackCan, head->pearlCan) & right) == 0)
            {
                why = CL_S_BANK_NOT_ALLOWED;
                return std::nullopt;
            }
            return head;
        }

        // An item the bank takes: one whose script acts as it leaves its owner
        // (onItemDrop -- the firefly cages, which hand back a temporary cage)
        // is refused, or a deposit and a withdrawal would make one each time
        auto bankable(const CItem* PItem) -> bool
        {
            return !luautils::getCachedFileFunction(fmt::format("./scripts/items/{}.lua", PItem->getName()), "onItemDrop").valid();
        }
    } // namespace

    void ensureTables()
    {
        db::preparedStmt("CREATE TABLE IF NOT EXISTS `cardian_ls_bank` ("
                         "`lsid` int(10) unsigned NOT NULL, "
                         "`slot` tinyint(3) unsigned NOT NULL, " // 1 to CL_BANK_SIZE, the bank's order
                         "`itemId` smallint(5) unsigned NOT NULL, "
                         "`quantity` int(10) unsigned NOT NULL DEFAULT 1, "
                         "`extra` blob(24) DEFAULT NULL, " // the item's extra data, as char_inventory's
                         "PRIMARY KEY (`lsid`, `slot`)) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_general_ci");
        db::preparedStmt("CREATE TABLE IF NOT EXISTS `cardian_ls_bank_head` ("
                         "`lsid` int(10) unsigned NOT NULL, "
                         "`gil` int(10) unsigned NOT NULL DEFAULT 0, "
                         "`version` int(10) unsigned NOT NULL DEFAULT 0, "      // every move of its stacks changes it
                         "`sack_can` tinyint(3) unsigned NOT NULL DEFAULT 15, " // CL_BANK_* bits the holder lets a sack holder
                         "`pearl_can` tinyint(3) unsigned NOT NULL DEFAULT 1, " // ... a pearl holder
                         "PRIMARY KEY (`lsid`)) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_general_ci");
        // a move's mark, written inside its transaction and forgotten once it
        // has committed: one still there tells a COMMIT whose answer was lost
        // that it went through (#478)
        db::preparedStmt("CREATE TABLE IF NOT EXISTS `cardian_ls_bank_marks` ("
                         "`mark` bigint(20) unsigned NOT NULL, "
                         "`at` timestamp NOT NULL DEFAULT current_timestamp(), "
                         "PRIMARY KEY (`mark`)) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_general_ci");
        db::preparedStmt("DELETE FROM cardian_ls_bank_marks WHERE at < NOW() - INTERVAL 1 DAY"); // left by a map stopped mid-move
    }

    auto ensureHead(const uint32 lsid) -> bool
    {
        return db::preparedStmt("INSERT IGNORE INTO cardian_ls_bank_head (lsid, gil, version, sack_can, pearl_can) VALUES (?, 0, 0, ?, ?)", lsid,
                                cardian::lsbank::kSackDefault, cardian::lsbank::kPearlDefault) != nullptr;
    }

    auto readHead(const uint32 lsid) -> Head
    {
        if (const auto rset = db::preparedStmt("SELECT gil, version, sack_can, pearl_can FROM cardian_ls_bank_head WHERE lsid = ?", lsid); rset && rset->next())
        {
            return headFrom(rset);
        }
        return Head{};
    }

    auto readStacks(const uint32 lsid) -> std::optional<std::vector<Stack>>
    {
        auto rset = db::preparedStmt("SELECT itemId, quantity, extra FROM cardian_ls_bank WHERE lsid = ? ORDER BY slot", lsid);
        if (takeFault(MoveFault::BankReadFails))
        {
            rset.reset(); // xi_test's: the read failed
        }
        if (rset == nullptr)
        {
            return std::nullopt;
        }
        std::vector<Stack> bank;
        while (rset->next())
        {
            Stack stack;
            stack.item = rset->get<uint16>("itemId");
            stack.qty  = rset->get<uint32>("quantity");
            db::extractFromBlob(rset, "extra", stack.extra);
            stack.stack = stackSizeOf(stack.item);
            bank.push_back(stack);
        }
        return bank;
    }

    // Built as charutils::LoadInventory builds an item, item by item: its
    // quantity and extra data, a linkshell item's kind when its data has none,
    // an enchanted item's last use (its recast) and a piece of gear's
    // augments. A copy of upstream's steps: compare them at every merge from
    // base
    auto rebuild(const uint16 itemId, const uint32 qty, const Extra& extra) -> std::unique_ptr<CItem>
    {
        auto PItem = xi::items::spawn(itemId);
        if (PItem == nullptr)
        {
            return nullptr;
        }
        PItem->setQuantity(qty);
        std::memcpy(PItem->m_extra, extra.data(), std::min<std::size_t>(extra.size(), CItem::extra_size));

        if (PItem->isType(ITEM_LINKSHELL))
        {
            auto* PLink = static_cast<CItemLinkshell*>(PItem.get());
            if (PLink->GetLSType() == 0)
            {
                PLink->SetLSType(static_cast<LSTYPE>(PItem->getID() - 0x200));
            }
        }
        if (auto* PUsable = dynamic_cast<CItemUsable*>(PItem.get()))
        {
            uint32 useTime = 0;
            std::memcpy(&useTime, PUsable->m_extra + 0x04, sizeof(useTime));
            if (useTime != 0)
            {
                PUsable->setLastUseTime(timer::now() - std::chrono::seconds(earth_time::vanadiel_timestamp() - useTime));
            }
        }
        if ((PItem->isType(ITEM_EQUIPMENT) || PItem->isType(ITEM_WEAPON)) && !PItem->isSubType(ITEM_CHARGED))
        {
            auto* PEquip = static_cast<CItemEquipment*>(PItem.get());
            for (uint8 slot = 0; slot < 4; ++slot)
            {
                if (PEquip->getAugment(slot) != 0)
                {
                    PEquip->ApplyAugment(slot);
                }
            }
        }
        return PItem;
    }

    // A stack from her bag into the bank: the slot holds what the page saw,
    // idle (not worn, not in a trade or a bazaar), and the bank has room for
    // it once merged
    auto deposit(CCharEntity* PHolder, const Shell& shell, const cl_bank_deposit& ask, uint8& landed) -> uint16
    {
        const auto lsid    = shell.lsid;
        auto*      storage = PHolder->getStorage(ask.loc);
        CItem*     PItem   = storage != nullptr && ask.slot > 0 ? storage->GetItem(ask.slot) : nullptr;
        if (PItem == nullptr || PItem->getQuantity() == 0)
        {
            return CL_S_NO_ITEM;
        }
        if (PItem->isType(ITEM_CURRENCY))
        {
            return CL_S_GIL_NOT_AN_ITEM;
        }
        if (PItem->getID() != ask.item)
        {
            return CL_S_ITEM_SLIPPED_AWAY;
        }
        if (PItem->state() == ItemState::Equipped)
        {
            return CL_S_ITEM_EQUIPPED;
        }
        if (ask.qty == 0 || ask.qty > PItem->getQuantity())
        {
            return CL_S_BAD_QUANTITY;
        }
        if (!bankable(PItem))
        {
            return CL_S_ITEM_CANNOT_MOVE;
        }
        // a stack taken whole leaves her row to be put back if the move fails
        const auto whole   = ask.qty == PItem->getQuantity() ? std::optional<Slot>(Slot{ ask.loc, ask.slot }) : std::nullopt;
        const auto touched = std::vector<Slot>{ Slot{ ask.loc, ask.slot } };
        const auto what    = fmt::format("deposit of {} x{} (shell {})", ask.item, ask.qty, lsid);
        Stack      in;
        in.item  = PItem->getID();
        in.qty   = ask.qty;
        in.stack = std::max<uint32>(1, PItem->getStackSize());
        std::memcpy(in.extra.data(), PItem->m_extra, std::min<std::size_t>(in.extra.size(), CItem::extra_size));

        if (!ensureHead(lsid))
        {
            return CL_S_REFUSED;
        }
        Txn txn;
        if (!txn.ok())
        {
            return CL_S_REFUSED;
        }
        uint16     why  = CL_S_OK;
        const auto head = beginMove(shell, CL_BANK_PUT, why);
        if (!head.has_value())
        {
            return why;
        }
        // never a failed read taken for an empty bank: the deposit would
        // write the bank as its one stack
        const auto stacks = readStacks(lsid);
        if (!stacks.has_value())
        {
            return CL_S_REFUSED;
        }
        const auto taken = cardian::lsbank::deposit(*stacks, in);
        if (!taken.has_value())
        {
            return CL_S_BANK_FULL;
        }
        auto items = ItemClaimTransaction::start(PHolder);
        if (items == nullptr || items->claimSlot(ask.loc, ask.slot) == nullptr)
        {
            return CL_S_ITEM_BUSY;
        }
        if (!items->take(ask.loc, ask.slot, ask.qty))
        {
            abandon(txn, *items, PHolder, whole, touched);
            return CL_S_ITEM_SLIPPED_AWAY;
        }
        const auto mark = newMark();
        if (!writeStacks(lsid, taken->bank) || !writeHead(lsid, head->gil) || !writeMark(mark))
        {
            ShowWarningFmt("lsbank: {}'s {} could not be written: put back", PHolder->getName(), what);
            abandon(txn, *items, PHolder, whole, touched);
            return CL_S_REFUSED;
        }
        if (!commitMove(txn, *items, PHolder, mark, whole, touched, what))
        {
            return CL_S_REFUSED;
        }
        landed = static_cast<uint8>(taken->landed + 1);
        ShowInfoFmt("lsbank: {} banks {} x{} from bag {} (shell {}, {} of {} slots used)", PHolder->getName(), in.item, in.qty, ask.loc, lsid,
                    taken->bank.size(), cardian::lsbank::kSize);
        return CL_S_OK;
    }

    // A stack from the bank into her bag, as the game gives an item: gear
    // alone into a wardrobe, no second of a Rare item, and a free slot
    auto withdraw(CCharEntity* PHolder, const Shell& shell, const cl_bank_withdraw& ask, uint8& landed) -> uint16
    {
        const auto lsid = shell.lsid;
        if (!ensureHead(lsid))
        {
            return CL_S_REFUSED;
        }
        Txn txn;
        if (!txn.ok())
        {
            return CL_S_REFUSED;
        }
        uint16     why  = CL_S_OK;
        const auto head = beginMove(shell, CL_BANK_TAKE, why);
        if (!head.has_value())
        {
            return why;
        }
        // the bank as the page saw it: a move since may have put another
        // stack of the same item in that slot (another shell's pearl, a
        // piece with other augments)
        if ((head->version & 0xFFFF) != ask.version)
        {
            return CL_S_ITEM_SLIPPED_AWAY;
        }
        const auto read = readStacks(lsid);
        if (!read.has_value())
        {
            return CL_S_REFUSED;
        }
        const auto& before = *read;
        if (const auto refused = cardian::lsbank::judgeWithdraw(before, ask.slot, ask.item, ask.qty); refused != CL_S_OK)
        {
            return refused;
        }
        const auto& stack = before[ask.slot - 1];
        auto        PItem = rebuild(stack.item, ask.qty, stack.extra);
        if (PItem == nullptr)
        {
            ShowErrorFmt("lsbank: item {} in the bank of shell {} is no item the game knows", stack.item, lsid);
            return CL_S_ITEM_CANNOT_MOVE;
        }
        if (cardian::lsbank::wardrobe(ask.loc) && !PItem->isType(ITEM_EQUIPMENT))
        {
            return CL_S_NOT_EQUIPMENT;
        }
        const auto* storage = PHolder->getStorage(ask.loc);
        const bool  rare    = PItem->hasFlag(ItemFlag::Rare);
        const bool  owned   = rare && charutils::HasItem(PHolder, stack.item);
        if (const auto refused = cardian::lsbank::judgeReceive(rare, owned, storage != nullptr ? storage->GetFreeSlotsCount() : 0); refused != CL_S_OK)
        {
            return refused;
        }
        const auto after = cardian::lsbank::withdraw(before, ask.slot, ask.qty);
        if (!after.has_value())
        {
            return CL_S_BAD_QUANTITY;
        }
        const uint16 itemId = stack.item;
        auto         items  = ItemClaimTransaction::start(PHolder);
        if (items == nullptr)
        {
            return CL_S_REFUSED;
        }
        const auto given = items->give(ask.loc, std::move(PItem), Silence::Yes);
        if (!given.has_value())
        {
            abandon(txn, *items, PHolder, std::nullopt, {});
            return CL_S_NO_SPACE;
        }
        const auto touched = std::vector<Slot>{ Slot{ ask.loc, *given } };
        const auto what    = fmt::format("withdrawal of {} x{} (shell {})", itemId, ask.qty, lsid);
        const auto mark    = newMark();
        if (!writeStacks(lsid, *after) || !writeHead(lsid, head->gil) || !writeMark(mark))
        {
            ShowWarningFmt("lsbank: {}'s {} could not be written: put back", PHolder->getName(), what);
            abandon(txn, *items, PHolder, std::nullopt, touched);
            return CL_S_REFUSED;
        }
        if (!commitMove(txn, *items, PHolder, mark, std::nullopt, touched, what))
        {
            return CL_S_REFUSED;
        }
        landed = *given;
        ShowInfoFmt("lsbank: {} takes {} x{} from the bank into bag {} (shell {}, {} of {} slots used)", PHolder->getName(), itemId, ask.qty, ask.loc, lsid,
                    after->size(), cardian::lsbank::kSize);
        return CL_S_OK;
    }

    // Gil between the bank and her purse, the game's own pay and earn on her
    // side: the purse it comes from must hold it
    auto gil(CCharEntity* PHolder, const Shell& shell, const cl_bank_gil& ask, uint32& bankGil) -> uint16
    {
        const auto lsid = shell.lsid;
        // one move is at most what a purse holds, as the game's own gil
        // amounts are: the amount never reaches an int32's sign bit
        if (ask.amount == 0 || ask.amount > kPurseMax)
        {
            return CL_S_BAD_QUANTITY;
        }
        if (!ensureHead(lsid))
        {
            return CL_S_REFUSED;
        }
        Txn txn;
        if (!txn.ok())
        {
            return CL_S_REFUSED;
        }
        uint16     why  = CL_S_OK;
        const auto head = beginMove(shell, ask.out != 0 ? CL_BANK_GIL_OUT : CL_BANK_GIL_IN, why);
        if (!head.has_value())
        {
            return why;
        }
        auto items = ItemClaimTransaction::start(PHolder);
        if (items == nullptr)
        {
            return CL_S_REFUSED;
        }
        // her purse's row is never taken away, even emptied (the game keeps a
        // purse of 0), so a gil move leaves no row to put back
        const auto purse = std::vector<Slot>{ Slot{ LOC_INVENTORY, 0 } };
        const auto what  = fmt::format("{} gil {} (shell {})", ask.amount, ask.out != 0 ? "out" : "in", lsid);
        uint32     after = head->gil;
        if (ask.out != 0)
        {
            if (head->gil < ask.amount)
            {
                return CL_S_NOT_ENOUGH_GIL;
            }
            if (!items->earn(ask.amount))
            {
                abandon(txn, *items, PHolder, std::nullopt, purse);
                return CL_S_REFUSED;
            }
            after = head->gil - ask.amount;
        }
        else
        {
            if (std::numeric_limits<uint32>::max() - head->gil < ask.amount)
            {
                return CL_S_BAD_QUANTITY;
            }
            if (!items->pay(ask.amount))
            {
                abandon(txn, *items, PHolder, std::nullopt, purse);
                return CL_S_NOT_ENOUGH_GIL;
            }
            after = head->gil + ask.amount;
        }
        const auto mark = newMark();
        if (!writeGil(lsid, after) || !writeMark(mark))
        {
            ShowWarningFmt("lsbank: {}'s {} could not be written: put back", PHolder->getName(), what);
            abandon(txn, *items, PHolder, std::nullopt, purse);
            return CL_S_REFUSED;
        }
        if (!commitMove(txn, *items, PHolder, mark, std::nullopt, purse, what))
        {
            return CL_S_REFUSED;
        }
        bankGil = after;
        ShowInfoFmt("lsbank: {} moves {} gil {} the bank (shell {}, {} gil in it)", PHolder->getName(), ask.amount, ask.out != 0 ? "out of" : "into", lsid, after);
        return CL_S_OK;
    }

    void failNext(const MoveFault fault)
    {
        nextFault = fault;
    }

    auto pendingFault() -> MoveFault
    {
        return nextFault;
    }
} // namespace pawn::lsbank::moves
