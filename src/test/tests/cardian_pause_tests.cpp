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

// The combat pause's manager (map/pause/pause.h): who holds, for how long, and the
// refusals. The clock underneath it is pinned by cardian_clock_tests.cpp, and what a
// hold does to a live scene by scripts/tests/cardian/pause.lua.

#include <catch2/catch_test_macros.hpp>

#include "cardian_clock_guard.h"
#include "common/timer.h"
#include "map/pause/pause.h"

using namespace std::chrono_literals;
using cardian::pause::Result;

TEST_CASE("pause: a hold is taken once and let go once", "[cardian][pause]")
{
    const ClockGuard guard;

    REQUIRE_FALSE(cardian::pause::isHeld());
    REQUIRE(cardian::pause::release("nothing to release") == Result::NotHeld);

    REQUIRE(cardian::pause::hold(7, "Tester") == Result::Ok);
    REQUIRE(cardian::pause::isHeld());
    REQUIRE(timer::is_held());

    // A second taker is refused and the first keeps it.
    REQUIRE(cardian::pause::hold(8, "Latecomer") == Result::AlreadyHeld);
    REQUIRE(cardian::pause::status().holder == 7);
    REQUIRE(cardian::pause::status().holderName == "Tester");

    REQUIRE(cardian::pause::release("done") == Result::Ok);
    REQUIRE_FALSE(cardian::pause::isHeld());
    REQUIRE(cardian::pause::release("done twice") == Result::NotHeld);
}

TEST_CASE("pause: ten minutes held cost a cast nothing", "[cardian][pause]")
{
    const ClockGuard guard;

    const auto castFinishesAt = timer::now() + 60s;

    REQUIRE(cardian::pause::hold(0, "a test") == Result::Ok);
    timer::add_offset(10min);

    // Held: the clock has not reached the cast, and the book knows how long it has been.
    REQUIRE(timer::now() < castFinishesAt);
    REQUIRE(std::chrono::abs(cardian::pause::status().heldFor - 10min) < 1s);

    REQUIRE(cardian::pause::release("done") == Result::Ok);
    REQUIRE(std::chrono::abs((castFinishesAt - timer::now()) - 60s) < 1s);
}

TEST_CASE("pause: the book counts holds and the time they took", "[cardian][pause]")
{
    const ClockGuard guard;

    const auto before = cardian::pause::status();

    for (int i = 0; i < 3; ++i)
    {
        REQUIRE(cardian::pause::hold(0, "a test") == Result::Ok);
        timer::add_offset(5min);
        REQUIRE(cardian::pause::release("done") == Result::Ok);
    }

    const auto after = cardian::pause::status();
    REQUIRE_FALSE(after.held);
    REQUIRE(after.holds == before.holds + 3);
    REQUIRE(std::chrono::abs((after.heldTotal - before.heldTotal) - 15min) < 1s);
    REQUIRE(after.holder == 0);
    REQUIRE(after.holderName.empty());
}

TEST_CASE("pause: a hold lets go when its holder is offline, and only then", "[cardian][pause]")
{
    const ClockGuard guard;

    // Nothing held: nothing to let go.
    REQUIRE_FALSE(cardian::pause::letGoIfHolderLeft());

    // Held by nobody in particular: there is no holder to go offline.
    REQUIRE(cardian::pause::hold(0, "the system") == Result::Ok);
    REQUIRE_FALSE(cardian::pause::letGoIfHolderLeft());
    REQUIRE(cardian::pause::isHeld());
    REQUIRE(cardian::pause::release("done") == Result::Ok);

    // Held by a character who is not online (no session, no body anywhere).
    constexpr uint32 nobodyHome = 4242424;
    REQUIRE(cardian::pause::hold(nobodyHome, "Ghost") == Result::Ok);
    REQUIRE(cardian::pause::letGoIfHolderLeft());
    REQUIRE_FALSE(cardian::pause::isHeld());
}
