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

// The herd round a mob (herd_math.h, ROADMAP A item 9): bearings in world
// terms, the spacing pass that keeps order and moves as little as it can,
// and the evening-out step. Pure, so the ring's shape is pinned here and
// the controller only gathers the bodies and walks the cardians.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "map/pawn/herd_math.h"

#include <cstddef>
#include <numbers>
#include <vector>

using namespace cardian::herd;
using Catch::Matchers::WithinAbs;

namespace
{
    constexpr auto deg(const float d) -> float
    {
        return d * std::numbers::pi_v<float> / 180.0f;
    }

    auto loose(const float bearing) -> Body
    {
        return Body{ .bearing = bearing };
    }

    auto settledAt(const float bearing) -> Body
    {
        return Body{ .bearing = bearing, .settled = true };
    }

    auto fixedAt(const float bearing) -> Body
    {
        return Body{ .bearing = bearing, .fixed = true };
    }

    // The gap from each body to the next one round the ring
    auto gapsRound(const std::vector<float>& bearings) -> std::vector<float>
    {
        std::vector<Body> bodies;
        for (const float b : bearings)
        {
            bodies.push_back(loose(b));
        }
        const auto         order = ringOrder(bodies);
        std::vector<float> gaps;
        for (std::size_t k = 0; k < order.size(); ++k)
        {
            gaps.push_back(ccw(bearings[order[k]], bearings[order[(k + 1) % order.size()]]));
        }
        return gaps;
    }
} // namespace

TEST_CASE("herd: a bearing reads as the in-game map does, north at 0 and clockwise", "[cardian][herd]")
{
    CHECK_THAT(compassDegrees(0.0f), WithinAbs(90.0f, 0.01f));             // east
    CHECK_THAT(compassDegrees(deg(90.0f)), WithinAbs(0.0f, 0.01f));        // north (+z)
    CHECK_THAT(compassDegrees(deg(180.0f)), WithinAbs(270.0f, 0.01f));     // west
    CHECK_THAT(compassDegrees(deg(270.0f)), WithinAbs(180.0f, 0.01f));     // south
    CHECK(compassRounded(deg(90.2f)) == 0);                                // a hair west of north rounds to 0, never 360
    CHECK_THAT(bearingOf(0.0f, 0.0f, 0.0f, 5.0f), WithinAbs(deg(90.0f), 0.0001f));
    CHECK_THAT(shortest(deg(350.0f), deg(10.0f)), WithinAbs(deg(20.0f), 0.0001f));
    CHECK_THAT(shortest(deg(10.0f), deg(350.0f)), WithinAbs(deg(-20.0f), 0.0001f));
}

TEST_CASE("herd: two on one spot are pushed apart by the gap, each half the way", "[cardian][herd]")
{
    const auto out = spread({ loose(1.0f), loose(1.0f) }, deg(40.0f));
    CHECK_THAT(ccw(out[0], out[1]), WithinAbs(deg(40.0f), 0.001f));
    CHECK_THAT(shortest(1.0f, out[0]), WithinAbs(deg(-20.0f), 0.001f));
    CHECK_THAT(shortest(1.0f, out[1]), WithinAbs(deg(20.0f), 0.001f));
}

TEST_CASE("herd: a fixed body never moves; the free one takes the whole push", "[cardian][herd]")
{
    const auto out = spread({ fixedAt(0.0f), loose(deg(10.0f)) }, deg(40.0f));
    CHECK_THAT(out[0], WithinAbs(0.0f, 0.0001f));
    CHECK_THAT(out[1], WithinAbs(deg(40.0f), 0.001f));
}

TEST_CASE("herd: a ring already spaced is left as it is", "[cardian][herd]")
{
    const auto out = spread({ fixedAt(0.0f), loose(deg(100.0f)), loose(deg(200.0f)) }, deg(40.0f));
    CHECK_THAT(out[1], WithinAbs(deg(100.0f), 0.0001f));
    CHECK_THAT(out[2], WithinAbs(deg(200.0f), 0.0001f));
}

TEST_CASE("herd: a newcomer makes room; the settled stay where they are", "[cardian][herd]")
{
    const auto out = spread({ settledAt(0.0f), settledAt(deg(90.0f)), loose(deg(30.0f)) }, deg(40.0f));
    CHECK_THAT(out[0], WithinAbs(0.0f, 0.0001f));
    CHECK_THAT(out[1], WithinAbs(deg(90.0f), 0.0001f));
    CHECK_THAT(out[2], WithinAbs(deg(40.0f), 0.001f));
}

TEST_CASE("herd: the settled give way only when a newcomer cannot make room alone", "[cardian][herd]")
{
    // 60 degrees between the two settled, 80 needed round a newcomer
    const auto out = spread({ settledAt(0.0f), settledAt(deg(60.0f)), loose(deg(30.0f)) }, deg(40.0f));
    for (const float gap : gapsRound(out))
    {
        CHECK(gap >= deg(40.0f) - 0.001f);
    }
    // The order round the mob is kept: the newcomer still between them
    CHECK(ccw(out[0], out[2]) < ccw(out[0], out[1]));
}

TEST_CASE("herd: a bunch keeps its order as it spreads", "[cardian][herd]")
{
    const auto out = spread({ loose(deg(10.0f)), loose(deg(11.0f)), loose(deg(12.0f)) }, deg(40.0f));
    CHECK_THAT(ccw(out[0], out[1]), WithinAbs(deg(40.0f), 0.001f));
    CHECK_THAT(ccw(out[1], out[2]), WithinAbs(deg(40.0f), 0.001f));
    CHECK_THAT(out[1], WithinAbs(deg(11.0f), 0.001f));
}

TEST_CASE("herd: more bodies than the gap allows share the ring a little under evenly", "[cardian][herd]")
{
    std::vector<Body> bodies(12, loose(0.0f));
    const auto        out = spread(bodies, deg(40.0f));
    const float       g   = effectiveGap(12, deg(40.0f));
    CHECK(g < deg(30.0f));
    for (const float gap : gapsRound(out))
    {
        CHECK(gap >= g - 0.001f);
    }
}

TEST_CASE("herd: when the gaps cannot be met round fixed bodies, the free ones take even places between them, and hold", "[cardian][herd]")
{
    // A free body between two fixed ones five degrees apart: the middle
    auto out = spread({ fixedAt(0.0f), fixedAt(deg(5.0f)), loose(deg(2.0f)) }, deg(40.0f));
    CHECK_THAT(out[2], WithinAbs(deg(2.5f), 0.0001f));

    // Two free bodies between fixed ones sixty degrees apart, both near the
    // first: thirds, and the pass after finds them there and moves nobody
    out = spread({ fixedAt(0.0f), fixedAt(deg(60.0f)), loose(deg(5.0f)), loose(deg(8.0f)) }, deg(40.0f));
    CHECK_THAT(out[2], WithinAbs(deg(20.0f), 0.0001f));
    CHECK_THAT(out[3], WithinAbs(deg(40.0f), 0.0001f));
    const auto again = spread({ fixedAt(0.0f), fixedAt(deg(60.0f)), loose(out[2]), loose(out[3]) }, deg(40.0f));
    CHECK_THAT(again[2], WithinAbs(out[2], 0.0001f));
    CHECK_THAT(again[3], WithinAbs(out[3], 0.0001f));
}

TEST_CASE("herd: the even step turns a body towards the middle of its gap, a step at a time", "[cardian][herd]")
{
    // Alone with the tank, she heads for its far side
    auto out = evenStep({ fixedAt(0.0f), loose(deg(30.0f)) }, deg(20.0f), deg(10.0f));
    CHECK_THAT(out[0], WithinAbs(0.0f, 0.0001f));
    CHECK_THAT(out[1], WithinAbs(deg(50.0f), 0.001f));

    // Near enough the middle, she stays
    out = evenStep({ fixedAt(0.0f), loose(deg(170.0f)) }, deg(20.0f), deg(15.0f));
    CHECK_THAT(out[1], WithinAbs(deg(170.0f), 0.0001f));

    // An even ring is left alone
    out = evenStep({ fixedAt(0.0f), loose(deg(120.0f)), loose(deg(240.0f)) }, deg(20.0f), deg(10.0f));
    CHECK_THAT(out[1], WithinAbs(deg(120.0f), 0.0001f));
    CHECK_THAT(out[2], WithinAbs(deg(240.0f), 0.0001f));

    // A body alone has nowhere to go
    out = evenStep({ loose(deg(45.0f)) }, deg(20.0f), deg(10.0f));
    CHECK_THAT(out[0], WithinAbs(deg(45.0f), 0.0001f));
}

TEST_CASE("herd: two settled too close are pushed apart, as a settled one by a fixed one", "[cardian][herd]")
{
    auto out = spread({ settledAt(0.0f), settledAt(deg(10.0f)) }, deg(40.0f));
    CHECK_THAT(ccw(out[0], out[1]), WithinAbs(deg(40.0f), 0.001f));

    // A Thief on the mob's back, fixed, onto a cardian settled there
    out = spread({ fixedAt(0.0f), settledAt(deg(5.0f)) }, deg(40.0f));
    CHECK_THAT(out[0], WithinAbs(0.0f, 0.0001f));
    CHECK_THAT(out[1], WithinAbs(deg(40.0f), 0.001f));
}

TEST_CASE("herd: a body boxed in between two fixed ones takes the middle, and the rest of the ring still spreads", "[cardian][herd]")
{
    const auto out = spread({ fixedAt(0.0f), fixedAt(deg(30.0f)), loose(deg(3.0f)), loose(deg(180.0f)), loose(deg(180.0f)) }, deg(40.0f));
    CHECK_THAT(out[2], WithinAbs(deg(15.0f), 0.0001f));
    CHECK_THAT(ccw(out[3], out[4]), WithinAbs(deg(40.0f), 0.001f));
}

TEST_CASE("herd: spacing reaches across north, where the bearings wrap", "[cardian][herd]")
{
    const auto out = spread({ loose(deg(350.0f)), loose(deg(5.0f)) }, deg(40.0f));
    CHECK_THAT(ccw(out[0], out[1]), WithinAbs(deg(40.0f), 0.001f));
    CHECK_THAT(out[0], WithinAbs(deg(337.5f), 0.001f));
}

TEST_CASE("herd: the even step settles a ring with no fixed body instead of swinging it", "[cardian][herd]")
{
    // Gaps of 115 and 65 in turn: a whole step to the middle would swap them every beat
    std::vector<Body> bodies{ loose(0.0f), loose(deg(115.0f)), loose(deg(180.0f)), loose(deg(295.0f)) };
    for (int beat = 0; beat < 3; ++beat)
    {
        const auto out = evenStep(bodies, 1.0f, 0.01f);
        for (std::size_t i = 0; i < bodies.size(); ++i)
        {
            bodies[i].bearing = out[i];
        }
    }
    std::vector<float> bearings;
    for (const auto& b : bodies)
    {
        bearings.push_back(b.bearing);
    }
    for (const float gap : gapsRound(bearings))
    {
        CHECK_THAT(gap, WithinAbs(deg(90.0f), 0.01f));
    }
}

TEST_CASE("herd: two free bodies even out by stepping away from each other", "[cardian][herd]")
{
    const auto out = evenStep({ loose(0.0f), loose(deg(90.0f)) }, deg(20.0f), deg(5.0f));
    CHECK_THAT(out[0], WithinAbs(deg(340.0f), 0.001f));
    CHECK_THAT(out[1], WithinAbs(deg(110.0f), 0.001f));
}
