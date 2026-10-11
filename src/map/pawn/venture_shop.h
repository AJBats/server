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

#include "common/cbasetypes.h"

#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

class CCharEntity;

// Where a venture buys her supplies, her rod and her bait (RESEARCH §11.13,
// "Buying her supplies"): wherever they cost least now, the auction house or
// a guild shop open now, through the game's own purchase either way -- the
// auction house's (auctionutils::PurchasingItems: its cheapest listing at the
// bid), or the guild shop's (xi.cardian.ventureShops, over xi.guildShops:
// its stock, today's price, its hours). Her walk to the counter is
// simulated: she buys from wherever it is cheapest in the world as she sets
// out (OPEN_ISSUES #431: where a venture goes, and so where she shops, once
// cardians travel). A venture buys a rod one at a time and a bait by the
// whole stack (a lure, one): its purchase. What the auction house's crowd and
// the guild shops keep in stock for her is theirs to keep (tools/economy,
// the flow goods; the guild shop's shelf refilled after her purchase).
namespace pawn::ventureshop
{
    struct Offer
    {
        enum class Where : uint8
        {
            Auction,
            Guild,
        };
        Where       where = Where::Auction;
        uint32      total = 0;     // the gil the whole purchase costs
        bool        stack = false; // the auction house: a stack listing, else one piece
        std::string shop;          // a guild shop: its name
    };

    // How many of an item a venture buys at a time: a rod one, a bait its
    // whole stack (a lure, one)
    auto purchaseQuantity(uint16 item) -> uint32;

    // Every way to make a venture's purchase of an item now, cheapest first:
    // the auction house's cheapest listing of its form (one piece, or a whole
    // stack), and each guild shop open now with all of it on its shelf
    auto offers(uint16 item) -> std::vector<Offer>;

    // What a venture's purchase of each item costs now at its cheapest; an
    // item nobody sells now is missing. One look at the auction house and
    // one at the guild shops, however many items
    auto purchasePrices(const std::vector<uint16>& items) -> std::unordered_map<uint16, uint32>;
    auto purchasePrice(uint16 item) -> std::optional<uint32>;

    // An offer taken by her character, whose purse holds its total: false
    // when it did not go through (sold meanwhile, the shop closed or short)
    auto take(CCharEntity* PChar, uint16 item, const Offer& offer) -> bool;

    // An offer for people: "the auction house", or the guild shop's name
    auto whereOf(const Offer& offer) -> std::string;
} // namespace pawn::ventureshop
