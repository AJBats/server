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

// The stake's arithmetic (RESEARCH §12.16): the tank's tow point and the
// dissolve rule. Pure, so a change here fails before a map server runs.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "map/pawn/stake_math.h"

#include <numbers>

using namespace cardian::stake;
using Catch::Matchers::WithinAbs;

TEST_CASE("Stake: a stopped player pull immediately owns its spot in the front half", "[cardian][stake]")
{
    // The sapling report: already stopped 3.8 y forward, 3.9 y from the
    // old landing point. Neither six nor eight seconds of fighting is needed.
    CHECK(keepsFightSpot(false, true, 5.2f, 3.8f));
    CHECK_FALSE(keepsFightSpot(false, false, 5.2f, 3.8f));
    CHECK(keepsFightSpot(true, false, 5.4f, 3.9f)); // local movement after acceptance
    CHECK(keepsFightSpot(false, true, 20.0f, 0.0f));
    CHECK_FALSE(keepsFightSpot(true, true, 20.01f, 2.0f));
    CHECK_FALSE(keepsFightSpot(true, true, 5.0f, -0.01f));
    CHECK_FALSE(keepsFightSpot(false, true, 49.0f, 40.0f)); // admission is not relocation
}

TEST_CASE("Stake: leaving the accepted area requires a new stop, with no distance timeout", "[cardian][stake]")
{
    bool kept = keepsFightSpot(false, true, 19.0f, 3.0f);
    REQUIRE(kept);
    kept = keepsFightSpot(kept, false, 21.0f, 3.0f);
    CHECK_FALSE(kept);
    kept = keepsFightSpot(kept, false, 19.0f, 3.0f);
    CHECK_FALSE(kept);
    kept = keepsFightSpot(kept, true, 19.0f, 3.0f);
    CHECK(kept);
    // The radius is centered on the flag, at every heading, not the point
    // two yalms in front of it. A 19-yalm lateral pull is acceptable.
    for (int heading = 0; heading < 256; heading += 8)
    {
        const auto rotation = static_cast<uint8>(heading);
        const float angle = heading * 2.0f * std::numbers::pi_v<float> / 256.0f;
        const float x = 0.1f * std::cos(angle) + 19.0f * std::sin(angle);
        const float z = -0.1f * std::sin(angle) + 19.0f * std::cos(angle);
        CHECK(keepsFightSpot(false, true, std::hypot(x, z), forwardOf(0, 0, rotation, x, z)));
    }
}

TEST_CASE("Stake settlement: confirm a melee stop on the next mob update, never two reads of one update", "[cardian][stake][settlement]")
{
    Settlement stop;
    CHECK_FALSE(stop.observe(100, 99, 5, 0, 3, true));
    CHECK_FALSE(stop.observe(100, 99, 5, 0, 3, true));
    CHECK(stop.observe(101, 100, 5, 0, 3, true));
    // There is no eight-second dwell, weapon-delay or first-swing condition.
    CHECK(keepsFightSpot(false, stop.observe(102, 101, 5, 0, 3, true), std::hypot(5.f, 3.f), 5));
}

TEST_CASE("Stake settlement: a pathless pause without available melee cannot establish the spot", "[cardian][stake][settlement]")
{
    Settlement stop;
    // The adapter reports unavailable melee for a dead/missing target,
    // an active path, a wall, a ranged/casting pause or an out-of-reach target.
    for (int tick = 1; tick <= 30; ++tick)
    {
        CHECK_FALSE(stop.observe(tick, tick - 1, 5, 0, 3, false));
    }
    CHECK_FALSE(stop.observe(31, 30, 5, 0, 3, true));
    CHECK(stop.observe(32, 31, 5, 0, 3, true));
    // Starting an action between observations requires a new confirmation.
    CHECK_FALSE(stop.observe(33, 32, 5, 0, 3, false));
    CHECK_FALSE(stop.observe(34, 33, 5, 0, 3, true));
    CHECK(stop.observe(35, 34, 5, 0, 3, true));
}

TEST_CASE("Stake settlement: arrival movement does not count, including vertical movement", "[cardian][stake][settlement]")
{
    Settlement stop;
    CHECK_FALSE(stop.observe(1, 0, 10, 0, 0, true));
    CHECK_FALSE(stop.observe(2, 1, 9, 0, 0, true));
    CHECK_FALSE(stop.observe(3, 2, 8, 0, 0, true));
    CHECK(stop.observe(4, 3, 8.02f, 0, 0, true)); // positional slack
    CHECK_FALSE(stop.observe(5, 4, 8.02f, 1, 0, true));
    CHECK_FALSE(stop.observe(6, 5, 8.10f, 1.08f, 0, true)); // >0.1 y in 3D
    CHECK(stop.observe(7, 6, 8.10f, 1.08f, 0, true));
}

TEST_CASE("Stake settlement: missing observations and new fights cannot reuse old stillness", "[cardian][stake][settlement]")
{
    Settlement stop;
    CHECK_FALSE(stop.observe(1, 0, 5, 0, 0, true));
    CHECK_FALSE(stop.observe(10, 9, 5, 0, 0, true)); // no observation of update 9
    CHECK(stop.observe(11, 10, 5, 0, 0, true));
    stop = {}; // new mob, new camp, explicit replacement or end of fight
    CHECK_FALSE(stop.observe(12, 11, 5, 0, 0, true));
    CHECK(stop.observe(13, 12, 5, 0, 0, true));
}

TEST_CASE("Stake settlement: later spells and hate changes preserve an accepted fight", "[cardian][stake][settlement]")
{
    Settlement stop;
    CHECK_FALSE(stop.observe(1, 0, 5, 0, 0, true));
    bool kept = keepsFightSpot(false, stop.observe(2, 1, 5, 0, 0, true), 5, 5);
    REQUIRE(kept);
    kept = keepsFightSpot(kept, stop.observe(3, 2, 5, 0, 0, false), 5, 5); // casts
    CHECK(kept);
    kept = keepsFightSpot(kept, stop.observe(4, 3, 6, 0, 0, false), 6, 6); // pursues nearby new target
    CHECK(kept);
    kept = keepsFightSpot(kept, stop.observe(5, 4, -1, 0, 0, true), 1, -1);
    CHECK_FALSE(kept); // crossing the frontline still revokes permission
}

TEST_CASE("Stake receive: arrival or hate joins immediately, never an idle draw", "[cardian][stake][receive]")
{
    const ReceiveConfig config;
    Receive r;
    CHECK(r.update(0, 2, true, false, false, config) == ReceiveAction::Wait);
    CHECK(r.update(20, 2, true, false, false, config) == ReceiveAction::Wait);
    CHECK(r.update(21, 3, true, false, true, config) == ReceiveAction::Join);
    r = {};
    CHECK(r.update(0, 20, true, false, true, config) == ReceiveAction::Wait);
    CHECK(r.update(0.1, 20, true, true, true, config) == ReceiveAction::Join);
    // Provoke succeeded: subsequent hate loss never repeats the receive.
    CHECK(r.update(0.2, 20, true, false, true, config) == ReceiveAction::Join);
}

TEST_CASE("Stake receive: stationary pulls wait by distance with an eight-second cap", "[cardian][stake][receive]")
{
    const ReceiveConfig config;
    for (const auto [d, wait] : { std::pair{7.f, 2.0}, {11.f, 4.0}, {15.f, 6.0}, {19.f, 8.0}, {40.f, 8.0} })
    {
        Receive r;
        CHECK(r.update(100, d, true, false, true, config) == ReceiveAction::Wait);
        CHECK(r.update(101, d, true, false, true, config) == ReceiveAction::Wait);
        CHECK(r.update(100 + wait - 0.01, d, true, false, true, config) == ReceiveAction::Wait);
        CHECK(r.update(100 + wait, d, true, false, true, config) == ReceiveAction::Join);
    }
}

TEST_CASE("Stake receive: approach cancels the wait, a stop starts it from last progress", "[cardian][stake][receive]")
{
    const ReceiveConfig config;
    Receive r;
    for (int t = 0; t <= 20; ++t)
    {
        CHECK(r.update(t, 27.f - t, true, false, true, config) == ReceiveAction::Wait);
    }
    CHECK(r.update(21, 7, true, false, true, config) == ReceiveAction::Wait);
    CHECK(r.update(21.9, 7, true, false, true, config) == ReceiveAction::Wait);
    CHECK(r.update(22, 7, true, false, true, config) == ReceiveAction::Join);
}

TEST_CASE("Stake receive: a flyby freezes the deadline and jitter cannot extend it", "[cardian][stake][receive]")
{
    const ReceiveConfig config;
    Receive r;
    CHECK(r.update(0, 12, true, false, true, config) == ReceiveAction::Wait);
    CHECK(r.update(1, 11, true, false, true, config) == ReceiveAction::Wait);
    CHECK(r.update(2, 15, true, false, true, config) == ReceiveAction::Wait);
    CHECK(r.update(3, 20, true, false, true, config) == ReceiveAction::Wait);
    CHECK(r.update(4, 19.8f, true, false, true, config) == ReceiveAction::Wait);
    CHECK(r.update(5, 24, true, false, true, config) == ReceiveAction::Join);
}

TEST_CASE("Stake receive: resumed approach earns a fresh grace period", "[cardian][stake][receive]")
{
    const ReceiveConfig config;
    Receive r;
    CHECK(r.update(0, 11, true, false, true, config) == ReceiveAction::Wait);
    CHECK(r.update(1, 11, true, false, true, config) == ReceiveAction::Wait);
    CHECK(r.update(3.5, 10, true, false, true, config) == ReceiveAction::Wait);
    CHECK(r.update(4.5, 10, true, false, true, config) == ReceiveAction::Wait);
    CHECK(r.update(6.9, 10, true, false, true, config) == ReceiveAction::Wait);
    CHECK(r.update(7, 10, true, false, true, config) == ReceiveAction::Join);
}

TEST_CASE("Stake receive: camp boundary applies before commitment, not after", "[cardian][stake][receive]")
{
    const ReceiveConfig config;
    Receive r;
    CHECK(r.update(0, 40, true, false, true, config) == ReceiveAction::Wait);
    CHECK(r.update(8, 55, false, false, true, config) == ReceiveAction::Outside);
    CHECK_FALSE(r.sampled);
    CHECK(r.update(10, 11, true, false, true, config) == ReceiveAction::Wait);
    CHECK(r.update(13.9, 11, true, false, true, config) == ReceiveAction::Wait);
    CHECK(r.update(14, 11, true, false, true, config) == ReceiveAction::Join);
    CHECK(r.update(15, 55, false, false, true, config) == ReceiveAction::Join);
}

TEST_CASE("Stake: the tow point is a reach past the stake, away from the mob", "[cardian][stake]")
{
    // The mob ten east of the stake at the origin: she waits a reach west
    // of it, and the mob chasing her stops on the stake
    const auto [x, z] = towPoint(10.0f, 0.0f, 0.0f, 0.0f, 3.0f, 0.0f);
    CHECK_THAT(x, WithinAbs(-3.0f, 1e-4));
    CHECK_THAT(z, WithinAbs(0.0f, 1e-4));

    // From the north-east, the point sits on the same line beyond the stake
    const auto [dx, dz] = towPoint(10.0f, 10.0f, 4.0f, 4.0f, 2.0f, 0.0f);
    CHECK_THAT(dx, WithinAbs(4.0f - std::sqrt(2.0f), 1e-3));
    CHECK_THAT(dz, WithinAbs(4.0f - std::sqrt(2.0f), 1e-3));
}

TEST_CASE("Stake: a mob on the stake has no line, so the tow point takes the fallback bearing", "[cardian][stake]")
{
    const auto [x, z] = towPoint(5.0f, 5.0f, 5.0f, 5.0f, 3.0f, 64);
    CHECK_THAT(x, WithinAbs(5.0f, 1e-3));
    CHECK_THAT(z, WithinAbs(2.0f, 1e-3));
}

TEST_CASE("Stake: only a settled monster receives the tow drift band", "[cardian][stake]")
{
    CHECK(towing(true, false, 8.0f, 5.2f));
    CHECK(towing(false, true, 8.0f, 5.2f));
    CHECK_FALSE(towing(false, true, 5.2f, 5.2f));
    CHECK_FALSE(towing(false, false, 8.0f, 5.2f));
    CHECK(towing(false, false, 10.5f, 5.2f));
    CHECK_FALSE(towing(true, true, 3.0f, 5.2f));
}

TEST_CASE("Stake: distant waiting bodies do not establish the owner's departure", "[cardian][stake]")
{
    Census loading(30);
    loading.observe(31, false); // an owned cardian waiting elsewhere
    CHECK_FALSE(loading.dissolves());
    loading.observe(31, true); // owner actually arrived elsewhere
    CHECK(loading.dissolves());

    Census occupied(30);
    occupied.observe(31, true);
    occupied.observe(30, false); // somebody still keeps the camp
    CHECK_FALSE(occupied.dissolves());

    Census ownerHere(30);
    ownerHere.observe(30, true);
    ownerHere.observe(31, false);
    CHECK_FALSE(ownerHere.dissolves());
    CHECK_FALSE(Census(30).dissolves());
}

TEST_CASE("Stake: the flag is a frontline at every camp heading", "[cardian][stake]")
{
    for (int heading = 0; heading < 256; heading += 8)
    {
        const auto rotation = static_cast<uint8>(heading);
        const float angle = heading * 2.0f * std::numbers::pi_v<float> / 256.0f;
        const float fx = std::cos(angle);
        const float fz = -std::sin(angle);
        const float goalX = 100 + fx * kMobAhead;
        const float goalZ = -200 + fz * kMobAhead;
        CHECK_THAT(forwardOf(100, -200, rotation, goalX, goalZ), WithinAbs(kMobAhead, 1e-4));
        CHECK(forwardOf(100, -200, rotation, 100 - fx * 5, -200 - fz * 5) < 0);

        // Incoming mobs from any side are aimed at the same forward stop.
        for (int approach = 0; approach < 8; ++approach)
        {
            const float a = approach * std::numbers::pi_v<float> / 4;
            const float mx = 100 + std::cos(a) * 10;
            const float mz = -200 + std::sin(a) * 10;
            const auto [tx, tz] = towPoint(mx, mz, goalX, goalZ, 3.3f, rotation);
            const float length = std::hypot(tx - mx, tz - mz);
            const float stopX = tx - (tx - mx) * 3.3f / length;
            const float stopZ = tz - (tz - mz) * 3.3f / length;
            CHECK_THAT(forwardOf(100, -200, rotation, stopX, stopZ), WithinAbs(kMobAhead, 1e-4));
        }
    }
}

TEST_CASE("Stake: tight settling allows small drift but never accepts the rear half", "[cardian][stake]")
{
    CHECK(frontlineTowing(true, false, 2.0f, 2.0f));
    CHECK(frontlineTowing(false, true, 2.0f, 2.0f));
    CHECK_FALSE(frontlineTowing(false, true, 1.5f, 0.5f));
    CHECK_FALSE(frontlineTowing(false, false, 2.9f, 2.0f));
    CHECK(frontlineTowing(false, false, 3.1f, 2.0f));
    CHECK(frontlineTowing(false, false, 2.1f, -0.1f));
    CHECK(kMobAhead - kSettle > 0); // the entire arrival circle is forward
}

TEST_CASE("Stake: tow point keeps camp between the tank and incoming mob", "[cardian][stake]")
{
    for (const auto mob : { std::pair{ 10.f, 0.f }, std::pair{ -3.f, 5.f }, std::pair{ 2.f, -7.f } })
    {
        const auto p = towPoint(mob.first, mob.second, 0, 0, 3, 0);
        CHECK_THAT(std::hypot(p.first, p.second), WithinAbs(3.f, 1e-4));
        CHECK_THAT(std::hypot(p.first - mob.first, p.second - mob.second), WithinAbs(std::hypot(mob.first, mob.second) + 3, 1e-4));
    }
}

TEST_CASE("Stake: it dissolves only once the party is seen elsewhere and nobody is left in its zone", "[cardian][stake]")
{
    CHECK(dissolves(0, 1));
    CHECK(dissolves(0, 4));
    CHECK_FALSE(dissolves(1, 3));
    CHECK_FALSE(dissolves(3, 0));
    // A zone line: everyone is between zones, nobody is seen anywhere
    CHECK_FALSE(dissolves(0, 0));
}

TEST_CASE("Stake: recovery detours converge instead of chasing the tank's live flank", "[cardian][stake]")
{
    // Re-evaluate at each step while the mob continually turns toward the
    // tank. Its heading must not move the waypoint she is trying to reach.
    for (int heading = 0; heading < 360; heading += 15)
    {
        const float angle = heading * std::numbers::pi_v<float> / 180.0f;
        for (const float goalRadius : { 1.5f, 3.3f, 8.0f })
        {
            const float clearance = cardian::formation::meleeClearance(std::min(goalRadius, 3.0f), 1.5f);
            const float gx = std::cos(angle) * goalRadius;
            const float gz = std::sin(angle) * goalRadius;
            float x = -std::cos(angle) * 3.0f;
            float z = -std::sin(angle) * 3.0f;
            Route route;
            int ticks = 0;
            for (; ticks < 160 && std::hypot(x - gx, z - gz) > 0.5f; ++ticks)
            {
                const auto [px, pz] = route.point(0, 0, clearance, x, z, gx, gz);
                const float gap = std::hypot(px - x, pz - z);
                REQUIRE(gap > 0.01f);
                const float step = std::min(0.6f, gap);
                x += (px - x) * step / gap;
                z += (pz - z) * step / gap;
            }
            INFO("heading " << heading << " goal radius " << goalRadius);
            CHECK(ticks < 160);
            CHECK(std::hypot(x - gx, z - gz) <= 0.5f);
        }
    }
}

TEST_CASE("Stake: a tank and pursuing mob recover from behind camp and settle", "[cardian][stake]")
{
    const float clearance = cardian::formation::meleeClearance(1.6f, 1.5f);
    for (const float sideways : { -3.0f, 0.0f, 3.0f })
    {
        float mx = -3.0f, mz = sideways;
        float tx = -6.0f, tz = sideways;
        Route route;
        bool towing = true;
        float finalMotion = 0.0f;
        auto step = [](float& x, float& z, const float gx, const float gz, const float stop)
        {
            const float distance = std::hypot(gx - x, gz - z);
            const float moved = std::clamp(distance - stop, 0.0f, 0.6f);
            if (moved > 0.0f)
            {
                x += (gx - x) * moved / distance;
                z += (gz - z) * moved / distance;
            }
            return moved;
        };
        for (int tick = 0; tick < 300; ++tick)
        {
            const bool wasTowing = towing;
            towing = frontlineTowing(tick == 0, towing, std::hypot(mx - kMobAhead, mz), mx);
            if (wasTowing != towing)
            {
                route.reset();
            }
            const auto goal = towing ? towPoint(mx, mz, kMobAhead, 0, 3.3f, 0) : std::pair{mx, mz - 1.6f};
            route.observe(mx, mz);
            float moved = 0.0f;
            if (std::hypot(tx - goal.first, tz - goal.second) > (towing ? 0.5f : 1.2f))
            {
                const auto point = route.point(mx, mz, clearance, tx, tz, goal.first, goal.second);
                moved = step(tx, tz, point.first, point.second, 0.3f);
            }
            else
            {
                route.reset(); // the controller forgets its detour on arrival
            }
            step(mx, mz, tx, tz, 3.6f);
            if (tick >= 280)
            {
                finalMotion += moved;
            }
        }
        INFO("initial sideways offset " << sideways);
        CHECK_FALSE(towing);
        CHECK(mx >= 0.0f);
        CHECK(std::hypot(mx - kMobAhead, mz) <= 3.0f);
        CHECK_THAT(finalMotion, WithinAbs(0.0f, 1e-4));
    }
}

TEST_CASE("Stake: a shifted rabbit releases the old long-way route", "[cardian][stake]")
{
    // Rounded 2026-09-18 playtest positions: the old direction sent Jevyak
    // east to (-620.1, 572.9) before circling back to her western seat.
    Route route;
    route.observe(-619.3f, 574.4f);
    route.direction = 1.0f;
    const auto point = route.point(-621.7f, 573.7f, 1.6f, -620.7f, 572.3f, -623.7f, 573.9f);
    CHECK(point.first < -620.7f);
    CHECK(route.direction == -1.0f);
}

TEST_CASE("Stake: small mob shifts preserve the route but accumulated drift replans", "[cardian][stake]")
{
    Route route;
    route.observe(0, 0);
    route.direction = -1.0f;
    CHECK_FALSE(route.observe(0.3f, 0));
    CHECK_FALSE(route.observe(0.6f, 0));
    CHECK_FALSE(route.observe(0.9f, 0));
    CHECK(route.direction == -1.0f);
    CHECK(route.observe(1.2f, 0));
    CHECK(route.direction == 0.0f);
}

TEST_CASE("Stake: a rabbit stepping onto its tank cannot cause a near-full lap", "[cardian][stake]")
{
    const float clearance = cardian::formation::meleeClearance(2.0f, 1.5f);
    for (const auto start : { std::pair{-0.7f, -0.7f}, std::pair{-0.001f, 0.0f},
                              std::pair{0.0f, 0.0f}, std::pair{-1.0f, 0.2f} })
    {
        for (const float oldDirection : { -1.0f, 0.0f, 1.0f })
        {
            float x = start.first, z = start.second;
            Route route;
            route.observe(0, 0);
            route.direction = oldDirection;
            float walked = 0.0f;
            for (int tick = 0; tick < 80 && std::hypot(x + 2.0f, z - 0.2f) > 0.5f; ++tick)
            {
                const auto p = route.point(0, 0, clearance, x, z, -2.0f, 0.2f);
                const float gap = std::hypot(p.first - x, p.second - z);
                REQUIRE(gap > 0.0f);
                const float step = std::min(0.6f, gap);
                x += (p.first - x) * step / gap;
                z += (p.second - z) * step / gap;
                walked += step;
            }
            INFO("start " << start.first << ", " << start.second << "; direction " << oldDirection);
            CHECK(std::hypot(x + 2.0f, z - 0.2f) <= 0.5f);
            CHECK(walked < 4.0f); // a short adjustment, not the old ~10-y lap
        }
    }
}

TEST_CASE("Stake: the shared spacing leaves a reachable seat after backing off", "[cardian][stake]")
{
    // Small and large mobs, with the tank backing away at different
    // bearings: the preferred seat must stay reachable outside the same
    // threshold that calls for a step back.
    for (const float preferred : { 0.7f, 1.0f, 1.6f, 2.0f, 3.0f })
    {
        const float clearance = cardian::formation::meleeClearance(preferred, 1.5f);
        for (int degrees = 0; degrees < 360; degrees += 30)
        {
            const float angle = degrees * std::numbers::pi_v<float> / 180.0f;
            const auto backoff = cardian::formation::backOff(0, 0, 0, 0, preferred, angle);
            CHECK(std::hypot(backoff.first, backoff.second) > clearance);
            float x = backoff.first, z = backoff.second;
            Route route;
            int ticks = 0;
            for (; ticks < 100 && std::hypot(x - preferred, z) > 0.1f; ++ticks)
            {
                const auto p = route.point(0, 0, clearance, x, z, preferred, 0);
                const float gap = std::hypot(p.first - x, p.second - z);
                REQUIRE(gap > 0.001f);
                const float step = std::min(0.3f, gap);
                x += (p.first - x) * step / gap;
                z += (p.second - z) * step / gap;
            }
            INFO("preferred radius " << preferred << "; initial bearing " << degrees);
            CHECK(ticks < 100);
            CHECK(std::hypot(x - preferred, z) <= 0.1f);
        }
    }
}
