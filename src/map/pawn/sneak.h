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
#include <functional>
#include <vector>

// The sneak-by (ROADMAP T, slice 3): where the mobs hold every way on her
// trek, the timed crossing past them, to ground the everyday trek can take
// over from. What the server knows of a mob decides it: after
// each walk it rests a number of seconds fixed as the walk ends, its next
// walk stays within its territory, and its sight reaches only 45 degrees
// either side of where it faces. While it rests its reach stays where it is;
// once its rest is over it is taken to turn toward her, as players assume,
// and its whole reach spreads at its walking speed, never past its
// territory. A crossing fits when every yalm of her walk, at the second she
// reaches it, is out of every mob's reach.
namespace cardian::sneak
{
    // A mob as a sneak-by reads it
    struct Mob
    {
        position_t at{};            // where it stands now; its rotation, the way it faces
        float      cone     = 0.0f; // its sight's reach, within its cone; 0 for none
        float      allRound = 0.0f; // the rest of its reach, every way round; 0 for none
        float      speed    = 0.0f; // yalms a second at its walk
        float      restLeft = 0.0f; // seconds before its rest ends; 0 when it walks now

        // Its territory, where it can ever stand: its roam circle round
        // `home`, or its region, as how far a point lies outside it (0
        // inside). One the model cannot bound -- a patrol, one far from its
        // territory -- leaves no ground out of its reach
        position_t                              home{};
        float                                   roam = 0.0f;
        std::function<float(const position_t&)> outside;
        bool                                    unbounded = false;

        auto reach() const -> float
        {
            return std::max(cone, allRound);
        }

        // How near to `p` it can ever stand: its territory, or where it
        // stands now if that is nearer (planar for a region, as the game
        // measures one)
        auto nearest(const position_t& p) const -> float
        {
            const float here = distance(at, p);
            if (unbounded)
            {
                return 0.0f;
            }
            const float territory = outside ? outside(p) : std::max(0.0f, distance(p, home, true) - roam);
            return std::min(here, territory);
        }

        // Whether it reaches `p` at `t` seconds from now, its rest taken to
        // end `slack` seconds early. Resting, its sight holds only its cone
        // -- the game's own test, 32 of 256 either side -- and the rest all
        // round; its rest over, it may stand anywhere its walk has reached
        // within its territory, facing her
        auto threatens(const position_t& p, const float t, const float slack) const -> bool
        {
            const float d       = distance(at, p);
            const float restEnd = std::max(0.0f, restLeft - slack);
            if (t < restEnd)
            {
                return d < allRound || (d < cone && facing(at, p, 64));
            }
            const float walked = speed * (t - restEnd);
            return d - walked < reach() && nearest(p) < reach();
        }

        // Whether it can never reach `p`, wherever it goes
        auto neverReaches(const position_t& p) const -> bool
        {
            return !unbounded && nearest(p) >= reach();
        }
    };

    struct Hop
    {
        bool                    fits = false;
        std::vector<position_t> points;          // her walk from her own spot past the mobs, its end last
        float                   seconds  = 0.0f; // when she reaches its end, from now
        bool                    island   = false; // its end is ground no mob can ever reach
        float                   walked   = 0.0f; // fits: the walk's length; fits not, how far along she would be caught
        int                     caughtBy = -1;   // fits not: the mob that would reach her, -1 when none would but she is never past them
        float                   caughtAt = 0.0f; // ... and when, from now
    };

    // Her walk along `route` (her own spot first) at `speed` yalms a second,
    // setting out `delay` seconds from now, played forward against every
    // mob a yalm at a time. It fits when no mob reaches any of it when she
    // does, until she is past them: once she has walked into a mob's whole
    // reach -- beside or behind it, its back turned or its rest still on --
    // the first ground out of every mob's reach as it stands, which none can
    // reach for `settle` seconds more; the everyday trek takes over there. Or
    // the route's end, when that is safe of itself (`endSafe`: a zone line)
    inline auto plan(const std::vector<Mob>& mobs, const std::vector<position_t>& route, const float speed, const float delay, const float slack,
                     const float settle, const bool endSafe) -> Hop
    {
        Hop hop;
        if (route.empty() || speed <= 0.0f)
        {
            return hop;
        }
        const auto caught = [&](const position_t& p, const float t) -> int
        {
            for (size_t m = 0; m < mobs.size(); ++m)
            {
                if (mobs[m].threatens(p, t, slack))
                {
                    return static_cast<int>(m);
                }
            }
            return -1;
        };
        bool       among = false; // she has walked into a mob's whole reach
        const auto past  = [&](const position_t& p, const float t)
        {
            return std::all_of(mobs.begin(), mobs.end(), [&](const Mob& m) { return distance(m.at, p) >= m.reach() && !m.threatens(p, t + settle, slack); });
        };
        const auto island = [&](const position_t& p)
        {
            return std::all_of(mobs.begin(), mobs.end(), [&](const Mob& m) { return m.neverReaches(p); });
        };

        // Her own spot, as long as she stands on it
        position_t from = route.front();
        if (const int m = caught(from, delay); m >= 0)
        {
            hop.caughtBy = m;
            hop.caughtAt = delay;
            return hop;
        }
        hop.points.push_back(from);

        float walked = 0.0f;
        for (size_t i = 1; i < route.size(); ++i)
        {
            const position_t& to    = route[i];
            const float       len   = distance(from, to);
            const int         steps = std::max(1, static_cast<int>(std::ceil(len)));
            for (int k = 1; k <= steps; ++k)
            {
                const float      f = static_cast<float>(k) / static_cast<float>(steps);
                const position_t p(from.x + (to.x - from.x) * f, from.y + (to.y - from.y) * f, from.z + (to.z - from.z) * f, 0, 0);
                const float      t = delay + (walked + len * f) / speed;
                if (const int m = caught(p, t); m >= 0)
                {
                    hop.caughtBy = m;
                    hop.caughtAt = t;
                    hop.walked   = walked + len * f;
                    hop.points.clear();
                    return hop;
                }
                among = among || std::any_of(mobs.begin(), mobs.end(), [&](const Mob& m) { return distance(m.at, p) < m.reach(); });
                if (among && past(p, t))
                {
                    hop.points.push_back(p);
                    hop.fits    = true;
                    hop.seconds = t;
                    hop.island  = island(p);
                    hop.walked  = walked + len * f;
                    return hop;
                }
            }
            walked += len;
            hop.points.push_back(to);
            from = to;
        }
        if (endSafe)
        {
            hop.fits    = true;
            hop.seconds = delay + walked / speed;
            hop.walked  = walked;
            return hop;
        }
        hop.points.clear();
        hop.walked = walked;
        return hop;
    }
} // namespace cardian::sneak
