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
        using itemmoves::abandon;
        using itemmoves::commitMove;
        using itemmoves::newMark;
        using itemmoves::takeFault;
        using itemmoves::Txn;
        using itemmoves::writeMark;
        using Slot = itemmoves::Slot;

        constexpr uint32 kPurseMax = 999'999'999; // the most gil a purse holds, and so the most one move carries

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

        // ---- the moves --------------------------------------------------------
        //
        // Each is one database transaction (Txn, item_moves.h, which keeps a
        // move whole when the database fails partway, #478): the shell's head locked, the
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
        itemmoves::ensureTable(); // a move's marks (#478)
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
        if (takeFault(itemmoves::Fault::BankReadFails))
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

    auto rebuild(const uint16 itemId, const uint32 qty, const Extra& extra) -> std::unique_ptr<CItem>
    {
        return itemmoves::rebuild(itemId, qty, extra);
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
        const auto whole   = ask.qty == PItem->getQuantity() ? std::optional<Slot>(Slot{ PHolder, ask.loc, ask.slot }) : std::nullopt;
        const auto touched = std::vector<Slot>{ Slot{ PHolder, ask.loc, ask.slot } };
        const auto what    = fmt::format("{}'s deposit of {} x{} (shell {})", PHolder->getName(), ask.item, ask.qty, lsid);
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
            abandon(txn, *items, whole, touched);
            return CL_S_ITEM_SLIPPED_AWAY;
        }
        const auto mark = newMark();
        if (!writeStacks(lsid, taken->bank) || !writeHead(lsid, head->gil) || !writeMark(mark))
        {
            ShowWarningFmt("lsbank: {} could not be written: put back", what);
            abandon(txn, *items, whole, touched);
            return CL_S_REFUSED;
        }
        if (!commitMove(txn, *items, mark, whole, touched, what))
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
            abandon(txn, *items, std::nullopt, {});
            return CL_S_NO_SPACE;
        }
        const auto touched = std::vector<Slot>{ Slot{ PHolder, ask.loc, *given } };
        const auto what    = fmt::format("{}'s withdrawal of {} x{} (shell {})", PHolder->getName(), itemId, ask.qty, lsid);
        const auto mark    = newMark();
        if (!writeStacks(lsid, *after) || !writeHead(lsid, head->gil) || !writeMark(mark))
        {
            ShowWarningFmt("lsbank: {} could not be written: put back", what);
            abandon(txn, *items, std::nullopt, touched);
            return CL_S_REFUSED;
        }
        if (!commitMove(txn, *items, mark, std::nullopt, touched, what))
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
        const auto purse = std::vector<Slot>{ Slot{ PHolder, LOC_INVENTORY, 0 } };
        const auto what  = fmt::format("{}'s {} gil {} (shell {})", PHolder->getName(), ask.amount, ask.out != 0 ? "out" : "in", lsid);
        uint32     after = head->gil;
        if (ask.out != 0)
        {
            if (head->gil < ask.amount)
            {
                return CL_S_NOT_ENOUGH_GIL;
            }
            if (!items->earn(ask.amount))
            {
                abandon(txn, *items, std::nullopt, purse);
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
                abandon(txn, *items, std::nullopt, purse);
                return CL_S_NOT_ENOUGH_GIL;
            }
            after = head->gil + ask.amount;
        }
        const auto mark = newMark();
        if (!writeGil(lsid, after) || !writeMark(mark))
        {
            ShowWarningFmt("lsbank: {} could not be written: put back", what);
            abandon(txn, *items, std::nullopt, purse);
            return CL_S_REFUSED;
        }
        if (!commitMove(txn, *items, mark, std::nullopt, purse, what))
        {
            return CL_S_REFUSED;
        }
        bankGil = after;
        ShowInfoFmt("lsbank: {} moves {} gil {} the bank (shell {}, {} gil in it)", PHolder->getName(), ask.amount, ask.out != 0 ? "out of" : "into", lsid, after);
        return CL_S_OK;
    }

    auto depositEach(CCharEntity* PHolder, const Shell& shell, const uint8 loc, const std::function<bool(uint16)>& pick, const bool look, cl_bank_junk& answer) -> uint16
    {
        answer.count  = 0;
        answer.full   = 0;
        auto* storage = PHolder->getStorage(loc);
        if (storage == nullptr)
        {
            return CL_S_NO_SUCH_BAG;
        }
        // a look counts the bank's room as each deposit would, on the bank as
        // it would stand after the ones before
        std::optional<std::vector<Stack>> bank;
        if (look)
        {
            bank = readStacks(shell.lsid);
            if (!bank.has_value())
            {
                return CL_S_REFUSED;
            }
        }
        uint16 skipped = CL_S_NO_ITEM; // why the first stack named and left stayed: none named, until one is
        uint16 stopped = CL_S_OK;      // the refusal that ended it partway, if one did
        for (uint8 slot = 1; slot <= storage->GetSize() && answer.count < std::size(answer.items); ++slot)
        {
            const CItem* PItem = storage->GetItem(slot);
            if (PItem == nullptr || PItem->getQuantity() == 0 || PItem->isType(ITEM_CURRENCY) || !pick(PItem->getID()))
            {
                continue;
            }
            const uint16 itemId = PItem->getID();
            const uint32 qty    = PItem->getQuantity();
            uint16       status = CL_S_OK;
            if (look)
            {
                // in deposit's own order: worn, one the bank refuses, the
                // bank's room, then a stack in a trade or a bazaar
                Stack in;
                in.item  = itemId;
                in.qty   = qty;
                in.stack = std::max<uint32>(1, PItem->getStackSize());
                std::memcpy(in.extra.data(), PItem->m_extra, std::min<std::size_t>(in.extra.size(), CItem::extra_size));
                if (PItem->state() == ItemState::Equipped)
                {
                    status = CL_S_ITEM_EQUIPPED;
                }
                else if (!bankable(PItem))
                {
                    status = CL_S_ITEM_CANNOT_MOVE;
                }
                else if (auto taken = cardian::lsbank::deposit(*bank, in); !taken.has_value())
                {
                    status = CL_S_BANK_FULL;
                }
                else if (PItem->isBusy())
                {
                    status = CL_S_ITEM_BUSY;
                }
                else
                {
                    bank = std::move(taken->bank);
                }
            }
            else
            {
                cl_bank_deposit ask{};
                ask.cardian  = PHolder->id;
                ask.loc      = loc;
                ask.slot     = slot;
                ask.item     = itemId;
                ask.qty      = qty;
                uint8 landed = 0;
                status       = deposit(PHolder, shell, ask, landed);
            }
            if (status == CL_S_OK)
            {
                auto& moved = answer.items[answer.count++];
                moved.slot  = slot;
                moved.id    = itemId;
                moved.qty   = qty;
            }
            else if (status == CL_S_BANK_FULL)
            {
                answer.full = 1;
            }
            else if (status == CL_S_ITEM_EQUIPPED || status == CL_S_ITEM_BUSY || status == CL_S_ITEM_CANNOT_MOVE)
            {
                skipped = skipped == CL_S_NO_ITEM ? status : skipped;
            }
            else
            {
                stopped = status; // the bank or the database refused: nothing more is tried
                break;
            }
        }
        if (stopped != CL_S_OK)
        {
            return stopped; // what moved before it is in answer all the same
        }
        if (answer.count > 0)
        {
            return CL_S_OK;
        }
        return answer.full != 0 ? static_cast<uint16>(CL_S_BANK_FULL) : skipped;
    }
} // namespace pawn::lsbank::moves
