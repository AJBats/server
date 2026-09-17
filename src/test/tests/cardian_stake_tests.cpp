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

// The stake's arithmetic (RESEARCH §12.16): the tank's tow point and the
// dissolve rule. Pure, so a change here fails before a map server runs.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "map/pawn/stake_math.h"

#include <numbers>

using namespace cardian::stake;
using Catch::Matchers::WithinAbs;

TEST_CASE("Stake: the tow point is a reach past the stake, away from the mob", "[cardian][stake]")
{
    // The mob ten east of the stake at the origin: she waits a reach west
    // of it, and the mob chasing her stops on the stake
    const auto [x, z] = towPoint(10.0f, 0.0f, 0.0f, 0.0f, 3.0f, 0.0f);
    CHECK_THAT(x, WithinAbs(-3.0f, 1e-4));
    CHECK_THAT(z, WithinAbs(0.0f, 1e-4));

    // From the north-east, the point sits on the same line beyond the stake
    const auto [dx, dz] = towPoint(10.0f, 10.0f, 4.0f, 4.0f, 2.0f, 0.0f);
    CHECK_THAT(dx, WithinAbs(4.0f - std::sqrt(2.0f), 1e-3));
    CHECK_THAT(dz, WithinAbs(4.0f - std::sqrt(2.0f), 1e-3));
}

TEST_CASE("Stake: a mob on the stake has no line, so the tow point takes the fallback bearing", "[cardian][stake]")
{
    const auto [x, z] = towPoint(5.0f, 5.0f, 5.0f, 5.0f, 3.0f, std::numbers::pi_v<float> / 2.0f);
    CHECK_THAT(x, WithinAbs(5.0f, 1e-3));
    CHECK_THAT(z, WithinAbs(8.0f, 1e-3));
}

TEST_CASE("Stake: it dissolves only once the party is seen elsewhere and nobody is left in its zone", "[cardian][stake]")
{
    CHECK(dissolves(0, 1));
    CHECK(dissolves(0, 4));
    CHECK_FALSE(dissolves(1, 3));
    CHECK_FALSE(dissolves(3, 0));
    // A zone line: everyone is between zones, nobody is seen anywhere
    CHECK_FALSE(dissolves(0, 0));
}
