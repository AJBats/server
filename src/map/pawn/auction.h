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

#include <unordered_map>
#include <vector>

class CCharEntity;

// The Auction House screen's server half (ROADMAP L): what the auction
// house has for a member of the party to wear, and what the crowd expects
// it to cost. The auction house stays blind, as retail's: a listing's
// price is never told, only how many are listed and what the last sales paid.
namespace pawn::auction
{
    // One piece of gear the auction house has listed singly
    struct Listing
    {
        uint16 itemId   = 0;
        uint8  level    = 0; // the level it asks for
        uint32 stock    = 0; // how many are listed now; 0 when sold out
        uint32 going    = 0; // the going rate; 0 when nothing has sold
        uint8  category = 0; // the auction house's own category (xi.itemAHCategory)
    };

    // What PChar could wear in equipSlot now -- her main job, her level,
    // her race, as the equip handler weighs them -- among every item the
    // auction house has ever listed singly, in stock or sold out, the shelf
    // its own browse shows (search.OMIT_NO_HISTORY). By the auction house's
    // categories in its own order (a slot like Ammo mixes ammunition with
    // fishing gear), and within one the highest level first, then the
    // dearest (the census wardrobe's order, ROADMAP D2).
    auto wearableAtAuction(CCharEntity* PChar, uint8 equipSlot) -> std::vector<Listing>;

    // The crowd's going rate for each item (tools/economy/market.py,
    // `going`): the median of its last ten sales, single or stack. xi_map
    // reads no price book, so the book's floor and cap on the rate are left
    // out; the crowd keeps the history anchored to the book (market.py,
    // `seed_history`). An item with no sale is absent. Copied, not shared:
    // tech debt to pay back if the two drift (the user, 2026-09-26).
    auto goingRates(const std::vector<uint16>& itemIds, bool stack) -> std::unordered_map<uint16, uint32>;
} // namespace pawn::auction
