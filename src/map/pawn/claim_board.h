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
#include <optional>
#include <span>
#include <vector>

// The party's claim board (RESEARCH §12.15, OPEN_ISSUES #250): no two party
// members aim for one spot. Every cardian holds a claim -- where the walker
// is taking her while she walks, where she stands while she stands. A spot
// a mover proposes within kSpacing of the claim of a member before her in
// the party order gives way: it slides to the nearest clear point that
// keeps the mover's purpose -- round a centre at the distance the spot had
// from it (the attended mob, so the crescent stays outside the mob's
// reach; the party's place elsewhere), and, when the mover asks, within a
// reach of a point (the tank she cures). Only the later member gives way,
// so two never swap; with nothing clear nearby the spot stands. Pure, so
// xi_test pins it; the controller reads the board and vets the candidates
// against the world (the mesh, the danger map, the camp's line).
namespace cardian::claims
{
    // Two claims nearer than this are one spot: about a body's width with
    // room to stand, the spacing the camp's backline kept (RESEARCH §12.8)
    inline constexpr float kSpacing = 2.0f;
    // A slide puts the spot this far from every claim it gives way to, so
    // arriving up to kArrive short still leaves her clear of kSpacing
    inline constexpr float kClearance = 2.5f;
    inline constexpr float kArrive    = 0.4f;
    // How far round its circle a slide may go, in yalms along it: past
    // this nothing clear is near, and the spot stands
    inline constexpr float kReach = 8.0f;
    // The slide's step round the circle, in yalms along it
    inline constexpr float kStep = 0.25f;
    // A spot this near its centre has no circle to slide round
    inline constexpr float kMinRadius = 0.5f;
    // An attending mage this near the claim of an attending mage before her
    // stands with her; farther, she goes to her side (AttendIntent), and
    // walks on until she is this near: beside her, a body's width apart.
    // The spot beside her is up to a step past the clearance, and the walk
    // there may end kArrive short on the far side, so beside must reach it
    inline constexpr float kTogether = 5.0f;
    inline constexpr float kBeside   = 3.5f;
    static_assert(kBeside >= kClearance + kStep + kArrive + 0.25f, "a mage walking beside another must count as beside wherever she stops");
    static_assert(kTogether > kBeside, "a mage beside another stands with her");

    // A spot on the ground plane
    struct Point
    {
        float x = 0.0f;
        float z = 0.0f;

        auto operator==(const Point&) const -> bool = default;
    };

    inline auto distance(const Point a, const Point b) -> float
    {
        return std::hypot(a.x - b.x, a.z - b.z);
    }

    // The first claim nearer the spot than `spacing`, by its place in
    // `claims`; none when the spot is clear of them all
    inline auto crowdedBy(const Point spot, const std::span<const Point> claims, const float spacing = kSpacing) -> std::optional<std::size_t>
    {
        for (std::size_t i = 0; i < claims.size(); ++i)
        {
            if (distance(spot, claims[i]) < spacing)
            {
                return i;
            }
        }
        return std::nullopt;
    }

    // The claims a member gives way to: those of the members before her in
    // the party order. `board` is in that order and she is at `mine`
    inline auto before(const std::span<const Point> board, const std::size_t mine) -> std::span<const Point>
    {
        return board.first(std::min(mine, board.size()));
    }

    // What a slide keeps: the spot's distance from `centre`, and when
    // `keepTo` is set, a place within `within` of it -- or no farther from
    // it than the spot itself was, when the spot was already farther
    struct Purpose
    {
        Point                centre{};
        std::optional<Point> keepTo{};
        float                within = 0.0f;
    };

    // The slide's candidates, nearest first: points round the purpose's
    // centre at the spot's own distance from it, stepped out both ways
    // `step` yalms at a time along the circle up to `reach` (and never past
    // half way round, where the two sides meet), each at least `clearance`
    // from every claim and keeping the purpose. Of two points equally far
    // round, the one nearer `from` -- where she stands -- comes first, so she
    // does not walk through the member she gives way to. Empty when the spot
    // has no circle to slide round, or nothing clear is that near
    inline auto slides(const Point spot, const Point from, const std::span<const Point> claims, const Purpose& purpose, const float clearance = kClearance,
                       const float reach = kReach, const float step = kStep) -> std::vector<Point>
    {
        std::vector<Point> out;
        const float        dx     = spot.x - purpose.centre.x;
        const float        dz     = spot.z - purpose.centre.z;
        const float        radius = std::hypot(dx, dz);
        if (radius < kMinRadius || step <= 0.0f)
        {
            return out;
        }
        const float within = purpose.keepTo.has_value() ? std::max(purpose.within, distance(spot, *purpose.keepTo)) : 0.0f;
        const auto  keeps  = [&](const Point p)
        {
            if (purpose.keepTo.has_value() && distance(p, *purpose.keepTo) > within + 1e-4f)
            {
                return false;
            }
            return std::none_of(claims.begin(), claims.end(), [&](const Point c) { return distance(p, c) < clearance; });
        };
        const float at    = std::atan2(dz, dx);
        const float most  = std::min(reach, std::numbers::pi_v<float> * radius);
        const int   steps = static_cast<int>(std::floor(most / step + 1e-4f));
        for (int k = 1; k <= steps; ++k)
        {
            const float turn = static_cast<float>(k) * step / radius;
            const Point a{ purpose.centre.x + radius * std::cos(at + turn), purpose.centre.z + radius * std::sin(at + turn) };
            const Point b{ purpose.centre.x + radius * std::cos(at - turn), purpose.centre.z + radius * std::sin(at - turn) };
            const bool  aFirst = distance(a, from) <= distance(b, from);
            for (const Point p : { aFirst ? a : b, aFirst ? b : a })
            {
                if (keeps(p))
                {
                    out.push_back(p);
                }
            }
        }
        return out;
    }

    // The board's verdict on one spot
    struct Verdict
    {
        std::optional<std::size_t> by{}; // the claim that crowds the spot, by its place on the board; none: the spot is hers
        std::vector<Point>         to{}; // where it may slide, nearest first; empty with `by` set: nothing clear is near, and the spot stands
    };

    // The rule: the spot she proposes, checked against the claims of the
    // members before her (`mine` her place on `board`, in party order)
    inline auto giveWay(const std::span<const Point> board, const std::size_t mine, const Point spot, const Point from, const Purpose& purpose) -> Verdict
    {
        const auto earlier = before(board, mine);
        Verdict    v;
        v.by = crowdedBy(spot, earlier);
        if (v.by.has_value())
        {
            v.to = slides(spot, from, earlier, purpose);
        }
        return v;
    }

    // Together: within `reach` of one of the claims
    inline auto together(const Point at, const std::span<const Point> claims, const float reach = kTogether) -> bool
    {
        return std::any_of(claims.begin(), claims.end(), [&](const Point c) { return distance(at, c) <= reach; });
    }

    // The way to a spot on the far side of a fight: `to` itself when the
    // straight walk from `from` keeps outside `ring` round the centre, or
    // when `to` is no more than `maxTurn` radians round; else the point on
    // the circle through `to`, turned from `from`'s bearing toward `to`'s
    // by `maxTurn`, so she goes round the mob's reach rather than through
    // it, a leg at a time. The shorter way, unless it crosses the bearing
    // `never` (radians from the centre: the side of the mob away from the
    // tank, out of cure range): then the other way
    inline auto roundTheRing(const Point centre, const Point from, const Point to, const float ring, const float maxTurn, const std::optional<float> never = std::nullopt) -> Point
    {
        constexpr float kPi = std::numbers::pi_v<float>;
        const auto      wrapped = [](float a)
        {
            while (a > kPi)
            {
                a -= 2.0f * kPi;
            }
            while (a < -kPi)
            {
                a += 2.0f * kPi;
            }
            return a;
        };
        const float fx = from.x - centre.x;
        const float fz = from.z - centre.z;
        const float tx = to.x - centre.x;
        const float tz = to.z - centre.z;
        if (std::hypot(fx, fz) < kMinRadius || std::hypot(tx, tz) < kMinRadius)
        {
            return to;
        }
        // The nearest the straight walk comes to the centre
        const float wx   = tx - fx;
        const float wz   = tz - fz;
        const float len2 = wx * wx + wz * wz;
        const float t    = len2 > 1e-6f ? std::clamp(-(fx * wx + fz * wz) / len2, 0.0f, 1.0f) : 0.0f;
        if (std::hypot(fx + wx * t, fz + wz * t) >= ring)
        {
            return to;
        }
        const float start = std::atan2(fz, fx);
        float       turn  = wrapped(std::atan2(tz, tx) - start);
        if (never.has_value())
        {
            // The shorter way crosses it: the bearing lies between the two,
            // on the side the turn goes
            const float off = wrapped(*never - start);
            if (off * turn > 0.0f && std::abs(off) < std::abs(turn))
            {
                turn += turn > 0.0f ? -2.0f * kPi : 2.0f * kPi;
            }
        }
        if (std::abs(turn) <= maxTurn)
        {
            return to;
        }
        const float radius = std::max(std::hypot(tx, tz), ring);
        const float at     = start + (turn > 0.0f ? maxTurn : -maxTurn);
        return { centre.x + radius * std::cos(at), centre.z + radius * std::sin(at) };
    }
} // namespace cardian::claims
