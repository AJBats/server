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

#include "common/mmo.h"
#include "common/utils.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>
#include <vector>

// The walker's rules and its route's arithmetic (ROADMAP W): every walk
// a cardian takes is a route the scored search plans round the danger map
// (pawn/route_search.h), walked as planned. A mover proposes where; these
// say how the walker treats the walk, and the arithmetic below is what the
// walker does with a route once it has one
namespace cardian::walk
{
    // How the walker treats one walk. The everyday walk keeps her rows'
    // rings, a wall hiding a mob, and holds short of a choke; a trek keeps
    // every ring whole, plans its zone at once, and races when caught
    struct Rules
    {
        bool  danger     = true;  // keep out of the danger map's rings at all
        bool  forceAvoid = false; // aggro and links kept clear whatever her rows say (a trek lends both)
        bool  sightHides = true;  // a wall hides a mob (the game's line of sight); false keeps every ring whole
        bool  holds      = true;  // where every way passes a ring, hold short of it; racing, she never holds
        bool  sneaks     = true;  // holding, look for a sneak-by past the mobs (pawn/sneak.h)
        bool  wholeZone  = false; // a trek: the zone's every mob, the route planned whole and kept, stretches planned again
        float extra      = 0.0f;  // yalms beyond the danger map's buffer round every ring
        float closeTo    = 0.0f;  // the route ends this many yalms short of the point, along it
        bool  curves     = false; // in a town, her corners taken as curves as her path finder is handed them: a walk planned once (an errand's, the lounge's), never one re-planned every tick
        float visitStay  = 0.0f;  // her point may lie inside a ring: held beside it, a timed visit in to it behind its mobs' backs when one fits (sneak.h visit), this many seconds there; 0 for none
    };

    // What the walk came to this tick
    enum class Outcome : uint8_t
    {
        Walking,   // on her way to the point
        Escaping,  // in a ring whose mob sees her: out of it first
        Held,      // short of the point: a choke ahead of her, or the point inside a ring
        BackingOff, // to the nearest ground out of every ring, with no spot short of a choke that is
        Hopping,   // a sneak-by under way
        NoRoute,   // no walk on the mesh joins her to the point
    };

    // The yalms along a route from its first point to its last
    inline auto lengthOf(const std::vector<position_t>& route) -> float
    {
        float length = 0.0f;
        for (size_t i = 1; i < route.size(); ++i)
        {
            length += distance(route[i - 1], route[i]);
        }
        return length;
    }

    // The route cut `closeTo` yalms short of its end, along it: the walk to
    // a range, never past it. A route no longer than that is her own spot
    // alone. The first point, her own spot, always stays
    inline void cutShort(std::vector<position_t>& route, const float closeTo)
    {
        if (closeTo <= 0.0f || route.size() < 2)
        {
            return;
        }
        float left = closeTo;
        while (route.size() > 1)
        {
            const auto& a   = route[route.size() - 2];
            const auto& b   = route.back();
            const float leg = distance(a, b);
            if (leg > left)
            {
                const float t = (leg - left) / leg;
                route.back()  = position_t(a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t, b.moving, b.rotation);
                return;
            }
            left -= leg;
            route.pop_back();
        }
    }

    // How far `here` stands off the leg a-b
    inline auto offLeg(const position_t& a, const position_t& b, const position_t& here) -> float
    {
        const float dx  = b.x - a.x;
        const float dy  = b.y - a.y;
        const float dz  = b.z - a.z;
        const float len = dx * dx + dy * dy + dz * dz;
        const float t   = len > 0.0f ? std::clamp(((here.x - a.x) * dx + (here.y - a.y) * dy + (here.z - a.z) * dz) / len, 0.0f, 1.0f) : 0.0f;
        return distance(here, position_t(a.x + dx * t, a.y + dy * t, a.z + dz * t, 0, 0));
    }

    // The waypoint she walks to next on a route whose first point is where
    // she set out: of the next few legs from `next`, the one she stands
    // nearest, and past its end once she stands on it
    inline auto progress(const std::vector<position_t>& route, const size_t next, const position_t& here) -> size_t
    {
        if (next == 0 || next >= route.size())
        {
            return next;
        }
        size_t best   = next;
        float  bestAt = std::numeric_limits<float>::max();
        for (size_t i = next; i < std::min(route.size(), next + 4); ++i)
        {
            const float d = offLeg(route[i - 1], route[i], here);
            if (d < bestAt - 0.01f)
            {
                bestAt = d;
                best   = i;
            }
        }
        return distance(here, route[best]) < 1.0f ? best + 1 : best;
    }

    // A leg of a route that is a drop -- an off-mesh link down a ledge: down
    // at least kDropLeast, and more steeply than any ground a client walks
    // (its floors end at 45 degrees)
    constexpr float kDropLeast = 2.0f;
    constexpr float kDropSteep = 1.5f; // yalms down for every yalm across, at the least

    inline auto isDrop(const position_t& lip, const position_t& land) -> bool
    {
        const float down   = land.y - lip.y; // FFXI's height grows downward
        const float across = std::hypot(land.x - lip.x, land.z - lip.z);
        return down >= kDropLeast && down >= across * kDropSteep;
    }

    // How far from `next` her path finder is handed a route, short of `end`:
    // to the lip of the first drop ahead of her, the lip included. It walks
    // every leg as ground and spends a tick's step past a waypoint, so a
    // drop handed to it is walked down through the ledge; the drop is hers,
    // from the lip (CPawnController::DropOnRoute)
    inline auto toNextLip(const std::vector<position_t>& route, const size_t next, const size_t end) -> size_t
    {
        for (size_t i = next; i + 1 < end; ++i)
        {
            if (isDrop(route[i], route[i + 1]))
            {
                return i + 1;
            }
        }
        return end;
    }
} // namespace cardian::walk
