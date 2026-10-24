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

// The walker's route arithmetic (ROADMAP W, pawn/safe_walk.h)

#include "map/pawn/safe_walk.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <vector>

namespace
{
    auto at(const float x, const float z) -> position_t
    {
        return position_t(x, 0.0f, z, 0, 0);
    }
} // namespace

TEST_CASE("One walker: a route cut short of its end, along it", "[cardian][walk]")
{
    using cardian::walk::cutShort;
    using cardian::walk::lengthOf;

    SECTION("within its last leg")
    {
        std::vector<position_t> route{ at(0, 0), at(10, 0), at(10, 10) };
        cutShort(route, 3.0f);
        REQUIRE(route.size() == 3);
        CHECK(route.back().x == Catch::Approx(10.0f));
        CHECK(route.back().z == Catch::Approx(7.0f));
        CHECK(lengthOf(route) == Catch::Approx(17.0f));
    }

    SECTION("past its last leg: the legs it swallows go")
    {
        std::vector<position_t> route{ at(0, 0), at(10, 0), at(10, 2) };
        cutShort(route, 5.0f);
        REQUIRE(route.size() == 2);
        CHECK(route.back().x == Catch::Approx(7.0f));
        CHECK(route.back().z == Catch::Approx(0.0f));
    }

    SECTION("no longer than the cut: her own spot alone")
    {
        std::vector<position_t> route{ at(0, 0), at(1, 0), at(2, 0) };
        cutShort(route, 3.0f);
        REQUIRE(route.size() == 1);
        CHECK(route.front().x == Catch::Approx(0.0f));
    }

    SECTION("no cut, no change")
    {
        std::vector<position_t> route{ at(0, 0), at(4, 0) };
        cutShort(route, 0.0f);
        REQUIRE(route.size() == 2);
        CHECK(route.back().x == Catch::Approx(4.0f));
    }
}

TEST_CASE("One walker: her progress along a route is the leg she stands nearest", "[cardian][walk]")
{
    using cardian::walk::progress;
    const std::vector<position_t> route{ at(0, 0), at(10, 0), at(10, 10), at(20, 10) };

    CHECK(progress(route, 1, at(5, 0.2f)) == 1);   // on the first leg
    CHECK(progress(route, 1, at(10.1f, 5)) == 2);  // past its corner, on the second
    CHECK(progress(route, 1, at(10, 9.5f)) == 3);  // on the second leg's end: the third waypoint is next
    CHECK(progress(route, 2, at(1, 0)) == 2);      // never back to a leg behind her
    CHECK(progress(route, 4, at(20, 10)) == 4);    // walked it all
}

TEST_CASE("One walker: her path finder is handed a route only as far as the lip of its next drop", "[cardian][walk]")
{
    using cardian::walk::isDrop;
    using cardian::walk::toNextLip;
    const auto p = [](const float x, const float y, const float z) { return position_t(x, y, z, 0, 0); };

    CHECK(isDrop(p(0, -23.6f, 0), p(1, -16.0f, 0)));     // 7.6 down, 1 across: a ledge's link
    CHECK_FALSE(isDrop(p(0, -23.6f, 0), p(4, -20.0f, 0))); // down less steeply than 1.5 in 1: a ramp
    CHECK_FALSE(isDrop(p(0, -16.0f, 0), p(0, -17.5f, 0))); // up
    CHECK_FALSE(isDrop(p(0, 0, 0), p(0, 1.5f, 0)));       // too small a step down to be one

    // her spot, two legs on the shelf, the drop, a leg below, another drop
    const std::vector<position_t> route{ p(0, -23.6f, 0), p(3, -23.6f, 0), p(5, -23.6f, 0), p(6, -16.0f, 0), p(9, -16.0f, 0), p(9, -8.0f, 1) };
    CHECK(toNextLip(route, 1, route.size()) == 3); // up to the lip, route[2], and no further
    CHECK(toNextLip(route, 2, route.size()) == 3); // walking to the lip: the lip alone
    CHECK(toNextLip(route, 3, route.size()) == 5); // past the drop: on to the next lip
    CHECK(toNextLip(route, 5, route.size()) == 6); // no drop ahead: all of it
    CHECK(toNextLip(route, 1, 2) == 2);             // a choke short of the lip holds first
}
