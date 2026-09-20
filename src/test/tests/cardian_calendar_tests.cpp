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

// The calendar under the combat pause (common/earth_time.h, pause P6).
//
// Vana'diel time is real time since an epoch. A hold stops it where it stands and a
// release carries it on from there, so after a pause the calendar runs behind real
// time by the pause's length: the drift. Everything that reads the calendar -- the
// Vana'diel clock, the game timestamp the client is told, the conversions between the
// two calendars -- reads it through the one drifted epoch, and real time is never
// moved: sessions live on it. Real time going by is earth_time::add_offset here.

#include <catch2/catch_test_macros.hpp>

#include "common/earth_time.h"
#include "common/vana_time.h"

using namespace std::chrono_literals;

namespace
{

// The calendar's state and the Earth clock's offset are process-global, and the Lua
// suite runs after these cases in the same process.
struct CalendarGuard
{
    earth_time::duration      savedOffset = earth_time::time_offset;
    earth_time::duration::rep savedState  = earth_time::calendar_state.load();

    CalendarGuard()                                = default;
    CalendarGuard(const CalendarGuard&)            = delete;
    CalendarGuard& operator=(const CalendarGuard&) = delete;

    ~CalendarGuard()
    {
        earth_time::calendar_state.store(savedState);
        earth_time::time_offset = savedOffset;
    }
};

auto gameSeconds() -> int64
{
    return earth_time::vanadiel_timestamp();
}

} // namespace

TEST_CASE("calendar: it stands still through a hold while real time goes by", "[cardian][calendar]")
{
    const CalendarGuard guard;

    const auto realBefore = earth_time::now();
    const auto gameBefore = gameSeconds();
    const auto vanaBefore = vanadiel_time::now();

    earth_time::hold_calendar();
    earth_time::add_offset(10min);

    REQUIRE(earth_time::now() - realBefore >= 10min);
    REQUIRE(gameSeconds() - gameBefore <= 1);
    REQUIRE(vanadiel_time::now() - vanaBefore < xi::vanadiel_clock::minutes(1));
}

TEST_CASE("calendar: it carries on from where it stopped, never catching up", "[cardian][calendar]")
{
    const CalendarGuard guard;

    const auto gameBefore = gameSeconds();

    earth_time::hold_calendar();
    earth_time::add_offset(10min);
    earth_time::release_calendar();

    REQUIRE(gameSeconds() - gameBefore <= 1);

    // and it runs again at its own pace
    earth_time::add_offset(30s);
    const auto ran = gameSeconds() - gameBefore;
    REQUIRE(ran >= 30);
    REQUIRE(ran <= 31);
}

TEST_CASE("calendar: holds add up, and a second hold() or a stray release() changes nothing", "[cardian][calendar]")
{
    const CalendarGuard guard;

    const auto gameBefore = gameSeconds();

    earth_time::release_calendar(); // not held: nothing to let go of

    earth_time::hold_calendar();
    earth_time::add_offset(5min);
    earth_time::hold_calendar(); // already held: the first instant stands
    earth_time::add_offset(5min);
    earth_time::release_calendar();

    earth_time::add_offset(1min);

    earth_time::hold_calendar();
    earth_time::add_offset(1h);
    earth_time::release_calendar();

    const auto ran = gameSeconds() - gameBefore;
    REQUIRE(ran >= 60);
    REQUIRE(ran <= 61);
}

TEST_CASE("calendar: the two calendars still convert into each other, held and drifted", "[cardian][calendar]")
{
    const CalendarGuard guard;

    earth_time::hold_calendar();
    earth_time::add_offset(10min);
    earth_time::release_calendar();

    // An instant an hour of game time ahead, there and back
    const auto ahead = vanadiel_time::now() + xi::vanadiel_clock::hours(1);
    const auto back  = vanadiel_time::from_earth_time(vanadiel_time::to_earth_time(ahead));
    REQUIRE(std::chrono::abs(back - ahead) < xi::vanadiel_clock::seconds(1));

    // and a real instant 144 s from now (an hour of game time) is an hour ahead on the game's clock
    const auto inGameSeconds = static_cast<int64>(earth_time::vanadiel_timestamp(earth_time::now() + 144s)) - gameSeconds();
    REQUIRE(inGameSeconds >= 143);
    REQUIRE(inGameSeconds <= 145);
}

TEST_CASE("calendar: real time is never moved", "[cardian][calendar]")
{
    const CalendarGuard guard;

    const auto realBefore = earth_time::now();

    earth_time::hold_calendar();
    earth_time::release_calendar();

    REQUIRE(std::chrono::abs(earth_time::now() - realBefore) < 1s);
    REQUIRE(earth_time::timestamp() - earth_time::timestamp(realBefore) <= 1);
}
