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

// Cardian: a lift's platform as the game runs it, read off what the game
// tells the client (transports/elevator_handler.cpp): the platform NPC's
// animation says which of its two ends it rests at or heads for, and its
// TransportTimestamp and TransportTravel when its last leg began and how
// long a leg takes. The server never moves the platform; the client draws
// it, and the riders with it. Pure, so the arithmetic is pinned by
// cardian_lift_tests.cpp; pawn/lifts.h reads the game's values into it.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace cardian::lift
{
    // A lift's two ends, as its data names them: its lower door's (end 0)
    // and its upper door's (end 1). The names are the data's, not the
    // floors': a shaft may name its door on the top floor "lower"
    struct Platform
    {
        bool     animUp      = false; // the platform's animation is the elevator's "up"
        bool     reversed    = false; // the shaft plays its animations the other way
        uint32_t timestampS  = 0;     // TransportTimestamp: whole seconds of the Vana'diel clock the last leg began at; 0 before any
        uint32_t travelS     = 0;     // TransportTravel: seconds a leg takes
        uint32_t periodMs    = 0;     // a timed lift's leg, travel and rest together; 0 for a lever's
    };

    // The end the platform rests at, or heads for while it moves: the game's
    // "top" is the upper door's end. A leg up plays the "up" animation, but
    // for a reversed shaft, and the animation stays as the leg ends
    inline auto endOf(const Platform& p) -> int
    {
        return p.animUp != p.reversed ? 1 : 0;
    }

    // When the last leg began, in milliseconds of the Vana'diel clock: a
    // timed lift begins its legs on whole periods, which the whole seconds
    // the client is told round down from
    inline auto legStartMs(const Platform& p) -> int64_t
    {
        const int64_t told = static_cast<int64_t>(p.timestampS) * 1000;
        if (p.periodMs == 0 || told == 0)
        {
            return told;
        }
        const int64_t period = p.periodMs;
        const int64_t onPeriod = ((told + period - 1) / period) * period;
        return onPeriod - told < 1000 ? onPeriod : told;
    }

    struct State
    {
        int   end      = 0;     // the end it rests at, or heads for
        bool  moving   = false;
        float progress = 1.0f;  // 0 as it leaves the other end, 1 as it arrives
    };

    inline auto stateAt(const Platform& p, const int64_t nowMs) -> State
    {
        State s;
        s.end = endOf(p);
        if (p.timestampS == 0 || p.travelS == 0)
        {
            return s; // no leg since the server came up: it rests where the data put it
        }
        const int64_t start  = legStartMs(p);
        const int64_t travel = static_cast<int64_t>(p.travelS) * 1000;
        if (nowMs >= start && nowMs < start + travel)
        {
            s.moving   = true;
            s.progress = static_cast<float>(nowMs - start) / static_cast<float>(travel);
        }
        return s;
    }

    // The platform's height: at its end's floor at rest, between the two
    // floors as it moves
    inline auto heightAt(const State& s, const float floorEnd0, const float floorEnd1) -> float
    {
        const float to   = s.end == 1 ? floorEnd1 : floorEnd0;
        const float from = s.end == 1 ? floorEnd0 : floorEnd1;
        return s.moving ? from + (to - from) * std::clamp(s.progress, 0.0f, 1.0f) : to;
    }

    // A timed lift's next leg begins on the next whole period of the clock
    inline auto nextDepartureMs(const Platform& p, const int64_t nowMs) -> int64_t
    {
        if (p.periodMs == 0)
        {
            return INT64_MAX; // a lever's lift leaves when someone pulls the lever
        }
        const int64_t period = p.periodMs;
        return nowMs - (nowMs % period) + period;
    }

    // Whether a body at rest on the platform's floor may still step aboard:
    // the platform rests at that end, and leaves no sooner than `needMs`
    // from now, its door shutting `doorDelayMs` before
    inline auto boardable(const Platform& p, const int end, const int64_t nowMs, const int64_t needMs, const int64_t doorDelayMs) -> bool
    {
        const State s = stateAt(p, nowMs);
        return !s.moving && s.end == end && nextDepartureMs(p, nowMs) - doorDelayMs - nowMs >= needMs;
    }

    // On the platform: within `radius` of its centre, on the level
    inline auto onPlatform(const float cx, const float cz, const float radius, const float x, const float z) -> bool
    {
        return std::hypot(x - cx, z - cz) <= radius;
    }
} // namespace cardian::lift
