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

#include "ah_sell.h"

#include "cardian_link_protocol.h"
#include "item_moves.h"

#include "common/database.h"
#include "common/earth_time.h"
#include "common/logging.h"
#include "common/settings.h"
#include "entities/char_entity.h"
#include "enums/item_flag.h"
#include "enums/item_state.h"
#include "item_container.h"
#include "items/item.h"
#include "items/item_usable.h"
#include "items/transaction.h"
#include "items/transactions/item_claim.h"
#include "packets/s2c/0x01d_item_same.h"
#include "utils/charutils.h"
#include "utils/itemutils.h"

#include <algorithm>
#include <map>
#include <optional>
#include <utility>
#include <vector>

namespace pawn::ahsell
{
    namespace
    {
        using itemmoves::Slot;

        constexpr uint32 kMaxPrice = 999999999;

        // An enchanted item with charges spent, which the game's own listing
        // refuses (auctionutils' isPartiallyUsed)
        auto partlyUsed(CItem* PItem) -> bool
        {
            if (PItem->isSubType(ITEM_CHARGED))
            {
                const auto* PCharged = static_cast<CItemUsable*>(PItem);
                return PCharged->getCurrentCharges() < PCharged->getMaxCharges();
            }
            return false;
        }

        auto gilOf(CCharEntity* PChar) -> uint32
        {
            const auto* storage = PChar->getStorage(LOC_INVENTORY);
            const CItem* PGil   = storage != nullptr ? storage->GetItem(0) : nullptr;
            return PGil != nullptr ? PGil->getQuantity() : 0;
        }

        // The listing's work on the characters, across two of them, as the
        // shared purchase's (pawn/auction.cpp): his fee paid, her piece taken
        class Listing final : public Transaction
        {
        public:
            ~Listing() override
            {
                this->rollbackIfOpen();
            }

            auto settle(CCharEntity* PSeller, const uint32 fee, CCharEntity* PHolder, const uint8 loc, const uint8 slot, const uint32 quantity) -> bool
            {
                return (fee == 0 || this->pay(PSeller, fee)) && this->take(PHolder, loc, slot, quantity);
            }

            // a new price's fee alone: the listing keeps its piece
            auto charge(CCharEntity* PSeller, const uint32 fee) -> bool
            {
                return fee == 0 || this->pay(PSeller, fee);
            }

        protected:
            // pay and take above already applied and recorded the work
            auto doCommit() -> bool override
            {
                return true;
            }

            void doRollback() override
            {
            }
        };
    } // namespace

    auto feeOf(const uint32 price, const bool stack) -> uint32
    {
        // the game's own listing's arithmetic, float and all
        const auto fee = stack ? static_cast<uint32>(settings::get<uint32>("map.AH_BASE_FEE_STACKS") + (price * settings::get<float>("map.AH_TAX_RATE_STACKS") / 100))
                               : static_cast<uint32>(settings::get<uint32>("map.AH_BASE_FEE_SINGLE") + (price * settings::get<float>("map.AH_TAX_RATE_SINGLE") / 100));
        return std::clamp<uint32>(fee, 0, settings::get<uint32>("map.AH_MAX_FEE"));
    }

    auto openCount(const uint32 charid) -> uint32
    {
        const auto rset = db::preparedStmt("SELECT COUNT(*) AS open FROM auction_house WHERE seller = ? AND sale = 0", charid);
        return rset && rset->next() ? rset->get<uint32>("open") : 0;
    }

    auto sellsFrom(const uint8 loc) -> bool
    {
        return loc == LOC_INVENTORY || loc == LOC_MOGSATCHEL || loc == LOC_MOGSACK || loc == LOC_MOGCASE || loc == LOC_WARDROBE ||
               (loc >= LOC_WARDROBE2 && loc <= LOC_WARDROBE8);
    }

    auto judge(CCharEntity* PSeller, const uint16 itemId, const bool stack, const uint32 price) -> uint16
    {
        if (price == 0 || price > kMaxPrice)
        {
            return CL_S_BAD_PRICE;
        }
        const CItem* PKind = xi::items::lookup(itemId);
        if (PKind == nullptr)
        {
            return CL_S_NO_SUCH_ITEM;
        }
        if (PKind->hasFlag(ItemFlag::NoAuction) || PKind->getAHCat() == 0)
        {
            return CL_S_NOT_AUCTIONABLE;
        }
        if (stack && PKind->getStackSize() <= 1)
        {
            return CL_S_NOT_IN_STACKS;
        }
        const auto limit = settings::get<uint8>("map.AH_LIST_LIMIT");
        if (limit != 0 && openCount(PSeller->id) >= limit)
        {
            return CL_S_LISTING_LIMIT;
        }
        return gilOf(PSeller) < feeOf(price, stack) ? static_cast<uint16>(CL_S_NOT_ENOUGH_GIL) : static_cast<uint16>(CL_S_OK);
    }

    auto list(CCharEntity* PSeller, CCharEntity* PHolder, const uint8 loc, const uint8 slot, const uint16 itemId, const bool stack, const uint32 price) -> Listed
    {
        if (PSeller == nullptr || PHolder == nullptr)
        {
            return { CL_S_REFUSED };
        }
        if (!sellsFrom(loc))
        {
            return { CL_S_NO_SUCH_BAG };
        }
        if (const auto why = judge(PSeller, itemId, stack, price); why != CL_S_OK)
        {
            return { why };
        }
        auto*  storage = PHolder->getStorage(loc);
        CItem* PItem   = storage != nullptr && slot >= 1 && slot <= storage->GetSize() ? storage->GetItem(slot) : nullptr;
        if (PItem == nullptr)
        {
            return { CL_S_NO_ITEM };
        }
        if (PItem->getID() != itemId)
        {
            return { CL_S_ITEM_SLIPPED_AWAY };
        }
        if (PItem->state() == ItemState::Equipped)
        {
            return { CL_S_ITEM_EQUIPPED };
        }
        if (PItem->isBusy())
        {
            return { CL_S_ITEM_BUSY };
        }
        if (partlyUsed(PItem))
        {
            return { CL_S_PARTLY_USED };
        }
        if (stack && PItem->getQuantity() != PItem->getStackSize())
        {
            return { CL_S_NOT_A_FULL_STACK };
        }

        const auto fee      = feeOf(price, stack);
        const auto quantity = stack ? PItem->getStackSize() : 1u;
        // a piece taken whole leaves her row to be put back if the move fails
        const auto whole   = quantity == PItem->getQuantity() ? std::optional<Slot>(Slot{ PHolder, loc, slot }) : std::nullopt;
        const auto touched = std::vector<Slot>{ Slot{ PHolder, loc, slot }, Slot{ PSeller, LOC_INVENTORY, 0 } };
        const auto what    = fmt::format("{}'s listing of item {} x{} from {} at {}", PSeller->getName(), itemId, quantity, PHolder->getName(), price);

        itemmoves::Txn txn;
        if (!txn.ok())
        {
            return { CL_S_REFUSED };
        }
        Listing items;
        if (!items.settle(PSeller, fee, PHolder, loc, slot, quantity))
        {
            itemmoves::abandon(txn, items, whole, touched);
            return { CL_S_REFUSED };
        }
        uint32 listing = 0;
        if (db::preparedStmt("INSERT INTO auction_house(itemid, stack, seller, seller_name, date, price) VALUES(?, ?, ?, ?, ?, ?)", itemId, stack ? 1 : 0,
                             PSeller->id, PSeller->getName(), earth_time::timestamp(), price))
        {
            if (const auto rset = db::preparedStmt("SELECT LAST_INSERT_ID() AS id"); rset && rset->next())
            {
                listing = rset->get<uint32>("id");
            }
        }
        const auto mark = itemmoves::newMark();
        if (listing == 0 || !itemmoves::writeMark(mark))
        {
            ShowWarningFmt("ahsell: {} could not be written: put back", what);
            itemmoves::abandon(txn, items, whole, touched);
            return { CL_S_REFUSED };
        }
        if (!itemmoves::commitMove(txn, items, mark, whole, touched, what))
        {
            return { CL_S_REFUSED };
        }
        return { CL_S_OK, fee, listing };
    }

    auto listingsOf(const uint32 charid, const std::size_t most, std::size_t& open) -> std::vector<Mine>
    {
        std::vector<Mine> out;
        open = 0;
        if (const auto rset = db::preparedStmt("SELECT id, itemid, stack, price, date FROM auction_house WHERE seller = ? AND sale = 0 ORDER BY id ASC", charid))
        {
            while (rset->next())
            {
                ++open;
                if (out.size() < most)
                {
                    out.push_back(Mine{ rset->get<uint32>("id"), rset->get<uint32>("price"), rset->get<uint32>("date"), rset->get<uint16>("itemid"), rset->get<uint8>("stack") != 0, false });
                }
            }
        }

        // each sale's gil waiting in his delivery box, by item and price (the
        // auction house's trigger writes one row of gil a sale, from 'AH-Jeuno')
        std::map<std::pair<uint16, uint32>, uint32> waiting;
        if (const auto rset = db::preparedStmt("SELECT itemsubid, quantity FROM delivery_box WHERE charid = ? AND box = 1 AND itemid = 65535 AND sender = 'AH-Jeuno'", charid))
        {
            while (rset->next())
            {
                ++waiting[{ rset->get<uint16>("itemsubid"), rset->get<uint32>("quantity") }];
            }
        }
        if (waiting.empty() || out.size() >= most)
        {
            return out;
        }
        if (const auto rset = db::preparedStmt("SELECT id, itemid, stack, sale, sell_date FROM auction_house WHERE seller = ? AND sale != 0 ORDER BY sell_date DESC, id DESC LIMIT 500", charid))
        {
            while (rset->next() && out.size() < most)
            {
                const auto it = waiting.find({ rset->get<uint16>("itemid"), rset->get<uint32>("sale") });
                if (it == waiting.end() || it->second == 0)
                {
                    continue;
                }
                --it->second;
                out.push_back(Mine{ rset->get<uint32>("id"), rset->get<uint32>("sale"), rset->get<uint32>("sell_date"), rset->get<uint16>("itemid"), rset->get<uint8>("stack") != 0, true });
            }
        }
        return out;
    }

    auto cancel(CCharEntity* PSeller, const uint32 listing, const uint16 itemId) -> Cancelled
    {
        uint16 listed = 0;
        bool   stack  = false;
        {
            const auto rset = db::preparedStmt("SELECT itemid, stack FROM auction_house WHERE id = ? AND seller = ? AND sale = 0", listing, PSeller->id);
            if (!rset || !rset->next())
            {
                return { CL_S_LISTING_GONE };
            }
            listed = rset->get<uint16>("itemid");
            stack  = rset->get<uint8>("stack") != 0;
        }
        if (listed != itemId)
        {
            return { CL_S_LISTING_GONE };
        }
        const CItem* PKind = xi::items::lookup(listed);
        if (PKind == nullptr)
        {
            return { CL_S_NO_SUCH_ITEM };
        }
        if (PSeller->getStorage(LOC_INVENTORY)->GetFreeSlotsCount() == 0)
        {
            return { CL_S_NO_SPACE };
        }
        if (PKind->hasFlag(ItemFlag::Rare) && charutils::HasItem(PSeller, listed))
        {
            return { CL_S_RARE_OWNED };
        }

        const auto what = fmt::format("{}'s take down of listing {} (item {})", PSeller->getName(), listing, listed);
        itemmoves::Txn txn;
        if (!txn.ok())
        {
            return { CL_S_REFUSED };
        }
        const auto gone = db::preparedStmt("DELETE FROM auction_house WHERE id = ? AND seller = ? AND sale = 0", listing, PSeller->id);
        if (gone == nullptr)
        {
            return { CL_S_REFUSED };
        }
        if (gone->rowsAffected() == 0)
        {
            return { CL_S_LISTING_GONE }; // sold, or taken back, since it was read
        }
        auto items = ItemClaimTransaction::start(PSeller);
        if (items == nullptr)
        {
            return { CL_S_REFUSED };
        }
        const auto landed = items->give(LOC_INVENTORY, listed, stack ? PKind->getStackSize() : 1, Silence::Yes);
        if (!landed.has_value())
        {
            itemmoves::abandon(txn, *items, std::nullopt, {});
            return { CL_S_NO_SPACE };
        }
        const auto touched = std::vector<Slot>{ Slot{ PSeller, LOC_INVENTORY, *landed } };
        const auto mark    = itemmoves::newMark();
        if (!itemmoves::writeMark(mark))
        {
            ShowWarningFmt("ahsell: {} could not be written: put back", what);
            itemmoves::abandon(txn, *items, std::nullopt, touched);
            return { CL_S_REFUSED };
        }
        if (!itemmoves::commitMove(txn, *items, mark, std::nullopt, touched, what))
        {
            return { CL_S_REFUSED };
        }
        PSeller->pushPacket<GP_SERV_COMMAND_ITEM_SAME>(PSeller);
        return { CL_S_OK, *landed };
    }

    auto reprice(CCharEntity* PSeller, const uint32 listing, const uint16 itemId, const uint32 price) -> Listed
    {
        if (price == 0 || price > kMaxPrice)
        {
            return { CL_S_BAD_PRICE };
        }
        uint16 listed = 0;
        bool   stack  = false;
        uint32 was    = 0;
        {
            const auto rset = db::preparedStmt("SELECT itemid, stack, price FROM auction_house WHERE id = ? AND seller = ? AND sale = 0", listing, PSeller->id);
            if (!rset || !rset->next())
            {
                return { CL_S_LISTING_GONE };
            }
            listed = rset->get<uint16>("itemid");
            stack  = rset->get<uint8>("stack") != 0;
            was    = rset->get<uint32>("price");
        }
        if (listed != itemId)
        {
            return { CL_S_LISTING_GONE };
        }
        if (price == was)
        {
            return { CL_S_OK, 0, listing };
        }
        const auto fee = feeOf(price, stack);
        if (gilOf(PSeller) < fee)
        {
            return { CL_S_NOT_ENOUGH_GIL };
        }

        const auto touched = std::vector<Slot>{ Slot{ PSeller, LOC_INVENTORY, 0 } };
        const auto what    = fmt::format("{}'s new price of {} for listing {}", PSeller->getName(), price, listing);
        itemmoves::Txn txn;
        if (!txn.ok())
        {
            return { CL_S_REFUSED };
        }
        const auto moved = db::preparedStmt("UPDATE auction_house SET price = ?, date = ? WHERE id = ? AND seller = ? AND sale = 0", price, earth_time::timestamp(),
                                            listing, PSeller->id);
        if (moved == nullptr)
        {
            return { CL_S_REFUSED };
        }
        if (moved->rowsAffected() != 1)
        {
            return { CL_S_LISTING_GONE }; // sold since it was read
        }
        Listing items;
        if (!items.charge(PSeller, fee))
        {
            itemmoves::abandon(txn, items, std::nullopt, touched);
            return { CL_S_NOT_ENOUGH_GIL };
        }
        const auto mark = itemmoves::newMark();
        if (!itemmoves::writeMark(mark))
        {
            ShowWarningFmt("ahsell: {} could not be written: put back", what);
            itemmoves::abandon(txn, items, std::nullopt, touched);
            return { CL_S_REFUSED };
        }
        if (!itemmoves::commitMove(txn, items, mark, std::nullopt, touched, what))
        {
            return { CL_S_REFUSED };
        }
        return { CL_S_OK, fee, listing };
    }
} // namespace pawn::ahsell
