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

// A door as a wall on the ground (pawn_doors.h): its plane, read off the
// client's zone data by tools/walls/doors.py, and where a straight walk
// meets it. Pure, for the tests (cardian_door_tests.cpp)
#pragma once

#include <cmath>
#include <optional>
#include <vector>

namespace cardian::door
{
    // A door NPC sits about a yalm above its floor: one further off than
    // this stands on another storey
    constexpr float kSameFloor = 2.5f;

    // The body: a walk passing this near a door's edge meets the door
    constexpr float kBodyMargin = 0.5f;

    // A door's plane on the ground: its middle, the unit normal of its + side,
    // and its width across the doorway
    struct Plane
    {
        float x     = 0.0f;
        float y     = 0.0f;
        float z     = 0.0f;
        float nx    = 1.0f;
        float nz    = 0.0f;
        float width = 0.0f;
    };

    // How far in front of the door's plane a point stands: positive on its +
    // side
    inline auto ahead(const Plane& door, const float px, const float pz) -> float
    {
        return (px - door.x) * door.nx + (pz - door.z) * door.nz;
    }

    // The side of the door a point stands on: +1 or -1
    inline auto sideOf(const Plane& door, const float px, const float pz) -> int
    {
        return ahead(door, px, pz) >= 0.0f ? 1 : -1;
    }

    // Where the straight walk from a to b meets the door: the share of the
    // way, 0 to 1, at which it crosses the door's plane within the doorway
    // and on the door's storey; nullopt when it passes by
    inline auto meets(const Plane& door, const float ax, const float ay, const float az, const float bx, const float by, const float bz) -> std::optional<float>
    {
        const float sa = ahead(door, ax, az);
        const float sb = ahead(door, bx, bz);
        if ((sa >= 0.0f) == (sb >= 0.0f) || sa == sb)
        {
            return std::nullopt;
        }
        const float t  = sa / (sa - sb);
        const float ix = ax + (bx - ax) * t;
        const float iy = ay + (by - ay) * t;
        const float iz = az + (bz - az) * t;

        // Along the door's plane, from its middle
        const float across = std::abs((ix - door.x) * -door.nz + (iz - door.z) * door.nx);
        if (across > door.width / 2.0f + kBodyMargin || std::abs(iy - door.y) > kSameFloor)
        {
            return std::nullopt;
        }
        return t;
    }

    // A walk's route meeting a door: the route's point the leg that meets it
    // ends at, and the share of that leg at which it does
    struct Meeting
    {
        size_t leg = 0;
        float  t   = 0.0f;
    };

    // The first leg of a route -- its points in order, x y z each -- that
    // meets the door; nullopt when none does
    template <typename Point>
    auto firstMeeting(const Plane& door, const std::vector<Point>& route) -> std::optional<Meeting>
    {
        for (size_t i = 1; i < route.size(); ++i)
        {
            const auto& a = route[i - 1];
            const auto& b = route[i];
            if (const auto t = meets(door, a.x, a.y, a.z, b.x, b.y, b.z); t.has_value())
            {
                return Meeting{ i, *t };
            }
        }
        return std::nullopt;
    }

    // Where a walk stops short of a door it meets: `keepShort` yalms back
    // along the leg from where it would cross, never behind the leg's start.
    // The share of the leg
    inline auto stopShort(const float legLength, const float t, const float keepShort) -> float
    {
        if (legLength <= 0.0f)
        {
            return 0.0f;
        }
        const float at = t * legLength - keepShort;
        return at <= 0.0f ? 0.0f : at / legLength;
    }
} // namespace cardian::door
