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

class CCharEntity;

// A venture's catch sold, the map's (RESEARCH §11.13, "Selling her catch").
// What a fishing venture lands is her line to sell: the Venture Report's
// rows in her bags (cardian_venture_items, CL_VENTURE_CARRIED), and nothing
// else she owns. Every few seconds, during the trip and after it until the
// line is empty, each piece goes where it pays her most: to a vendor at the
// game's own price (xi.shop.onSellPriceCheck, her nation's fame), at once
// and all of it when the vendor pays at least the auction house; else onto
// the auction house through the server's own listing (auctionutils::
// ProofOfPurchase: its fee, its seven slots) at a price the crowd takes
// next -- a gil under the cheapest listing of its form, never over the going
// rate -- and only in a form the crowd has bought within its listing window.
// A stackable catch waits to go up by the full stack while a venture of hers
// is under way, its odd ones sold singly once none is. As listings sell she
// posts the next in line, and takes the gil the auction house leaves in her
// delivery box into her purse; a listing sent home unsold stays in her
// delivery box for her player.
//
// The report is a venture's own: what a new venture's catch fetches is its
// earnings, while what an earlier one left in the line still sells, first,
// as the earlier venture's -- its rows kept apart (`earlier`), its sales and
// fees never this venture's. A stack listed from both is shared between them
// by count.
namespace pawn::sales
{
    void ensureTables();

    // A new venture begins: what is left of her line becomes an earlier
    // venture's, and the sales, returns and kept pieces already reported
    // are cleared
    void newVenture(uint32 charid);

    // The line worked, from the professions' tick
    void tick();
} // namespace pawn::sales
