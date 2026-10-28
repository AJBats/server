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

// The healers' Regen (regen_math.h): what one heals, what a top-up leaves to
// it, and which member a healer puts hers on.

#include <catch2/catch_test_macros.hpp>

#include "map/pawn/conveyor_math.h"
#include "map/pawn/regen_math.h"

#include <vector>

using namespace cardian::regen;

TEST_CASE("Cardian regen: what a Regen heals is its power for each tick left", "[cardian][regen]")
{
    // 5 a tick for 75 s: 25 ticks of the zone's 3 s
    CHECK(healsWhole(Seen{ 5.0, 75.0 }) == 125.0);
    CHECK(healsLeft(5.0, 10.0) == 15.0); // three whole ticks left
    CHECK(healsLeft(5.0, 2.9) == 0.0);
    CHECK(healsLeft(5.0, -1.0) == 0.0);
    // a top-up answers the gap past what is still to heal
    CHECK(gapAfter(100, 40.0) == 60);
    CHECK(gapAfter(30, 40.0) == 0);
}

TEST_CASE("Cardian regen: a healer's Regen goes where its whole is spent", "[cardian][regen]")
{
    const Seen seen{ 5.0, 75.0 }; // 125 HP

    SECTION("missing and the damage expected before the fight ends cover it")
    {
        CHECK(worthCasting(seen, 0.0, 2.0, 75.0));       // 150 expected over its whole run
        CHECK_FALSE(worthCasting(seen, 0.0, 2.0, 20.0)); // the fight ends in 20 s: 40 of it spent
        CHECK(worthCasting(seen, 90.0, 2.0, 20.0));      // 90 missing and 40 to come
        CHECK_FALSE(worthCasting(seen, 0.0, 1.0, 75.0)); // 75 expected: half of it wasted
        CHECK(worthCasting(seen, 0.0, 2.0, 300.0));      // a long fight counts its run alone
    }

    SECTION("a fight of no known length counts no time: the HP drops to it first")
    {
        CHECK_FALSE(worthCasting(seen, 40.0, 5.0, 0.0));
        CHECK(worthCasting(seen, 130.0, 5.0, 0.0));
    }

    SECTION("of those it is worth casting on, the one taking the most, then the most missing")
    {
        const std::vector<Candidate> party{ { 1, 0.5, 200.0 }, { 2, 4.0, 10.0 }, { 3, 4.0, 50.0 } };
        CHECK(pick(party) == 2u);
        CHECK_FALSE(pick(std::vector<Candidate>{}).has_value());
    }
}

TEST_CASE("Cardian regen: a routine cure keeps a quarter of her HP missing in a fight", "[cardian][regen]")
{
    using cardian::tactics::cureBuffer;
    CHECK(cureBuffer(700, true) == 175);
    CHECK(cureBuffer(701, true) == 176); // rounded up: never under a quarter
    CHECK(cureBuffer(700, false) == 0);  // between fights the healers top up
}
