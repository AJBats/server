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
#include "common/timer.h"
#include "common/types/position.h"
#include "common/utils.h"

#include <chrono>

// A trek's approach to the zone line it walks to (ROADMAP T): the line is
// taken once the walker is within kTransferDistance of its spot; or, within
// kCrossingSlack of it, once the walk has made no headway for kStall -- the
// mesh often ends short of a zone's mouth, and the server takes a crossing
// from some 40 yalms out -- or finds no way the rest of it. A walk held at
// a choke or on a sneak-by is no stall (CPawnController::TravelTick)
namespace cardian::trek
{
    constexpr float kTransferDistance = 3.0f;
    constexpr float kCrossingSlack    = 40.0f;
    constexpr float kHeadway          = 0.5f; // nearer by this much is headway
    constexpr float kSameSpot         = 1.0f; // a line's spot moved less than this is the same line
    constexpr auto  kStall            = std::chrono::seconds(3);

    // Close enough to take the line now
    inline auto reached(const float dist) -> bool
    {
        return dist < kTransferDistance;
    }

    // Near enough that a walk which cannot get nearer takes it from here
    inline auto withinSlack(const float dist) -> bool
    {
        return dist < kCrossingSlack;
    }

    struct LineApproach
    {
        uint16            zone = 0; // the zone the watched line leads to; 0 before any
        position_t        walkTo{};
        float             best = 0.0f; // the nearest the walk has come
        timer::time_point headwayAt{};

        // This tick's distance to the line: a line other than the one watched
        // starts the watch afresh, and coming nearer is headway
        void watch(const uint16 toZone, const position_t& to, const float dist, const timer::time_point now)
        {
            if (toZone != zone || distance(to, walkTo) > kSameSpot)
            {
                zone      = toZone;
                walkTo    = to;
                best      = dist;
                headwayAt = now;
                return;
            }
            if (dist + kHeadway < best)
            {
                best      = dist;
                headwayAt = now;
            }
        }

        // Held at a choke or sneaking by: no stall
        void held(const timer::time_point now)
        {
            headwayAt = now;
        }

        // The next line watched starts afresh
        void forget()
        {
            zone = 0;
        }

        // No headway for kStall within the slack: she takes the line from
        // where she stands
        auto stalled(const float dist, const timer::time_point now) const -> bool
        {
            return now - headwayAt > kStall && withinSlack(dist);
        }
    };
} // namespace cardian::trek
