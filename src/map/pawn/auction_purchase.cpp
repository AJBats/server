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

// A cardian's purchase on the auction house with the player's purse behind
// hers (auction.h purchaseShared). Apart from the pawn module's other
// auction work so xi_test can link it (item_moves.h's rule: the game's code
// and header-only Cardian files only), and drive it through failed moves
// (scripts/tests/cardian/ah_buy.lua).

#include "auction.h"
#include "item_moves.h"

#include "common/database.h"
#include "common/earth_time.h"
#include "common/logging.h"
#include "entities/char_entity.h"
#include "item_container.h"
#include "items/transaction.h"

#include <optional>
#include <vector>

namespace pawn::auction
{
    namespace
    {
        using itemmoves::Slot;

        // The shared purchase's work on the characters, across both, as
        // pawn_items' CardianTransfer moves gil between two: her share and
        // his paid, her piece given
        class SharedPurchase final : public Transaction
        {
        public:
            ~SharedPurchase() override
            {
                this->rollbackIfOpen();
            }

            // The inventory slot her piece landed in; none when a step was
            // refused
            auto settle(CCharEntity* PChar, CCharEntity* PPurse, const uint32 hers, const uint32 his, const uint16 itemId, const uint32 quantity) -> std::optional<uint8>
            {
                if ((hers != 0 && !this->pay(PChar, hers)) || (his != 0 && !this->pay(PPurse, his)))
                {
                    return std::nullopt;
                }
                return this->give(PChar, LOC_INVENTORY, itemId, quantity);
            }

        protected:
            // give and pay above already applied and recorded the work
            auto doCommit() -> bool override
            {
                return true;
            }

            void doRollback() override
            {
            }
        };
    } // namespace

    auto purchaseShared(CCharEntity* PChar, CCharEntity* PPurse, const uint16 itemId, const bool stack, const uint32 quantity, const uint32 price, const uint32 fromPurse) -> bool
    {
        const auto what = fmt::format("{}'s purchase of item {} x{} at {}, {} of it from {}'s purse", PChar->getName(), itemId, quantity, price, fromPurse, PPurse->getName());

        itemmoves::Txn txn;
        if (!txn.ok())
        {
            return false;
        }
        // The game's own purchase: the cheapest listing at or under the bid
        // marked sold to her at the bid (the auction house's trigger pays the
        // seller as it is), then the payment and her piece. Nothing listed
        // there any more, or the statement refused, and nothing has changed:
        // the Txn rolls back as it goes
        const auto sold = db::preparedStmt("UPDATE auction_house SET buyer = ?, buyer_name = ?, sale = ?, sell_date = ? WHERE itemid = ? AND buyer_name IS NULL "
                                           "AND stack = ? AND price <= ? ORDER BY price ASC, id ASC LIMIT 1",
                                           PChar->id,
                                           PChar->getName(),
                                           price,
                                           earth_time::timestamp(),
                                           itemId,
                                           stack ? 1 : 0,
                                           price);
        if (sold == nullptr)
        {
            ShowWarningFmt("auction: {} could not mark its listing sold", what);
            return false;
        }
        if (sold->rowsAffected() == 0)
        {
            return false;
        }

        // Both purses' gil, and her piece once it has landed
        std::vector<Slot> touched{ Slot{ PChar, LOC_INVENTORY, 0 }, Slot{ PPurse, LOC_INVENTORY, 0 } };
        SharedPurchase    items;
        const auto        landed = items.settle(PChar, PPurse, price - fromPurse, fromPurse, itemId, quantity);
        if (!landed.has_value())
        {
            ShowWarningFmt("auction: {} could not be paid for or given: put back", what);
            itemmoves::abandon(txn, items, std::nullopt, touched);
            return false;
        }
        touched.push_back(Slot{ PChar, LOC_INVENTORY, *landed });

        const auto mark = itemmoves::newMark();
        if (!itemmoves::writeMark(mark))
        {
            ShowWarningFmt("auction: {} could not be written: put back", what);
            itemmoves::abandon(txn, items, std::nullopt, touched);
            return false;
        }
        return itemmoves::commitMove(txn, items, mark, std::nullopt, touched, what);
    }
} // namespace pawn::auction
