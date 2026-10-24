// Cardian: a lift's platform read off what the game tells the client
// (pawn/lift_math.h): which end it rests at or heads for, whether it moves,
// its height between the two floors, and when a body may still board.
#include "map/pawn/lift_math.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

using namespace cardian::lift;
using Catch::Matchers::WithinAbs;

namespace
{
    // Metalworks' timed lifts: an 18.25 s leg, 8 s of it travel
    constexpr uint32_t kPeriod = 18250;
    constexpr uint32_t kTravel = 8;

    auto timed(const bool animUp, const int64_t legStartMs) -> Platform
    {
        Platform p;
        p.animUp     = animUp;
        p.travelS    = kTravel;
        p.periodMs   = kPeriod;
        p.timestampS = static_cast<uint32_t>(legStartMs / 1000);
        return p;
    }
} // namespace

TEST_CASE("Lift: the end it rests at or heads for follows the animation, the other way for a reversed shaft", "[cardian][lift]")
{
    Platform p;
    p.animUp = true;
    CHECK(endOf(p) == 1);
    p.animUp = false;
    CHECK(endOf(p) == 0);
    p.reversed = true;
    CHECK(endOf(p) == 1);
    p.animUp = true;
    CHECK(endOf(p) == 0);
}

TEST_CASE("Lift: before any leg since the server came up it rests where the data put it", "[cardian][lift]")
{
    Platform p;
    p.animUp   = true;
    p.travelS  = kTravel;
    p.periodMs = kPeriod;
    const auto s = stateAt(p, 123456789);
    CHECK_FALSE(s.moving);
    CHECK(s.end == 1);
}

TEST_CASE("Lift: a timed leg's start is the whole period the told seconds round down from", "[cardian][lift]")
{
    const int64_t start = int64_t{ kPeriod } * 100001; // a whole period, 250 ms past a whole second
    REQUIRE(start % 1000 != 0);
    const auto p = timed(true, start);
    CHECK(static_cast<int64_t>(p.timestampS) * 1000 < start); // the game tells it in whole seconds, rounded down
    CHECK(legStartMs(p) == start);
    // a lever's lift: the told seconds as they are
    Platform lever = p;
    lever.periodMs = 0;
    CHECK(legStartMs(lever) == static_cast<int64_t>(p.timestampS) * 1000);
}

TEST_CASE("Lift: it moves for its travel time from the leg's start, then rests at the end it headed for", "[cardian][lift]")
{
    const int64_t start = int64_t{ kPeriod } * 100000;
    const auto    p     = timed(false, start);
    auto          s     = stateAt(p, start + 4000);
    CHECK(s.moving);
    CHECK(s.end == 0);
    CHECK_THAT(s.progress, WithinAbs(0.5, 1e-4));
    s = stateAt(p, start + 8000);
    CHECK_FALSE(s.moving);
    CHECK(s.end == 0);
}

TEST_CASE("Lift: its height runs from the floor it left to the floor it heads for", "[cardian][lift]")
{
    const int64_t start = int64_t{ kPeriod } * 100000;
    // heading for end 1, the upper door's, at 2.0; end 0's floor at -10.0
    const auto p = timed(true, start);
    CHECK_THAT(heightAt(stateAt(p, start), -10.0f, 2.0f), WithinAbs(-10.0, 1e-4));
    CHECK_THAT(heightAt(stateAt(p, start + 2000), -10.0f, 2.0f), WithinAbs(-7.0, 1e-4));
    CHECK_THAT(heightAt(stateAt(p, start + 9000), -10.0f, 2.0f), WithinAbs(2.0, 1e-4));
    // heading the other way
    const auto down = timed(false, start);
    CHECK_THAT(heightAt(stateAt(down, start + 6000), -10.0f, 2.0f), WithinAbs(-7.0, 1e-4));
}

TEST_CASE("Lift: a timed lift leaves on the next whole period; a lever's never on its own", "[cardian][lift]")
{
    const int64_t start = int64_t{ kPeriod } * 100000;
    const auto    p     = timed(true, start);
    CHECK(nextDepartureMs(p, start + 9000) == start + kPeriod);
    Platform lever = p;
    lever.periodMs = 0;
    CHECK(nextDepartureMs(lever, start) == INT64_MAX);
}

TEST_CASE("Lift: a body boards only while it rests at her end with the time to step aboard", "[cardian][lift]")
{
    const int64_t start = int64_t{ kPeriod } * 100000;
    const auto    p     = timed(true, start); // rests at end 1 from start + 8 s until start + 18.25 s
    CHECK(boardable(p, 1, start + 9000, 2500, 0));
    CHECK_FALSE(boardable(p, 0, start + 9000, 2500, 0));   // not at her end
    CHECK_FALSE(boardable(p, 1, start + 4000, 2500, 0));   // under way
    CHECK_FALSE(boardable(p, 1, start + 16500, 2500, 0));  // leaves too soon
    CHECK_FALSE(boardable(p, 1, start + 14000, 2500, 2000)); // its door shuts first
}

TEST_CASE("Lift: on the platform is within its radius of the centre", "[cardian][lift]")
{
    CHECK(onPlatform(-56.0f, -12.0f, 2.2f, -54.0f, -12.4f));
    CHECK_FALSE(onPlatform(-56.0f, -12.0f, 2.2f, -51.5f, -12.0f));
}
