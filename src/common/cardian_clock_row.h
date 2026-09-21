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
#include "common/database.h"
#include "common/earth_time.h"
#include "common/logging.h"
#include "common/settings.h"

#include <algorithm>
#include <chrono>
#include <optional>
#include <string>

// The one row of `cardian_clock`: what the real clock and the game clock read at the
// same moment, and whether the game clock was held then. It is the shared truth about
// the game clock between processes. One process owns the clock and writes the row at
// every change of behaviour -- a hold, a release, its boot and its shutdown -- and once
// a minute as a heartbeat; any other process follows it by reading the row and
// computing the game clock from its own real clock:
//
//   held      the game clock stands at game_ms
//   running   the game clock is real time less (real_ms - game_ms)
//
// so between two writes the row never goes stale, and a follower keeps no clock of its
// own for an error to build up in. Today xi_map owns the clock and xi_world follows.
//
// The time the owner was off is cardian.CLOCK_RUNS_OFFLINE's to decide:
//
//   true    the game's time went by while it was off: the drift is what the row says
//   false   the game carries on from the last game_ms written: the time off joins the
//           drift
//
// A follower cannot see the owner stop, so a row nobody has written for kOwnerSilent
// means the owner is off, and the follower reads the row as the owner's boot will:
// running by the row's drift under true, even if the row says held (the owner died in
// a hold); standing at game_ms under false.
//
// Flipping the setting between two runs migrates nothing.
namespace cardian::clock_row
{

struct Row
{
    int64 realMs = 0;     // real Unix time of the write
    int64 gameMs = 0;     // what the game clock read at that moment
    bool  held   = false; // the game clock stood still at that moment
};

// The heartbeat is a minute: three missed means the owner is off, not slow.
constexpr auto kOwnerSilent = std::chrono::minutes(3);

inline auto toMs(const earth_time::time_point tp) -> int64
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(tp.time_since_epoch()).count();
}

inline auto fromMs(const int64 ms) -> earth_time::time_point
{
    return earth_time::time_point{ std::chrono::duration_cast<earth_time::duration>(std::chrono::milliseconds(ms)) };
}

// What this process's clocks read now.
inline auto snapshot() -> Row
{
    return Row{ toMs(earth_time::now()), toMs(earth_time::game_now()), (earth_time::calendar_state.load() & 1) != 0 };
}

// The owner's drift to start with, from the last row written. A real clock that reads
// earlier than the row (a restored backup, a corrected clock) counts as no time off.
inline auto driftAtBoot(const Row& row, const earth_time::time_point realNow, const bool runsOffline) -> earth_time::duration
{
    const auto atWrite = std::chrono::milliseconds(row.realMs - row.gameMs);
    const auto off     = std::max<int64>(0, toMs(realNow) - row.realMs);
    return atWrite + std::chrono::milliseconds(runsOffline ? 0 : off);
}

// Does a follower take the game clock as standing at the row's game_ms? While the
// owner writes, the row says; once it has fallen silent it is off, and the follower
// reads the row as the owner's boot will (driftAtBoot), held or not.
inline bool stands(const Row& row, const earth_time::time_point realNow, const bool runsOffline)
{
    const bool ownerOff = realNow - fromMs(row.realMs) > kOwnerSilent;
    return ownerOff ? !runsOffline : row.held;
}

// Sets this process's calendar to what the row says. For a follower, every tick.
inline void follow(const Row& row, const earth_time::time_point realNow, const bool runsOffline)
{
    if (stands(row, realNow, runsOffline))
    {
        earth_time::hold_calendar_at(fromMs(row.gameMs));
    }
    else
    {
        earth_time::set_calendar_drift(std::chrono::milliseconds(row.realMs - row.gameMs));
    }
}

// The owner makes sure of its own table before it reads it: module SQL is skipped by
// dbtool on an up-to-date database, and a boot that cannot read the row forgets the
// drift. The same two statements as modules/cardian/sql/cardian_clock.sql.
inline void ensureTable()
{
    db::preparedStmt("CREATE TABLE IF NOT EXISTS `cardian_clock` ("
                     "`id` tinyint(3) unsigned NOT NULL, "
                     "`real_ms` bigint(20) NOT NULL, "
                     "`game_ms` bigint(20) NOT NULL, "
                     "`held` tinyint(1) NOT NULL DEFAULT 0, "
                     "PRIMARY KEY (`id`)) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_general_ci");
    db::preparedStmt("ALTER TABLE `cardian_clock` ADD COLUMN IF NOT EXISTS `held` tinyint(1) NOT NULL DEFAULT 0");
}

// The row, if it has been written. `failed` is told when the query itself failed: no
// table, one from before the `held` column, or a database that did not answer.
inline auto read(bool* failed = nullptr) -> std::optional<Row>
{
    const auto rset = db::preparedStmt("SELECT real_ms, game_ms, held FROM cardian_clock WHERE id = 1");
    if (failed)
    {
        *failed = !rset;
    }

    if (!rset || !rset->next())
    {
        return std::nullopt;
    }

    return Row{ rset->get<int64>("real_ms"), rset->get<int64>("game_ms"), rset->get<uint8>("held") != 0 };
}

// As text: the statement binder carries no 64-bit integer, and the server converts exactly.
inline void write(const Row& row)
{
    db::preparedStmt("REPLACE INTO cardian_clock (id, real_ms, game_ms, held) VALUES (1, ?, ?, ?)",
                     std::to_string(row.realMs),
                     std::to_string(row.gameMs),
                     row.held ? 1 : 0);
}

// Set by a process whose calendar is its own to follow with: xi_world proper. Never
// the test server, which holds the world engine and the map engine on one calendar.
inline bool isFollower = false;

// A follower's whole tick: read the row and set the calendar by it. With no row the
// calendar stays as it is, level with real time in a process that never set it.
// A read that fails says so and is asked again in a minute, never given up on: the
// calendar meanwhile stays where the last row put it.
inline void followSaved()
{
    static earth_time::time_point askAgainAt{};
    if (!isFollower || earth_time::now() < askAgainAt)
    {
        return;
    }

    bool failed = false;
    if (const auto row = read(&failed))
    {
        follow(*row, earth_time::now(), settings::get<bool>("cardian.CLOCK_RUNS_OFFLINE"));
    }
    else if (failed)
    {
        ShowWarning("clock row: cardian_clock could not be read (the map makes the table as it boots): this process's game clock stays as it is, asking again in a minute");
        askAgainAt = earth_time::now() + std::chrono::minutes(1);
    }
}

} // namespace cardian::clock_row
