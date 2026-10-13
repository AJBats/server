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
