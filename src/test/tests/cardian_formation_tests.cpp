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
