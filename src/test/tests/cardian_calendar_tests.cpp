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
#include "map/pause/calendar_store.h"

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

// The game clock as Unix time (pause P7): what Lua is told the time is.

TEST_CASE("calendar: the game clock in Unix seconds is the Vana'diel calendar on Earth's scale", "[cardian][calendar]")
{
    const CalendarGuard guard;

    earth_time::hold_calendar();
    earth_time::add_offset(10min);
    earth_time::release_calendar();

    // behind real time by the pause
    const auto behind = earth_time::now() - earth_time::game_now();
    REQUIRE(behind >= 10min);
    REQUIRE(behind < 10min + 1s);

    // and the same instant as the game timestamp the client is told
    const auto sinceEpoch = std::chrono::floor<std::chrono::seconds>(earth_time::game_now() - earth_time::vanadiel_epoch).count();
    REQUIRE(std::abs(sinceEpoch - gameSeconds()) <= 1);

    // standing still in a hold
    earth_time::hold_calendar();
    const auto heldAt = earth_time::game_now();
    earth_time::add_offset(1h);
    REQUIRE(std::chrono::abs(earth_time::game_now() - heldAt) < 1s);
}

TEST_CASE("calendar: a JST midnight of the game clock is a Vana'diel midnight, whatever the drift", "[cardian][calendar]")
{
    const CalendarGuard guard;

    earth_time::set_calendar_drift(3h + 17min + 5s);

    const auto midnight = earth_time::jst::get_next_midnight(earth_time::game_now());

    // a whole number of Earth days from the epoch, each of them 25 Vana'diel days
    REQUIRE((midnight - earth_time::vanadiel_epoch) % std::chrono::days(1) == 0s);

    // and on the Vana'diel clock, which converts from real instants: the game's midnight, the drift later
    const auto drift = earth_time::now() - earth_time::game_now();
    const auto vana  = vanadiel_time::from_earth_time(midnight + drift);
    const auto off   = std::chrono::abs(vana - std::chrono::round<xi::vanadiel_clock::days>(vana));
    REQUIRE(off < xi::vanadiel_clock::minutes(1));
}

TEST_CASE("calendar: the saved pair gives the drift at boot under either rule for time off", "[cardian][calendar]")
{
    using cardian::pause::calendar::driftAtBoot;
    using cardian::pause::calendar::Saved;

    // written with the game 10 minutes behind; the server then stayed off for 2 hours
    const Saved saved{ 1'800'000'600'000, 1'800'000'000'000 };
    const auto  boot = earth_time::time_point(std::chrono::milliseconds(saved.realMs) + 2h);

    REQUIRE(driftAtBoot(saved, boot, true) == 10min);       // its time went by
    REQUIRE(driftAtBoot(saved, boot, false) == 2h + 10min); // it carries on from where it stopped

    // a real clock behind the row counts as no time off
    const auto earlier = earth_time::time_point(std::chrono::milliseconds(saved.realMs) - 1h);
    REQUIRE(driftAtBoot(saved, earlier, false) == 10min);
}

TEST_CASE("calendar: a row written in the middle of a hold keeps the hold so far", "[cardian][calendar]")
{
    const CalendarGuard guard;

    earth_time::hold_calendar();
    earth_time::add_offset(30min);

    const auto saved = cardian::pause::calendar::snapshot();
    const auto drift = cardian::pause::calendar::driftAtBoot(saved, earth_time::now(), true);
    REQUIRE(drift >= 30min);
    REQUIRE(drift < 30min + 1s);
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
