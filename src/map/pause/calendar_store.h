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

#pragma once

#include "common/cbasetypes.h"
#include "common/earth_time.h"

// The game clock, kept across a restart.
//
// The game clock (earth_time::game_now) is real time less the drift: every pause so
// far. Scripts keep lockouts and NM windows in its seconds, and some of those are
// saved, so a restart that forgot the drift would jump the game forward by all of it.
// One row of `cardian_clock` therefore says what both clocks read at the same moment:
// real time and game time. A pair, not the bare drift, because either policy for the
// time the server was off reads off it, a crash in the middle of a hold included:
//
//   cardian.CLOCK_RUNS_OFFLINE = true   the drift is what it was at the last write:
//                                       the game's time went by while it was off
//   cardian.CLOCK_RUNS_OFFLINE = false  the game carries on from the second it
//                                       stopped: the time it was off joins the drift
//
// Flipping the setting between two runs migrates nothing. "Off" means this process
// was not running, not that no player was logged in. Main thread only.
namespace cardian::pause::calendar
{

struct Saved
{
    int64 realMs = 0; // real Unix time of the write
    int64 gameMs = 0; // what the game clock read at that moment
};

// What both clocks read now.
auto snapshot() -> Saved;

// The drift to start with, from the last row written. A real clock that reads earlier
// than the row (a restored backup, a corrected clock) counts as no time off at all.
auto driftAtBoot(const Saved& saved, earth_time::time_point realNow, bool runsOffline) -> earth_time::duration;

// Reads the row and sets the calendar's drift, then arms save(). Called once, after
// the database connects and before anything reads the calendar. The test server
// never calls it: it shares a database with dev and its clocks are fast-forwarded.
void load();

// Writes the row: at every release, once a minute, and as the map shuts down, so a
// clean stop loses nothing and a crash at most a minute. Nothing until load() has run.
void save();

// save(), when a minute of real time has gone by since the last one. For a tick that
// keeps coming while held.
void saveIfDue();

} // namespace cardian::pause::calendar
