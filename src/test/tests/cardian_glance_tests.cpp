// Cardian: an idle cardian's eyes and emotes, and a mage's heading at her
// safety spot (pawn/glance_math.h): she looks ahead, glancing at the player
// only now and then for a few seconds; her emotes are rare, held by any fight
// in her party, and aimed at nobody, the player or another member; and at her
// spot she faces the battle, give or take the arc.
#include "pawn/glance_math.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <algorithm>
#include <cmath>
#include <vector>

using namespace cardian::glance;
using Catch::Matchers::WithinAbs;

namespace
{
    // Dice from a list, round and round
    struct Dice
    {
        std::vector<double> rolls;
        std::size_t         next = 0;

        auto operator()() -> double
        {
            return rolls[next++ % rolls.size()];
        }
    };

    const GlanceTiming kTiming{ { 20.0, 60.0 }, { 2.0, 4.0 } };
} // namespace

TEST_CASE("Glances: a die draws evenly between a span's ends, either way round", "[cardian][glance]")
{
    CHECK_THAT(draw({ 20.0, 60.0 }, 0.0), WithinAbs(20.0, 1e-9));
    CHECK_THAT(draw({ 20.0, 60.0 }, 0.5), WithinAbs(40.0, 1e-9));
    CHECK_THAT(draw({ 60.0, 20.0 }, 0.25), WithinAbs(30.0, 1e-9));
    CHECK_THAT(draw({ -5.0, 10.0 }, 0.0), WithinAbs(0.0, 1e-9)); // a negative end counts as zero
}

TEST_CASE("Glances: the first tick only starts the clock; she looks ahead until the glance is due", "[cardian][glance]")
{
    Glances clock;
    Dice    dice{ { 0.5, 0.5, 0.0 } };
    CHECK_FALSE(step(clock, 100.0, true, kTiming, dice)); // the clock starts: due at 140
    CHECK_THAT(clock.nextAt, WithinAbs(140.0, 1e-9));
    CHECK_FALSE(step(clock, 139.9, true, kTiming, dice));
    CHECK(step(clock, 140.0, true, kTiming, dice)); // due: a glance of 3 s
    CHECK_THAT(clock.until, WithinAbs(143.0, 1e-9));
    CHECK_THAT(clock.nextAt, WithinAbs(163.0, 1e-9)); // the next one a gap after this one ends
    CHECK(step(clock, 142.9, true, kTiming, dice));
    CHECK_FALSE(step(clock, 143.0, true, kTiming, dice)); // over: ahead again
    CHECK_FALSE(step(clock, 162.0, true, kTiming, dice));
}

TEST_CASE("Glances: she glances only a small share of the time", "[cardian][glance]")
{
    // The longest glance after the shortest gap, every time: 4 s on the
    // player in every 24, one tick in six at most, over an hour of 0.4 s ticks
    Glances clock;
    Dice    dice{ { 0.0, 1.0 } }; // gaps draw 0 (20 s), lengths draw 1 (4 s)
    int     looking = 0;
    int     ticks   = 0;
    for (int tick = 0; tick < 9000; ++tick, ++ticks)
    {
        looking += step(clock, tick * 0.4, true, kTiming, dice) ? 1 : 0;
    }
    CHECK(looking > 0);
    CHECK(static_cast<double>(looking) / ticks <= 1.0 / 6.0 + 0.01);
}

TEST_CASE("Glances: with the player out of sight a glance under way ends and a due one is let go", "[cardian][glance]")
{
    Glances clock;
    Dice    dice{ { 0.0 } }; // every gap 20 s, every glance 2 s
    CHECK_FALSE(step(clock, 0.0, true, kTiming, dice));
    CHECK(step(clock, 20.0, true, kTiming, dice));
    CHECK_FALSE(step(clock, 21.0, false, kTiming, dice)); // he walked out of range mid-glance
    CHECK_FALSE(step(clock, 21.4, true, kTiming, dice));  // back in range, the glance is not resumed
    CHECK_FALSE(step(clock, 42.0, false, kTiming, dice)); // due while away: let go, the next drawn
    CHECK_THAT(clock.nextAt, WithinAbs(62.0, 1e-9));
    CHECK_FALSE(step(clock, 42.4, true, kTiming, dice));
    CHECK(step(clock, 62.0, true, kTiming, dice));
}

TEST_CASE("Glances: a restarted clock -- after a fight -- never glances on its first tick", "[cardian][glance]")
{
    Glances clock{ 10.0, -1.0 }; // came due long ago, during the fight
    clock = Glances{};          // the fight restarts it
    Dice dice{ { 0.0 } };
    CHECK_FALSE(step(clock, 500.0, true, kTiming, dice));
    CHECK_THAT(clock.nextAt, WithinAbs(520.0, 1e-9));
}

TEST_CASE("Glances: a gap of zero turns them off", "[cardian][glance]")
{
    Glances      clock;
    GlanceTiming off{ { 0.0, 0.0 }, { 2.0, 4.0 } };
    Dice         dice{ { 0.0 } };
    for (double now = 0.0; now < 100.0; now += 0.4)
    {
        CHECK_FALSE(step(clock, now, true, off, dice));
    }
}

TEST_CASE("Emotes: the first call starts the clock, and the next is counted from the last emote", "[cardian][glance]")
{
    Emotes     clock;
    const Span gap{ 180.0, 420.0 };
    Dice       dice{ { 0.5 } }; // every gap 300 s
    CHECK_FALSE(due(clock, 1000.0, gap, dice)); // arriving: no emote
    CHECK_FALSE(due(clock, 1299.0, gap, dice));
    CHECK(due(clock, 1300.0, gap, dice));
    CHECK(due(clock, 1310.0, gap, dice)); // waiting out a cast: still due
    rearm(clock, 1310.0, gap, dice);      // it goes
    CHECK_THAT(clock.nextAt, WithinAbs(1610.0, 1e-9));
    CHECK_FALSE(due(clock, 1310.4, gap, dice)); // one emote, not one a tick
}

TEST_CASE("Emotes: one let go on a walk is gone, with no backlog when the party stops", "[cardian][glance]")
{
    Emotes     clock;
    const Span gap{ 180.0, 420.0 };
    Dice       dice{ { 0.0 } }; // every gap 180 s
    CHECK_FALSE(due(clock, 0.0, gap, dice));
    // Following the player for ten minutes: each emote that comes due is let go
    int letGo = 0;
    for (double now = 0.4; now < 600.0; now += 0.4)
    {
        if (due(clock, now, gap, dice))
        {
            rearm(clock, now, gap, dice);
            ++letGo;
        }
    }
    CHECK(letGo == 3);
    // The party stops: nothing is due at once
    CHECK_FALSE(due(clock, 600.0, gap, dice));
}

TEST_CASE("Emotes: after a fight one already due waits a short stagger, and one due later keeps its time", "[cardian][glance]")
{
    const Span stagger{ 5.0, 30.0 };
    Dice       dice{ { 0.5 } }; // every stagger 17.5 s

    Emotes overdue{ 900.0 }; // came due during the fight
    settle(overdue, 1000.0, stagger, dice);
    CHECK_THAT(overdue.nextAt, WithinAbs(1017.5, 1e-9));

    Emotes later{ 1200.0 }; // not due for minutes yet: the gap is not started over
    settle(later, 1000.0, stagger, dice);
    CHECK_THAT(later.nextAt, WithinAbs(1200.0, 1e-9));

    Emotes unstarted;
    settle(unstarted, 1000.0, stagger, dice);
    CHECK(unstarted.nextAt < 0.0); // a clock not yet started is left to start on its first call
}

TEST_CASE("Emotes: two members back from one fight emote apart", "[cardian][glance]")
{
    const Span stagger{ 5.0, 30.0 };
    Emotes     a{ 900.0 };
    Emotes     b{ 950.0 };
    Dice       first{ { 0.1 } };
    Dice       second{ { 0.8 } };
    settle(a, 1000.0, stagger, first);
    settle(b, 1000.0, stagger, second);
    CHECK(std::abs(a.nextAt - b.nextAt) > 10.0);
}

TEST_CASE("Emotes: a gap of zero turns them off", "[cardian][glance]")
{
    Emotes clock;
    Dice   dice{ { 0.0 } };
    for (double now = 0.0; now < 1000.0; now += 0.4)
    {
        CHECK_FALSE(due(clock, now, { 0.0, 0.0 }, dice));
    }
}

TEST_CASE("Emotes: any fight in her party holds them", "[cardian][glance]")
{
    CHECK_FALSE(inFight({}));
    CHECK(inFight({ .own = true }));
    CHECK(inFight({ .memberEngaged = true }));
    CHECK(inFight({ .mobOnParty = true }));
}

TEST_CASE("Emotes: aimed at the player, at another member or at nobody by the percentages", "[cardian][glance]")
{
    // 20 percent at the player, 30 at a member, the rest a fidget of her own
    CHECK(aim(0.00, 20, 30, true, true) == Aim::Player);
    CHECK(aim(0.19, 20, 30, true, true) == Aim::Player);
    CHECK(aim(0.20, 20, 30, true, true) == Aim::Member);
    CHECK(aim(0.49, 20, 30, true, true) == Aim::Member);
    CHECK(aim(0.50, 20, 30, true, true) == Aim::Nobody);
    CHECK(aim(0.99, 20, 30, true, true) == Aim::Nobody);
}

TEST_CASE("Emotes: an aim with nobody to take it is a fidget of her own", "[cardian][glance]")
{
    CHECK(aim(0.10, 20, 30, false, true) == Aim::Nobody); // the player away
    CHECK(aim(0.30, 20, 30, true, false) == Aim::Nobody); // no member near her
    CHECK(aim(0.30, 0, 0, true, true) == Aim::Nobody);    // both shares zero
    CHECK(aim(0.99, 100, 0, true, true) == Aim::Player);  // all at the player
}

TEST_CASE("Facing: the short turn between two headings", "[cardian][glance]")
{
    CHECK(turnBetween(0, 32) == 32);
    CHECK(turnBetween(32, 0) == -32);
    CHECK(turnBetween(250, 10) == 16); // across north
    CHECK(turnBetween(10, 250) == -16);
    CHECK(turnBetween(0, 128) == -128);
}

TEST_CASE("Facing: at her spot she faces the battle within 45 degrees either side", "[cardian][glance]")
{
    // 45 degrees is 32 of the game's 256 to the turn
    CHECK(battleHeading(100, 45.0, 0.5) == 100); // the middle of the arc: straight at the mob
    CHECK(battleHeading(100, 45.0, 0.0) == 68);  // the arc's left end
    CHECK(turnBetween(100, battleHeading(100, 45.0, 0.999)) == 32);
    for (int toward = 0; toward < 256; toward += 7)
    {
        for (double roll = 0.0; roll < 1.0; roll += 0.01)
        {
            const int off = turnBetween(static_cast<uint8_t>(toward), battleHeading(static_cast<uint8_t>(toward), 45.0, roll));
            CHECK(off >= -32);
            CHECK(off <= 32);
        }
    }
}

TEST_CASE("Facing: the arc wraps across north and spreads the picks", "[cardian][glance]")
{
    CHECK(battleHeading(250, 45.0, 1.0) == 26); // 250 + 32, past north
    CHECK(battleHeading(5, 45.0, 0.0) == 229);  // 5 - 32, past north the other way
    CHECK(battleHeading(5, 45.0, 0.25) != battleHeading(5, 45.0, 0.75));
    CHECK(battleHeading(77, 0.0, 0.9) == 77);   // no arc: straight at the mob
    CHECK(battleHeading(0, 400.0, 1.0) == 128); // the arc is held to a half turn
}
