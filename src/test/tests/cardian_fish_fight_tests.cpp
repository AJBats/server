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

#include "map/pawn/fish_fight.h"

#include <catch2/catch_test_macros.hpp>

using cardian::fishfight::Fish;
using cardian::fishfight::fight;
using cardian::fishfight::Tuning;

namespace
{
    constexpr Tuning kTuning{ .arrowSeconds = 2.0f, .slowdown = 0.15f };
} // namespace

TEST_CASE("Fish fight: a fish at her skill is landed, an arrow at a time", "[cardian][fishing]")
{
    // stamina 2300, 200 a right arrow: 12 arrows, 24 seconds
    const auto out = fight({ .stamina = 2300, .attack = 200, .regen = 128, .timeLimit = 60 }, kTuning);
    CHECK(out.won);
    CHECK(out.seconds == 24);
}

TEST_CASE("Fish fight: a fish that drains is no faster, and a large one's extra point is neutral", "[cardian][fishing]")
{
    CHECK(fight({ .stamina = 1000, .attack = 100, .regen = 120, .timeLimit = 60 }, kTuning).seconds == 20);
    CHECK(fight({ .stamina = 1000, .attack = 100, .regen = 129, .timeLimit = 60, .large = true }, kTuning).seconds == 20);
    CHECK(fight({ .stamina = 1000, .attack = 100, .regen = 129, .timeLimit = 60 }, kTuning).seconds > 20);
}

TEST_CASE("Fish fight: a fish far over her skill slows her, then gets away", "[cardian][fishing]")
{
    const auto easy = fight({ .stamina = 2000, .attack = 200, .regen = 128, .timeLimit = 60 }, kTuning);
    const auto hard = fight({ .stamina = 2000, .attack = 200, .regen = 130, .timeLimit = 60 }, kTuning);
    REQUIRE(easy.won);
    REQUIRE(hard.won);
    CHECK(hard.seconds > easy.seconds);

    // at 7 points past neutral her pace is gone: it gets away at its time limit
    const auto beyond = fight({ .stamina = 2000, .attack = 200, .regen = 135, .timeLimit = 60 }, kTuning);
    CHECK_FALSE(beyond.won);
    CHECK(beyond.seconds == 60);
}

TEST_CASE("Fish fight: a fight longer than the fish's time limit gets away at the limit", "[cardian][fishing]")
{
    const auto out = fight({ .stamina = 9000, .attack = 100, .regen = 128, .timeLimit = 60 }, kTuning);
    CHECK_FALSE(out.won);
    CHECK(out.seconds == 60);
}

TEST_CASE("Fish fight: a fight is at least one arrow, and a rod that does no damage lands nothing", "[cardian][fishing]")
{
    CHECK(fight({ .stamina = 10, .attack = 500, .regen = 128, .timeLimit = 60 }, kTuning).seconds == 2);
    CHECK(fight({ .stamina = 0, .attack = 500, .regen = 128, .timeLimit = 60 }, kTuning).seconds == 2);
    CHECK_FALSE(fight({ .stamina = 100, .attack = 0, .regen = 128, .timeLimit = 60 }, kTuning).won);
}
