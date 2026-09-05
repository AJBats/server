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

// The rules a cardian fights by (pawn_rules.h): may she draw on this, and
// is the pull clean. Pure, and asked the same way from every door into a
// fight, so a change here is a change everywhere at once.

#include <catch2/catch_test_macros.hpp>

#include "map/pawn/pawn_rules.h"

using namespace cardian::rules;

namespace
{
    // A live, hostile, unclaimed mob at `distance`, nothing in the way
    auto fairMob(const float distance) -> EngageFacts
    {
        EngageFacts f;
        f.exists   = true;
        f.alive    = true;
        f.hostile  = true;
        f.distance = distance;
        return f;
    }
} // namespace

TEST_CASE("mayFight: a fair mob in range is allowed", "[cardian][rules]")
{
    const auto v = mayFight(fairMob(10.0f));
    REQUIRE(v.ok);
    REQUIRE(v.why.empty());
}

TEST_CASE("mayFight: the draw range sits inside the server's sight range", "[cardian][rules]")
{
    REQUIRE(kDrawRange < kServerSightRange);
    REQUIRE(mayFight(fairMob(kDrawRange)).ok);
    REQUIRE_FALSE(mayFight(fairMob(kDrawRange + 0.5f)).ok);
    REQUIRE(mayFight(fairMob(31.4f)).why == "31 y away");
}

TEST_CASE("mayFight: the reasons come in the order they matter", "[cardian][rules]")
{
    EngageFacts f;
    REQUIRE(mayFight(f).why == "no target");

    f.exists = true;
    REQUIRE(mayFight(f).why == "dead");

    f.alive      = true;
    f.retreating = true;
    f.distance   = 50.0f;
    REQUIRE(mayFight(f).why == "retreating"); // not "50 y away": the retreat is the reader's fact

    f.retreating  = false;
    f.underground = true;
    REQUIRE(mayFight(f).why == "underground");

    f.underground = false;
    REQUIRE(mayFight(f).why == "not hostile");

    f.hostile   = true;
    f.claimable = false;
    REQUIRE(mayFight(f).why == "claimed by another party");

    f.claimable = true;
    REQUIRE(mayFight(f).why == "50 y away");

    f.distance = 5.0f;
    f.cooldown = true;
    REQUIRE(mayFight(f).why == "draw cooldown");

    f.cooldown = false;
    REQUIRE(mayFight(f).ok);
}

TEST_CASE("worthWalkingIn: only distance and the draw's wait are walked off", "[cardian][rules]")
{
    REQUIRE_FALSE(worthWalkingIn(fairMob(5.0f))); // nothing to cure: she may draw now

    REQUIRE(worthWalkingIn(fairMob(45.0f)));

    auto waiting = fairMob(5.0f);
    waiting.cooldown = true;
    REQUIRE(worthWalkingIn(waiting));

    auto claimed = fairMob(45.0f);
    claimed.claimable = false;
    REQUIRE_FALSE(worthWalkingIn(claimed)); // walking in cures the distance, not the claim

    auto dead = fairMob(45.0f);
    dead.alive = false;
    REQUIRE_FALSE(worthWalkingIn(dead));
}

TEST_CASE("padded: every circle grows by the clearance", "[cardian][rules]")
{
    const Circles circles{ { 0.0f, 0.0f, 8.0f }, { 20.0f, 0.0f, 15.0f } };
    const auto    p = padded(circles);

    REQUIRE(p.size() == 2);
    REQUIRE(p[0].radius == 8.0f + kClearance);
    REQUIRE(p[1].radius == 15.0f + kClearance);
    REQUIRE(p[1].x == 20.0f);
}

TEST_CASE("pullBlocked: a clean pull is clean", "[cardian][rules]")
{
    // A guard far off to the side; the way in from (0,0) to (20,0) never
    // nears it
    const auto circles = padded(Circles{ { 10.0f, 30.0f, 8.0f } });
    REQUIRE_FALSE(pullBlocked(circles, 0.0f, 0.0f, 20.0f, 0.0f).has_value());
}

TEST_CASE("pullBlocked: prey inside a guard's padded circle names the guard", "[cardian][rules]")
{
    // The guard at (20, 9): 9 y from the prey, outside its true 8 y circle
    // and inside the padded 9.5 y one. This is the band the loop lived in.
    const auto circles = padded(Circles{ { 20.0f, 9.0f, 8.0f } });
    const auto block   = pullBlocked(circles, 0.0f, 0.0f, 20.0f, 0.0f);

    REQUIRE(block.has_value());
    REQUIRE(block->targetInside);
    REQUIRE(block->circle == 0);
}

TEST_CASE("pullBlocked: a way in that enters a circle is blocked, and says which", "[cardian][rules]")
{
    // The prey at (30, 0) is clear; a mob at (15, 4) with a 6 y circle
    // (7.5 padded) sits across the straight way in
    const auto circles = padded(Circles{ { 0.0f, 40.0f, 8.0f }, { 15.0f, 4.0f, 6.0f } });
    const auto block   = pullBlocked(circles, 0.0f, 0.0f, 30.0f, 0.0f);

    REQUIRE(block.has_value());
    REQUIRE_FALSE(block->targetInside);
    REQUIRE(block->circle == 1);
}

TEST_CASE("pullBlocked: the target's own circle is no reason", "[cardian][rules]")
{
    // An aggressive prey: its own circle surrounds it and lies across the
    // way in. Ignored, the pull is clean; counted, it is blocked.
    const auto circles = padded(Circles{ { 20.0f, 0.0f, 15.0f } });

    REQUIRE_FALSE(pullBlocked(circles, 0.0f, 0.0f, 20.0f, 0.0f, 0).has_value());
    REQUIRE(pullBlocked(circles, 0.0f, 0.0f, 20.0f, 0.0f).has_value());
}

TEST_CASE("pullBlocked: judged from where she stands now, it can turn unclean on the walk", "[cardian][rules]")
{
    // The same prey and guard: from the far side the way in is clear, from
    // the near side it cuts through the guard's circle. The fight and the
    // pick ask this the same way, so neither can hold what the other drops.
    const auto circles = padded(Circles{ { 10.0f, 6.0f, 6.0f } });

    REQUIRE_FALSE(pullBlocked(circles, 20.0f, -20.0f, 20.0f, 0.0f).has_value());
    REQUIRE(pullBlocked(circles, 0.0f, 0.0f, 20.0f, 0.0f).has_value());
}
