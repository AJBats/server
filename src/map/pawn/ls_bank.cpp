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

#include "ls_bank.h"

#include "auction.h"
#include "cardian_link.h"
#include "cardian_link_messages.h"
#include "club.h"
#include "errands.h"
#include "link_api.h"
#include "ls_bank_math.h"
#include "mog_house.h"
#include "pawn.h"
#include "pawn_items.h"
#include "seats.h"

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
#include "utils/charutils.h"
#include "utils/itemutils.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <memory>
#include <optional>
#include <set>
#include <utility>
#include <vector>

namespace pawn::lsbank
{
    namespace
    {
        using namespace cardian::link;
        using cardian::lsbank::Extra;
        using cardian::lsbank::Stack;

        constexpr std::size_t kPart     = sizeof(cl_bank::items) / sizeof(cl_item);
        constexpr std::size_t kBagItems = sizeof(cl_bank_holder::items) / sizeof(cl_item);
        constexpr uint32      kPurseMax      = 999'999'999; // the most gil a purse holds, and so the most one move carries

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
                if (open_)
                {
                    db::transactionRollback();
                    db::getDatabase().setInTransaction(false);
                }
            }

            Txn(const Txn&)            = delete;
            Txn& operator=(const Txn&) = delete;

            auto ok() const -> bool
            {
                return open_;
            }

            auto commit() -> bool
            {
                if (!open_)
                {
                    return false;
                }
                const bool done = db::transactionCommit();
                if (!done)
                {
                    db::transactionRollback();
                }
                db::getDatabase().setInTransaction(false);
                open_ = false;
                return done;
            }

        private:
            bool open_ = false;
        };

        // ---- the bank's rows --------------------------------------------------

        // The shell's head row: its gil, a version every move of its stacks changes, and what
        // the holder lets a sack holder and a pearl holder do
        struct Head
        {
            uint32 gil      = 0;
            uint32 version  = 0;
            uint8  sackCan  = cardian::lsbank::kSackDefault;
            uint8  pearlCan = cardian::lsbank::kPearlDefault;
        };

        auto ensureHead(const uint32 lsid) -> bool
        {
            return db::preparedStmt("INSERT IGNORE INTO cardian_ls_bank_head (lsid, gil, version, sack_can, pearl_can) VALUES (?, 0, 0, ?, ?)", lsid,
                                    cardian::lsbank::kSackDefault, cardian::lsbank::kPearlDefault) != nullptr;
        }

        auto headFrom(const auto& rset) -> Head
        {
            Head head;
            head.gil      = rset->get<uint32>("gil");
            head.version  = rset->get<uint32>("version");
            head.sackCan  = rset->get<uint8>("sack_can");
            head.pearlCan = rset->get<uint8>("pearl_can");
            return head;
        }

        // The head as it stands, unlocked: for a listing
        auto readHead(const uint32 lsid) -> Head
        {
            if (const auto rset = db::preparedStmt("SELECT gil, version, sack_can, pearl_can FROM cardian_ls_bank_head WHERE lsid = ?", lsid); rset && rset->next())
            {
                return headFrom(rset);
            }
            return Head{};
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

        // The shell's stacks as its rows hold them, in its order
        auto readStacks(const uint32 lsid) -> std::vector<Stack>
        {
            std::vector<Stack> bank;
            const auto         rset = db::preparedStmt("SELECT itemId, quantity, extra FROM cardian_ls_bank WHERE lsid = ? ORDER BY slot", lsid);
            while (rset && rset->next())
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

        // An item built as the game builds one it loads from a character's
        // rows (charutils::LoadInventory, item by item): its quantity and
        // extra data, a linkshell item's kind when its data has none, an
        // enchanted item's last use (its recast) and a piece of gear's
        // augments. A copy of upstream's steps: compare them at every merge
        // from base
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

        // A stack as the Link lists one: the item it would come out as
        auto listed(const Stack& stack, const uint8 slot) -> cl_item
        {
            if (const auto PItem = rebuild(stack.item, stack.qty, stack.extra); PItem != nullptr)
            {
                return pawn::linkapi::itemOf(PItem.get(), slot);
            }
            cl_item item{};
            item.slot = slot;
            item.id   = stack.item;
            item.qty  = stack.qty;
            return item;
        }

        // The bank's stacks as BANK parts of up to kPart, at least one, each
        // with what the asker may do and the shell's name and colour
        auto partsOf(const std::vector<Stack>& bank, const uint8 holders, const Head& head, const Shell& shell) -> std::vector<cl_bank>
        {
            std::vector<cl_bank> parts;
            std::size_t          at = 0;
            do
            {
                auto part     = make<cl_bank>();
                part.holders  = holders;
                part.used     = static_cast<uint8_t>(bank.size());
                part.rank     = shell.rank;
                part.can      = cardian::lsbank::canOf(shell.rank, head.sackCan, head.pearlCan);
                part.sackCan  = head.sackCan;
                part.pearlCan = head.pearlCan;
                part.gil      = head.gil;
                part.version  = head.version;
                part.color    = shell.color;
                setText(part.shell, shell.name);
                for (; at < bank.size() && part.count < kPart; ++at)
                {
                    part.items[part.count++] = listed(bank[at], static_cast<uint8>(at + 1));
                }
                parts.push_back(part);
            } while (at < bank.size());
            return parts;
        }

        // ---- the characters it reaches -------------------------------------

        // What a character is to him as a holder: himself, an alt of his
        // account or a cardian it owns; none for anyone else (a wild recruit
        // wearing his pearl is no holder)
        auto kindOf(const CCharEntity* PPlayer, const uint32 charid) -> std::optional<uint8>
        {
            if (charid == PPlayer->id)
            {
                return static_cast<uint8>(CL_HOLDER_SELF);
            }
            const auto member = pawn::club::memberOf(PPlayer, charid);
            if (member == cardian::errand::Member::Alt)
            {
                return static_cast<uint8>(CL_HOLDER_ALT);
            }
            if (member == cardian::errand::Member::Owned)
            {
                return static_cast<uint8>(CL_HOLDER_OWNED);
            }
            return std::nullopt;
        }

        // Every holder in his order: himself, then his alts and his own
        // cardians by name (the club's order)
        auto holdersOf(const CCharEntity* PPlayer) -> std::vector<std::pair<uint32, uint8>>
        {
            std::vector<std::pair<uint32, uint8>> holders{ { PPlayer->id, static_cast<uint8>(CL_HOLDER_SELF) } };
            for (const auto& [charid, member] : pawn::club::membersOf(PPlayer))
            {
                if (member == cardian::errand::Member::Alt)
                {
                    holders.emplace_back(charid, static_cast<uint8>(CL_HOLDER_ALT));
                }
                else if (member == cardian::errand::Member::Owned)
                {
                    holders.emplace_back(charid, static_cast<uint8>(CL_HOLDER_OWNED));
                }
            }
            return holders;
        }

        // An alt or a cardian of his is reached only standing in the world
        // where the place he stands at reaches her, by that place's own rule
        // (the user, 2026-10-14: no bags are reached across the world, and
        // the bank reaches a character wherever that place's menu does): in
        // his Mog House anyone of his in his city, by a Nomad Moogle one near
        // it -- the Mog House's job change's rule, pawn::moghouse::reaches --
        // and by an auction counter one near it, as the auction house shops
        // (pawn::auction::whereShopping). So every character a move changes is
        // one this map server holds in memory, changed through the game's own
        // item transaction, and none is ever loaded from her rows for it
        auto beside(const CCharEntity* PPlayer, const uint32 charid) -> CCharEntity*
        {
            CCharEntity* PPawn = pawn::findPawn(charid);
            if (PPawn == nullptr || PPawn->loc.zone == nullptr)
            {
                return nullptr;
            }
            const auto place = pawn::moghouse::placeOf(PPlayer);
            if ((place.mogHouse || place.moogle != nullptr) && pawn::moghouse::reaches(place, PPlayer, PPawn) == CL_S_OK)
            {
                return PPawn;
            }
            if (const auto* PCounter = pawn::auction::counterNear(PPlayer);
                PCounter != nullptr && pawn::auction::whereShopping(PPlayer, PPawn, PCounter) == CL_S_OK)
            {
                return PPawn;
            }
            return nullptr;
        }

        // Why a member's bags cannot be reached now, CL_S_OK when they can:
        // away on a venture, not standing with him, or in an event
        auto memberWhy(const CCharEntity* PPlayer, const uint32 charid) -> uint16
        {
            if (pawn::errands::viewOf(charid).has_value())
            {
                return CL_S_ON_ERRAND;
            }
            const auto* PPawn = beside(PPlayer, charid);
            if (PPawn == nullptr)
            {
                return CL_S_NOT_WITH_YOU;
            }
            return PPawn->isInEvent() ? CL_S_IN_EVENT : CL_S_OK;
        }

        // The same for the player himself: what the game's own item handlers
        // refuse him (an event, or not standing as normal) the bank refuses too
        auto selfWhy(const CCharEntity* PPlayer) -> uint16
        {
            if (PPlayer->isInEvent())
            {
                return CL_S_IN_EVENT;
            }
            return PPlayer->status == xi::Status::Normal ? CL_S_OK : CL_S_CANNOT_NOW;
        }

        auto inMogHouse(const CCharEntity* PPlayer) -> bool
        {
            return pawn::moghouse::placeOf(PPlayer).mogHouse;
        }

        // Her Mog Locker's lease, as the game reads it (charutils::
        // hasMogLockerAccess, whose zone rule the Mog House answers here)
        auto leased(const int32 expiry) -> bool
        {
            return expiry > 0 && earth_time::vanadiel_timestamp() < static_cast<uint32>(expiry);
        }

        auto gilOf(CCharEntity* PHolder) -> uint32
        {
            const auto* storage = PHolder->getStorage(LOC_INVENTORY);
            const CItem* PGil   = storage != nullptr ? storage->GetItem(0) : nullptr;
            return PGil != nullptr ? PGil->getQuantity() : 0;
        }

        // A holder's bag from a character in memory, every bag's size, and
        // her gil; inMogHouse: the player stands in his Mog House
        void fillFromMemory(cl_bank_holder& msg, CCharEntity* PHolder, const bool inMogHouse)
        {
            for (uint8 loc = 0; loc < MAX_CONTAINER_ID && loc < sizeof(msg.sizes); ++loc)
            {
                const auto* storage = PHolder->getStorage(loc);
                const bool  opens   = cardian::lsbank::judgeBag(loc, inMogHouse) == CL_S_OK &&
                                   (loc != LOC_MOGLOCKER || leased(PHolder->getCharVar("mog-locker-expiry-timestamp")));
                msg.sizes[loc] = opens && storage != nullptr ? storage->GetSize() : 0;
            }
            msg.gil             = gilOf(PHolder);
            const auto* storage = PHolder->getStorage(msg.loc);
            if (storage == nullptr || msg.sizes[msg.loc] == 0)
            {
                return;
            }
            msg.size = storage->GetSize();
            msg.free = storage->GetFreeSlotsCount();
            for (uint8 slot = 1; slot <= storage->GetSize() && msg.count < kBagItems; ++slot)
            {
                const CItem* PItem = storage->GetItem(slot);
                if (PItem != nullptr && PItem->getQuantity() > 0)
                {
                    msg.items[msg.count++] = pawn::linkapi::itemOf(PItem, slot);
                }
            }
        }

        // One holder's bag (loc) as the bank's page shows it, every bag's
        // size and her gil, and why her bags cannot be reached now, if they
        // cannot (they are then not shown)
        auto holderOf(CCharEntity* PPlayer, const uint32 charid, const uint8 kind, const uint8 loc) -> cl_bank_holder
        {
            auto msg        = make<cl_bank_holder>();
            msg.cardian     = charid;
            msg.kind        = kind;
            msg.loc         = loc < sizeof(msg.sizes) ? loc : 0;
            const bool inMH = inMogHouse(PPlayer);
            if (kind == CL_HOLDER_SELF)
            {
                setText(msg.name, PPlayer->getName());
                msg.why = selfWhy(PPlayer);
                fillFromMemory(msg, PPlayer, inMH);
                return msg;
            }
            setText(msg.name, pawn::seats::nameOf(charid));
            msg.why = memberWhy(PPlayer, charid);
            if (msg.why == CL_S_OK)
            {
                fillFromMemory(msg, beside(PPlayer, charid), inMH);
            }
            return msg;
        }

        // A move run on a holder's character as this map server holds it --
        // the player himself, or a member standing with him -- through the
        // game's own item transaction. A member whose bags cannot be reached
        // now is refused why. move(PHolder) returns the outcome; tell(PHolder)
        // is run once it has moved
        template <typename Move, typename Tell>
        auto onHolder(CCharEntity* PPlayer, const uint32 charid, const uint8 kind, Move&& move, Tell&& tell) -> uint16
        {
            if (kind == CL_HOLDER_SELF)
            {
                if (const auto why = selfWhy(PPlayer); why != CL_S_OK)
                {
                    return why;
                }
                const auto status = move(PPlayer);
                if (status == CL_S_OK)
                {
                    tell(PPlayer);
                }
                return status;
            }
            if (const auto why = memberWhy(PPlayer, charid); why != CL_S_OK)
            {
                return why;
            }
            auto*      PPawn  = beside(PPlayer, charid);
            const auto status = move(PPawn);
            if (status == CL_S_OK)
            {
                pawn::items::tidyStacks(PPawn);
                tell(PPawn);
            }
            return status;
        }

        // ---- the moves --------------------------------------------------------
        //
        // Each is one database transaction (Txn): the shell's head locked, the
        // bank read under the lock and judged against what the page saw, the
        // character's item transaction run on the same connection, the bank's
        // rows written, and the database committed before the item transaction
        // is. A refusal or a failure rolls the item transaction back while the
        // database transaction is still open, so its undo is undone with it.

        // The common start of a move, inside its transaction: the head locked,
        // and his rank's right to the move read under that lock, since the
        // holder may have changed it after judgeMove read it. The head; none
        // and why when it may not go
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

        // A stack from her bag into the bank: the slot holds what the page saw,
        // idle (not worn, not in a trade or a bazaar), and the bank has room for
        // it once merged
        auto depositFrom(CCharEntity* PHolder, const Shell& shell, const cl_bank_deposit& ask, uint8& landed) -> uint16
        {
            const auto lsid = shell.lsid;
            auto*  storage = PHolder->getStorage(ask.loc);
            CItem* PItem   = storage != nullptr && ask.slot > 0 ? storage->GetItem(ask.slot) : nullptr;
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
            Stack in;
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
            const auto taken = cardian::lsbank::deposit(readStacks(lsid), in);
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
                items->rollback();
                return CL_S_ITEM_SLIPPED_AWAY;
            }
            if (!writeStacks(lsid, taken->bank) || !writeHead(lsid, head->gil))
            {
                items->rollback();
                return CL_S_REFUSED;
            }
            if (!txn.commit())
            {
                items->rollback();
                ShowErrorFmt("lsbank: the deposit of {} x{} by {} did not commit", in.item, in.qty, PHolder->getName());
                return CL_S_REFUSED;
            }
            if (!items->commit())
            {
                ShowErrorFmt("lsbank: {}'s item transaction would not let go after the bank committed", PHolder->getName());
            }
            landed = static_cast<uint8>(taken->landed + 1);
            ShowInfoFmt("lsbank: {} banks {} x{} from bag {} (shell {}, {} of {} slots used)", PHolder->getName(), in.item, in.qty, ask.loc, lsid,
                        taken->bank.size(), cardian::lsbank::kSize);
            return CL_S_OK;
        }

        // A stack from the bank into her bag, as the game gives an item: gear
        // alone into a wardrobe, no second of a Rare item, and a free slot
        auto withdrawTo(CCharEntity* PHolder, const Shell& shell, const cl_bank_withdraw& ask, uint8& landed) -> uint16
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
            const auto before = readStacks(lsid);
            if (const auto why = cardian::lsbank::judgeWithdraw(before, ask.slot, ask.item, ask.qty); why != CL_S_OK)
            {
                return why;
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
                items->rollback();
                return CL_S_NO_SPACE;
            }
            if (!writeStacks(lsid, *after) || !writeHead(lsid, head->gil))
            {
                items->rollback();
                return CL_S_REFUSED;
            }
            if (!txn.commit())
            {
                items->rollback();
                ShowErrorFmt("lsbank: the withdrawal of {} x{} by {} did not commit", itemId, ask.qty, PHolder->getName());
                return CL_S_REFUSED;
            }
            if (!items->commit())
            {
                ShowErrorFmt("lsbank: {}'s item transaction would not let go after the bank committed", PHolder->getName());
            }
            landed = *given;
            ShowInfoFmt("lsbank: {} takes {} x{} from the bank into bag {} (shell {}, {} of {} slots used)", PHolder->getName(), itemId, ask.qty, ask.loc, lsid,
                        after->size(), cardian::lsbank::kSize);
            return CL_S_OK;
        }

        // Gil between the bank and her purse, the game's own pay and earn on
        // her side: the purse it comes from must hold it
        auto moveGil(CCharEntity* PHolder, const Shell& shell, const cl_bank_gil& ask, uint32& bankGil) -> uint16
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
            uint32 after = head->gil;
            if (ask.out != 0)
            {
                if (head->gil < ask.amount)
                {
                    return CL_S_NOT_ENOUGH_GIL;
                }
                if (!items->earn(ask.amount))
                {
                    items->rollback();
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
                    items->rollback();
                    return CL_S_NOT_ENOUGH_GIL;
                }
                after = head->gil + ask.amount;
            }
            if (!writeGil(lsid, after))
            {
                items->rollback();
                return CL_S_REFUSED;
            }
            if (!txn.commit())
            {
                items->rollback();
                ShowErrorFmt("lsbank: {} gil {} by {} did not commit", ask.amount, ask.out != 0 ? "out" : "in", PHolder->getName());
                return CL_S_REFUSED;
            }
            if (!items->commit())
            {
                ShowErrorFmt("lsbank: {}'s item transaction would not let go after the bank committed", PHolder->getName());
            }
            bankGil = after;
            ShowInfoFmt("lsbank: {} moves {} gil {} the bank (shell {}, {} gil in it)", PHolder->getName(), ask.amount, ask.out != 0 ? "out of" : "into", lsid, after);
            return CL_S_OK;
        }

        // Where a stack that landed in a character's bag is once her stacks
        // are merged: the slot it landed in, or the first holding its item
        // when the merge moved it
        auto landedNow(CCharEntity* PHolder, const uint8 loc, const uint8 landed, const uint16 itemId) -> uint8
        {
            const auto* storage = PHolder->getStorage(loc);
            if (storage == nullptr)
            {
                return landed;
            }
            if (const CItem* PItem = storage->GetItem(landed); PItem != nullptr && PItem->getID() == itemId)
            {
                return landed;
            }
            for (uint8 slot = 1; slot <= storage->GetSize(); ++slot)
            {
                if (const CItem* PItem = storage->GetItem(slot); PItem != nullptr && PItem->getID() == itemId)
                {
                    return slot;
                }
            }
            return landed;
        }

        // ---- the Link ---------------------------------------------------------

        // Why the bank cannot be used by him at all now, CL_S_OK when it can,
        // and the shell he wears
        auto judgeAsker(CCharEntity* PChar, std::optional<Shell>& shell) -> uint16
        {
            shell = shellOf(PChar);
            if (!shell.has_value())
            {
                return CL_S_NO_SHELL_WORN;
            }
            return opens(PChar) ? static_cast<uint16>(CL_S_OK) : static_cast<uint16>(CL_S_BANK_CLOSED);
        }

        // Whether his rank may do `right` (CL_BANK_*) in the shell's bank now
        auto allowed(const Shell& shell, const uint8 right) -> bool
        {
            const auto head = readHead(shell.lsid);
            return (cardian::lsbank::canOf(shell.rank, head.sackCan, head.pearlCan) & right) != 0;
        }

        void bank(CCharEntity* PChar, const cl_bank& /* ask */, Reply& reply)
        {
            std::optional<Shell> shell;
            if (const auto why = judgeAsker(PChar, shell); why != CL_S_OK)
            {
                reply.finish(make<cl_bank>(), why);
                return;
            }
            uint8 sent = 0;
            for (const auto& [charid, kind] : holdersOf(PChar))
            {
                if (sent == UINT8_MAX)
                {
                    break;
                }
                reply.more(holderOf(PChar, charid, kind, LOC_INVENTORY));
                ++sent;
            }
            auto parts = partsOf(readStacks(shell->lsid), sent, readHead(shell->lsid), *shell);
            for (std::size_t i = 0; i + 1 < parts.size(); ++i)
            {
                reply.more(parts[i]);
            }
            reply.finish(parts.back(), CL_S_OK);
        }

        void holder(CCharEntity* PChar, const cl_bank_holder& ask, Reply& reply)
        {
            std::optional<Shell> shell;
            if (const auto why = judgeAsker(PChar, shell); why != CL_S_OK)
            {
                reply.finish(ask, why);
                return;
            }
            const auto kind = kindOf(PChar, ask.cardian);
            if (!kind.has_value())
            {
                reply.finish(ask, CL_S_NOT_MANAGED);
                return;
            }
            if (const auto why = cardian::lsbank::judgeBag(ask.loc, inMogHouse(PChar)); why != CL_S_OK)
            {
                reply.finish(ask, why);
                return;
            }
            reply.finish(holderOf(PChar, ask.cardian, *kind, ask.loc), CL_S_OK);
        }

        // A move's answer once it has moved: the holder's bag and the bank as
        // they now stand, then the move itself with its outcome
        template <typename Message>
        void answerMove(CCharEntity* PChar, const Shell& shell, Message answer, const std::optional<cl_bank_holder>& told, Reply& reply, const uint16 status)
        {
            if (status == CL_S_OK)
            {
                if (told.has_value())
                {
                    reply.more(*told);
                }
                for (const auto& part : partsOf(readStacks(shell.lsid), 0, readHead(shell.lsid), shell))
                {
                    reply.more(part);
                }
            }
            reply.finish(answer, status);
        }

        // Everything a move asks before it moves anything: the bank open to
        // him, the holder his to manage, the bag one the bank reaches here,
        // and his rank's right to do it. CL_S_OK and the shell and the
        // holder's kind when it may go
        auto judgeMove(CCharEntity* PChar, const uint32 charid, const uint8 loc, const uint8 right, std::optional<Shell>& shell, std::optional<uint8>& kind) -> uint16
        {
            if (const auto why = judgeAsker(PChar, shell); why != CL_S_OK)
            {
                return why;
            }
            kind = kindOf(PChar, charid);
            if (!kind.has_value())
            {
                return CL_S_NOT_MANAGED;
            }
            if (const auto why = cardian::lsbank::judgeBag(loc, inMogHouse(PChar)); why != CL_S_OK)
            {
                return why;
            }
            // the Mog Locker opens only on a lease, as the game has it
            if (loc == LOC_MOGLOCKER)
            {
                auto* PHolder = *kind == CL_HOLDER_SELF ? PChar : beside(PChar, charid);
                if (PHolder != nullptr && !leased(PHolder->getCharVar("mog-locker-expiry-timestamp")))
                {
                    return CL_S_NO_SUCH_BAG;
                }
            }
            return allowed(*shell, right) ? static_cast<uint16>(CL_S_OK) : static_cast<uint16>(CL_S_BANK_NOT_ALLOWED);
        }

        void deposit(CCharEntity* PChar, const cl_bank_deposit& ask, Reply& reply)
        {
            auto answer   = ask;
            answer.landed = 0;
            std::optional<Shell> shell;
            std::optional<uint8> kind;
            if (const auto why = judgeMove(PChar, ask.cardian, ask.loc, CL_BANK_PUT, shell, kind); why != CL_S_OK)
            {
                reply.finish(answer, why);
                return;
            }
            std::optional<cl_bank_holder> told;
            const auto                    status = onHolder(
                PChar, ask.cardian, *kind,
                [&](CCharEntity* PHolder) { return depositFrom(PHolder, *shell, ask, answer.landed); },
                [&](CCharEntity*) { told = holderOf(PChar, ask.cardian, *kind, ask.loc); });
            answerMove(PChar, *shell, answer, told, reply, status);
        }

        void withdraw(CCharEntity* PChar, const cl_bank_withdraw& ask, Reply& reply)
        {
            auto answer   = ask;
            answer.landed = 0;
            std::optional<Shell> shell;
            std::optional<uint8> kind;
            if (const auto why = judgeMove(PChar, ask.cardian, ask.loc, CL_BANK_TAKE, shell, kind); why != CL_S_OK)
            {
                reply.finish(answer, why);
                return;
            }
            std::optional<cl_bank_holder> told;
            const auto                    status = onHolder(
                PChar, ask.cardian, *kind,
                [&](CCharEntity* PHolder) { return withdrawTo(PHolder, *shell, ask, answer.landed); },
                [&](CCharEntity* PHolder)
                {
                    answer.landed = landedNow(PHolder, ask.loc, answer.landed, ask.item);
                    told          = holderOf(PChar, ask.cardian, *kind, ask.loc);
                });
            answerMove(PChar, *shell, answer, told, reply, status);
        }

        void gil(CCharEntity* PChar, const cl_bank_gil& ask, Reply& reply)
        {
            auto                 answer = ask;
            std::optional<Shell> shell;
            std::optional<uint8> kind;
            if (const auto why = judgeMove(PChar, ask.cardian, LOC_INVENTORY, ask.out != 0 ? CL_BANK_GIL_OUT : CL_BANK_GIL_IN, shell, kind); why != CL_S_OK)
            {
                reply.finish(answer, why);
                return;
            }
            std::optional<cl_bank_holder> told;
            const auto                    status = onHolder(
                PChar, ask.cardian, *kind,
                [&](CCharEntity* PHolder) { return moveGil(PHolder, *shell, ask, answer.bankGil); },
                [&](CCharEntity* PHolder)
                {
                    answer.herGil = gilOf(PHolder);
                    const auto shown = cardian::lsbank::judgeBag(ask.loc, inMogHouse(PChar)) == CL_S_OK ? ask.loc : static_cast<uint8>(LOC_INVENTORY);
                    told             = holderOf(PChar, ask.cardian, *kind, shown);
                });
            answerMove(PChar, *shell, answer, told, reply, status);
        }

        void rights(CCharEntity* PChar, const cl_bank_rights& ask, Reply& reply)
        {
            auto                 answer = ask;
            std::optional<Shell> shell;
            if (const auto why = judgeAsker(PChar, shell); why != CL_S_OK)
            {
                reply.finish(answer, why);
                return;
            }
            if (ask.set != 0)
            {
                if (shell->rank != CL_RANK_HOLDER)
                {
                    reply.finish(answer, CL_S_HOLDER_ONLY);
                    return;
                }
                const auto sack  = static_cast<uint8>(ask.sack & cardian::lsbank::kAll);
                const auto pearl = static_cast<uint8>(ask.pearl & cardian::lsbank::kAll);
                if (!ensureHead(shell->lsid) ||
                    !db::preparedStmt("UPDATE cardian_ls_bank_head SET sack_can = ?, pearl_can = ? WHERE lsid = ?", sack, pearl, shell->lsid))
                {
                    reply.finish(answer, CL_S_REFUSED);
                    return;
                }
                ShowInfoFmt("lsbank: {} sets the bank of shell {}: sack holders {:#x}, pearl holders {:#x}", PChar->getName(), shell->lsid, sack, pearl);
            }
            const auto head = readHead(shell->lsid);
            answer.sack     = head.sackCan;
            answer.pearl    = head.pearlCan;
            reply.finish(answer, CL_S_OK);
        }
    } // namespace

    void ensureTable()
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
                         "`version` int(10) unsigned NOT NULL DEFAULT 0, "       // every move of its stacks changes it
                         "`sack_can` tinyint(3) unsigned NOT NULL DEFAULT 15, "  // CL_BANK_* bits the holder lets a sack holder
                         "`pearl_can` tinyint(3) unsigned NOT NULL DEFAULT 1, "  // ... a pearl holder
                         "PRIMARY KEY (`lsid`)) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_general_ci");
    }

    void registerHandlers()
    {
        cardian::link::handle<cl_bank>(bank);
        cardian::link::handle<cl_bank_holder>(holder);
        cardian::link::handle<cl_bank_deposit>(deposit);
        cardian::link::handle<cl_bank_withdraw>(withdraw);
        cardian::link::handle<cl_bank_gil>(gil);
        cardian::link::handle<cl_bank_rights>(rights);
    }

    auto opens(const CCharEntity* PPlayer) -> bool
    {
        if (PPlayer == nullptr || PPlayer->loc.zone == nullptr)
        {
            return false;
        }
        if (pawn::auction::counterNear(PPlayer) != nullptr)
        {
            return true;
        }
        const auto place = pawn::moghouse::placeOf(PPlayer);
        return place.mogHouse || place.moogle != nullptr;
    }

    auto shellOf(CCharEntity* PChar) -> std::optional<Shell>
    {
        if (PChar == nullptr)
        {
            return std::nullopt;
        }
        for (const auto slot : { SLOT_LINK1, SLOT_LINK2 })
        {
            auto* PItem = dynamic_cast<CItemLinkshell*>(reinterpret_cast<CItem*>(PChar->getEquip(slot)));
            if (PItem == nullptr || PItem->GetLSID() == 0)
            {
                continue;
            }
            uint8 rank = CL_RANK_NONE;
            switch (PItem->GetLSType())
            {
                case LSTYPE_LINKSHELL:
                    rank = CL_RANK_HOLDER;
                    break;
                case LSTYPE_PEARLSACK:
                    rank = CL_RANK_SACK;
                    break;
                case LSTYPE_LINKPEARL:
                    rank = CL_RANK_PEARL;
                    break;
                default:
                    continue; // a new linkshell not yet made, a broken pearl
            }
            return Shell{ .lsid = PItem->GetLSID(), .rank = rank, .name = PItem->getSignature(), .color = PItem->GetLSRawColor() };
        }
        return std::nullopt;
    }

    void fillClub(cl_club& club, CCharEntity* PPlayer)
    {
        const auto shell = shellOf(PPlayer);
        if (!shell.has_value())
        {
            return;
        }
        setText(club.shellName, shell->name);
        club.shellColor = shell->color;
        club.rank       = shell->rank;
        if (const auto rset = db::preparedStmt("SELECT COUNT(*) AS used FROM cardian_ls_bank WHERE lsid = ?", shell->lsid); rset && rset->next())
        {
            club.bankUsed = static_cast<uint8_t>(std::min<uint32>(rset->get<uint32>("used"), CL_BANK_SIZE));
        }
        club.bankGil = readHead(shell->lsid).gil;
    }
} // namespace pawn::lsbank
