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

// The local planner: a one-yalm grid round the cardian, a field of soft
// shapes, the navmesh as the walkability callback. Pure, so a change to
// the search or the field fails here before it fails in the field.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "map/pawn/local_planner.h"

#include <cmath>
#include <cstdlib>

using namespace cardian::planner;
using Catch::Matchers::WithinAbs;

namespace
{
    auto openGround(float, float) -> bool
    {
        return true;
    }

    // Along +x, from (0.5, 0.5) to (10.5, 0.5)
    auto straightQuery() -> Query
    {
        Query q;
        q.sx = 0.5f;
        q.sz = 0.5f;
        q.gx = 10.5f;
        q.gz = 0.5f;
        return q;
    }

    auto closestApproach(const Plan& p, const float x, const float z) -> float
    {
        float best = 1e9f;
        for (const auto& [px, pz] : p.points)
        {
            best = std::min(best, cardian::formation::planarDistance(px, pz, x, z));
        }
        return best;
    }
} // namespace

TEST_CASE("plan: open ground and no field, the straight line at its own cost", "[cardian][planner]")
{
    const auto p = plan(straightQuery(), Field{}, openGround);
    REQUIRE(p.has_value());
    REQUIRE_THAT(p->length, WithinAbs(10.0f, 0.01f));
    REQUIRE_THAT(p->cost, WithinAbs(10.0f, 0.01f));
    REQUIRE_THAT(p->straight, WithinAbs(10.0f, 0.01f));
    for (const auto& [x, z] : p->points)
    {
        REQUIRE_THAT(z, WithinAbs(0.5f, 0.01f));
    }
}

TEST_CASE("plan: a body on the line, the way round it where there is room", "[cardian][planner]")
{
    Field f;
    f.discs.push_back(Disc{ 5.5f, 0.5f, 1.5f, 6.0f });

    const auto p = plan(straightQuery(), f, openGround);
    REQUIRE(p.has_value());
    REQUIRE(closestApproach(*p, 5.5f, 0.5f) >= 1.5f - 0.01f);
    REQUIRE(p->length > 10.5f);
    REQUIRE_THAT(p->cost, WithinAbs(p->length, 0.01f)); // no extras paid
}

TEST_CASE("plan: a wake filling a corridor, straight through it and paying", "[cardian][planner]")
{
    // Three cells wide along +x; the wake is wider than the corridor
    const auto corridor = [](const float, const float z)
    {
        return std::fabs(z - 0.5f) < 1.5f;
    };
    Field f;
    f.capsules.push_back(Capsule{ 3.0f, 0.5f, 9.0f, 0.5f, 2.5f, 2.0f });

    const auto p = plan(straightQuery(), f, corridor);
    REQUIRE(p.has_value());
    REQUIRE_THAT(p->length, WithinAbs(10.0f, 0.01f));
    REQUIRE(p->cost > p->length + 5.0f);
}

TEST_CASE("plan: a wall with one gap, the plan takes the gap and cuts no corner", "[cardian][planner]")
{
    // A wall at x in [5, 6) with a gap at z in [3, 4)
    const auto walled = [](const float x, const float z)
    {
        return !(x >= 5.0f && x < 6.0f) || (z >= 3.0f && z < 4.0f);
    };

    const auto p = plan(straightQuery(), Field{}, walled);
    REQUIRE(p.has_value());
    REQUIRE(closestApproach(*p, 5.5f, 3.5f) < 0.01f);
    for (std::size_t i = 1; i < p->points.size(); ++i)
    {
        const auto& a = p->points[i - 1];
        const auto& b = p->points[i];
        if (std::fabs(b.first - a.first) > 0.5f && std::fabs(b.second - a.second) > 0.5f)
        {
            REQUIRE(walled(b.first, a.second));
            REQUIRE(walled(a.first, b.second));
        }
    }
}

TEST_CASE("plan: no way through, no plan", "[cardian][planner]")
{
    const auto wall = [](const float x, const float)
    {
        return !(x >= 5.0f && x < 6.0f);
    };
    REQUIRE_FALSE(plan(straightQuery(), Field{}, wall).has_value());
}

TEST_CASE("plan: a goal beyond the window is planned to the window's edge toward it", "[cardian][planner]")
{
    Query q  = straightQuery();
    q.window = 4;
    const auto p = plan(q, Field{}, openGround);
    REQUIRE(p.has_value());
    REQUIRE_THAT(p->points.back().first, WithinAbs(4.5f, 0.01f));
    REQUIRE_THAT(p->points.back().second, WithinAbs(0.5f, 0.01f));
}

TEST_CASE("plan: last tick's side is kept when the two ways round cost the same", "[cardian][planner]")
{
    Field f;
    f.discs.push_back(Disc{ 5.5f, 0.5f, 1.5f, 6.0f });

    Query left    = straightQuery();
    left.sideBias = 1.0f;
    Query right    = straightQuery();
    right.sideBias = -1.0f;
    const auto pl  = plan(left, f, openGround);
    const auto pr  = plan(right, f, openGround);
    REQUIRE(pl.has_value());
    REQUIRE(pr.has_value());
    float zl = 0.0f;
    float zr = 0.0f;
    for (const auto& [x, z] : pl->points)
    {
        zl += z - 0.5f;
    }
    for (const auto& [x, z] : pr->points)
    {
        zr += z - 0.5f;
    }
    REQUIRE(zl * zr < 0.0f);
}

TEST_CASE("along: the point so many yalms along the plan, its end when shorter", "[cardian][planner]")
{
    const auto p = plan(straightQuery(), Field{}, openGround);
    REQUIRE(p.has_value());
    const auto [x, z] = along(*p, 3.0f);
    REQUIRE_THAT(x, WithinAbs(3.5f, 0.01f));
    REQUIRE_THAT(z, WithinAbs(0.5f, 0.01f));
    const auto [ex, ez] = along(*p, 50.0f);
    REQUIRE_THAT(ex, WithinAbs(10.5f, 0.01f));
}

TEST_CASE("reaches: a walk through a shape or grazing a capsule's side is planned, one clear of the field is not", "[cardian][planner]")
{
    Field f;
    f.discs.push_back(Disc{ 5.0f, 0.0f, 1.5f, 6.0f });
    f.capsules.push_back(Capsule{ 3.0f, 10.0f, 9.0f, 10.0f, 2.5f, 2.0f });

    REQUIRE(f.reaches(0.0f, 0.0f, 10.0f, 0.0f));   // through the disc
    REQUIRE(f.reaches(0.0f, 12.2f, 10.0f, 12.2f)); // grazing the capsule's side, between its samples
    REQUIRE(f.reaches(6.0f, 5.0f, 6.0f, 15.0f));   // across the capsule
    REQUIRE_FALSE(f.reaches(0.0f, 5.0f, 10.0f, 5.0f));  // between the two, clear of both
    REQUIRE_FALSE(f.reaches(0.0f, 13.0f, 10.0f, 13.0f)); // just outside the capsule
    REQUIRE_FALSE(Field{}.reaches(0.0f, 0.0f, 10.0f, 0.0f));
}
