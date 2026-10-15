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

#include "item_moves.h"

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
#include "items/transaction.h"
#include "packets/s2c/0x01d_item_same.h"
#include "packets/s2c/0x01e_item_num.h"
#include "packets/s2c/0x020_item_attr.h"
#include "utils/itemutils.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <random>
#include <set>
#include <string>
#include <thread>

namespace pawn::itemmoves
{
    namespace
    {
        Fault nextFault = Fault::None; // xi_test's (failNext)

        // xi_test's lost connection: MariaDB drops it as it drops any client
        // gone quiet, and rolls back the transaction it held
        void loseConnection()
        {
            db::preparedStmt("SET SESSION wait_timeout = 1");
            std::this_thread::sleep_for(std::chrono::milliseconds(2500));
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

        // The characters in memory put back as the rollback left their rows.
        // The item transaction's undo writes their rows as it puts them back,
        // so it runs in a scratch transaction thrown away after it, whose START
        // reconnects a lost connection; and a stack the move took whole has its
        // row cleared in that scratch first -- the rollback put the row back
        // already, and the undo's own insert of it would fail and leave the
        // stack out of memory
        void putBack(Transaction& items, const std::optional<Slot>& whole)
        {
            Txn scratch;
            if (scratch.ok() && whole.has_value() && whole->who != nullptr)
            {
                db::preparedStmt("DELETE FROM char_inventory WHERE charid = ? AND location = ? AND slot = ?", whole->who->id, whole->loc, whole->slot);
            }
            if (takeFault(Fault::LostInPutBack))
            {
                loseConnection();
            }
            items.rollback();
            scratch.rollback();
        }

        // A character in memory made to follow her rows in the slots a move
        // that did not happen touched. putBack's undo leaves memory as it was
        // wherever a write of it fails (a connection lost again), so memory is
        // checked against the rows, which are the truth after any rollback --
        // and after a COMMIT nobody can settle, whichever way it went. A slot
        // that agrees is left alone; one that does not is set from its row as
        // the game loads one, with a critical line in the log
        void followRows(const std::vector<Slot>& touched)
        {
            std::set<CCharEntity*> changed;
            for (const auto& [who, loc, slot] : touched)
            {
                auto* storage = who != nullptr ? who->getStorage(loc) : nullptr;
                if (storage == nullptr)
                {
                    continue;
                }
                const auto rset = db::preparedStmt("SELECT itemId, quantity, signature, extra FROM char_inventory WHERE charid = ? AND location = ? AND slot = ?", who->id, loc, slot);
                if (rset == nullptr)
                {
                    ShowCriticalFmt("itemmoves: {}'s slot {} of bag {} could not be read to check memory against it", who->getName(), slot, loc);
                    continue;
                }
                CItem* mine = storage->GetItem(slot);
                if (!rset->next())
                {
                    if (mine != nullptr)
                    {
                        ShowCriticalFmt("itemmoves: {} holds {} x{} in slot {} of bag {} in memory, and nothing in its row: taken out of memory", who->getName(),
                                        mine->getID(), mine->getQuantity(), slot, loc);
                        storage->RemoveItem(slot);
                        who->pushPacket<GP_SERV_COMMAND_ITEM_ATTR>(nullptr, static_cast<CONTAINER_ID>(loc), slot);
                        changed.insert(who);
                    }
                    continue;
                }
                const auto itemId = rset->get<uint16>("itemId");
                const auto qty    = rset->get<uint32>("quantity");
                if (mine != nullptr && mine->getID() == itemId)
                {
                    if (mine->getQuantity() != qty)
                    {
                        ShowCriticalFmt("itemmoves: {} holds {} x{} in slot {} of bag {} in memory, and x{} in its row: set from the row", who->getName(), itemId,
                                        mine->getQuantity(), slot, loc, qty);
                        mine->setQuantity(qty);
                        who->pushPacket<GP_SERV_COMMAND_ITEM_NUM>(static_cast<CONTAINER_ID>(loc), slot, qty);
                        changed.insert(who);
                    }
                    continue;
                }
                cardian::lsbank::Extra extra{};
                db::extractFromBlob(rset, "extra", extra);
                auto PItem = rebuild(itemId, qty, extra);
                if (PItem == nullptr)
                {
                    ShowCriticalFmt("itemmoves: {}'s row in slot {} of bag {} holds item {}, which the game does not know", who->getName(), slot, loc, itemId);
                    continue;
                }
                if (PItem->isType(ITEM_LINKSHELL) || PItem->hasFlag(ItemFlag::Inscribable))
                {
                    PItem->setSignature(rset->get<std::string>("signature"));
                }
                ShowCriticalFmt("itemmoves: {} holds {} in slot {} of bag {} in memory, and {} x{} in its row: set from the row", who->getName(),
                                mine != nullptr ? mine->getID() : 0, slot, loc, itemId, qty);
                if (mine != nullptr)
                {
                    storage->RemoveItem(slot);
                }
                if (storage->InsertItem(std::move(PItem), slot) == ERROR_SLOTID)
                {
                    ShowCriticalFmt("itemmoves: {}'s slot {} of bag {} would not take its row's item back into memory", who->getName(), slot, loc);
                    who->pushPacket<GP_SERV_COMMAND_ITEM_ATTR>(nullptr, static_cast<CONTAINER_ID>(loc), slot);
                    changed.insert(who);
                    continue;
                }
                who->pushPacket<GP_SERV_COMMAND_ITEM_ATTR>(storage->GetItem(slot), static_cast<CONTAINER_ID>(loc), slot);
                changed.insert(who);
            }
            for (auto* who : changed)
            {
                who->pushPacket<GP_SERV_COMMAND_ITEM_SAME>(who);
            }
        }
    } // namespace

    void failNext(const Fault fault)
    {
        nextFault = fault;
    }

    auto pendingFault() -> Fault
    {
        return nextFault;
    }

    auto takeFault(const Fault fault) -> bool
    {
        if (nextFault != fault)
        {
            return false;
        }
        nextFault = Fault::None;
        return true;
    }

    void ensureTable()
    {
        // a move's mark, written inside its transaction and forgotten once it
        // has committed: one still there tells a COMMIT whose answer was lost
        // that it went through (#478). Named for the bank, its first user
        db::preparedStmt("CREATE TABLE IF NOT EXISTS `cardian_ls_bank_marks` ("
                         "`mark` bigint(20) unsigned NOT NULL, "
                         "`at` timestamp NOT NULL DEFAULT current_timestamp(), "
                         "PRIMARY KEY (`mark`)) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_general_ci");
        db::preparedStmt("DELETE FROM cardian_ls_bank_marks WHERE at < NOW() - INTERVAL 1 DAY"); // left by a map stopped mid-move
    }

    Txn::Txn()
    : open_(db::transactionStart())
    {
        if (open_)
        {
            db::getDatabase().setInTransaction(true); // a lost connection is not retried mid-transaction
        }
    }

    Txn::~Txn()
    {
        rollback();
    }

    auto Txn::ok() const -> bool
    {
        return open_;
    }

    auto Txn::commit() -> bool
    {
        if (!open_)
        {
            return false;
        }
        if (takeFault(Fault::LostAtCommit))
        {
            loseConnection();
        }
        bool done = false;
        if (takeFault(Fault::RolledBack))
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
            else if (takeFault(Fault::Committed))
            {
                done = false; // committed, and its answer lost
            }
            else if (takeFault(Fault::CommittedThenLost))
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

    void Txn::rollback()
    {
        if (open_)
        {
            db::transactionRollback();
            db::getDatabase().setInTransaction(false);
            open_ = false;
        }
    }

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
        if (takeFault(Fault::LostBeforeCommit))
        {
            loseConnection();
        }
        else if (takeFault(Fault::LostTwice))
        {
            nextFault = Fault::LostInPutBack;
            loseConnection();
        }
        return db::preparedStmt("INSERT INTO cardian_ls_bank_marks (mark) VALUES (?)", mark) != nullptr;
    }

    void abandon(Txn& txn, Transaction& items, const std::optional<Slot>& whole, const std::vector<Slot>& touched)
    {
        txn.rollback();
        putBack(items, whole);
        followRows(touched);
    }

    auto commitMove(Txn& txn, Transaction& items, const uint64 mark, const std::optional<Slot>& whole, const std::vector<Slot>& touched, const std::string_view what) -> bool
    {
        bool happened = txn.commit();
        if (!happened)
        {
            switch (outcomeOf(mark))
            {
                case Outcome::Committed:
                    ShowWarningFmt("itemmoves: {} was told its COMMIT failed, and it went through: kept", what);
                    happened = true;
                    break;
                case Outcome::RolledBack:
                    ShowWarningFmt("itemmoves: {} did not commit: put back", what);
                    break;
                case Outcome::Unknown:
                    ShowCriticalFmt("itemmoves: {} failed to commit, and the database cannot say whether it went through: the characters follow their rows", what);
                    break;
            }
        }
        if (!happened)
        {
            abandon(txn, items, whole, touched);
            return false;
        }
        if (!items.commit())
        {
            ShowErrorFmt("itemmoves: {}: the item transaction would not let go after the database committed", what);
        }
        forgetMark(mark);
        return true;
    }

    auto rebuild(const uint16 itemId, const uint32 qty, const cardian::lsbank::Extra& extra) -> std::unique_ptr<CItem>
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
} // namespace pawn::itemmoves
