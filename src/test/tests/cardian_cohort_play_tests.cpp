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

// The cohorts in play and the town sign-in's arithmetic (cohort_play_math.h): which cohorts a played
// job brings into play, whether a body is, and each town's target, shortfall, excess and the order
// the sign-ins go in.

#include <catch2/catch_test_macros.hpp>

#include "map/pawn/cohort_play_math.h"

#include <array>
#include <unordered_map>
#include <utility>
#include <vector>

using namespace cardian::cohorts;

namespace
{
    // Prod's two cohorts as the user found them (2026-10-18): Misha (1) plays her Warrior 24, which
    // rides Jevyak's (2) Monk cohort; her Monk rides its own
    constexpr uint8_t  kWar = 1;
    constexpr uint8_t  kMnk = 2;
    constexpr uint8_t  kThf = 6;
    constexpr uint32_t kMishaMnk  = 102;
    constexpr uint32_t kJevyakMnk = 202;

    auto prodRides() -> std::unordered_map<uint64_t, uint32_t>
    {
        return { { rideKey(1, kWar), kJevyakMnk }, { rideKey(1, kMnk), kMishaMnk }, { rideKey(2, kMnk), kJevyakMnk } };
    }

    auto active(const std::vector<std::pair<uint32_t, uint8_t>>& played) -> std::unordered_set<uint32_t>
    {
        return activeOf(prodRides(), played);
    }
} // namespace

TEST_CASE("Cohorts: in play is the cohort the job played now rides", "[cardian][cohorts]")
{
    // Misha online on her Warrior brings Jevyak's cohort into play, not her own Monk's
    CHECK(active({ { 1, kWar } }) == std::unordered_set<uint32_t>{ kJevyakMnk });
    // she changes to her Monk: her own comes in, Jevyak's goes
    CHECK(active({ { 1, kMnk } }) == std::unordered_set<uint32_t>{ kMishaMnk });
    // both of them online, each on his Monk
    CHECK(active({ { 1, kMnk }, { 2, kMnk } }) == std::unordered_set<uint32_t>{ kMishaMnk, kJevyakMnk });
    // a job that rides nothing (at or under the newbie pool's levels), or nobody online
    CHECK(active({ { 1, kThf } }).empty());
    CHECK(active({}).empty());
}

TEST_CASE("Cohorts: a body is in play while one of her cohorts is", "[cardian][cohorts]")
{
    const auto playing = active({ { 1, kWar } });
    CHECK(inPlay("settled", 0, 0, playing));
    CHECK(inPlay("newbie", 0, 0, {}));
    CHECK(inPlay("peer", kJevyakMnk, 0, playing));
    CHECK(inPlay("rival", kJevyakMnk, 0, playing));
    CHECK_FALSE(inPlay("peer", kMishaMnk, 0, playing));
    // in two cohorts: by her second as much as her first
    CHECK(inPlay("peer", kMishaMnk, kJevyakMnk, playing));
    CHECK_FALSE(inPlay("peer", kMishaMnk, kJevyakMnk, {}));
    // a cohort row with no cohort is the shared pools' rule
    CHECK(inPlay("peer", 0, 0, {}));
}

TEST_CASE("Town sign-in: a town's target is its regulars times the share", "[cardian][cohorts]")
{
    CHECK(targetOf(87, 1.0) == 87);
    CHECK(targetOf(87, 0.5) == 44);
    CHECK(targetOf(87, 0.0) == 0);
    CHECK(targetOf(0, 1.0) == 0);

    Town t{ .zone = 230, .regulars = 40, .cohort = 10, .free = 6, .room = 700 };
    CHECK(shortfallOf(t, 1.0) == 30);
    CHECK(excessOf(t, 1.0) == 0);
    // the faded cap's room bounds the shortfall
    t.room = 12;
    CHECK(shortfallOf(t, 1.0) == 12);
    // at its target, and within the slack over it, nothing moves
    t = Town{ .zone = 230, .regulars = 40, .cohort = 45, .free = 30, .room = 700 };
    CHECK(shortfallOf(t, 1.0) == 0);
    CHECK(excessOf(t, 1.0) == 0);
    // past the slack, the excess goes, of the free alone
    t.cohort = 52;
    CHECK(excessOf(t, 1.0) == 7);
    t.free = 3;
    CHECK(excessOf(t, 1.0) == 3);
    // the sign-in off: every town's target is nobody
    t = Town{ .zone = 230, .regulars = 40, .cohort = 0, .free = 0, .room = 700 };
    CHECK(shortfallOf(t, 0.0) == 0);
}

TEST_CASE("Town sign-in: each sign-in goes to the town short the most", "[cardian][cohorts]")
{
    // Southern San d'Oria short by 3, Bastok Markets by 2, Port Windurst full: the first goes to
    // San d'Oria, then the two tie and the lower zone id takes it, then Bastok, and no more
    std::vector<Town> towns{ { .zone = 230, .regulars = 10, .cohort = 7, .free = 0, .room = 700 },
                             { .zone = 235, .regulars = 10, .cohort = 8, .free = 0, .room = 700 },
                             { .zone = 240, .regulars = 10, .cohort = 10, .free = 0, .room = 700 } };
    CHECK(planSignIns(towns, 1.0, 10) == std::vector<uint16_t>{ 230, 230, 235, 230, 235 });
    CHECK(planSignIns(towns, 1.0, 2) == std::vector<uint16_t>{ 230, 230 });
    CHECK(planSignIns(towns, 1.0, 0).empty());
    // a town with no faded room left takes nobody
    towns[0].room = 0;
    CHECK(planSignIns(towns, 1.0, 10) == std::vector<uint16_t>{ 235, 235 });
}

TEST_CASE("Town sign-in: the totals, and when the map log says them again", "[cardian][cohorts]")
{
    const std::array<Town, 2> towns{ Town{ .zone = 230, .regulars = 80, .cohort = 20, .free = 10, .room = 700 },
                                     Town{ .zone = 235, .regulars = 60, .cohort = 70, .free = 40, .room = 700 } };
    const auto totals = totalsOf(towns, 1.0);
    CHECK(totals.towns == 2);
    CHECK(totals.regulars == 140);
    CHECK(totals.cohort == 90);
    CHECK(totals.target == 140);
    CHECK(totals.shortfall == 60);

    auto moved = totals;
    CHECK_FALSE(worthSaying(totals, 50, moved, 50 + kSayEvery - 1));
    CHECK(worthSaying(totals, 50, moved, 50 + kSayEvery));
    moved.target += kSayEvery;
    CHECK(worthSaying(totals, 50, moved, 50));
    // the towns full, or short again
    moved           = totals;
    moved.shortfall = 0;
    CHECK(worthSaying(totals, 50, moved, 50));
}
