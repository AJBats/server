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

#include "map/pawn/kit_loss.h"

#include <catch2/catch_test_macros.hpp>

using cardian::kitloss::Counts;
using cardian::kitloss::judge;
using cardian::kitloss::Outcome;

TEST_CASE("Kit loss: a cast that costs nothing, or bait used up, fishes on", "[cardian][fishing]")
{
    CHECK(judge({ .rodsBefore = 1, .rodsAfter = 1, .baitBefore = 1, .baitAfter = 1 }, true, true) == Outcome::FishOn);
    // bait used up on a catch is bought again as ever, whatever the Line snap choice
    CHECK(judge({ .rodsBefore = 1, .rodsAfter = 1, .baitBefore = 99, .baitAfter = 98 }, false, true) == Outcome::FishOn);
}

TEST_CASE("Kit loss: a lure lost to a snapped line ends the venture only where the player chose so", "[cardian][fishing]")
{
    const Counts snapped{ .rodsBefore = 1, .rodsAfter = 1, .baitBefore = 1, .baitAfter = 0 };
    CHECK(judge(snapped, true, true) == Outcome::LureSnapped);
    CHECK(judge(snapped, true, false) == Outcome::FishOn);
}

TEST_CASE("Kit loss: a broken rod is replaced by a spare, or ends the venture", "[cardian][fishing]")
{
    CHECK(judge({ .rodsBefore = 2, .rodsAfter = 1, .baitBefore = 99, .baitAfter = 98 }, false, true) == Outcome::SpareRod);
    CHECK(judge({ .rodsBefore = 1, .rodsAfter = 0, .baitBefore = 99, .baitAfter = 98 }, false, false) == Outcome::RodBroke);
}

TEST_CASE("Kit loss: a broken rod that takes the lure is never told as a snapped line", "[cardian][fishing]")
{
    // a spare rod, and the lure replaced: she fishes on with both
    CHECK(judge({ .rodsBefore = 2, .rodsAfter = 1, .baitBefore = 1, .baitAfter = 0 }, true, false) == Outcome::SpareRodAndLure);
    // a spare rod, but a lost lure ends the venture: it ends on the rod and the lure
    CHECK(judge({ .rodsBefore = 2, .rodsAfter = 1, .baitBefore = 1, .baitAfter = 0 }, true, true) == Outcome::RodAndLure);
    // no spare rod, whatever the choice
    CHECK(judge({ .rodsBefore = 1, .rodsAfter = 0, .baitBefore = 3, .baitAfter = 2 }, true, false) == Outcome::RodAndLure);
}
