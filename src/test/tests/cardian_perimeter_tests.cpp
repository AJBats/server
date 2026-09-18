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

// The perimeter's arithmetic (RESEARCH §12.15): a mob's TP reach from its
// skill list as two circles, the ring they make, and the nearest safe spot
// of the crescent outside that ring and inside cure range of the tank.
// Pure, so a change here fails before a map server runs.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "map/pawn/perimeter_math.h"

#include <cmath>
#include <numbers>
#include <vector>

using namespace cardian::perimeter;
using Catch::Matchers::WithinAbs;

TEST_CASE("Perimeter: the sapling's powder is a circle round the mob, none round its target", "[cardian][perimeter]")
{
    // Slumber Powder: aoe 1, radius 10; Whirl Claws-style single target at 6
    const std::vector<Move> moves{ { .aoe = 1, .distance = 0.0f, .radius = 10.0f, .name = "slumber_powder" }, { .aoe = 0, .distance = 6.0f, .radius = 0.0f, .name = "whirl_claws" } };
    const auto              r = reachOf(moves, 3.4f, 2.0f);
    CHECK_THAT(r.mob, WithinAbs(12.0f, 1e-4));
    CHECK_THAT(r.target, WithinAbs(0.0f, 1e-4));
    CHECK(r.mobBy == "slumber_powder");
    CHECK(r.targetBy.empty());
}

TEST_CASE("Perimeter: a target-centred round is a circle round the target alone", "[cardian][perimeter]")
{
    const std::vector<Move> moves{ { .aoe = 2, .distance = 0.0f, .radius = 8.0f } };
    const auto              r = reachOf(moves, 3.4f, 2.0f);
    CHECK_THAT(r.mob, WithinAbs(5.4f, 1e-4));
    CHECK_THAT(r.target, WithinAbs(10.0f, 1e-4));
}

TEST_CASE("Perimeter: cones and single-target moves reach by their distance, the largest wins", "[cardian][perimeter]")
{
    const std::vector<Move> moves{ { .aoe = 4, .distance = 9.0f }, { .aoe = 8, .distance = 7.0f }, { .aoe = 0, .distance = 6.0f }, { .aoe = 1, .radius = 5.0f } };
    const auto              r = reachOf(moves, 3.4f, 1.0f);
    CHECK_THAT(r.mob, WithinAbs(10.0f, 1e-4));
    CHECK_THAT(r.target, WithinAbs(0.0f, 1e-4));
}

TEST_CASE("Perimeter: the melee reach is the floor, and no circle takes no margin", "[cardian][perimeter]")
{
    const std::vector<Move> none;
    const auto              floor = reachOf(none, 4.0f, 2.0f);
    CHECK_THAT(floor.mob, WithinAbs(6.0f, 1e-4));
    CHECK_THAT(floor.target, WithinAbs(0.0f, 1e-4));
    CHECK(floor.mobBy.empty());

    const auto nothing = reachOf(none, 0.0f, 2.0f);
    CHECK_THAT(nothing.mob, WithinAbs(0.0f, 1e-4));
    CHECK_THAT(nothing.target, WithinAbs(0.0f, 1e-4));
}

TEST_CASE("Perimeter: a move that cannot land on an enemy adds nothing", "[cardian][perimeter]")
{
    const std::vector<Move> moves{ { .aoe = 1, .radius = 30.0f, .hostile = false }, { .aoe = 2, .radius = 30.0f, .hostile = false } };
    const auto              r = reachOf(moves, 3.0f, 1.0f);
    CHECK_THAT(r.mob, WithinAbs(4.0f, 1e-4));
    CHECK_THAT(r.target, WithinAbs(0.0f, 1e-4));
}

TEST_CASE("Perimeter: a point at range sits on the ray from the mob; on the mob, the fallback bearing", "[cardian][perimeter]")
{
    // The seat at (3, 4), five from the mob at the origin: the ray's unit is (0.6, 0.8)
    const auto [x, z] = atRange(0.0f, 0.0f, 3.0f, 4.0f, 19.0f, 0.0f);
    CHECK_THAT(x, WithinAbs(11.4f, 1e-3));
    CHECK_THAT(z, WithinAbs(15.2f, 1e-3));

    const auto [fx, fz] = atRange(10.0f, 10.0f, 10.0f, 10.0f, 5.0f, std::numbers::pi_v<float> / 2.0f);
    CHECK_THAT(fx, WithinAbs(10.0f, 1e-3));
    CHECK_THAT(fz, WithinAbs(15.0f, 1e-3));
}

TEST_CASE("Perimeter: the ring is the mob's circle, or the tank's carried out past the tank", "[cardian][perimeter]")
{
    CHECK_THAT(ringOf(Reach{ .mob = 17.0f, .target = 0.0f }, 3.0f), WithinAbs(17.0f, 1e-4));
    CHECK_THAT(ringOf(Reach{ .mob = 12.0f, .target = 10.0f }, 3.0f), WithinAbs(13.0f, 1e-4));
    CHECK_THAT(ringOf(Reach{ .mob = 12.0f, .target = 8.0f }, 3.0f), WithinAbs(12.0f, 1e-4));
    // No target circle: the ring is the mob's own, however far the tank stands
    CHECK_THAT(ringOf(Reach{ .mob = 9.0f, .target = 0.0f }, 15.0f), WithinAbs(9.0f, 1e-4));
}

TEST_CASE("Perimeter: the nearest safe spot is a step out, round the ring only as far as range asks, or in toward the tank", "[cardian][perimeter]")
{
    // The mob at the origin, the tank three east of it, a 12 y ring, the cure's 20 y
    // Inside the ring, south of the mob: straight out to the ring, still in range
    const auto south = safeSpot(0.0f, 0.0f, 3.0f, 0.0f, 12.0f, 20.0f, 0.0f, -5.0f);
    REQUIRE(south.has_value());
    CHECK_THAT(south->first, WithinAbs(0.0f, 1e-3));
    CHECK_THAT(south->second, WithinAbs(-12.0f, 1e-3));

    // Inside a 19 y ring on the far side: straight out would be 22 from the
    // tank, so round the ring to where range is met, on her side of the mob
    const auto far = safeSpot(0.0f, 0.0f, 3.0f, 0.0f, 19.0f, 20.0f, -5.0f, 0.5f);
    REQUIRE(far.has_value());
    CHECK_THAT(std::hypot(far->first, far->second), WithinAbs(19.0f, 1e-3));
    CHECK_THAT(std::hypot(far->first - 3.0f, far->second), WithinAbs(20.0f, 1e-2));
    CHECK(far->second > 0.0f);

    // Outside the ring but out of range: in toward the tank, to the range
    const auto out = safeSpot(0.0f, 0.0f, 3.0f, 0.0f, 12.0f, 20.0f, 30.0f, 0.0f);
    REQUIRE(out.has_value());
    CHECK_THAT(out->first, WithinAbs(23.0f, 1e-3));
    CHECK_THAT(out->second, WithinAbs(0.0f, 1e-3));

    // Already in the crescent: where she stands
    const auto fine = safeSpot(0.0f, 0.0f, 3.0f, 0.0f, 12.0f, 20.0f, 0.0f, -14.0f);
    REQUIRE(fine.has_value());
    CHECK_THAT(fine->second, WithinAbs(-14.0f, 1e-3));

    // A tank far beyond the ring: its range disc misses the ring, so the
    // nearest safe spot is in toward the tank, at range from it
    const auto chase = safeSpot(0.0f, 0.0f, 50.0f, 0.0f, 12.0f, 20.0f, 0.0f, -5.0f);
    REQUIRE(chase.has_value());
    CHECK_THAT(std::hypot(chase->first - 50.0f, chase->second), WithinAbs(20.0f, 1e-2));
    CHECK(std::hypot(chase->first, chase->second) > 12.0f);

    // A genuinely empty crescent still lets the controller prioritize cures.
    CHECK_FALSE(safeSpot(0.0f, 0.0f, 3.4f, 0.0f, 24.0f, 20.0f, 0.0f, -5.0f).has_value());
}

TEST_CASE("Perimeter: no safe spot once the ring point behind the tank is out of cast range", "[cardian][perimeter]")
{
    // Leafstorm's 17 behind a tank 3 from the mob is 14 from the tank: fine at 20
    CHECK_FALSE(noSafeSpot(17.0f, 3.0f, 20.0f));
    CHECK(noSafeSpot(24.0f, 3.0f, 20.0f));
    CHECK(noSafeSpot(30.0f, 3.0f, 20.0f));
}

TEST_CASE("Perimeter: thin crescents never take the external-circle chase branch", "[cardian][perimeter]")
{
    // The reported Bomb geometry: the old half-width inset collapsed the
    // crescent and rounded c above one, returning a point 19.2 from the mob.
    const float inset = crescentInset(3.2f + 20.0f - 22.0f);
    const auto spot = safeSpot(0, 0, 3.2f, 0, 22 + inset, 20 - inset, 0, -22);
    REQUIRE(spot.has_value());
    CHECK(std::hypot(spot->first, spot->second) >= 22 + inset - 1e-4f);
    CHECK(std::hypot(spot->first - 3.2f, spot->second) <= 20 - inset + 1e-4f);

    // Pin the old tangent inputs too: a rounding error must never turn an
    // internal tangent into a point deep inside the forbidden circle.
    const float oldInset = (3.2f + 20.0f - 22.0f) / 2;
    const auto tangent = safeSpot(0, 0, 3.2f, 0, 22 + oldInset, 20 - oldInset, 0, -22);
    if (tangent)
    {
        CHECK(std::hypot(tangent->first, tangent->second) >= 22 - 1e-4f);
        CHECK(std::hypot(tangent->first - 3.2f, tangent->second) <= 20 + 1e-4f);
    }

    // Different seeds exercise both arc edges and the chase.
    for (int di = 10; di <= 80; di += 2)
    {
        for (int wi = 11; wi <= 80; wi += 3)
        {
            const float d = di / 10.0f;
            const float width = wi / 10.0f;
            const float ring = d + 20 - width;
            const float pad = crescentInset(width);
            for (const auto seed : { std::pair{ 0.f, -22.f }, std::pair{ -22.f, 0.f }, std::pair{ 30.f, 20.f } })
            {
                const auto p = safeSpot(0, 0, d, 0, ring + pad, 20 - pad, seed.first, seed.second);
                REQUIRE(p.has_value());
                CHECK(std::hypot(p->first, p->second) >= ring + pad - 1e-3f);
                CHECK(std::hypot(p->first - d, p->second) <= 20 - pad + 1e-3f);
            }
        }
    }
}
