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
