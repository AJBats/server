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

#include "cardian_link_messages.h"
#include "common/cbasetypes.h"

#include <string>
#include <unordered_map>
#include <vector>

class CBaseEntity;
class CCharEntity;

// The Auction House screen's server half (ROADMAP L): what the auction
// house has for a member of the party -- to wear, to learn, or anything on
// its shelves -- and what the crowd expects it to cost. The auction house stays blind, as retail's: a listing's
// price is never told, only how many are listed and what the last sales paid.
namespace pawn::auction
{
    // One item in one form the auction house lists it in: singly, or by the
    // stack (ammunition, medicine), each a row of its own as on the game's
    // own auction house
    struct Listing
    {
        uint16 itemId    = 0;
        uint8  level     = 0; // the level it asks for: gear's, a scroll's to learn; else 0
        uint32 stock     = 0; // how many are listed now in this form; 0 when sold out
        uint32 going     = 0; // the going rate in this form; 0 when nothing has sold
        uint8  category  = 0; // the auction house's own category (xi.itemAHCategory)
        bool   stack     = false;
        uint32 stackSize = 1; // how many pieces the form buys
    };

    // What PChar could wear in equipSlot now -- her main job, her level,
    // her race, as the equip handler weighs them -- among every item the
    // auction house has ever listed, in stock or sold out, the shelf its own
    // browse shows (search.OMIT_NO_HISTORY), one row per form it has been
    // listed in. By the auction house's categories in its own order (a slot
    // like Ammo mixes ammunition with fishing gear), and within one the
    // highest level first, then the dearest by the piece (the census
    // wardrobe's order, ROADMAP D2), an item's single row before its stack.
    auto wearableAtAuction(CCharEntity* PChar, uint8 equipSlot) -> std::vector<Listing>;

    // Every item the auction house has ever listed in the categories, in
    // stock or sold out, one row per form (ROADMAP L step 4: a group under
    // her grid, a category of Browse), whoever asks. With `learnable`, only
    // the spell scrolls PChar can learn now and has not, each at the level
    // she learns it by. By category in the order given, then the highest
    // level first (gear's level, a scroll's), then by name, an item's single
    // row before its stack.
    auto inCategories(CCharEntity* PChar, const std::vector<uint8>& categories, bool learnable) -> std::vector<Listing>;

    // The crowd's going rate for each item (tools/economy/market.py,
    // `going`): the median of its last ten sales, single or stack. xi_map
    // reads no price book, so the book's floor and cap on the rate are left
    // out; the crowd keeps the history anchored to the book (market.py,
    // `seed_history`). An item with no sale is absent. Copied, not shared:
    // tech debt to pay back if the two drift (the user, 2026-09-26).
    auto goingRates(const std::vector<uint16>& itemIds, bool stack) -> std::unordered_map<uint16, uint32>;

    // One sale off an item's history
    struct Sale
    {
        uint32      date  = 0; // when it sold, Unix time
        uint32      price = 0; // what the buyer paid (his bid)
        std::string seller;
        std::string buyer;
    };

    // What the game's own auction house shows of an item in one form,
    // singly or by the stack: its stock and its last ten sales, newest
    // first; and the going rate they make
    struct History
    {
        uint32            stock = 0;
        uint32            going = 0;
        std::vector<Sale> sales;
    };

    auto history(uint16 itemId, bool stack) -> History;

    // What a bid came to: CL_S_OK when won (the Link's outcomes), else why
    // not -- CL_S_NOTHING_AT_PRICE when the auction house had no listing at
    // or under the bid. Won: where the piece is (the inventory when the move
    // on to the bag asked for fell through), whether she wears it and, when
    // she was to and does not, why not; and how much of the price the purse
    // gave.
    struct BidResult
    {
        uint16 status    = CL_S_REFUSED; // CL_S_OK only once the piece is hers
        uint8  location  = 0;
        bool   equipped  = false;
        uint16 notWorn   = 0;
        uint32 fromPurse = 0;
    };

    // PChar bids `price` for one piece, or one stack, of any item, as the
    // game's own purchase does (auctionutils::PurchasingItems): the cheapest
    // listing at or under the bid is hers, and she pays the bid, as on retail. The
    // player buys through PurchasingItems itself; a cardian with his purse
    // behind hers through a copy of it that pays from both. Won, the
    // piece goes on from her inventory to `location` (the inventory, or a
    // bag she carries into the field), and with `equip` she wears it in
    // equipSlot -- from the inventory or a wardrobe only. What would refuse
    // the purchase is judged first, so the refusal says why. PPurse, when
    // it is not PChar (the player, for a cardian of his), is the shared
    // purse: her gil first, the rest his, paid in the purchase's own
    // transaction, so no gil moves unless the piece is won. With nothing
    // listed at or under the bid, no purchase is tried.
    auto bid(CCharEntity* PChar, CCharEntity* PPurse, uint16 itemId, bool stack, uint32 price, uint8 location, uint8 equipSlot, bool equip) -> BidResult;

    // The auction counter within the player's reach (8 yalms, a gate
    // guard's), nullptr for none
    auto counterNear(const CCharEntity* PPlayer) -> const CBaseEntity*;

    // Whether a member of his party may shop there now: CL_S_OK, or why not
    // -- he stands by no counter (CL_S_NOT_BY_COUNTER), or she is not within
    // 15 yalms of the one he stands at (CL_S_TOO_FAR_TO_SHOP). The player
    // himself stands by it by being within reach of one.
    auto whereShopping(const CCharEntity* PPlayer, const CCharEntity* PMember) -> uint16;
} // namespace pawn::auction
