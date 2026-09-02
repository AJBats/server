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

// The formation arithmetic behind the Cardian Link's position stream: the
// numbers the lead and the followers aim and pace by. Pure functions, so a
// tuning change that breaks a stop, a turn or a 180 fails here first.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "map/pawn/formation_math.h"

#include <cmath>
#include <numbers>
#include <utility>
#include <vector>

using namespace cardian::formation;
using Catch::Matchers::WithinAbs;

namespace
{
    constexpr float kTick = 0.1f; // seconds between the two samples in these cases
}

TEST_CASE("deriveMotion: a straight run yields distance over time", "[cardian][formation]")
{
    const Sample previous{ 0.0f, 0.0f, 64, true };
    const Sample fresh{ 0.5f, 0.0f, 64, true };

    const auto m = deriveMotion(previous, Motion{}, fresh, kTick);

    REQUIRE_THAT(m.vx, WithinAbs(5.0f, 0.001f));
    REQUIRE_THAT(m.vz, WithinAbs(0.0f, 0.001f));
    REQUIRE_THAT(m.yawRate, WithinAbs(0.0f, 0.001f));
}

TEST_CASE("deriveMotion: a continuing run is smoothed halfway toward the new reading", "[cardian][formation]")
{
    const Sample previous{ 0.0f, 0.0f, 64, true };
    const Sample fresh{ 0.3f, 0.0f, 64, true }; // raw 3 y/s after a 5 y/s history

    const auto m = deriveMotion(previous, Motion{ 5.0f, 0.0f, 0.0f }, fresh, kTick);

    REQUIRE_THAT(m.vx, WithinAbs(4.0f, 0.001f));
}

TEST_CASE("deriveMotion: the heading wraps the short way round", "[cardian][formation]")
{
    const Sample previous{ 0.0f, 0.0f, 250, true };
    const Sample fresh{ 0.5f, 0.0f, 4, true }; // +10 steps across the 255/0 seam

    const auto forward = deriveMotion(previous, Motion{}, fresh, kTick);
    const auto back    = deriveMotion(fresh, Motion{}, previous, kTick);

    const float expected = 10.0f * (2.0f * std::numbers::pi_v<float> / 256.0f) / kTick;
    REQUIRE_THAT(forward.yawRate, WithinAbs(expected, 0.001f));
    REQUIRE_THAT(back.yawRate, WithinAbs(-expected, 0.001f));
}

TEST_CASE("deriveMotion: a stop zeroes the motion at once", "[cardian][formation]")
{
    const Sample previous{ 0.0f, 0.0f, 64, true };
    const Sample fresh{ 0.5f, 0.0f, 64, false };

    const auto m = deriveMotion(previous, Motion{ 5.0f, 0.0f, 1.0f }, fresh, kTick);

    REQUIRE(m.vx == 0.0f);
    REQUIRE(m.vz == 0.0f);
    REQUIRE(m.yawRate == 0.0f);
}

TEST_CASE("deriveMotion: the first sample after a stop is taken raw, not blended with a stale history", "[cardian][formation]")
{
    const Sample previous{ 0.0f, 0.0f, 64, false }; // was standing
    const Sample fresh{ 0.5f, 0.0f, 64, true };

    const auto m = deriveMotion(previous, Motion{ 9.0f, 9.0f, 9.0f }, fresh, kTick);

    REQUIRE_THAT(m.vx, WithinAbs(5.0f, 0.001f));
    REQUIRE_THAT(m.vz, WithinAbs(0.0f, 0.001f));
}

TEST_CASE("deriveMotion: implausible sample spacing is ignored", "[cardian][formation]")
{
    const Sample previous{ 0.0f, 0.0f, 64, true };
    const Sample fresh{ 0.5f, 0.0f, 64, true };

    REQUIRE(deriveMotion(previous, Motion{}, fresh, 0.005f).vx == 0.0f); // same frame twice
    REQUIRE(deriveMotion(previous, Motion{}, fresh, 2.0f).vx == 0.0f);   // the stream had died
}

TEST_CASE("predictAhead: horizon times velocity", "[cardian][formation]")
{
    const auto p = predictAhead(Motion{ 5.0f, 0.0f, 0.0f }, true, 0.7f, 6.0f);

    REQUIRE_THAT(p.dx, WithinAbs(3.5f, 0.001f));
    REQUIRE_THAT(p.dz, WithinAbs(0.0f, 0.001f));
    REQUIRE_THAT(p.ahead, WithinAbs(3.5f, 0.001f));
}

TEST_CASE("predictAhead: the displacement is capped, direction kept", "[cardian][formation]")
{
    const auto p = predictAhead(Motion{ 6.0f, 8.0f, 0.0f }, true, 1.0f, 6.0f); // 10 y wanted

    REQUIRE_THAT(p.ahead, WithinAbs(6.0f, 0.001f));
    REQUIRE_THAT(p.dx, WithinAbs(3.6f, 0.001f));
    REQUIRE_THAT(p.dz, WithinAbs(4.8f, 0.001f));
}

TEST_CASE("predictAhead: turning shortens the horizon, down to a quarter", "[cardian][formation]")
{
    const auto gentle = predictAhead(Motion{ 5.0f, 0.0f, 1.5f }, true, 0.7f, 6.0f);
    const auto hard   = predictAhead(Motion{ 5.0f, 0.0f, 3.0f }, true, 0.7f, 6.0f);
    const auto spin   = predictAhead(Motion{ 5.0f, 0.0f, 30.0f }, true, 0.7f, 6.0f);

    REQUIRE_THAT(gentle.ahead, WithinAbs(1.75f, 0.001f));
    REQUIRE_THAT(hard.ahead, WithinAbs(0.875f, 0.001f));
    REQUIRE_THAT(spin.ahead, WithinAbs(0.875f, 0.001f));
}

TEST_CASE("predictAhead: nothing for a standing or crawling player", "[cardian][formation]")
{
    REQUIRE(predictAhead(Motion{ 5.0f, 0.0f, 0.0f }, false, 0.7f, 6.0f).ahead == 0.0f);
    REQUIRE(predictAhead(Motion{ 0.3f, 0.0f, 0.0f }, true, 0.7f, 6.0f).ahead == 0.0f);
    REQUIRE(predictAhead(Motion{ 5.0f, 0.0f, 0.0f }, true, 0.0f, 6.0f).ahead == 0.0f);
}

TEST_CASE("catchUpSpeed: normal within a yalm, full at the catch-up distance, a ramp between", "[cardian][formation]")
{
    REQUIRE_THAT(catchUpSpeed(0.5f, true, 107.0f, 135.0f, 3.0f), WithinAbs(107.0f, 0.001f));
    REQUIRE_THAT(catchUpSpeed(2.0f, true, 107.0f, 135.0f, 3.0f), WithinAbs(121.0f, 0.02f));
    REQUIRE_THAT(catchUpSpeed(3.0f, true, 107.0f, 135.0f, 3.0f), WithinAbs(135.0f, 0.02f));
    REQUIRE_THAT(catchUpSpeed(15.0f, true, 107.0f, 135.0f, 3.0f), WithinAbs(135.0f, 0.001f));
}

TEST_CASE("catchUpSpeed: never faster than normal while the player stands", "[cardian][formation]")
{
    REQUIRE_THAT(catchUpSpeed(15.0f, false, 107.0f, 135.0f, 3.0f), WithinAbs(107.0f, 0.001f));
}

TEST_CASE("catchUpSpeed: a catch-up distance under a yalm still ramps sanely", "[cardian][formation]")
{
    REQUIRE_THAT(catchUpSpeed(0.5f, true, 107.0f, 135.0f, 0.2f), WithinAbs(107.0f, 0.001f));
    REQUIRE_THAT(catchUpSpeed(5.0f, true, 107.0f, 135.0f, 0.2f), WithinAbs(135.0f, 0.001f));
}

// ---------------------------------------------------------------- aggro avoidance geometry

TEST_CASE("depthInside / insideAny: rim counts as outside", "[cardian][avoid]")
{
    const std::vector<Circle> circles{ { 0.0f, 0.0f, 8.0f } };

    REQUIRE(insideAny(circles, 5.0f, 0.0f));
    REQUIRE_FALSE(insideAny(circles, 8.0f, 0.0f));
    REQUIRE_FALSE(insideAny(circles, 9.0f, 0.0f));
    REQUIRE_THAT(depthInside(circles[0], 5.0f, 0.0f), WithinAbs(3.0f, 0.001f));
}

TEST_CASE("pushOut: a point inside is moved radially to the rim plus clearance", "[cardian][avoid]")
{
    const std::vector<Circle> circles{ { 0.0f, 0.0f, 8.0f } };

    const auto [x, z] = pushOut(circles, 3.0f, 4.0f, 0.1f); // 5 y in, along (0.6, 0.8)

    REQUIRE_THAT(std::hypot(x, z), WithinAbs(8.1f, 0.001f));
    REQUIRE_THAT(x / std::hypot(x, z), WithinAbs(0.6f, 0.001f));
    REQUIRE_THAT(z / std::hypot(x, z), WithinAbs(0.8f, 0.001f));
}

TEST_CASE("pushOut: a clear point is untouched, a centred point goes along +x", "[cardian][avoid]")
{
    const std::vector<Circle> circles{ { 0.0f, 0.0f, 8.0f } };

    REQUIRE(pushOut(circles, 20.0f, 0.0f) == std::pair{ 20.0f, 0.0f });

    const auto [x, z] = pushOut(circles, 0.0f, 0.0f, 0.1f);
    REQUIRE_THAT(x, WithinAbs(8.1f, 0.001f));
    REQUIRE_THAT(z, WithinAbs(0.0f, 0.001f));
}

TEST_CASE("pushOut: overlapping circles settle in a few passes", "[cardian][avoid]")
{
    const std::vector<Circle> circles{ { 0.0f, 0.0f, 8.0f }, { 12.0f, 0.0f, 8.0f } };

    const auto [x, z] = pushOut(circles, 6.0f, 1.0f, 0.1f); // inside both

    REQUIRE_FALSE(insideAny(circles, x, z));
}

TEST_CASE("safestAngleOnRing: the ideal angle when clear, the nearest clear angle otherwise", "[cardian][avoid]")
{
    const float pi   = std::numbers::pi_v<float>;
    const auto  ring = [](const float r)
    {
        return [r](const float a)
        {
            return std::pair{ r * std::cos(a), r * std::sin(a) };
        };
    };

    const std::vector<Circle> none{};
    REQUIRE_THAT(*safestAngleOnRing(none, 0.3f, ring(5.0f)), WithinAbs(0.3f, 0.0001f));

    // a danger sitting exactly where the ideal spot (5 y ahead along +x) is
    const std::vector<Circle> ahead{ { 5.0f, 0.0f, 3.0f } };
    const auto                a = safestAngleOnRing(ahead, 0.0f, ring(5.0f));
    REQUIRE(a.has_value());
    REQUIRE(std::fabs(*a) > 0.0f);
    REQUIRE(std::fabs(*a) < pi / 2.0f); // it stayed on the forward half
    REQUIRE_FALSE(insideAny(ahead, 5.0f * std::cos(*a), 5.0f * std::sin(*a)));
}

TEST_CASE("safestAngleOnRing: nothing when the whole ring is engulfed", "[cardian][avoid]")
{
    const std::vector<Circle> engulf{ { 0.0f, 0.0f, 50.0f } };
    REQUIRE_FALSE(safestAngleOnRing(engulf, 0.0f, [](const float a) { return std::pair{ 5.0f * std::cos(a), 5.0f * std::sin(a) }; }).has_value());
}

TEST_CASE("segmentCrosses: through, past, and ending inside", "[cardian][avoid]")
{
    const Circle c{ 10.0f, 0.0f, 3.0f };

    REQUIRE(segmentCrosses(c, 0.0f, 0.0f, 20.0f, 0.0f));       // straight through
    REQUIRE_FALSE(segmentCrosses(c, 0.0f, 5.0f, 20.0f, 5.0f)); // passes 5 y to the side
    REQUIRE(segmentCrosses(c, 0.0f, 0.0f, 10.0f, 0.0f));       // ends at the centre
    REQUIRE_FALSE(segmentCrosses(c, 0.0f, 0.0f, 6.0f, 0.0f));  // stops a yalm short
}

TEST_CASE("detourAround: the waypoint clears the circle on the side the path leans toward", "[cardian][avoid]")
{
    const Circle c{ 10.0f, 1.0f, 3.0f }; // centre slightly left of the +x path

    const auto [wx, wz] = detourAround(c, 0.0f, 0.0f, 20.0f, 0.0f, 0.5f);

    REQUIRE_THAT(planarDistance(c.x, c.z, wx, wz), WithinAbs(3.5f, 0.001f));
    REQUIRE(wz < c.z); // went round the right (the side away from the centre)
    REQUIRE_FALSE(segmentCrosses(c, 0.0f, 0.0f, wx, wz));
    REQUIRE_FALSE(segmentCrosses(c, wx, wz, 20.0f, 0.0f));
}
