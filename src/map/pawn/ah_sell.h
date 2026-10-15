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

#include <cstddef>
#include <vector>

class CCharEntity;

// Selling on the auction house from the Auction House screen's Sell page
// (ROADMAP L): every listing is the player's -- his slots, his fee, his name
// on it, its gil to his delivery box -- whoever's bag the item comes from.
// The game's own listing (auctionutils::ProofOfPurchase) takes the item, the
// fee and the name from one character; this takes the item from the
// holder's bag and the rest from him, in one transaction. A listing, its take
// down and a new price are each one move of item_moves.h, kept whole when the
// database fails partway (#478's discipline). Outcomes are the Link's
// (CL_S_*). Where he and the holder stand is the Link handler's to judge, not
// this file's, so xi_test drives it on players of its own
// (scripts/tests/cardian/ah_sell.lua)
namespace pawn::ahsell
{
    // The fee for a listing at price, as the game's own listing charges it
    auto feeOf(uint32 price, bool stack) -> uint32;

    // His listings on sale now
    auto openCount(uint32 charid) -> uint32;

    // The bags a holder sells from: those she carries into the field -- the
    // inventory, a satchel, sack or case, a wardrobe -- never the Mog House's
    auto sellsFrom(uint8 loc) -> bool;

    // What refuses a listing of the item at price before any piece of it is
    // looked at: the price, the auction house's own flag and shelves, a stack
    // of an item that comes in none, his listings at the limit
    // (map.AH_LIST_LIMIT), too little gil for the fee. CL_S_OK when nothing does
    auto judge(CCharEntity* PSeller, uint16 itemId, bool stack, uint32 price) -> uint16;

    struct Listed
    {
        uint16 status  = 0; // CL_S_OK once it is up
        uint32 fee     = 0;
        uint32 listing = 0; // its own number, auction_house.id
    };

    // PHolder's item at (loc, slot) -- his own or another's -- put up by
    // PSeller at price, one piece or the full stack: the piece off her bag,
    // the fee off his purse and the listing in his name, all of it or none
    auto list(CCharEntity* PSeller, CCharEntity* PHolder, uint8 loc, uint8 slot, uint16 itemId, bool stack, uint32 price) -> Listed;

    // One listing of his, on sale or sold
    struct Mine
    {
        uint32 listing = 0;
        uint32 price   = 0; // on sale: his price; sold: what it sold for
        uint32 date    = 0; // when it went up, or when it sold
        uint16 item    = 0;
        bool   stack   = false;
        bool   sold    = false;
    };

    // His sales status: his listings on sale in the order they went up, then
    // each sold one whose gil still waits in his delivery box (a sale's gil
    // and its listing are paired by item and price), the newest first; at
    // most `most` rows. `open` is told every listing on sale, shown or not
    auto listingsOf(uint32 charid, std::size_t most, std::size_t& open) -> std::vector<Mine>;

    struct Cancelled
    {
        uint16 status = 0; // CL_S_OK once it is back
        uint8  landed = 0; // his inventory's slot
    };

    // His listing on sale by its number taken back into his inventory, as the
    // game's own cancel (auctionutils::CancelSale): refused with his
    // inventory full, or a Rare one in it already
    auto cancel(CCharEntity* PSeller, uint32 listing, uint16 itemId) -> Cancelled;

    // His listing on sale by its number given a new price, as taking it down
    // and putting it up again would: the fee at the new price paid again and
    // its date fresh, so its time on sale starts over, while the listing
    // stays up under its own number. Its own price again changes nothing and
    // costs nothing
    auto reprice(CCharEntity* PSeller, uint32 listing, uint16 itemId, uint32 price) -> Listed;
} // namespace pawn::ahsell
