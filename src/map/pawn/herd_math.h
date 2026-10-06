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

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <numbers>
#include <numeric>
#include <vector>

// The melee ring round a mob as a herd (ROADMAP A item 9): every body in
// melee on the mob stands at a bearing from it, in world terms, so the
// mob turning moves nobody. The herd pass spaces the bodies out: it keeps
// their order round the mob, pushes neighbours apart to a minimum gap with
// the least movement it can, and evens the ring out a step at a time.
// Fixed bodies -- the tank, the player, a Thief on her Sneak Attack walk --
// are spaced round and never moved. Pure, so xi_test pins it; the
// controller gathers the bodies and walks the cardians.
//
// A bearing is an angle on the ground plane, in radians from world east
// (+x) towards world north (+z), kept in [0, 2pi).
namespace cardian::herd
{
    inline constexpr float kTau = 2.0f * std::numbers::pi_v<float>;

    // An angle in [0, 2pi)
    inline auto wrap(const float a) -> float
    {
        const float w = std::fmod(a, kTau);
        return w < 0.0f ? w + kTau : w;
    }

    // Going counter-clockwise from `from`, how far to `to`, in [0, 2pi)
    inline auto ccw(const float from, const float to) -> float
    {
        return wrap(to - from);
    }

    // The shorter turn from `from` to `to`, signed, in (-pi, pi]
    inline auto shortest(const float from, const float to) -> float
    {
        const float d = ccw(from, to);
        return d > std::numbers::pi_v<float> ? d - kTau : d;
    }

    // The bearing of a point from a centre
    inline auto bearingOf(const float cx, const float cz, const float x, const float z) -> float
    {
        return wrap(std::atan2(z - cz, x - cx));
    }

    // A bearing as the in-game map reads it: degrees from north, clockwise
    inline auto compassDegrees(const float bearing) -> float
    {
        const float deg = 90.0f - bearing * 180.0f / std::numbers::pi_v<float>;
        return std::fmod(std::fmod(deg, 360.0f) + 360.0f, 360.0f);
    }

    // The same, in whole degrees for a log line: 0 to 359, never 360
    inline auto compassRounded(const float bearing) -> int
    {
        return static_cast<int>(std::lround(compassDegrees(bearing))) % 360;
    }

    // A body on the ring: where it is (or is going), whether the pass may
    // move it at all, and whether it has settled in place -- a settled body
    // gives way only when the newcomers cannot make room by themselves
    struct Body
    {
        float bearing = 0.0f;
        bool  fixed   = false;
        bool  settled = false;
    };

    // The bodies' order round the ring, by bearing, ties by their index
    inline auto ringOrder(const std::vector<Body>& bodies) -> std::vector<std::size_t>
    {
        std::vector<std::size_t> order(bodies.size());
        std::iota(order.begin(), order.end(), std::size_t{ 0 });
        std::ranges::stable_sort(order, [&](const std::size_t a, const std::size_t b)
                                 {
                                     return wrap(bodies[a].bearing) < wrap(bodies[b].bearing);
                                 });
        return order;
    }

    // Neighbours pushed apart to at least `gap`, along a line of positions
    // already unrolled in ring order (`wraps`: the last one's neighbour is
    // the first, a lap on). A pair both free share the push, a pair with
    // one pinned the other takes it all. A pass that carries a body past
    // its neighbour is undone by the next, so order holds. False when the
    // gaps cannot be met: a free body boxed in pushes to and fro, and a
    // pair too close with both pinned fails when `pinnedPairFails` (the
    // settled are pinned only on a first try)
    inline auto pushApart(std::vector<float>& pos, const std::vector<bool>& pin, const float gap, const bool wraps, const bool pinnedPairFails) -> bool
    {
        const std::size_t n = pos.size();
        if (n < 2)
        {
            return true;
        }
        constexpr float   kSlack = 1e-4f;
        const std::size_t pairs  = wraps ? n : n - 1;
        for (int pass = 0; pass < 256; ++pass)
        {
            bool moved = false;
            for (std::size_t a = 0; a < pairs; ++a)
            {
                const std::size_t c    = (a + 1) % n;
                const float       d    = c == 0 ? pos[0] + kTau - pos[n - 1] : pos[c] - pos[a];
                const float       need = gap - d;
                if (need <= kSlack)
                {
                    continue;
                }
                if (pin[a] && pin[c])
                {
                    if (pinnedPairFails)
                    {
                        return false;
                    }
                    continue;
                }
                if (!pin[a] && !pin[c])
                {
                    pos[a] -= need / 2.0f;
                    pos[c] += need / 2.0f;
                }
                else if (pin[a])
                {
                    pos[c] += need;
                }
                else
                {
                    pos[a] -= need;
                }
                moved = true;
            }
            if (!moved)
            {
                return true;
            }
        }
        return false;
    }

    // The gap the pass keeps: the one asked for, shrunk when more bodies
    // are round the mob than it allows, so the ring never runs out of room
    inline auto effectiveGap(const std::size_t bodies, const float gap) -> float
    {
        // A little under an even share when crowded, so the gaps can all be met
        return bodies == 0 ? gap : std::min(gap, 0.95f * kTau / static_cast<float>(bodies));
    }

    // One stretch of the ring spaced: `members` (indices into the bodies,
    // in ring order) unrolled from the first, a lap at most. With `ends`,
    // the first and last are fixed bodies that bound it and never move.
    // The newcomers make room first, the settled held; only what they
    // cannot fix moves the settled. Between two fixed bodies too close for
    // every gap, the free ones take even places between them -- the most
    // room each can have, reached in one move and held, since the next pass
    // finds them there
    inline void spaceStretch(const std::vector<Body>& bodies, const std::vector<std::size_t>& members, const bool ends, const float gap, std::vector<float>& out)
    {
        const std::size_t  n = members.size();
        std::vector<float> pos(n);
        pos[0] = wrap(bodies[members[0]].bearing);
        for (std::size_t k = 1; k < n; ++k)
        {
            pos[k] = pos[k - 1] + ccw(wrap(bodies[members[k - 1]].bearing), wrap(bodies[members[k]].bearing));
        }
        if (ends && n >= 2 && pos[n - 1] <= pos[0])
        {
            pos[n - 1] = pos[0] + kTau; // a lone fixed body bounds its own lap
        }
        // Between two fixed bodies too close for every gap: even places
        if (ends && pos[n - 1] - pos[0] < static_cast<float>(n - 1) * gap - 1e-4f)
        {
            const float step = (pos[n - 1] - pos[0]) / static_cast<float>(n - 1);
            for (std::size_t k = 1; k + 1 < n; ++k)
            {
                out[members[k]] = wrap(pos[0] + step * static_cast<float>(k));
            }
            return;
        }
        std::vector<bool> settledHeld(n);
        std::vector<bool> fixedHeld(n);
        for (std::size_t k = 0; k < n; ++k)
        {
            const bool end = ends && (k == 0 || k == n - 1);
            fixedHeld[k]   = end;
            settledHeld[k] = end || bodies[members[k]].settled;
        }
        const std::size_t first = ends ? 1 : 0;
        const std::size_t last  = ends ? n - 1 : n;
        for (const bool settledFirst : { true, false })
        {
            auto tried = pos;
            if (pushApart(tried, settledFirst ? settledHeld : fixedHeld, gap, !ends, settledFirst))
            {
                for (std::size_t k = first; k < last; ++k)
                {
                    out[members[k]] = wrap(tried[k]);
                }
                return;
            }
        }
    }

    // The spacing pass: every body at least `gap` from its neighbours, the
    // order round the mob kept and fixed bodies never moved. The ring is
    // spaced stretch by stretch between fixed bodies, so a stretch that
    // cannot be met -- a body boxed in between two fixed ones -- takes even
    // places there without holding the rest; with no fixed body it is one
    // ring.
    // One bearing per body, in the bodies' order
    inline auto spread(const std::vector<Body>& bodies, const float gap) -> std::vector<float>
    {
        std::vector<float> out;
        out.reserve(bodies.size());
        for (const auto& b : bodies)
        {
            out.push_back(wrap(b.bearing));
        }
        const std::size_t n = bodies.size();
        if (n < 2)
        {
            return out;
        }
        const auto  order = ringOrder(bodies);
        const float g     = effectiveGap(n, gap);

        std::vector<std::size_t> fixedAt; // positions in ring order
        for (std::size_t k = 0; k < n; ++k)
        {
            if (bodies[order[k]].fixed)
            {
                fixedAt.push_back(k);
            }
        }
        if (fixedAt.empty())
        {
            spaceStretch(bodies, order, false, g, out);
            return out;
        }
        // Each stretch runs from one fixed body to the next round the ring,
        // both ends included; a lone fixed body bounds a whole lap
        for (std::size_t j = 0; j < fixedAt.size(); ++j)
        {
            const std::size_t from = fixedAt[j];
            const std::size_t to   = fixedAt.size() == 1 ? from + n : (j + 1 < fixedAt.size() ? fixedAt[j + 1] : fixedAt[0] + n);
            std::vector<std::size_t> stretch;
            for (std::size_t k = from; k <= to; ++k)
            {
                stretch.push_back(order[k % n]);
            }
            if (stretch.size() > 2)
            {
                spaceStretch(bodies, stretch, true, g, out);
            }
        }
        return out;
    }

    // The evening-out step: each free body turned half way towards the
    // middle of the gap between its neighbours, by at most `step`, and not at all
    // when it is within `minMove` of it. A body alone has nowhere to go;
    // a body with one neighbour heads for the far side from it. One
    // bearing per body, in the bodies' order
    inline auto evenStep(const std::vector<Body>& bodies, const float step, const float minMove) -> std::vector<float>
    {
        std::vector<float> out;
        out.reserve(bodies.size());
        for (const auto& b : bodies)
        {
            out.push_back(wrap(b.bearing));
        }
        const std::size_t n = bodies.size();
        if (n < 2)
        {
            return out;
        }
        const auto order = ringOrder(bodies);
        for (std::size_t k = 0; k < n; ++k)
        {
            const std::size_t i = order[k];
            if (bodies[i].fixed)
            {
                continue;
            }
            const std::size_t p      = order[(k + n - 1) % n];
            const std::size_t q      = order[(k + 1) % n];
            const float       before = ccw(wrap(bodies[p].bearing), wrap(bodies[i].bearing));
            const float       after  = n == 2 ? kTau - before : ccw(wrap(bodies[i].bearing), wrap(bodies[q].bearing));
            // Half the way to the middle: every body steps at once, from
            // where they all stood, and a whole step overshoots and swings
            const float       delta  = (after - before) / 4.0f;
            if (std::abs(delta) < minMove)
            {
                continue;
            }
            out[i] = wrap(out[i] + std::clamp(delta, -step, step));
        }
        return out;
    }
} // namespace cardian::herd
