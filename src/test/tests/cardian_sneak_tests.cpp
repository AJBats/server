// Cardian: the sneak-by (pawn/sneak.h, ROADMAP T slice 3): a mob's reach
// while it rests -- its sight a cone, the rest all round -- and once its rest
// is over, spreading at its walk within its territory; ground no mob can
// ever reach is an island; and a crossing fits only when her walk, played
// forward to the second, clears every mob until she is past them. With the
// danger map's radius split into the cone and the reach all round.
#include "pawn/pawn_danger.h"
#include "pawn/sneak.h"

#include <catch2/catch_test_macros.hpp>

using cardian::sneak::Mob;

namespace
{
    constexpr float kSlack = 2.0f;

    auto at(const float x, const float z) -> position_t
    {
        return position_t(x, 0.0f, z, 0, 0);
    }

    // A goblin at the origin facing +x (rotation 0: the game's world angle
    // toward +x), its sight 15 yalms and its reach all round 4, walking 2.4
    // yalms a second within 10 of the origin
    auto goblin(const float restLeft) -> Mob
    {
        Mob m;
        m.at       = position_t(0.0f, 0.0f, 0.0f, 0, 0);
        m.cone     = 15.0f;
        m.allRound = 4.0f;
        m.speed    = 2.4f;
        m.restLeft = restLeft;
        m.home     = m.at;
        m.roam     = 10.0f;
        return m;
    }
} // namespace

TEST_CASE("Sneak-by: the danger map's radius split into a cone and a reach all round", "[cardian][sneak]")
{
    using namespace pawn::danger;
    const Profile p = Profile::worstCase();
    Detection     sight;
    sight.sight      = true;
    sight.sightRange = 15.0f;
    Detection hearing;
    hearing.hearing    = true;
    hearing.soundRange = 8.0f;
    Detection both     = sight;
    both.hearing       = true;
    both.soundRange    = 8.0f;

    for (const auto& d : { sight, hearing, both })
    {
        CHECK(std::max(coneFor(d, p, 1.5f), allRoundFor(d, p, 1.5f, 3.0f)) == radiusFor(d, p, 1.5f, 3.0f));
    }
    CHECK(coneFor(sight, p, 1.5f) == 16.5f);
    CHECK(allRoundFor(sight, p, 1.5f, 3.0f) == 0.0f);
    CHECK(coneFor(hearing, p, 1.5f) == 0.0f);
    CHECK(allRoundFor(hearing, p, 1.5f, 3.0f) == 9.5f);

    // Invisible, sight reaches nowhere
    Profile invisible = p;
    invisible.invisible = true;
    CHECK(coneFor(sight, invisible, 1.5f) == 0.0f);
}

TEST_CASE("Sneak-by: a resting mob reaches its cone and its reach all round, nothing more", "[cardian][sneak]")
{
    const Mob m = goblin(30.0f);
    CHECK(m.threatens(at(10.0f, 0.0f), 0.0f, kSlack));   // in front, in sight
    CHECK_FALSE(m.threatens(at(-10.0f, 0.0f), 0.0f, kSlack)); // behind
    CHECK_FALSE(m.threatens(at(0.0f, 10.0f), 0.0f, kSlack));  // beside: 90 degrees off
    CHECK(m.threatens(at(-3.0f, 0.0f), 0.0f, kSlack));    // behind, inside its reach all round
    CHECK_FALSE(m.threatens(at(20.0f, 0.0f), 0.0f, kSlack)); // in front, past its sight
}

TEST_CASE("Sneak-by: its rest over, a mob faces her and its reach spreads within its territory", "[cardian][sneak]")
{
    const Mob m = goblin(5.0f); // its rest taken to end 2 s early: 3 s from now
    CHECK_FALSE(m.threatens(at(-10.0f, 0.0f), 2.9f, kSlack));
    CHECK(m.threatens(at(-10.0f, 0.0f), 3.1f, kSlack)); // turned toward her

    // its reach spreads at its walk: 18 yalms off, not yet at 3.5 s, by 5 s
    CHECK_FALSE(m.threatens(at(-18.0f, 0.0f), 3.5f, kSlack));
    CHECK(m.threatens(at(-18.0f, 0.0f), 5.0f, kSlack));

    // never past its territory: 30 yalms off, 20 past its roam circle
    CHECK_FALSE(m.threatens(at(-30.0f, 0.0f), 600.0f, kSlack));
    CHECK(m.neverReaches(at(-30.0f, 0.0f)));
    CHECK_FALSE(m.neverReaches(at(-20.0f, 0.0f)));

    // one the model cannot bound reaches anywhere in time
    Mob loose      = m;
    loose.unbounded = true;
    CHECK(loose.threatens(at(-60.0f, 0.0f), 60.0f, kSlack));
    CHECK_FALSE(loose.neverReaches(at(-300.0f, 0.0f)));
}

TEST_CASE("Sneak-by: a mob away from its territory reaches where it stands", "[cardian][sneak]")
{
    Mob m  = goblin(0.0f);
    m.home = at(100.0f, 0.0f); // its roam circle a long way off
    CHECK(m.threatens(at(-5.0f, 0.0f), 0.0f, kSlack));
    CHECK_FALSE(m.neverReaches(at(-5.0f, 0.0f)));
}

TEST_CASE("Sneak-by: a crossing behind a resting mob fits; in front of it, or too slow, it does not", "[cardian][sneak]")
{
    // From the edge of its whole reach, across behind it and out of that
    // reach again: past it, though still on its territory
    const std::vector<position_t> behind{ at(-10.0f, -12.0f), at(-10.0f, 30.0f) };

    const auto fits = cardian::sneak::plan({ goblin(30.0f) }, behind, 5.0f, 0.5f, kSlack, kSlack, false);
    REQUIRE(fits.fits);
    CHECK(fits.points.front().z == -12.0f);
    CHECK(distance(fits.points.back(), at(0.0f, 0.0f)) >= 15.0f - 0.01f); // out of its whole reach
    CHECK(fits.points.back().z < 14.0f);                                    // ... the first ground so, not the route's end
    CHECK_FALSE(fits.island);                                                 // it could roam there once it walks again
    CHECK(fits.seconds < 6.0f);

    // its rest ends 3 s in, before she is past: it turns and sees her
    const auto late = cardian::sneak::plan({ goblin(5.0f) }, behind, 5.0f, 0.5f, kSlack, kSlack, false);
    CHECK_FALSE(late.fits);
    CHECK(late.caughtBy == 0);
    CHECK(late.caughtAt > 3.0f);

    // in front of it, its cone takes her
    const std::vector<position_t> front{ at(10.0f, -17.0f), at(10.0f, 30.0f) };
    const auto                    seen = cardian::sneak::plan({ goblin(30.0f) }, front, 5.0f, 0.5f, kSlack, kSlack, false);
    CHECK_FALSE(seen.fits);
    CHECK(seen.caughtBy == 0);

    // behind it, but inside its reach all round
    Mob hears      = goblin(30.0f);
    hears.allRound = 12.0f;
    CHECK_FALSE(cardian::sneak::plan({ hears }, behind, 5.0f, 0.5f, kSlack, kSlack, false).fits);
}

TEST_CASE("Sneak-by: past one mob of a string is past, when the ground between is clear", "[cardian][sneak]")
{
    // two resting goblins 40 yalms apart, both facing +x, and a walk behind
    // both: the hop ends in the gap between them, past the first
    Mob second = goblin(30.0f);
    second.at  = position_t(0.0f, 0.0f, 40.0f, 0, 0);
    second.home = second.at;
    const std::vector<position_t> both{ at(-10.0f, -12.0f), at(-10.0f, 60.0f) };
    const auto                    hop = cardian::sneak::plan({ goblin(30.0f), second }, both, 5.0f, 0.5f, kSlack, kSlack, false);
    REQUIRE(hop.fits);
    CHECK(hop.points.back().z > 11.0f);
    CHECK(hop.points.back().z < 29.0f);

    // the first's rest about to end as she leaves its reach: none may reach
    // her where the hop ends for a moment after, so she goes on further
    const auto settling = cardian::sneak::plan({ goblin(7.6f), second }, both, 5.0f, 0.5f, kSlack, kSlack, false);
    REQUIRE(settling.fits);
    CHECK(settling.points.back().z > hop.points.back().z);
}

TEST_CASE("Sneak-by: never past the mobs on the way, only a safe end fits", "[cardian][sneak]")
{
    // a mob the model cannot bound, resting a long while, and a walk behind
    // it to a zone line inside its whole reach
    Mob m       = goblin(60.0f);
    m.unbounded = true;
    const std::vector<position_t> toLine{ at(-10.0f, -10.0f), at(-10.0f, 10.0f) };
    CHECK_FALSE(cardian::sneak::plan({ m }, toLine, 5.0f, 0.5f, kSlack, kSlack, false).fits);
    const auto line = cardian::sneak::plan({ m }, toLine, 5.0f, 0.5f, kSlack, kSlack, true);
    REQUIRE(line.fits);
    CHECK(line.points.back().z == 10.0f);
}
