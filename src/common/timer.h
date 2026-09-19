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

// CARDIAN: the two clock domains.
//
// timer:: (below) is the SIMULATION clock: it carries an offset, so gameplay
// deadlines (casts, recasts, status effects, respawns, AI) can be moved as a body --
// the test harness does this with sim:skipTime(), and it is what lets a simulation
// pause hold gameplay still. Everything scheduled against timer::time_point keeps its remaining time
// when that offset moves, which is the property that makes those features possible.
//
// realtime:: is the steady clock without that offset, for work that must keep
// measuring real elapsed time no matter what the simulation is doing: the
// inactivity watchdog, the Cardian Link's keepalive, packet flood control, log
// throttles and query timing. Freezing gameplay must never convince a watchdog that
// a genuinely wedged thread just updated. (Client session liveness is a third
// domain: it rides earth_time, the wall clock, which a simulation pause never
// touches.)
//
// The two are DELIBERATELY different types. realtime::time_point and
// timer::time_point are both steady-clock based, but a distinct clock type makes
// them distinct time_points, so putting a simulation timestamp in a liveness path
// (or the reverse) is a compile error rather than a bug that only shows up while
// paused. Durations are shared -- only the instants are domain-specific.
namespace realtime
{

// Test-only. How the test harness says "real time went by" without waiting for it:
// timer::add_offset, the harness's fast-forward, moves both domains. Nothing in a
// live server writes this, so there realtime is exactly the steady clock.
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

// CARDIAN: the offset is read from every thread that asks the time, and
// add_simulation_offset exists so that it can be written while they do, so it is
// atomic rather than a plain duration. Relaxed ordering: readers need the value to
// be whole, not ordered against other memory. (GM time commands do not come through
// here: !addtime moves earth_time's offset.)
inline std::atomic<duration::rep> time_offset{ 0 };

// CARDIAN: one read of the atomic offset, for now() and for callers that need to
// know how far the simulation clock currently sits from the steady clock.
inline duration get_offset()
{
    return duration{ time_offset.load(std::memory_order_relaxed) };
}

inline time_point now()
{
    return clock::now() + get_offset(); // CARDIAN: reads the atomic offset
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

// CARDIAN: moves the simulation clock alone: the entry for giving a held simulation
// its time back at resume. Real time is not this function's to move.
inline void add_simulation_offset(const duration& additional_offset)
{
    time_offset.fetch_add(additional_offset.count(), std::memory_order_relaxed);
}

inline void add_offset(const duration& additional_offset)
{
    // CARDIAN: upstream's fast-forward, called only by the test harness to say "this
    // much time went by" (sim:skipTime and every simulated tick). Time going by moves
    // both clock domains, so real-time work such as packet flood control sees the
    // harness's skips exactly as it did when there was one clock.
    add_simulation_offset(additional_offset);
    realtime::advance_for_tests(additional_offset);
}

inline void reset_offset()
{
    // CARDIAN: undoes add_offset, so it clears both domains.
    time_offset.store(0, std::memory_order_relaxed);
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
