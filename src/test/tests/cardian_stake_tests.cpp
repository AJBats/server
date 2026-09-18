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
            const float gx = std::cos(angle) * goalRadius;
            const float gz = std::sin(angle) * goalRadius;
            float x = -std::cos(angle) * 3.0f;
            float z = -std::sin(angle) * 3.0f;
            float direction = 0.0f;
            int ticks = 0;
            for (; ticks < 160 && std::hypot(x - gx, z - gz) > 0.5f; ++ticks)
            {
                const auto [px, pz] = routePoint(0, 0, 1.6f, x, z, gx, gz, direction);
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
    for (const float sideways : { -3.0f, 0.0f, 3.0f })
    {
        float mx = -3.0f, mz = sideways;
        float tx = -6.0f, tz = sideways;
        float direction = 0.0f;
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
                direction = 0.0f;
            }
            const auto goal = towing ? towPoint(mx, mz, kMobAhead, 0, 3.3f, 0) : std::pair{mx, mz - 1.6f};
            float moved = 0.0f;
            if (std::hypot(tx - goal.first, tz - goal.second) > (towing ? 0.5f : 1.2f))
            {
                const auto point = routePoint(mx, mz, 1.6f, tx, tz, goal.first, goal.second, direction);
                moved = step(tx, tz, point.first, point.second, 0.3f);
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
