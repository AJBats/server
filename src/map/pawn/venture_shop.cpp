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

#include "venture_shop.h"

#include "link_api.h"

#include "common/database.h"
#include "common/logging.h"
#include "entities/char_entity.h"
#include "items/item.h"
#include "lua/lua_base_entity.h"
#include "packets/c2s/0x04e_auc.h"
#include "utils/auctionutils.h"
#include "utils/itemutils.h"

#include <algorithm>

namespace pawn::ventureshop
{
    namespace
    {
        // Every way to make a venture's purchase of each item now, cheapest
        // first: one query of the auction house, one call to the guild shops
        auto offersFor(const std::vector<uint16>& items) -> std::unordered_map<uint16, std::vector<Offer>>
        {
            std::unordered_map<uint16, std::vector<Offer>> out;
            if (items.empty())
            {
                return out;
            }
            std::string ids;
            for (const auto item : items)
            {
                ids += (ids.empty() ? "" : ",") + std::to_string(item);
            }
            // the auction house's cheapest listing of each form: the venture buys
            // a stackable item by the stack, anything else one piece
            const auto rset = db::preparedStmt(
                fmt::format("SELECT itemid, stack, MIN(price) AS low FROM auction_house WHERE sale = 0 AND itemid IN ({}) GROUP BY itemid, stack", ids));
            while (rset && rset->next())
            {
                const auto item  = rset->get<uint16>("itemid");
                const bool stack = rset->get<uint8>("stack") != 0;
                if (stack == (purchaseQuantity(item) > 1))
                {
                    out[item].push_back({ .where = Offer::Where::Auction, .total = rset->get<uint32>("low"), .stack = stack });
                }
            }
            // the guild shops open now with all of it on the shelf
            if (auto call = linkapi::libraryCall("ventureShops", "offers"))
            {
                const auto res = (*call)(sol::as_table(items));
                if (const auto byItem = linkapi::libraryTable("ventureShops.offers", res))
                {
                    for (const auto& kv : *byItem)
                    {
                        const auto item     = kv.first.as<uint16>();
                        const auto quantity = purchaseQuantity(item);
                        for (const auto& shopKv : kv.second.as<sol::table>())
                        {
                            const auto shop = shopKv.second.as<sol::table>();
                            if (shop.get_or<uint32>("stock", 0) >= quantity)
                            {
                                out[item].push_back(
                                    { .where = Offer::Where::Guild, .total = shop.get_or<uint32>("price", 0) * quantity, .shop = shop.get_or<std::string>("shop", "") });
                            }
                        }
                    }
                }
            }
            for (auto& kv : out)
            {
                std::ranges::stable_sort(kv.second, {}, &Offer::total);
            }
            return out;
        }
    } // namespace

    auto purchaseQuantity(const uint16 item) -> uint32
    {
        const auto* PKind = xi::items::lookup(item);
        return PKind != nullptr ? std::max<uint32>(PKind->getStackSize(), 1) : 1;
    }

    auto offers(const uint16 item) -> std::vector<Offer>
    {
        auto found = offersFor({ item });
        return found.contains(item) ? found.at(item) : std::vector<Offer>{};
    }

    auto purchasePrices(const std::vector<uint16>& items) -> std::unordered_map<uint16, uint32>
    {
        std::unordered_map<uint16, uint32> out;
        for (const auto& kv : offersFor(items))
        {
            if (!kv.second.empty())
            {
                out[kv.first] = kv.second.front().total;
            }
        }
        return out;
    }

    auto purchasePrice(const uint16 item) -> std::optional<uint32>
    {
        const auto found = purchasePrices({ item });
        return found.contains(item) ? std::optional(found.at(item)) : std::nullopt;
    }

    auto take(CCharEntity* PChar, const uint16 item, const Offer& offer) -> bool
    {
        if (offer.where == Offer::Where::Auction)
        {
            return auctionutils::PurchasingItems(PChar, GP_AUC_PARAM_BID{ .BidPrice = offer.total, .ItemNo = item, .padding00 = 0, .ItemStacks = offer.stack ? 0u : 1u });
        }
        auto call = linkapi::libraryCall("ventureShops", "buy");
        if (!call)
        {
            return false;
        }
        const uint32 quantity = purchaseQuantity(item);
        const auto   res      = (*call)(CLuaBaseEntity(PChar), offer.shop, item, quantity);
        const auto   sale     = linkapi::libraryTable("ventureShops.buy", res);
        if (!sale || sale->get_or<uint32>("bought", 0) != quantity)
        {
            return false;
        }
        if (!sale->get_or("shelved", false))
        {
            ShowWarningFmt("ventureshop: item {} x{} bought at the guild shop of {} could not go back on its shelf (xi.guildShops.state)", item, quantity, offer.shop);
        }
        return true;
    }

    auto whereOf(const Offer& offer) -> std::string
    {
        return offer.where == Offer::Where::Auction ? std::string("the auction house") : fmt::format("the guild shop of {}", offer.shop);
    }
} // namespace pawn::ventureshop
