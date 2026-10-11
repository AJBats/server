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
#include "common/settings.h"

#include <chrono>
#include <optional>

// The conquest tally's schedule (RESEARCH §20.8), shared by xi_world, which runs the
// tally, and the map, which tells the client the days to it and runs the crowd's
// simulation by its periods. Every instant here is on the game clock: an Earth instant
// on the held, drifted calendar (earth_time::game_now(), timer::to_game_utc()), so a
// pause holds the tally and the countdown with it.
//
// cardian.CONQUEST_TALLY_DAYS sets it. 0 keeps retail's tally, Monday midnight JST on
// the game clock. A number of Vana'diel days tallies every that many days, at Vana'diel
// midnight, counted from the Vana'diel epoch: 3 is every 2 h 52 min 48 s.
//
// Only region control and the client's countdown follow it. The lockouts Lua keys to
// NextConquestTally() -- the experience rings' recharge, supply runs, Garrison,
// Expeditionary Force, the era quests' weekly limits -- keep the JST week.
namespace cardian::conquest_clock
{

// A Vana'diel day and hour on the Earth clock: 24 hours of 2.4 minutes.
constexpr auto kVanaDay  = std::chrono::milliseconds(3456000);
constexpr auto kVanaHour = std::chrono::milliseconds(144000);

inline auto tallyDays() -> uint32
{
    return settings::get<uint32>("cardian.CONQUEST_TALLY_DAYS");
}

// A period's length: that many Vana'diel days, or the JST week.
inline auto periodLength(const uint32 days) -> earth_time::duration
{
    if (days == 0)
    {
        return std::chrono::duration_cast<earth_time::duration>(std::chrono::days(7));
    }
    return std::chrono::duration_cast<earth_time::duration>(kVanaDay * days);
}

// The first tally strictly after this game instant.
inline auto nextTally(const earth_time::time_point game, const uint32 days) -> earth_time::time_point
{
    if (days == 0)
    {
        return earth_time::get_next_game_week(game);
    }
    const auto period = periodLength(days);
    const auto since  = game - earth_time::vanadiel_epoch;
    return earth_time::vanadiel_epoch + period * (since / period + 1);
}

// The tally that opened the period this game instant is in.
inline auto lastTally(const earth_time::time_point game, const uint32 days) -> earth_time::time_point
{
    return nextTally(game, days) - periodLength(days);
}

// Whole Vana'diel hours since the period began.
inline auto hoursIntoPeriod(const earth_time::time_point game, const uint32 days) -> int64
{
    return std::chrono::floor<std::chrono::milliseconds>(game - lastTally(game, days)) / kVanaHour;
}

// The short tally's trigger, for xi_world's tick. due() answers yes once as the game
// clock passes each tally. Never at its first look: the process has just started, the
// tallies that fell while it was off do not happen, and the next one counts everything
// since the last. Never twice for one tally: it keeps the latest period it has seen, so
// a game clock set back across a tally and carried forward again finds that tally done.
// With the retail week (days 0) it is never due: the Monday tick keeps that one.
class TallyWatch
{
public:
    bool due(const earth_time::time_point game, const uint32 days)
    {
        if (days == 0)
        {
            seen_.reset();
            return false;
        }
        const auto next = nextTally(game, days);
        if (!seen_.has_value())
        {
            seen_ = next;
            return false;
        }
        if (next <= *seen_)
        {
            return false;
        }
        seen_ = next;
        return true;
    }

private:
    std::optional<earth_time::time_point> seen_;
};

} // namespace cardian::conquest_clock
