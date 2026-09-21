/*
===========================================================================

  Copyright (c) 2010-2015 Darkstar Dev Teams
  Copyright (c) 2025 LandSandBoat Dev Teams

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

#include <atomic> // CARDIAN
#include <chrono>

#include "cbasetypes.h"
#include "earth_time.h"

// CARDIAN: the real-time clock domain -- the steady clock without timer::'s offset or hold. A
// distinct type, so its instants cannot mix with simulation time (RESEARCH.md 3.2).
namespace realtime
{

// Test-only: the harness's fast-forward (timer::add_offset) moves this. Zero on a live server.
inline std::atomic<std::chrono::steady_clock::rep> test_offset{ 0 };

struct clock
{
    using rep        = std::chrono::steady_clock::rep;
    using period     = std::chrono::steady_clock::period;
    using duration   = std::chrono::steady_clock::duration;
    using time_point = std::chrono::time_point<clock>;

    static constexpr bool is_steady = std::chrono::steady_clock::is_steady;

    static time_point now() noexcept
    {
        return time_point{ std::chrono::steady_clock::now().time_since_epoch() + duration{ test_offset.load(std::memory_order_relaxed) } };
    }
};

using duration   = clock::duration;
using time_point = clock::time_point;

inline time_point now() noexcept
{
    return clock::now();
}

inline void advance_for_tests(const duration& amount)
{
    test_offset.fetch_add(amount.count(), std::memory_order_relaxed);
}

}; // namespace realtime

namespace timer
{

// This clock is not stable across reboots.
// Use earth_time if you need real time.
// Use timer::to_utc/timer::from_utc to persist timestamps to the database (status effects).
using clock      = std::chrono::steady_clock;
using duration   = clock::duration;
using time_point = clock::time_point;

inline const time_point start_time = clock::now();

// CARDIAN: atomic -- every thread reads it while the test harness or a release() writes it.
inline std::atomic<duration::rep> time_offset{ 0 };

// CARDIAN: the hold -- zero while the clock runs, else the instant now() answers until release().
inline std::atomic<duration::rep> held_at{ 0 };

// CARDIAN: the simulation clock's offset from the steady clock (not meaningful while held).
inline duration get_offset()
{
    return duration{ time_offset.load() };
}

// CARDIAN: the simulation clock as it would read if nothing held it.
inline time_point now_running()
{
    return clock::now() + get_offset();
}

inline time_point now()
{
    // CARDIAN: held_at is read before the offset and release() writes them in the other order,
    // so a reader that finds the hold gone also finds the offset that continues it.
    if (const auto held = held_at.load(); held != 0)
    {
        return time_point{ duration{ held } };
    }

    return now_running();
}

// CARDIAN: is the simulation clock held. Safe from any thread.
inline bool is_held()
{
    return held_at.load() != 0;
}

inline duration get_uptime()
{
    return clock::now() - start_time;
}

// https://stackoverflow.com/questions/35282308/convert-between-c11-clocks/35282833#35282833
inline earth_time::time_point to_utc(const time_point& timer_tp = now())
{
    const auto utc_now   = earth_time::now();
    const auto timer_now = timer::now();
    return std::chrono::time_point_cast<earth_time::duration>(timer_tp - timer_now + utc_now);
};

inline time_point from_utc(const earth_time::time_point& utc_tp = earth_time::now())
{
    const auto timer_now = timer::now();
    const auto utc_now   = earth_time::now();
    return utc_tp - utc_now + timer_now;
};

// CARDIAN: a simulation instant on the game clock's Unix scale, for Lua to set beside GetSystemTime().
inline earth_time::time_point to_game_utc(const time_point& timer_tp)
{
    return std::chrono::time_point_cast<earth_time::duration>(timer_tp - timer::now() + earth_time::game_now());
}

// CARDIAN: moves the simulation clock alone (release() and tests); real time is not its to move.
inline void add_simulation_offset(const duration& additional_offset)
{
    time_offset.fetch_add(additional_offset.count());
}

// CARDIAN: stop the simulation clock where it stands. Main thread only; no-op if already held.
inline void hold()
{
    if (!is_held())
    {
        held_at.store(now_running().time_since_epoch().count());
    }
}

// CARDIAN: restart the clock from the instant it was held at, absorbing whatever went by.
// Main thread only; no-op if not held.
inline void release()
{
    if (const auto held = held_at.load(); held != 0)
    {
        add_simulation_offset(duration{ held } - now_running().time_since_epoch());
        held_at.store(0);
    }
}

inline void add_offset(const duration& additional_offset)
{
    // CARDIAN: only the test harness calls this ("time went by"), so it moves both clock domains.
    add_simulation_offset(additional_offset);
    realtime::advance_for_tests(additional_offset);
}

inline void reset_offset()
{
    // CARDIAN: undoes add_offset, so it clears both domains.
    time_offset.store(0);
    realtime::test_offset.store(0, std::memory_order_relaxed);
}

// Gets the Earth milliseconds of a duration.
template <typename Rep, typename Period>
auto count_milliseconds(const std::chrono::duration<Rep, Period>& d) -> int64
{
    return std::chrono::floor<std::chrono::milliseconds>(d).count();
};

// Gets the Earth seconds of a duration.
template <typename Rep, typename Period>
auto count_seconds(const std::chrono::duration<Rep, Period>& d) -> int64
{
    return std::chrono::floor<std::chrono::seconds>(d).count();
};

}; // namespace timer
