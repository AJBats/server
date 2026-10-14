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
#include "bodies.h"
#include "cardian_link.h"
#include "cardian_link_messages.h"
#include "club.h"
#include "errands.h"
#include "link_api.h"
#include "ls_bank_math.h"
#include "mog_house.h"
#include "pawn.h"
#include "pawn_items.h"
#include "players.h"
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
#include "utils/charutils.h"
#include "utils/itemutils.h"
#include "utils/zoneutils.h"

#include <algorithm>
#include <cstring>
#include <memory>
#include <optional>
#include <set>
#include <stdexcept>
#include <utility>
#include <vector>

namespace pawn::lsbank
{
    namespace
    {
        using namespace cardian::link;
        using cardian::lsbank::Extra;
        using cardian::lsbank::Stack;

        constexpr std::size_t kPart = sizeof(cl_bank::items) / sizeof(cl_item);

        // ---- the bank's stacks ------------------------------------------------

        // How many of an item one slot holds
        auto stackSizeOf(const uint16 itemId) -> uint32
        {
            const CItem* PItem = xi::items::lookup(itemId);
            return PItem != nullptr ? std::max<uint32>(1, PItem->getStackSize()) : 1;
        }

        // The account's bank as its rows hold it, in its order
        auto readBank(const uint32 accid) -> std::vector<Stack>
        {
            std::vector<Stack> bank;
            const auto rset = db::preparedStmt("SELECT itemId, quantity, extra FROM cardian_bank WHERE accid = ? ORDER BY slot", accid);
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

        // The account's bank written whole, a row a stack in its order: all
        // of it or, on any failure, none of it
        auto writeBank(const uint32 accid, const std::vector<Stack>& bank) -> bool
        {
            const bool written = db::transaction(
                [&]()
                {
                    if (!db::preparedStmt("DELETE FROM cardian_bank WHERE accid = ?", accid))
                    {
                        throw std::runtime_error("the bank's rows could not be cleared");
                    }
                    uint8 slot = 0;
                    for (const auto& stack : bank)
                    {
                        ++slot;
                        Extra extra = stack.extra; // the binding reads a blob from a mutable value
                        if (!db::preparedStmt("INSERT INTO cardian_bank (accid, slot, itemId, quantity, extra) VALUES (?, ?, ?, ?, ?)",
                                              accid, slot, stack.item, stack.qty, extra))
                        {
                            throw std::runtime_error("a stack of the bank could not be written");
                        }
                    }
                });
            if (!written)
            {
                ShowErrorFmt("lsbank: the bank of account {} could not be written", accid);
            }
            return written;
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

        // The bank's stacks as BANK parts of up to kPart, at least one
        auto partsOf(const std::vector<Stack>& bank, const uint8 holders) -> std::vector<cl_bank>
        {
            std::vector<cl_bank> parts;
            std::size_t          at = 0;
            do
            {
                auto part    = make<cl_bank>();
                part.holders = holders;
                part.used    = static_cast<uint8_t>(bank.size());
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
        // account or a cardian it owns; none for anyone else
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

        // A member's character as the map holds it now: standing as a
        // cardian, held in no zone for a venture, or played; none when she is
        // out of the world
        auto inMemory(const uint32 charid) -> CCharEntity*
        {
            if (auto* PPawn = pawn::findPawn(charid); PPawn != nullptr)
            {
                return PPawn;
            }
            if (auto* PBody = dynamic_cast<CCharEntity*>(pawn::bodies::find(charid)); PBody != nullptr)
            {
                return PBody;
            }
            return zoneutils::GetChar(charid);
        }

        // Why a member's inventory cannot be reached now, CL_S_OK when it can:
        // away on a venture, held in no zone by one, played by a client, or
        // between zones a moment
        auto memberWhy(const uint32 charid) -> uint16
        {
            if (pawn::errands::viewOf(charid).has_value())
            {
                return CL_S_ON_ERRAND;
            }
            if (pawn::bodies::find(charid) != nullptr)
            {
                return CL_S_CANNOT_NOW;
            }
            if (auto* PPawn = pawn::findPawn(charid); PPawn != nullptr)
            {
                return PPawn->isInEvent() ? CL_S_IN_EVENT : CL_S_OK;
            }
            if (zoneutils::GetChar(charid) != nullptr || pawn::players::online(charid))
            {
                return CL_S_CANNOT_NOW;
            }
            return CL_S_OK;
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

        // A holder's inventory from a character in memory
        void fillFromMemory(cl_bank_holder& msg, CCharEntity* PHolder)
        {
            const auto* storage = PHolder->getStorage(LOC_INVENTORY);
            if (storage == nullptr)
            {
                return;
            }
            msg.size = storage->GetSize();
            msg.free = storage->GetFreeSlotsCount();
            constexpr std::size_t capacity = sizeof(cl_bank_holder::items) / sizeof(cl_item);
            for (uint8 slot = 1; slot <= storage->GetSize() && msg.count < capacity; ++slot)
            {
                const CItem* PItem = storage->GetItem(slot);
                if (PItem != nullptr && PItem->getQuantity() > 0)
                {
                    msg.items[msg.count++] = pawn::linkapi::itemOf(PItem, slot);
                }
            }
        }

        // A holder's inventory from her rows, for one out of the world: what
        // a load of her would show, read without loading her
        void fillFromRows(cl_bank_holder& msg, const uint32 charid)
        {
            uint8 size = 0;
            if (const auto rset = db::preparedStmt("SELECT inventory FROM char_storage WHERE charid = ?", charid); rset && rset->next())
            {
                size = rset->get<uint8>("inventory");
            }
            std::set<uint8> worn;
            if (const auto rset = db::preparedStmt("SELECT slotid FROM char_equip WHERE charid = ? AND containerid = ?", charid, static_cast<uint8>(LOC_INVENTORY)))
            {
                while (rset->next())
                {
                    worn.insert(rset->get<uint8>("slotid"));
                }
            }
            constexpr std::size_t capacity = sizeof(cl_bank_holder::items) / sizeof(cl_item);
            const auto            rset     = db::preparedStmt("SELECT slot, itemId, quantity, extra FROM char_inventory "
                                                              "WHERE charid = ? AND location = ? AND slot > 0 ORDER BY slot",
                                                              charid, static_cast<uint8>(LOC_INVENTORY));
            uint8 used = 0;
            while (rset && rset->next() && msg.count < capacity)
            {
                Extra extra{};
                db::extractFromBlob(rset, "extra", extra);
                const auto slot = rset->get<uint8>("slot");
                auto       item = listed(Stack{ rset->get<uint16>("itemId"), rset->get<uint32>("quantity"), extra, 1 }, slot);
                if (worn.contains(slot))
                {
                    item.flags = static_cast<uint8_t>(item.flags | CL_ITEM_EQUIPPED);
                }
                msg.items[msg.count++] = item;
                ++used;
            }
            msg.size = size;
            msg.free = size > used ? static_cast<uint8_t>(size - used) : 0;
        }

        // One holder as the bank's page shows her: her inventory as it stands,
        // and why it cannot be reached now, if it cannot. PLoaded: her
        // character as a move that has just reached it holds it (one loaded
        // for the move among them), so nothing stands in her way
        auto holderOf(CCharEntity* PPlayer, const uint32 charid, const uint8 kind, CCharEntity* PLoaded = nullptr) -> cl_bank_holder
        {
            auto msg    = make<cl_bank_holder>();
            msg.cardian = charid;
            msg.kind    = kind;
            if (kind == CL_HOLDER_SELF)
            {
                setText(msg.name, PPlayer->getName());
                msg.why = selfWhy(PPlayer);
                fillFromMemory(msg, PPlayer);
                return msg;
            }
            setText(msg.name, pawn::seats::nameOf(charid));
            msg.why = PLoaded != nullptr ? static_cast<uint16>(CL_S_OK) : memberWhy(charid);
            if (auto* PHolder = PLoaded != nullptr ? PLoaded : inMemory(charid); PHolder != nullptr)
            {
                fillFromMemory(msg, PHolder);
            }
            else
            {
                fillFromRows(msg, charid);
            }
            return msg;
        }

        // A move run on a holder's character: the player himself; a member
        // standing as a cardian; or one out of the world, loaded for it a
        // moment and let go after -- her rows are written by the item
        // transactions themselves, and nothing else of hers changes. A
        // member whose inventory cannot be reached now is refused why.
        // move(PHolder) returns the outcome; tell(PHolder) is run on the
        // character still in memory once it has moved
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
            if (const auto why = memberWhy(charid); why != CL_S_OK)
            {
                return why;
            }
            if (auto* PPawn = pawn::findPawn(charid); PPawn != nullptr)
            {
                const auto status = move(PPawn);
                if (status == CL_S_OK)
                {
                    pawn::items::tidyStacks(PPawn);
                    tell(PPawn);
                }
                return status;
            }
            auto borrowed = charutils::LoadChar(charid);
            if (borrowed == nullptr)
            {
                ShowErrorFmt("lsbank: LoadChar failed for {}", charid);
                return CL_S_CANNOT_NOW;
            }
            borrowed->clearPacketList();
            // held while it is loaded, so the item transaction finds its owner again
            pawn::bodies::hold(charid, borrowed.get());
            const auto status = move(borrowed.get());
            if (status == CL_S_OK)
            {
                pawn::items::tidyStacks(borrowed.get());
                tell(borrowed.get());
            }
            borrowed->clearPacketList();
            pawn::bodies::letGo(charid);
            return status;
        }

        // ---- the moves --------------------------------------------------------

        // A stack from her inventory into the bank: the slot holds what the
        // page saw, idle (not worn, not in a trade or a bazaar), and the bank
        // has room for it once merged
        auto depositFrom(CCharEntity* PHolder, const uint32 accid, const cl_bank_deposit& ask, uint8& landed) -> uint16
        {
            auto*  storage = PHolder->getStorage(LOC_INVENTORY);
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

            Stack in;
            in.item  = PItem->getID();
            in.qty   = ask.qty;
            in.stack = std::max<uint32>(1, PItem->getStackSize());
            std::memcpy(in.extra.data(), PItem->m_extra, std::min<std::size_t>(in.extra.size(), CItem::extra_size));

            const auto before = readBank(accid);
            const auto taken  = cardian::lsbank::deposit(before, in);
            if (!taken.has_value())
            {
                return CL_S_BANK_FULL;
            }

            auto transaction = ItemClaimTransaction::start(PHolder);
            if (transaction == nullptr || transaction->claimSlot(LOC_INVENTORY, ask.slot) == nullptr)
            {
                return CL_S_ITEM_BUSY;
            }
            if (!transaction->take(LOC_INVENTORY, ask.slot, ask.qty))
            {
                transaction->rollback();
                return CL_S_ITEM_SLIPPED_AWAY;
            }
            if (!writeBank(accid, taken->bank))
            {
                transaction->rollback();
                return CL_S_REFUSED;
            }
            if (!transaction->commit())
            {
                transaction->rollback();
                writeBank(accid, before);
                return CL_S_REFUSED;
            }
            landed = static_cast<uint8>(taken->landed + 1);
            ShowInfoFmt("lsbank: {} banks {} x{} (account {}, {} of {} slots used)", PHolder->getName(), in.item, in.qty, accid, taken->bank.size(), cardian::lsbank::kSize);
            return CL_S_OK;
        }

        // A stack from the bank into her inventory, as the game gives an
        // item: no second of a Rare item, and a free slot for it
        auto withdrawTo(CCharEntity* PHolder, const uint32 accid, const cl_bank_withdraw& ask, uint8& landed) -> uint16
        {
            const auto before = readBank(accid);
            if (const auto why = cardian::lsbank::judgeWithdraw(before, ask.slot, ask.item, ask.qty); why != CL_S_OK)
            {
                return why;
            }
            const auto& stack = before[ask.slot - 1];
            auto        PItem = rebuild(stack.item, ask.qty, stack.extra);
            if (PItem == nullptr)
            {
                ShowErrorFmt("lsbank: item {} in the bank of account {} is no item the game knows", stack.item, accid);
                return CL_S_ITEM_CANNOT_MOVE;
            }
            const auto* storage = PHolder->getStorage(LOC_INVENTORY);
            const bool  rare    = PItem->hasFlag(ItemFlag::Rare);
            if (const auto why = cardian::lsbank::judgeReceive(rare, rare && charutils::HasItem(PHolder, stack.item), storage != nullptr ? storage->GetFreeSlotsCount() : 0);
                why != CL_S_OK)
            {
                return why;
            }
            const auto after = cardian::lsbank::withdraw(before, ask.slot, ask.qty);
            if (!after.has_value())
            {
                return CL_S_BAD_QUANTITY;
            }

            const uint16 itemId      = stack.item;
            const uint32 qty         = ask.qty;
            auto         transaction = ItemClaimTransaction::start(PHolder);
            if (transaction == nullptr)
            {
                return CL_S_REFUSED;
            }
            const auto given = transaction->give(LOC_INVENTORY, std::move(PItem), Silence::Yes);
            if (!given.has_value())
            {
                transaction->rollback();
                return CL_S_NO_SPACE;
            }
            if (!writeBank(accid, *after))
            {
                transaction->rollback();
                return CL_S_REFUSED;
            }
            if (!transaction->commit())
            {
                transaction->rollback();
                writeBank(accid, before);
                return CL_S_REFUSED;
            }
            landed = *given;
            ShowInfoFmt("lsbank: {} takes {} x{} from the bank (account {}, {} of {} slots used)", PHolder->getName(), itemId, qty, accid, after->size(), cardian::lsbank::kSize);
            return CL_S_OK;
        }

        // Where a stack that landed in a cardian's inventory is once her
        // stacks are merged: the slot it landed in, or the first holding its
        // item when the merge moved it
        auto landedNow(CCharEntity* PHolder, const uint8 landed, const uint16 itemId) -> uint8
        {
            const auto* storage = PHolder->getStorage(LOC_INVENTORY);
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

        void bank(CCharEntity* PChar, const cl_bank& /* ask */, Reply& reply)
        {
            if (!opens(PChar))
            {
                reply.finish(make<cl_bank>(), CL_S_BANK_CLOSED);
                return;
            }
            uint8 sent = 0;
            for (const auto& [charid, kind] : holdersOf(PChar))
            {
                if (sent == UINT8_MAX)
                {
                    break;
                }
                reply.more(holderOf(PChar, charid, kind));
                ++sent;
            }
            auto parts = partsOf(readBank(pawn::ownerAccountOf(PChar)), sent);
            for (std::size_t i = 0; i + 1 < parts.size(); ++i)
            {
                reply.more(parts[i]);
            }
            reply.finish(parts.back(), CL_S_OK);
        }

        void holder(CCharEntity* PChar, const cl_bank_holder& ask, Reply& reply)
        {
            if (!opens(PChar))
            {
                reply.finish(ask, CL_S_BANK_CLOSED);
                return;
            }
            const auto kind = kindOf(PChar, ask.cardian);
            if (!kind.has_value())
            {
                reply.finish(ask, CL_S_NOT_MANAGED);
                return;
            }
            reply.finish(holderOf(PChar, ask.cardian, *kind), CL_S_OK);
        }

        // A move's answer once it has moved: the holder and the bank as they
        // now stand, then the move itself with its outcome
        template <typename Message>
        void answerMove(CCharEntity* PChar, Message answer, const std::optional<cl_bank_holder>& told, Reply& reply, const uint16 status)
        {
            if (status == CL_S_OK)
            {
                if (told.has_value())
                {
                    reply.more(*told);
                }
                for (const auto& part : partsOf(readBank(pawn::ownerAccountOf(PChar)), 0))
                {
                    reply.more(part);
                }
            }
            reply.finish(answer, status);
        }

        void deposit(CCharEntity* PChar, const cl_bank_deposit& ask, Reply& reply)
        {
            auto answer   = ask;
            answer.landed = 0;
            if (!opens(PChar))
            {
                reply.finish(answer, CL_S_BANK_CLOSED);
                return;
            }
            const auto kind = kindOf(PChar, ask.cardian);
            if (!kind.has_value())
            {
                reply.finish(answer, CL_S_NOT_MANAGED);
                return;
            }
            const uint32                  accid = pawn::ownerAccountOf(PChar);
            std::optional<cl_bank_holder> told;
            const auto                    status = onHolder(
                PChar, ask.cardian, *kind,
                [&](CCharEntity* PHolder) { return depositFrom(PHolder, accid, ask, answer.landed); },
                [&](CCharEntity* PHolder) { told = holderOf(PChar, ask.cardian, *kind, PHolder); });
            answerMove(PChar, answer, told, reply, status);
        }

        void withdraw(CCharEntity* PChar, const cl_bank_withdraw& ask, Reply& reply)
        {
            auto answer   = ask;
            answer.landed = 0;
            if (!opens(PChar))
            {
                reply.finish(answer, CL_S_BANK_CLOSED);
                return;
            }
            const auto kind = kindOf(PChar, ask.cardian);
            if (!kind.has_value())
            {
                reply.finish(answer, CL_S_NOT_MANAGED);
                return;
            }
            const uint32                  accid = pawn::ownerAccountOf(PChar);
            std::optional<cl_bank_holder> told;
            const auto                    status = onHolder(
                PChar, ask.cardian, *kind,
                [&](CCharEntity* PHolder) { return withdrawTo(PHolder, accid, ask, answer.landed); },
                [&](CCharEntity* PHolder)
                {
                    answer.landed = landedNow(PHolder, answer.landed, ask.item);
                    told          = holderOf(PChar, ask.cardian, *kind, PHolder);
                });
            answerMove(PChar, answer, told, reply, status);
        }
    } // namespace

    void ensureTable()
    {
        db::preparedStmt("CREATE TABLE IF NOT EXISTS `cardian_bank` ("
                         "`accid` int(10) unsigned NOT NULL, "
                         "`slot` tinyint(3) unsigned NOT NULL, " // 1 to CL_BANK_SIZE, the bank's order
                         "`itemId` smallint(5) unsigned NOT NULL, "
                         "`quantity` int(10) unsigned NOT NULL DEFAULT 1, "
                         "`extra` blob(24) DEFAULT NULL, " // the item's extra data, as char_inventory's
                         "PRIMARY KEY (`accid`, `slot`)) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_general_ci");
    }

    void registerHandlers()
    {
        cardian::link::handle<cl_bank>(bank);
        cardian::link::handle<cl_bank_holder>(holder);
        cardian::link::handle<cl_bank_deposit>(deposit);
        cardian::link::handle<cl_bank_withdraw>(withdraw);
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
} // namespace pawn::lsbank
