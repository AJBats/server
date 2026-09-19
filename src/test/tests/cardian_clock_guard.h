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

#include "common/timer.h"

// The simulation hold and both clock offsets are process-global, and the Lua suite
// runs after the C++ cases in the same process: a test that touches any of them
// leaves all three exactly as it found them, even when it fails half way.
struct ClockGuard
{
    timer::duration      savedOffset = timer::get_offset();
    realtime::clock::rep savedReal   = realtime::test_offset.load();

    ClockGuard()                             = default;
    ClockGuard(const ClockGuard&)            = delete;
    ClockGuard& operator=(const ClockGuard&) = delete;

    ~ClockGuard()
    {
        timer::held_at.store(0);
        timer::time_offset.store(savedOffset.count());
        realtime::test_offset.store(savedReal);
    }
};
