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

// The danger map's radius rule (M3.87): which of a mob's ways of noticing a
// cardian count against her, and the largest winning. Pure, so a change to
// the rule fails here before it fails in the field.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <cmath>
#include <vector>

#include "map/pawn/pawn_danger.h"

using namespace pawn::danger;
using Catch::Matchers::WithinAbs;

namespace
{
    constexpr float kBuffer = 1.5f;
    constexpr float kTail   = 3.0f;

    auto sightAndSound(const float sight, const float sound) -> Detection
    {
        Detection d;
        d.sight      = true;
        d.hearing    = true;
        d.sightRange = sight;
        d.soundRange = sound;
        return d;
    }
} // namespace

TEST_CASE("radiusFor: the largest detection wins and the buffer goes on top", "[cardian][avoid]")
{
    const Profile p = Profile::worstCase();

    REQUIRE_THAT(radiusFor(sightAndSound(15.0f, 8.0f), p, kBuffer, kTail), WithinAbs(16.5f, 0.001f));
    REQUIRE_THAT(radiusFor(sightAndSound(8.0f, 15.0f), p, kBuffer, kTail), WithinAbs(16.5f, 0.001f));
}

TEST_CASE("radiusFor: a mob that both aggroes and links is the larger of its two circles", "[cardian][avoid]")
{
    const Profile p = Profile::worstCase();

    Detection d = sightAndSound(15.0f, 0.0f);
    d.links     = true;
    d.linkRange = 10.0f;
    REQUIRE_THAT(radiusFor(d, p, kBuffer, kTail), WithinAbs(16.5f, 0.001f)); // sight 15 beats link 10 + tail 3

    d.linkRange = 14.0f;
    REQUIRE_THAT(radiusFor(d, p, kBuffer, kTail), WithinAbs(18.5f, 0.001f)); // link 14 + tail 3 beats sight 15
}

TEST_CASE("radiusFor: a mob that only links is a circle only while its kin are on her", "[cardian][avoid]")
{
    const Profile p = Profile::worstCase();

    Detection d;
    d.linkRange = 10.0f;
    REQUIRE_THAT(radiusFor(d, p, kBuffer, kTail), WithinAbs(0.0f, 0.001f));

    d.links = true;
    REQUIRE_THAT(radiusFor(d, p, kBuffer, kTail), WithinAbs(14.5f, 0.001f));
}

TEST_CASE("counts: Avoid aggro and Avoid links each count their own circles", "[cardian][avoid]")
{
    // Both rows: a mob that would go for her, and an idle kin of her fight
    CHECK(counts(true, true, true, false).aggressive);
    CHECK(counts(true, true, false, true).links);

    // Avoid aggro alone: detection circles, and linking kin paid no mind
    CHECK(counts(true, false, true, true).aggressive);
    CHECK_FALSE(counts(true, false, true, true).links);
    CHECK_FALSE(counts(true, false, false, true).links);

    // Avoid links alone: idle mobs walked past, her own fight's kin kept clear of
    CHECK_FALSE(counts(false, true, true, false).aggressive);
    CHECK(counts(false, true, false, true).links);
    CHECK_FALSE(counts(false, true, true, true).aggressive);

    // Neither: nothing counts
    const auto none = counts(false, false, true, true);
    CHECK_FALSE(none.aggressive);
    CHECK_FALSE(none.links);
}

TEST_CASE("counts: an aggressive mob that also links is only its link circle when she avoids links alone", "[cardian][avoid]")
{
    const Profile p = Profile::worstCase();

    // As the map builds it: detections are counted only when they count
    const auto circle = [&](const bool avoidAggro, const bool avoidLinks)
    {
        const auto c = counts(avoidAggro, avoidLinks, true, true);
        Detection  d = sightAndSound(15.0f, 0.0f);
        d.sight      = c.aggressive;
        d.hearing    = c.aggressive;
        d.links      = c.links;
        d.linkRange  = 10.0f;
        return radiusFor(d, p, kBuffer, kTail);
    };
    REQUIRE_THAT(circle(true, true), WithinAbs(16.5f, 0.001f));  // sight 15 beats link 10 + tail 3
    REQUIRE_THAT(circle(true, false), WithinAbs(16.5f, 0.001f)); // sight alone
    REQUIRE_THAT(circle(false, true), WithinAbs(14.5f, 0.001f)); // link 10 + tail 3 alone
    REQUIRE_THAT(circle(false, false), WithinAbs(0.0f, 0.001f));
}

TEST_CASE("avoidsAggro: in a real player's party his switch decides alone, her row everywhere else", "[cardian][avoid]")
{
    // avoidsAggro(retreating, trekking, inPlayersParty, partySwitch, ownRow)

    // In his party the switch lends Avoid aggro to a member whose row is
    // off or missing, and takes it from one whose row is on
    CHECK(avoidsAggro(false, false, true, true, false));
    CHECK(avoidsAggro(false, false, true, true, true));
    CHECK_FALSE(avoidsAggro(false, false, true, false, true));
    CHECK_FALSE(avoidsAggro(false, false, true, false, false));

    // In no real player's party, her own row
    CHECK(avoidsAggro(false, false, false, false, true));
    CHECK_FALSE(avoidsAggro(false, false, false, false, false));

    // A trek always avoids, the switch off and her row off
    CHECK(avoidsAggro(false, true, true, false, false));
    CHECK(avoidsAggro(false, true, false, false, false));

    // A retreat never does, whatever lends it
    CHECK_FALSE(avoidsAggro(true, false, true, true, true));
    CHECK_FALSE(avoidsAggro(true, false, false, false, true));
    CHECK_FALSE(avoidsAggro(true, true, true, true, true));
}

TEST_CASE("radiusFor: sneak and invisible hide from sound and sight unless the mob truly detects", "[cardian][avoid]")
{
    Profile p   = Profile::worstCase();
    p.sneak     = true;
    p.invisible = true;

    Detection d = sightAndSound(15.0f, 8.0f);
    REQUIRE_THAT(radiusFor(d, p, kBuffer, kTail), WithinAbs(0.0f, 0.001f));

    d.trueDetection = true;
    REQUIRE_THAT(radiusFor(d, p, kBuffer, kTail), WithinAbs(16.5f, 0.001f));
}

TEST_CASE("radiusFor: linking ignores concealment -- the kin notices the mob on her, not her", "[cardian][avoid]")
{
    Profile p   = Profile::worstCase();
    p.sneak     = true;
    p.invisible = true;

    Detection d;
    d.links     = true;
    d.linkRange = 10.0f;
    REQUIRE_THAT(radiusFor(d, p, kBuffer, kTail), WithinAbs(14.5f, 0.001f));
}

TEST_CASE("radiusFor: magic only while casting, low HP only under the mark, ambush at three yalms", "[cardian][avoid]")
{
    Profile p = Profile::worstCase(); // casting, low on HP

    Detection magic;
    magic.magic      = true;
    magic.magicRange = 20.0f;
    REQUIRE_THAT(radiusFor(magic, p, kBuffer, kTail), WithinAbs(21.5f, 0.001f));
    p.casting = false;
    REQUIRE_THAT(radiusFor(magic, p, kBuffer, kTail), WithinAbs(0.0f, 0.001f));

    Detection low;
    low.lowHP = true;
    REQUIRE_THAT(radiusFor(low, p, kBuffer, kTail), WithinAbs(CloseDetectionRange + kBuffer, 0.001f));
    p.lowHP = false;
    REQUIRE_THAT(radiusFor(low, p, kBuffer, kTail), WithinAbs(0.0f, 0.001f));

    Detection ambush;
    ambush.ambush = true;
    REQUIRE_THAT(radiusFor(ambush, p, kBuffer, kTail), WithinAbs(AmbushRange + kBuffer, 0.001f));
    p.sneak = true;
    REQUIRE_THAT(radiusFor(ambush, p, kBuffer, kTail), WithinAbs(0.0f, 0.001f));
}
