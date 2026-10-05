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
#include <optional>
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

    // Neighbours pushed apart to at least `gap`, in the given order, the
    // pinned never moved: a pair both free share the push, a pair with one
    // pinned the other takes it all. The ring is unrolled along the order
    // first, so a push that carries a body past its neighbour is undone by
    // the next pass rather than read as a lap. Repeated until nothing is
    // too close; nullopt when that cannot be met (a free body pinned in
    // between two bodies closer than twice the gap pushes to and fro)
    inline auto pushApart(const std::vector<float>& bearings, const std::vector<std::size_t>& order, const std::vector<bool>& pinned, const float gap) -> std::optional<std::vector<float>>
    {
        const std::size_t n = bearings.size();
        if (n < 2)
        {
            return bearings;
        }
        std::vector<float> pos(n);
        std::vector<bool>  pin(n);
        pos[0] = wrap(bearings[order[0]]);
        pin[0] = pinned[order[0]];
        for (std::size_t k = 1; k < n; ++k)
        {
            pos[k] = pos[k - 1] + ccw(bearings[order[k - 1]], bearings[order[k]]);
            pin[k] = pinned[order[k]];
        }
        constexpr float kSlack = 1e-4f;
        bool            met    = false;
        for (int pass = 0; pass < 256 && !met; ++pass)
        {
            bool moved = false;
            for (std::size_t a = 0; a < n; ++a)
            {
                const std::size_t c    = (a + 1) % n;
                const float       d    = c == 0 ? pos[0] + kTau - pos[n - 1] : pos[c] - pos[a];
                const float       need = gap - d;
                if (need <= kSlack || (pin[a] && pin[c]))
                {
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
            met = !moved;
        }
        if (!met)
        {
            return std::nullopt;
        }
        std::vector<float> out(n);
        for (std::size_t k = 0; k < n; ++k)
        {
            out[order[k]] = wrap(pos[k]);
        }
        return out;
    }

    // The gap the pass keeps: the one asked for, shrunk when more bodies
    // are round the mob than it allows, so the ring never runs out of room
    inline auto effectiveGap(const std::size_t bodies, const float gap) -> float
    {
        // A little under an even share when crowded, so the gaps can all be met
        return bodies == 0 ? gap : std::min(gap, 0.95f * kTau / static_cast<float>(bodies));
    }

    // The spacing pass: every body at least `gap` from its neighbours, the
    // order round the mob kept. The newcomers make room first, the settled
    // bodies held; only what they cannot fix moves the settled ones. Fixed
    // bodies never move, and when the gaps cannot be met round them nobody
    // moves: a ring that stays put beats one that hops. One bearing per
    // body, in the bodies' order
    inline auto spread(const std::vector<Body>& bodies, const float gap) -> std::vector<float>
    {
        std::vector<float> bearings;
        bearings.reserve(bodies.size());
        for (const auto& b : bodies)
        {
            bearings.push_back(wrap(b.bearing));
        }
        if (bodies.size() < 2)
        {
            return bearings;
        }
        const auto  order = ringOrder(bodies);
        const float g     = effectiveGap(bodies.size(), gap);

        std::vector<bool> heldFirst(bodies.size());
        std::vector<bool> fixedOnly(bodies.size());
        for (std::size_t i = 0; i < bodies.size(); ++i)
        {
            heldFirst[i] = bodies[i].fixed || bodies[i].settled;
            fixedOnly[i] = bodies[i].fixed;
        }
        if (auto first = pushApart(bearings, order, heldFirst, g); first.has_value())
        {
            return *first;
        }
        if (auto second = pushApart(bearings, order, fixedOnly, g); second.has_value())
        {
            return *second;
        }
        return bearings;
    }

    // The evening-out step: each free body turned towards the middle of
    // the gap between its neighbours, by at most `step`, and not at all
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
            const float       delta  = (after - before) / 2.0f;
            if (std::abs(delta) < minMove)
            {
                continue;
            }
            out[i] = wrap(out[i] + std::clamp(delta, -step, step));
        }
        return out;
    }
} // namespace cardian::herd
