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

// The party's claim board (claim_board.h, RESEARCH §12.15, OPEN_ISSUES
// #250): a spot within two yalms of the claim of a member before her in the
// party order slides to the nearest clear point that keeps the mover's
// purpose; only the later member gives way; with nothing clear nearby the
// spot stands. Pure, so the rule is pinned here and the controller only
// reads the board and vets the candidates against the world.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "map/pawn/claim_board.h"
#include "map/pawn/perimeter_math.h"

#include <cmath>
#include <numbers>
#include <vector>

using namespace cardian::claims;
using Catch::Matchers::WithinAbs;

// Arriving a little short of a slid spot still leaves her clear of the
// claim it gave way to, and a mage walking to the first one's side counts
// as beside her wherever her walk ends
static_assert(kClearance - kArrive > kSpacing);
static_assert(kBeside >= kClearance + kStep + kArrive);
static_assert(kTogether > kBeside);

namespace
{
    constexpr auto deg(const float d) -> float
    {
        return d * std::numbers::pi_v<float> / 180.0f;
    }

    auto onCircle(const Point centre, const float radius, const float bearing) -> Point
    {
        return { centre.x + radius * std::cos(bearing), centre.z + radius * std::sin(bearing) };
    }

    auto bearingOf(const Point centre, const Point p) -> float
    {
        return std::atan2(p.z - centre.z, p.x - centre.x);
    }
} // namespace

TEST_CASE("claims: a spot within two yalms of a claim is crowded, one farther is not", "[cardian][claims]")
{
    const std::vector<Point> claims{ { 10.0f, 0.0f }, { 20.0f, 0.0f } };
    CHECK(crowdedBy({ 11.5f, 0.0f }, claims) == std::optional<std::size_t>{ 0 });
    CHECK(crowdedBy({ 19.0f, 0.5f }, claims) == std::optional<std::size_t>{ 1 });
    CHECK_FALSE(crowdedBy({ 12.2f, 0.0f }, claims).has_value());
    CHECK_FALSE(crowdedBy({ 15.0f, 0.0f }, std::span<const Point>{}).has_value());
}

TEST_CASE("claims: only the later member gives way, so two never swap", "[cardian][claims]")
{
    // Both aim for the same point: the first in the party order keeps it,
    // the second slides off it
    const Point              spot{ 10.0f, 0.0f };
    const std::vector<Point> board{ spot, spot };
    const Purpose            purpose{ .centre = { 0.0f, 0.0f } };

    const auto first = giveWay(board, 0, spot, { 12.0f, 3.0f }, purpose);
    CHECK_FALSE(first.by.has_value());
    CHECK(first.to.empty());

    const auto second = giveWay(board, 1, spot, { 12.0f, 3.0f }, purpose);
    REQUIRE(second.by == std::optional<std::size_t>{ 0 });
    REQUIRE_FALSE(second.to.empty());
    CHECK(distance(second.to.front(), spot) >= kClearance - 1e-3f);

    // A member after her on the board is never one she gives way to
    CHECK(before(board, 1).size() == 1);
    CHECK(before(board, 0).empty());
    CHECK(before(board, 7).size() == 2);
}

TEST_CASE("claims: the slide keeps the distance from its centre and takes the nearest clear point", "[cardian][claims]")
{
    const Point              mob{ 0.0f, 0.0f };
    const Point              spot = onCircle(mob, 14.0f, deg(30.0f));
    const std::vector<Point> claims{ onCircle(mob, 14.0f, deg(31.0f)) };
    const auto               to = slides(spot, onCircle(mob, 16.0f, deg(10.0f)), claims, Purpose{ .centre = mob });
    REQUIRE_FALSE(to.empty());
    for (const auto& p : to)
    {
        CHECK_THAT(distance(p, mob), WithinAbs(14.0f, 1e-3));
        CHECK(distance(p, claims[0]) >= kClearance - 1e-3f);
    }
    // Nearest first, and the first is just clear: no farther round than a
    // step past the clearance
    for (std::size_t k = 1; k < to.size(); ++k)
    {
        CHECK(distance(to[k - 1], spot) <= distance(to[k], spot) + 1e-3f);
    }
    CHECK(distance(to.front(), claims[0]) < kClearance + kStep + 1e-3f);
}

TEST_CASE("claims: of two points equally far round, the side she stands on comes first", "[cardian][claims]")
{
    // The spot is exactly on the claim: either side is as near, and she
    // takes hers rather than walking through the member before her
    const Point              mob{ 0.0f, 0.0f };
    const Point              spot = onCircle(mob, 12.0f, deg(90.0f));
    const std::vector<Point> claims{ spot };

    const auto fromEast = slides(spot, onCircle(mob, 12.0f, deg(60.0f)), claims, Purpose{ .centre = mob });
    REQUIRE_FALSE(fromEast.empty());
    CHECK(bearingOf(mob, fromEast.front()) < deg(90.0f));

    const auto fromWest = slides(spot, onCircle(mob, 12.0f, deg(120.0f)), claims, Purpose{ .centre = mob });
    REQUIRE_FALSE(fromWest.empty());
    CHECK(bearingOf(mob, fromWest.front()) > deg(90.0f));
}

TEST_CASE("claims: the slide stays within reach of the point it keeps to", "[cardian][claims]")
{
    // The crescent: round the mob at 10 y, within 8.5 y of the tank, which
    // is 2 y out on the east. The arc in reach runs to about 37 degrees
    // either side of east; the member before her stands at 20 degrees, and
    // every point she may slide to stays on that arc
    const Point              mob{ 0.0f, 0.0f };
    const Point              tank{ 2.0f, 0.0f };
    const Point              spot = onCircle(mob, 10.0f, deg(20.0f));
    const std::vector<Point> claims{ onCircle(mob, 10.0f, deg(20.0f)) };
    const Purpose            purpose{ .centre = mob, .keepTo = tank, .within = 8.5f };
    const auto               to = slides(spot, onCircle(mob, 12.0f, deg(40.0f)), claims, purpose);
    REQUIRE_FALSE(to.empty());
    for (const auto& p : to)
    {
        CHECK(distance(p, tank) <= 8.5f + 1e-3f);
        CHECK_THAT(distance(p, mob), WithinAbs(10.0f, 1e-3));
    }
    // A spot already farther than the reach may stay as far, no farther
    const Point farSpot = onCircle(mob, 10.0f, deg(45.0f)); // about 8.7 y from the tank
    const auto  wide    = slides(farSpot, farSpot, std::vector<Point>{ farSpot }, purpose);
    REQUIRE_FALSE(wide.empty());
    for (const auto& p : wide)
    {
        CHECK(distance(p, tank) <= distance(farSpot, tank) + 1e-3f);
    }
}

TEST_CASE("claims: with nothing clear nearby the spot stands", "[cardian][claims]")
{
    // Claims every two yalms along the whole stretch she may slide over
    const Point        mob{ 0.0f, 0.0f };
    const float        radius = 12.0f;
    const Point        spot   = onCircle(mob, radius, 0.0f);
    std::vector<Point> claims;
    for (int k = -6; k <= 6; ++k)
    {
        claims.push_back(onCircle(mob, radius, static_cast<float>(k) * 2.0f / radius));
    }
    const std::vector<Point> board = [&]
    {
        auto b = claims;
        b.push_back(spot); // she comes last
        return b;
    }();
    const auto v = giveWay(board, claims.size(), spot, spot, Purpose{ .centre = mob });
    CHECK(v.by.has_value());
    CHECK(v.to.empty());

    // A spot on its centre has no circle to slide round
    CHECK(slides(mob, { 3.0f, 0.0f }, std::vector<Point>{ mob }, Purpose{ .centre = mob }).empty());
}

TEST_CASE("claims: the two mages of 2026-10-04 stand a body's width apart", "[cardian][claims]")
{
    // Zapp chose (892.2, -266.7) and Gabriol (891.9, -266.3) on one Sand
    // Hare. Zapp is first in the party: Gabriol slides round the mob at her
    // own distance to the nearest point clear of Zapp, in cure range
    const Point   hare{ 880.0f, -260.0f };
    const Point   tank{ 882.0f, -258.0f };
    const Point   zapp{ 892.2f, -266.7f };
    const Point   gabriol{ 891.9f, -266.3f };
    const Purpose purpose{ .centre = hare, .keepTo = tank, .within = 20.0f - 1.25f };

    const std::vector<Point> board{ zapp, gabriol };
    const auto               v = giveWay(board, 1, gabriol, gabriol, purpose);
    REQUIRE(v.by == std::optional<std::size_t>{ 0 });
    REQUIRE_FALSE(v.to.empty());
    const Point to = v.to.front();
    CHECK(distance(to, zapp) >= kClearance - 1e-3f);
    CHECK(distance(to, zapp) < kClearance + kStep + 1e-3f); // together, not apart
    CHECK_THAT(distance(to, hare), WithinAbs(distance(gabriol, hare), 1e-3));
    CHECK(distance(to, tank) <= 18.75f + 1e-3f);
}

TEST_CASE("claims: a second mage seeded at the first's claim stands beside her, round the ring", "[cardian][claims]")
{
    // The crescent as AttendIntent asks for it: ring 12 y round the mob plus
    // the inset, within cure range of the tank less the inset. The first mage
    // stands north-east; the second starts on the far side. Seeded at the
    // first's claim, her crescent spot is the first's own, and the board
    // slides it a body's width round the mob
    const Point mob{ 0.0f, 0.0f };
    const Point tank{ 3.0f, 0.0f };
    const float ring  = 12.0f;
    const float range = 20.0f;
    const float inset = 2.5f;
    const Point first = onCircle(mob, ring + inset, deg(50.0f));
    const Point other = onCircle(mob, ring + inset, deg(-130.0f));

    const auto seeded = cardian::perimeter::safeSpot(mob.x, mob.z, tank.x, tank.z, ring + inset, range - inset, first.x, first.z);
    REQUIRE(seeded.has_value());
    const Point spot{ seeded->first, seeded->second };
    CHECK(distance(spot, first) < kSpacing); // on her: crowded

    const Purpose            purpose{ .centre = mob, .keepTo = tank, .within = range - inset / 2.0f };
    const std::vector<Point> board{ first, other };
    const auto               v = giveWay(board, 1, spot, other, purpose);
    REQUIRE_FALSE(v.to.empty());
    const Point beside = v.to.front();
    CHECK(distance(beside, first) >= kClearance - 1e-3f);
    CHECK(distance(beside, first) < kClearance + kStep + 1e-3f);
    CHECK(distance(beside, mob) >= ring);
    CHECK(distance(beside, tank) <= range);
    CHECK(together(beside, std::vector<Point>{ first }));
    CHECK_FALSE(together(other, std::vector<Point>{ first }));

    // Her way there goes round the ring a leg at a time, never through it
    const Point leg = roundTheRing(mob, other, beside, ring, deg(30.0f));
    CHECK(distance(leg, mob) >= ring);
    CHECK_THAT(std::abs(std::remainder(bearingOf(mob, leg) - bearingOf(mob, other), 2.0f * std::numbers::pi_v<float>)), WithinAbs(deg(30.0f), 1e-3));
}

TEST_CASE("claims: the way round the ring is straight when it can be", "[cardian][claims]")
{
    const Point mob{ 0.0f, 0.0f };
    const float ring = 10.0f;

    // Along the outside, a short way round: straight there
    const Point from = onCircle(mob, 14.0f, deg(0.0f));
    const Point aside = onCircle(mob, 14.0f, deg(40.0f));
    const Point got  = roundTheRing(mob, from, aside, ring, deg(30.0f));
    CHECK_THAT(got.x, WithinAbs(aside.x, 1e-4));
    CHECK_THAT(got.z, WithinAbs(aside.z, 1e-4));

    // To the far side: the shorter way round, by the turn asked, at the
    // spot's own distance
    const Point beyond = onCircle(mob, 13.0f, deg(150.0f));
    const Point leg    = roundTheRing(mob, from, beyond, ring, deg(30.0f));
    CHECK_THAT(distance(leg, mob), WithinAbs(13.0f, 1e-3));
    CHECK_THAT(bearingOf(mob, leg), WithinAbs(deg(30.0f), 1e-3));
    const Point other = onCircle(mob, 13.0f, deg(-150.0f));
    CHECK_THAT(bearingOf(mob, roundTheRing(mob, from, other, ring, deg(30.0f))), WithinAbs(deg(-30.0f), 1e-3));

    // The shorter way passes behind the mob, away from the tank: with that
    // bearing out of cure range, she goes round the tank's side instead
    const Point west  = onCircle(mob, 13.0f, deg(120.0f));
    const Point south = onCircle(mob, 13.0f, deg(-120.0f));
    CHECK_THAT(bearingOf(mob, roundTheRing(mob, west, south, ring, deg(30.0f))), WithinAbs(deg(150.0f), 1e-3));
    CHECK_THAT(bearingOf(mob, roundTheRing(mob, west, south, ring, deg(30.0f), deg(180.0f))), WithinAbs(deg(90.0f), 1e-3));
    CHECK_THAT(bearingOf(mob, roundTheRing(mob, west, south, ring, deg(30.0f), deg(0.0f))), WithinAbs(deg(150.0f), 1e-3));

    // The last leg, within the turn: the spot itself
    const Point last = onCircle(mob, 13.0f, deg(25.0f));
    const Point end  = roundTheRing(mob, onCircle(mob, 13.0f, deg(-0.5f)), last, 12.9f, deg(30.0f));
    CHECK_THAT(end.x, WithinAbs(last.x, 1e-4));
}
