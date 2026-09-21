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

// A held game clock answers its instant to within the two reads of the real clock it
// takes to say so, and a row carries milliseconds: a second is the neighbouring cases'
// allowance, and far below the minutes these cases let go by.
bool standsAt(const earth_time::time_point instant)
{
    return std::chrono::abs(earth_time::game_now() - instant) < 1s;
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

TEST_CASE("calendar: a real instant handed to Lua is as far back on its clock as it really is", "[cardian][calendar]")
{
    const CalendarGuard guard;

    const auto created = earth_time::now() - 72h; // a character made three days ago

    earth_time::hold_calendar();
    earth_time::add_offset(2h);
    earth_time::release_calendar();

    // Lua's age arithmetic, GetSystemTime() - getTimeCreated(): three days and the two hours, held or not
    const auto age = earth_time::game_now() - earth_time::to_game(created);
    REQUIRE(age >= 74h);
    REQUIRE(age < 74h + 1s);

    // an instant nobody set is not moved: Lua tests it against zero
    REQUIRE(earth_time::to_game(earth_time::time_point{}) == earth_time::time_point{});
    REQUIRE(earth_time::to_game(earth_time::time_point::min()) == earth_time::time_point::min());
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

// A process that follows the game clock by the saved row (common/cardian_clock_row.h,
// pause P7d). Owner and follower are one process here: the owner's row is taken, the
// calendar is put back level with real time as a follower's starts, and the follower
// is handed the row.

TEST_CASE("clock row: a follower of a running row reads the owner's game clock, however old the row", "[cardian][calendar]")
{
    const CalendarGuard guard;

    earth_time::set_calendar_drift(10min);
    const auto row = cardian::clock_row::snapshot();
    REQUIRE_FALSE(row.held);

    earth_time::set_calendar_drift(0s);
    cardian::clock_row::follow(row, earth_time::now(), true);
    REQUIRE(std::chrono::abs(earth_time::now() - earth_time::game_now() - 10min) < 1s);

    // an hour on, nobody has written: the row has not gone stale
    earth_time::add_offset(1h);
    cardian::clock_row::follow(row, earth_time::now(), true);
    REQUIRE(std::chrono::abs(earth_time::now() - earth_time::game_now() - 10min) < 1s);
}

TEST_CASE("clock row: a follower stands still with a held row and carries on with the release's", "[cardian][calendar]")
{
    using cardian::clock_row::follow;
    using cardian::clock_row::fromMs;
    using cardian::clock_row::snapshot;

    const CalendarGuard guard;

    earth_time::hold_calendar();
    const auto heldRow = snapshot();
    REQUIRE(heldRow.held);
    const auto ownerState = earth_time::calendar_state.load();

    earth_time::set_calendar_drift(0s);
    follow(heldRow, earth_time::now(), true);
    REQUIRE(standsAt(fromMs(heldRow.gameMs)));

    // half an hour into the hold, the owner's heartbeat keeps the row written
    earth_time::add_offset(30min);
    const cardian::clock_row::Row heartbeat{ cardian::clock_row::toMs(earth_time::now()), heldRow.gameMs, true };
    follow(heartbeat, earth_time::now(), true);
    REQUIRE(standsAt(fromMs(heldRow.gameMs)));

    // the owner lets go; the follower, still on the old row, is handed the new one
    earth_time::calendar_state.store(ownerState);
    earth_time::release_calendar();
    const auto ownerGame  = earth_time::game_now();
    const auto runningRow = snapshot();
    REQUIRE_FALSE(runningRow.held);

    earth_time::hold_calendar_at(fromMs(heldRow.gameMs));
    follow(runningRow, earth_time::now(), true);
    REQUIRE(std::chrono::abs(earth_time::game_now() - ownerGame) < 1s);
    REQUIRE(earth_time::now() - earth_time::game_now() > 30min - 1s);
}

TEST_CASE("clock row: a row nobody writes any more is read as the owner's boot will read it", "[cardian][calendar]")
{
    using cardian::clock_row::Row;
    using cardian::clock_row::stands;

    const Row  running{ 1'800'000'600'000, 1'800'000'000'000, false };
    const Row  held{ running.realMs, running.gameMs, true };
    const auto written = cardian::clock_row::fromMs(running.realMs);

    // a heartbeat late: the owner is slow, not off, and the row says
    REQUIRE_FALSE(stands(running, written + 2min, false));
    REQUIRE(stands(held, written + 2min, true));

    // the owner is off. Time standing still while off: it will carry on from game_ms
    REQUIRE(stands(running, written + 4min, false));
    REQUIRE(stands(held, written + 4min, false));

    // the owner is off. Its time goes by while off -- also for an owner that died in a hold
    REQUIRE_FALSE(stands(running, written + 4min, true));
    REQUIRE_FALSE(stands(held, written + 4min, true));

    const CalendarGuard guard;
    cardian::clock_row::follow(running, written + 4min, false);
    REQUIRE(standsAt(cardian::clock_row::fromMs(running.gameMs)));
}

TEST_CASE("clock row: a follower's late look at the row fires no hourly tick twice and loses none", "[cardian][calendar]")
{
    // The check xi_world's time_server makes every tick, on the instant it is handed.
    auto       fired = 0;
    const auto hour  = earth_time::time_point(1'800'000'000s); // on the hour
    auto       next  = hour;
    const auto tick  = [&](const earth_time::time_point gameTime)
    {
        if (gameTime >= next)
        {
            ++fired;
            next = std::chrono::ceil<std::chrono::hours>(gameTime);
        }
    };

    // A hold taken just short of the hour, seen one tick late: the follower ran on past
    // the hour, then its view steps back to the held instant, and later crosses again.
    tick(hour - 2s);
    tick(hour + 400ms);
    REQUIRE(fired == 1);
    tick(hour - 500ms);
    tick(hour - 500ms);
    tick(hour + 1900ms);
    REQUIRE(fired == 1);

    // A release seen late: the view stands, then jumps across the next hour.
    tick(hour + 1h - 1s);
    tick(hour + 1h + 1400ms);
    REQUIRE(fired == 2);
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
