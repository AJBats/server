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

#include "map/pawn/spell_movement.h"
#include "map/ai/helpers/pathfind/pathfind.h"
#include "map/ai/helpers/pathfind/path_owner.h"
#include "map/ai/helpers/pathfind/pathfind_step.h"
#include "map/navmesh/detour_navmesh.h"

#include <DetourNavMeshBuilder.h>
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <memory>
#include <numbers>

namespace
{
    // Three connected squares form an L. The missing upper-right square
    // forces real Detour routes around its corner instead of a direct step.
    void installFloor(DetourNavMesh& mesh)
    {
        const unsigned short verts[] = {
            0, 0, 0, 5, 0, 0, 10, 0, 0,
            0, 0, 5, 5, 0, 5, 10, 0, 5,
            0, 0, 10, 5, 0, 10
        };
        constexpr unsigned short none = 0xffff;
        const unsigned short polys[] = {
            0, 3, 4, 1, none, 1, 2, none,
            3, 6, 7, 4, none, none, none, 0,
            1, 4, 5, 2, 0, none, none, none
        };
        const unsigned short flags[] = {1, 1, 1};
        const unsigned char areas[] = {0, 0, 0};
        dtNavMeshCreateParams params{};
        params.verts = verts;
        params.vertCount = 8;
        params.polys = polys;
        params.polyFlags = flags;
        params.polyAreas = areas;
        params.polyCount = 3;
        params.nvp = 4;
        params.bmax[0] = params.bmax[2] = 10;
        params.bmax[1] = 2;
        params.cs = params.ch = 1;
        params.walkableHeight = 2;
        params.walkableRadius = 0.1f;
        params.walkableClimb = 1;
        params.buildBvTree = true;

        unsigned char* data = nullptr;
        int size = 0;
        REQUIRE(dtCreateNavMeshData(&params, &data, &size));
        auto* nav = dtAllocNavMesh();
        REQUIRE(nav != nullptr);
        REQUIRE(dtStatusSucceed(nav->init(data, size, DT_TILE_FREE_DATA)));
        REQUIRE(mesh.installNavMesh(nav));
    }

    // Only the entity adapter is replaced. Planning, Detour queries and
    // every FollowPath step are the production implementations.
    struct Walker final : pathfind::PathOwner
    {
        explicit Walker(NavMesh& mesh) : mesh(mesh) {}
        NavMesh& mesh;
        position_t pos{2, 0, -2, 0, 0};
        std::string label = "spell movement test";

        auto position() -> position_t& override { return pos; }
        auto position() const -> const position_t& override { return pos; }
        auto navMesh() -> NavMesh& override { return mesh; }
        auto markPositionDirty() -> void override {}
        auto baseSpeed() const -> uint8 override { return 40; }
        auto updateSpeed(bool) -> uint8 override { return 40; }
        auto isMobEntity() const -> bool override { return false; }
        auto isRoaming() const -> bool override { return false; }
        auto inWater() const -> bool override { return false; }
        auto battleTargetPosition() const -> const position_t* override { return nullptr; }
        auto onPathPoint() -> void override {}
        auto onPathComplete() -> void override {}
        auto name() const -> const std::string& override { return label; }
        auto id() const -> uint32 override { return 0; }
    };

    struct Fixture
    {
        DetourNavMesh mesh{0};
        Walker* walker = nullptr;
        CPathFind path;

        Fixture() : path(makeWalker()) { installFloor(mesh); }

        auto makeWalker() -> std::unique_ptr<Walker>
        {
            auto owner = std::make_unique<Walker>(mesh);
            walker = owner.get();
            return owner;
        }
    };
}

TEST_CASE("A final sub-yalm spell approach uses the real navmesh and enters range", "[cardian][casting][pathfind]")
{
    Fixture f;
    const position_t target{22.2f, 0, -2, 0, 0};
    const auto goal = cardian::casting::approach(f.walker->pos, target, 20, true);
    REQUIRE(goal.has_value());
    CHECK(distance(f.walker->pos, *goal) == Catch::Approx(0.7f));
    // Detour itself has always supported this; the pathfinder's early
    // one-yalm cutoff rejects it unless the pawn walker opts in.
    REQUIRE(f.mesh.findPath(f.walker->pos, *goal).has_value());
    REQUIRE(f.path.PathAround(*goal, cardian::casting::kArrival, PATHFLAG_RUN | PATHFLAG_CARDIAN));
    f.path.FollowPath(timer::now());
    CHECK(f.walker->pos.x == Catch::Approx(2.5f)); // successful precise request still moves only 0.5 y
    CHECK(distance(f.walker->pos, target) < 20);
    CHECK_FALSE(cardian::casting::approach(f.walker->pos, target, 20, true).has_value());
}

TEST_CASE("Short paths retain the walker's arrival tolerance", "[cardian][casting][pathfind]")
{
    Fixture f;
    for (const float hop : {0.0f, 0.05f, 0.09f})
    {
        INFO("hop " << hop);
        CHECK_FALSE(f.path.PathTo(position_t{2 + hop, 0, -2, 0, 0}, PATHFLAG_CARDIAN));
        CHECK_FALSE(f.path.IsFollowingPath());
    }
    for (const float hop : {0.11f, 0.3f, 0.7f, 0.99f, 1.2f})
    {
        INFO("hop " << hop);
        f.walker->pos = position_t{2, 0, -2, 0, 0};
        const position_t goal{2 + hop, 0, -2, 0, 0};
        REQUIRE(f.path.PathTo(goal, PATHFLAG_CARDIAN));
        for (int tick = 0; tick < 5 && f.path.IsFollowingPath(); ++tick)
        {
            f.path.FollowPath(timer::now());
        }
        CHECK(distance(f.walker->pos, goal) < 0.1f);
        CHECK_FALSE(f.path.IsFollowingPath());
    }
}

TEST_CASE("A sub-yalm request still routes around a corner", "[cardian][casting][pathfind]")
{
    Fixture f;
    f.walker->pos = position_t{4.9f, 0, -5.6f, 0, 0};
    const position_t goal{5.6f, 0, -4.9f, 0, 0};
    REQUIRE(distance(f.walker->pos, goal) < 1);
    REQUIRE(f.path.PathTo(goal, PATHFLAG_CARDIAN));
    for (int tick = 0; tick < 5 && f.path.IsFollowingPath(); ++tick)
    {
        f.path.FollowPath(timer::now());
        CHECK_FALSE((f.walker->pos.x > 5.01f && f.walker->pos.z < -5.01f));
    }
    CHECK(distance(f.walker->pos, goal) < 0.1f);
    CHECK_FALSE(f.path.IsFollowingPath());
}

TEST_CASE("A blocked in-range spell follows a route until sight returns", "[cardian][casting][pathfind]")
{
    Fixture f;
    f.walker->pos = position_t{4, 0, -9, 0, 0};
    const position_t target{9, 0, -4, 0, 0};
    const auto initial = f.walker->pos;
    bool ready = false;
    for (int tick = 0; tick < 20; ++tick)
    {
        // From the left leg of this L, the segment must cross x=5 at
        // z>=-5 to see the target, without entering the missing square.
        const auto& me = f.walker->pos;
        const float crossZ = me.z + (target.z - me.z) * (5 - me.x) / (target.x - me.x);
        const bool sight = me.z >= -5 || crossZ >= -5;
        const auto goal = cardian::casting::approach(me, target, 20, sight);
        if (!goal.has_value())
        {
            f.path.Clear(); // Move's Stand intent
            ready = true;
            break;
        }
        REQUIRE(distance(me, target) < 20); // range alone must never stop it
        REQUIRE(goal->x == target.x);
        REQUIRE(goal->z == target.z);
        REQUIRE(f.path.PathAround(*goal, cardian::casting::kArrival, PATHFLAG_RUN | PATHFLAG_CARDIAN));
        f.path.FollowPath(timer::now());
        CHECK_FALSE((me.x > 5.01f && me.z < -5.01f));
    }
    REQUIRE(ready);
    CHECK(distance(f.walker->pos, initial) > 1);
    CHECK(distance(f.walker->pos, target) > 2); // stops before reaching the body
    CHECK_FALSE(f.path.IsFollowingPath());
}

TEST_CASE("Spell approach needs both range and sight and follows a changed target", "[cardian][casting]")
{
    const position_t from{0, 0, 0, 0, 0};
    const position_t near{10, 0, 0, 0, 0};
    const position_t far{24, -8, 0, 0, 0};
    CHECK_FALSE(cardian::casting::approach(from, near, 20, true).has_value());
    CHECK_FALSE(cardian::casting::approach(from, from, 20, true).has_value());
    const auto blocked = cardian::casting::approach(from, far, 20, false);
    REQUIRE(blocked.has_value());
    CHECK(distance(*blocked, far) == 0);
    const auto visible = cardian::casting::approach(from, far, 20, true);
    REQUIRE(visible.has_value());
    CHECK(distance(*visible, far) == Catch::Approx(19.5f));
    CHECK(visible->y < 0); // range is three-dimensional, including a slope
    const auto switched = cardian::casting::approach(from, near, 20, false);
    REQUIRE(switched.has_value());
    CHECK(distance(*switched, near) == 0);
}

TEST_CASE("Unflagged path requests retain the original one-yalm cutoff", "[cardian][casting][pathfind]")
{
    Fixture f;
    for (const float hop : {0.0f, 0.05f, 0.7f, 0.99f})
    {
        INFO("hop " << hop);
        const position_t goal{2 + hop, 0, -2, 0, 0};
        CHECK_FALSE(f.path.PathTo(goal));
        CHECK_FALSE(f.path.PathTo(goal, PATHFLAG_RUN));
        CHECK_FALSE(f.path.PathAround(goal, 0.2f, PATHFLAG_RUN));
        CHECK_FALSE(f.path.PathInRange(goal, 0.2f, PATHFLAG_RUN));
        CHECK_FALSE(f.path.IsFollowingPath());
    }
    // Exactly one yalm was accepted by isNear's strict less-than check.
    for (const float hop : {1.0f, 1.2f})
    {
        INFO("hop " << hop);
        const position_t goal{2 + hop, 0, -2, 0, 0};
        CHECK(f.path.PathTo(goal, PATHFLAG_RUN));
        CHECK(f.path.PathAround(goal, 0.2f, PATHFLAG_RUN));
        CHECK(f.path.PathInRange(goal, 0.2f, PATHFLAG_RUN));
    }
}

TEST_CASE("A precise Cardian request does not enable short paths for later callers", "[cardian][casting][pathfind]")
{
    Fixture f;
    const position_t goal{2.7f, 0, -2, 0, 0};
    // This is the same option as CPawnController::PathToward. Replace an
    // active path without an explicit Clear to test each request's reset.
    constexpr uint8 precise = PATHFLAG_RUN | PATHFLAG_CARDIAN;
    REQUIRE(f.path.PathAround(goal, 0.2f, precise));
    CHECK_FALSE(f.path.PathAround(goal, 0.2f, PATHFLAG_RUN));
    REQUIRE(f.path.PathAround(goal, 0.2f, precise));
    CHECK_FALSE(f.path.PathTo(goal, PATHFLAG_RUN));
    REQUIRE(f.path.PathAround(goal, 0.2f, precise));
    CHECK_FALSE(f.path.PathInRange(goal, 0.2f, PATHFLAG_RUN));
    REQUIRE(f.path.PathAround(goal, 0.2f, precise));
    f.path.Clear();
    CHECK_FALSE(f.path.PathTo(goal));
    CHECK_FALSE(f.path.IsFollowingPath());
}

TEST_CASE("A short Cardian request still rejects an unusable one-point mesh route", "[cardian][casting][pathfind]")
{
    Fixture f;
    // Stand at the floor's edge and request a point 0.7 y beyond it.
    // Both endpoints project onto the same mesh point: Detour produces
    // only the start, which LSB must reject even with the Cardian flag.
    f.walker->pos = position_t{0, 0, -2, 0, 0};
    const auto before = f.walker->pos;
    const position_t goal{-0.7f, 0, -2, 0, 0};
    REQUIRE(distance(before, goal) > 0.1f);
    REQUIRE(distance(before, goal) < 1.0f);
    const auto projected = f.mesh.findClosestValidPoint(goal);
    REQUIRE(projected.has_value());
    REQUIRE(distance(before, *projected) < 0.0001f);
    REQUIRE_FALSE(f.mesh.findPath(before, goal).has_value());

    // A rejected replacement must also drop the old route, not continue
    // walking along an unrelated path or step directly into missing mesh.
    REQUIRE(f.path.PathTo(position_t{2, 0, -2, 0, 0}, PATHFLAG_RUN));
    REQUIRE(f.path.IsFollowingPath());
    CHECK_FALSE(f.path.PathAround(goal, cardian::casting::kArrival, PATHFLAG_RUN | PATHFLAG_CARDIAN));
    CHECK_FALSE(f.path.IsFollowingPath());
    f.path.FollowPath(timer::now());
    CHECK(distance(f.walker->pos, before) == 0.0f);
}

TEST_CASE("PathTo the spell target recovers a short waypoint collapsed onto the start", "[cardian][casting][pathfind]")
{
    Fixture f;
    // At this edge of the L, a 0.7-y request projects back onto the start.
    // The target projects onto the other leg: PathTo routes round the notch.
    f.walker->pos = position_t{5, 0, -6, 0, 0};
    const position_t target{25.2f, 0, -6, 0, 0};
    const auto initial = f.walker->pos;
    const auto goal = cardian::casting::approach(initial, target, 20, true);
    REQUIRE(goal.has_value());
    REQUIRE(distance(initial, *goal) == Catch::Approx(0.7f));
    const auto projected = f.mesh.findClosestValidPoint(*goal);
    REQUIRE(projected.has_value());
    REQUIRE(distance(initial, *projected) < 0.0001f);
    REQUIRE_FALSE(f.mesh.findPath(initial, *goal).has_value());

    constexpr uint8 flags = PATHFLAG_RUN | PATHFLAG_CARDIAN;
    REQUIRE_FALSE(f.path.PathAround(*goal, cardian::casting::kArrival, flags));
    REQUIRE_FALSE(f.path.PathAround(*projected, cardian::casting::kArrival, flags));
    f.mesh.snapToValidPosition(f.walker->pos);
    REQUIRE_FALSE(f.path.PathAround(*goal, cardian::casting::kArrival, flags));

    REQUIRE(f.path.PathTo(target, flags));
    f.path.FollowPath(timer::now());
    CHECK(distance(initial, f.walker->pos) > 0);
    CHECK_FALSE((f.walker->pos.x > 5.01f && f.walker->pos.z < -5.01f));

    // The retry is not a new mode: subsequent ticks use precise requests
    // again and stop as soon as the spell is in range.
    bool ready = false;
    for (int tick = 0; tick < 10; ++tick)
    {
        const auto next = cardian::casting::approach(f.walker->pos, target, 20, true);
        if (!next.has_value())
        {
            f.path.Clear();
            ready = true;
            break;
        }
        REQUIRE((f.path.PathAround(*next, cardian::casting::kArrival, flags) || f.path.PathTo(target, flags)));
        f.path.FollowPath(timer::now());
        CHECK_FALSE((f.walker->pos.x > 5.01f && f.walker->pos.z < -5.01f));
    }
    CHECK(ready);
    CHECK(distance(f.walker->pos, target) <= 20);
    CHECK_FALSE(f.path.IsFollowingPath());
}

TEST_CASE("PathTo fallback gets around a notch that a fixed 1.2-y request cannot", "[cardian][casting][pathfind]")
{
    Fixture f;
    f.walker->pos = position_t{5, 0, -8, 0, 0};
    const auto initial = f.walker->pos;
    const position_t target{25.2f, 0, -8, 0, 0};
    constexpr uint8 flags = PATHFLAG_RUN | PATHFLAG_CARDIAN;
    const auto goal = cardian::casting::approach(initial, target, 20, true);
    REQUIRE(goal.has_value());
    REQUIRE_FALSE(f.path.PathAround(*goal, cardian::casting::kArrival, flags));
    REQUIRE_FALSE(f.path.PathAround(position_t{6.2f, 0, -8, 0, 0}, cardian::casting::kArrival, flags));

    int fallbacks = 0;
    bool ready = false;
    for (int tick = 0; tick < 20; ++tick)
    {
        const auto next = cardian::casting::approach(f.walker->pos, target, 20, true);
        if (!next.has_value())
        {
            f.path.Clear();
            ready = true;
            break;
        }
        if (!f.path.PathAround(*next, cardian::casting::kArrival, flags))
        {
            ++fallbacks;
            REQUIRE(f.path.PathTo(target, flags));
        }
        const auto before = f.walker->pos;
        f.path.FollowPath(timer::now());
        CHECK(distance(before, f.walker->pos) > 0);
        CHECK_FALSE((f.walker->pos.x > 5.01f && f.walker->pos.z < -5.01f));
    }
    CHECK(fallbacks > 1); // full-target routing keeps working over successive ticks
    REQUIRE(ready);
    CHECK(distance(f.walker->pos, target) <= 20);
    CHECK(distance(f.walker->pos, target) > 19); // stops near cast range, not at the target
    CHECK_FALSE(f.path.IsFollowingPath());
}

TEST_CASE("Spell fallback still refuses an unreachable destination", "[cardian][casting][pathfind]")
{
    Fixture f;
    f.walker->pos = position_t{0, 0, -2, 0, 0};
    const auto initial = f.walker->pos;
    const position_t target{-20.2f, 0, -2, 0, 0};
    const auto goal = cardian::casting::approach(initial, target, 20, true);
    REQUIRE(goal.has_value());
    constexpr uint8 flags = PATHFLAG_RUN | PATHFLAG_CARDIAN;
    REQUIRE_FALSE(f.path.PathAround(*goal, cardian::casting::kArrival, flags));
    REQUIRE_FALSE(f.path.PathTo(target, flags));
    CHECK_FALSE(f.path.IsFollowingPath());
    f.path.FollowPath(timer::now());
    CHECK(distance(f.walker->pos, initial) == 0);
}

TEST_CASE("A projected spell route that cannot advance triggers PathTo the target", "[cardian][casting][pathfind]")
{
    // Both requests return a real, nonempty Detour route. Previously the
    // first produced no movement and the second stepped backwards, so
    // neither could reach the failure-only fallback in the pawn walker.
    for (const auto& target : {position_t{24.449717f, 0, -0.442938f, 0, 0},
                               position_t{25.2f, 0, -5, 0, 0}})
    {
        INFO("target " << target.x << ", " << target.z);
        Fixture f;
        f.walker->pos = position_t{5, 0, -6, 0, 0};
        const auto initial = f.walker->pos;
        const auto goal = cardian::casting::approach(initial, target, 20, true);
        REQUIRE(goal.has_value());
        const auto raw = f.mesh.findPath(initial, *goal);
        REQUIRE(raw.has_value());
        REQUIRE(raw->points.size() == 1);
        CHECK(distance(initial, raw->points.back().position) <= cardian::casting::kArrival);

        constexpr uint8 flags = PATHFLAG_RUN | PATHFLAG_CARDIAN;
        // A failed replacement also discards any previous, unrelated path.
        REQUIRE(f.path.PathTo(position_t{4, 0, -8, 0, 0}, flags));
        REQUIRE_FALSE(f.path.PathAround(*goal, cardian::casting::kArrival, flags));
        CHECK_FALSE(f.path.IsFollowingPath());
        f.path.FollowPath(timer::now());
        CHECK(distance(initial, f.walker->pos) == 0);

        // PathToward's endpoint/start retries must report the same failure.
        const auto projected = f.mesh.findClosestValidPoint(*goal);
        REQUIRE(projected.has_value());
        REQUIRE_FALSE(f.path.PathAround(*projected, cardian::casting::kArrival, flags));
        f.mesh.snapToValidPosition(f.walker->pos);
        REQUIRE_FALSE(f.path.PathAround(*goal, cardian::casting::kArrival, flags));

        REQUIRE(f.path.PathTo(target, flags));
        f.path.FollowPath(timer::now());
        CHECK(distance(initial, f.walker->pos) > 0);
        CHECK_FALSE((f.walker->pos.x > 5.01f && f.walker->pos.z < -5.01f));

        bool ready = false;
        for (int tick = 0; tick < 10; ++tick)
        {
            const auto next = cardian::casting::approach(f.walker->pos, target, 20, true);
            if (!next.has_value())
            {
                f.path.Clear();
                ready = true;
                break;
            }
            REQUIRE((f.path.PathAround(*next, cardian::casting::kArrival, flags) || f.path.PathTo(target, flags)));
            f.path.FollowPath(timer::now());
            CHECK_FALSE((f.walker->pos.x > 5.01f && f.walker->pos.z < -5.01f));
        }
        CHECK(ready);
        CHECK(distance(f.walker->pos, target) <= 20);
    }
}

TEST_CASE("Cardian arrival retains small forward steps and never backs away", "[cardian][casting][pathfind]")
{
    for (const float hop : {0.20001f, 0.2001f, 0.21f, 0.3f, 0.41f, 0.7f})
    {
        INFO("hop " << hop);
        Fixture f;
        const auto initial = f.walker->pos;
        const position_t goal{initial.x + hop, 0, -2, 0, 0};
        REQUIRE(f.path.PathAround(goal, 0.2f, PATHFLAG_RUN | PATHFLAG_CARDIAN));
        f.path.FollowPath(timer::now());
        CHECK(distance(initial, f.walker->pos) > 0);
        CHECK(distance(initial, f.walker->pos) == Catch::Approx(hop - 0.2f).margin(0.00001f));
    }

    Fixture f;
    const position_t goal{3, 0, -2, 0, 0};
    REQUIRE(f.path.PathAround(goal, 0.2f, PATHFLAG_RUN | PATHFLAG_CARDIAN));
    // Already inside arrival distance when the active path is followed:
    // completion must not push her back out by the missing 0.15 y.
    f.walker->pos = position_t{2.95f, 0, -2, 0, 0};
    const auto arrived = f.walker->pos;
    f.path.FollowPath(timer::now());
    CHECK(distance(arrived, f.walker->pos) == 0);
    CHECK_FALSE(f.path.IsFollowingPath());
}

TEST_CASE("A close final point retains a necessary Cardian corner detour", "[cardian][casting][pathfind]")
{
    Fixture f;
    f.walker->pos = position_t{4.99f, 0, -5.12f, 0, 0};
    const auto initial = f.walker->pos;
    const position_t goal{5.12f, 0, -4.99f, 0, 0};
    REQUIRE(distance(initial, goal) < 0.2f);
    const auto raw = f.mesh.findPath(initial, goal);
    REQUIRE(raw.has_value());
    REQUIRE(raw->points.size() > 1);
    REQUIRE(distance(initial, raw->points.front().position) > 0.1f);
    REQUIRE(f.path.PathAround(goal, 0.2f, PATHFLAG_RUN | PATHFLAG_CARDIAN));
    f.path.FollowPath(timer::now());
    CHECK(distance(initial, f.walker->pos) > 0.1f);
    CHECK_FALSE((f.walker->pos.x > 5.01f && f.walker->pos.z < -5.01f));
    const auto corner = f.walker->pos;
    f.path.FollowPath(timer::now());
    CHECK(distance(corner, f.walker->pos) == 0);
    CHECK_FALSE(f.path.IsFollowingPath());
}

TEST_CASE("Projected stop-short rejection is confined to Cardian requests", "[cardian][casting][pathfind]")
{
    Fixture f;
    f.walker->pos = position_t{0, 0, -2, 0, 0};
    const position_t goal{-1.2f, 0, -1.95f, 0, 0};
    // The original request clears the ordinary one-yalm cutoff, but its
    // projected route is inside stop-short. Ordinary admission is unchanged.
    REQUIRE(f.path.PathAround(goal, 0.2f, PATHFLAG_RUN));
    REQUIRE_FALSE(f.path.PathAround(goal, 0.2f, PATHFLAG_RUN | PATHFLAG_CARDIAN));
    CHECK_FALSE(f.path.IsFollowingPath());
    REQUIRE(f.path.PathAround(goal, 0.2f, PATHFLAG_RUN));
}

TEST_CASE("Blocked-sight fallback can reach a projected endpoint inside stop-short", "[cardian][casting][pathfind]")
{
    Fixture f;
    f.walker->pos = position_t{0, 0, -2, 0, 0};
    const auto initial = f.walker->pos;
    const position_t target{-0.7f, 0, -1.85f, 0, 0};
    const auto goal = cardian::casting::approach(initial, target, 20, false);
    REQUIRE(goal.has_value());
    REQUIRE(distance(initial, *goal) > cardian::casting::kTolerance);
    constexpr uint8 flags = PATHFLAG_RUN | PATHFLAG_CARDIAN;
    REQUIRE_FALSE(f.path.PathAround(*goal, cardian::casting::kArrival, flags));
    // The destination is the same, but PathTo drops the failed 0.2-y stop-short.
    REQUIRE(f.path.PathTo(target, flags));
    f.path.FollowPath(timer::now());
    CHECK(distance(initial, f.walker->pos) == Catch::Approx(0.15f));
    // The next check ends the route once sight returns, even with recast pending.
    REQUIRE_FALSE(cardian::casting::approach(f.walker->pos, target, 20, true).has_value());
    f.path.Clear();
    CHECK_FALSE(f.path.IsFollowingPath());
}

TEST_CASE("A spell route whose final step rounds to unchanged XYZ reaches the fallback", "[cardian][casting][pathfind]")
{
    Fixture f;
    constexpr uint8 flags = PATHFLAG_RUN | PATHFLAG_CARDIAN;
    const position_t initial{0, 0, -0.1f, 0, 0};
    const float angle = 195.0f * std::numbers::pi_v<float> / 180.0f;
    const position_t target{20.7f * std::cos(angle), 0, initial.z + 20.7f * std::sin(angle), 0, 0};
    // The original precise requests converged here, reporting success
    // with zero movement through tick 500, still 20.4527 y from the target.
    const position_t stalled{0.002390215639f, 0, -1.164338946f, 0, 0};
    f.walker->pos = stalled;
    const auto goal = cardian::casting::approach(stalled, target, 20, true);
    REQUIRE(goal.has_value());
    const auto raw = f.mesh.findPath(stalled, *goal);
    REQUIRE(raw.has_value());
    REQUIRE(raw->points.size() == 1);
    const auto& end = raw->points.back().position;
    // Squared-distance admission passes, but the actual step cannot advance.
    REQUIRE_FALSE(isWithinDistance(stalled, end, cardian::casting::kArrival));
    auto stepped = stalled;
    pathfind::stepTowards(stepped, end, 0.8f, cardian::casting::kArrival);
    REQUIRE(distanceSquared(stalled, stepped) == 0.0f);
    REQUIRE_FALSE(f.path.PathAround(*goal, cardian::casting::kArrival, flags));
    CHECK_FALSE(f.path.IsFollowingPath());

    // Include the pawn walker's snapped-endpoint and off-mesh-start retries:
    // none may turn this same unusable precise route back into success.
    const auto request = [&](const position_t& point, const float stopShort)
    {
        const auto path = [&](const position_t& p)
        {
            return stopShort == 0.0f ? f.path.PathTo(p, flags) : f.path.PathAround(p, stopShort, flags);
        };
        if (path(point))
        {
            return true;
        }
        if (const auto snapped = f.mesh.findClosestValidPoint(point); snapped.has_value() && path(*snapped))
        {
            return true;
        }
        f.mesh.snapToValidPosition(f.walker->pos);
        return path(point);
    };
    REQUIRE_FALSE(request(*goal, cardian::casting::kArrival));
    REQUIRE(request(target, 0.0f));
    f.path.FollowPath(timer::now());
    CHECK(distance(stalled, f.walker->pos) > 0.1f);

    // Keep precision on each new tick, falling back only on failure. Both
    // the original start and the frozen position must eventually enter range.
    for (const auto& start : {initial, stalled})
    {
        f.path.Clear();
        f.walker->pos = start;
        bool ready = false;
        int fallbacks = 0;
        for (int tick = 0; tick < 500; ++tick)
        {
            const auto next = cardian::casting::approach(f.walker->pos, target, 20, true);
            if (!next.has_value())
            {
                f.path.Clear();
                ready = true;
                break;
            }
            REQUIRE(distance(f.walker->pos, *next) > cardian::casting::kTolerance);
            if (!request(*next, cardian::casting::kArrival))
            {
                ++fallbacks;
                REQUIRE(request(target, 0.0f));
            }
            const auto before = f.walker->pos;
            f.path.FollowPath(timer::now());
            REQUIRE(distanceSquared(before, f.walker->pos) > 0.0f);
        }
        CHECK(ready);
        CHECK(fallbacks > 0);
        CHECK(distance(f.walker->pos, target) <= 20);
        CHECK_FALSE(f.path.IsFollowingPath());
    }
}

TEST_CASE("A blocked spell inside the old arrival tolerance still walks its corner route", "[cardian][casting][pathfind]")
{
    Fixture f;
    f.walker->pos = position_t{4.99f, 0, -5.12f, 0, 0};
    const auto initial = f.walker->pos;
    const position_t target{5.12f, 0, -4.99f, 0, 0};
    REQUIRE(distance(initial, target) < 0.3f);
    REQUIRE(f.mesh.findPath(initial, target).has_value());
    bool ready = false;
    for (int tick = 0; tick < 10; ++tick)
    {
        const auto& me = f.walker->pos;
        const float crossZ = me.z + (target.z - me.z) * (5 - me.x) / (target.x - me.x);
        const bool sight = me.z >= -5 || crossZ >= -5;
        const auto goal = cardian::casting::approach(me, target, 20, sight);
        if (!goal.has_value())
        {
            f.path.Clear();
            ready = true;
            break;
        }
        // Exercise the controller's arrival gate as well as the real route.
        // With the old 0.3-y tolerance this never attempts either request.
        if (distance(me, *goal) > cardian::casting::kTolerance)
        {
            constexpr uint8 flags = PATHFLAG_RUN | PATHFLAG_CARDIAN;
            REQUIRE((f.path.PathAround(*goal, cardian::casting::kArrival, flags) || f.path.PathTo(target, flags)));
            f.path.FollowPath(timer::now());
            CHECK_FALSE((me.x > 5.001f && me.z < -5.001f));
        }
        else
        {
            f.path.Clear();
        }
    }
    REQUIRE(ready);
    CHECK(distance(initial, f.walker->pos) > 0.1f);
    CHECK_FALSE(f.path.IsFollowingPath());
}
