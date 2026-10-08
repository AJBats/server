// Cardian: the auto rescue's judgement (pawn/rescue_watch.h): a cardian
// trying to reach her spot is caught only when she has tried for the whole
// wait without leaving her circle -- never while she walks, however the
// player moves -- and a spot she cannot stay at backs the rescues off.
#include "pawn/rescue_watch.h"

#include <catch2/catch_test_macros.hpp>

using namespace cardian::rescue;

namespace
{
    constexpr double kTick = 0.4; // a pawn tick, seconds

    const Rules kRules{}; // radius 4, 10 s, forget after 120 s, at most 8 times

    // Ticks from `start` to `end` with her at `at(t)`, trying throughout;
    // the first time she is caught, or a negative number when never
    template <typename At>
    auto firstCaught(Watch& w, const double start, const double end, At at, const bool trying = true) -> double
    {
        for (double t = start; t <= end + 1e-9; t += kTick)
        {
            if (caught(w, t, at(t), trying, kRules))
            {
                return t;
            }
        }
        return -1.0;
    }
} // namespace

TEST_CASE("Auto rescue: walking is never caught", "[cardian][rescue]")
{
    Watch w;
    // at a cardian's run (5 y/s) for a minute, however far the player leads
    CHECK(firstCaught(w, 0.0, 60.0, [](double t) { return Point{ static_cast<float>(5.0 * t), 0.0f, 0.0f }; }) < 0.0);
    // round a long detour: a slow circle of radius 20, at the same pace
    Watch round;
    CHECK(firstCaught(round, 0.0, 120.0, [](double t)
    {
        const double a = t * 5.0 / 20.0;
        return Point{ static_cast<float>(20.0 * std::cos(a)), 0.0f, static_cast<float>(20.0 * std::sin(a)) };
    }) < 0.0);
}

TEST_CASE("Auto rescue: standing caught after the wait, not before", "[cardian][rescue]")
{
    Watch      w;
    const auto t = firstCaught(w, 0.0, 30.0, [](double) { return Point{ 3.0f, 0.0f, 7.0f }; });
    CHECK(t >= 10.0);
    CHECK(t < 10.0 + kTick + 1e-9);
}

TEST_CASE("Auto rescue: stepping back and forth beside a wall is caught", "[cardian][rescue]")
{
    // a courtesy step of 2.4 y every three seconds, and back (OPEN_ISSUES #379)
    Watch      w;
    const auto t = firstCaught(w, 0.0, 30.0, [](double t) { return Point{ static_cast<int>(t / 3.0) % 2 == 0 ? 0.0f : 2.4f, 0.0f, 0.0f }; });
    CHECK(t >= 10.0);
    CHECK(t < 10.5);
}

TEST_CASE("Auto rescue: a break in trying starts the clock again", "[cardian][rescue]")
{
    Watch       w;
    const Point here{ 1.0f, 2.0f, 3.0f };
    CHECK(firstCaught(w, 0.0, 9.0, [&](double) { return here; }) < 0.0);
    // at her spot for a moment, or a spell under way: her clock stops
    CHECK_FALSE(caught(w, 9.4, here, false, kRules));
    CHECK_FALSE(w.running);
    // trying again: a fresh ten seconds from here
    CHECK(firstCaught(w, 9.8, 19.0, [&](double) { return here; }) < 0.0);
    CHECK(firstCaught(w, 19.4, 21.0, [&](double) { return here; }) > 0.0);
}

TEST_CASE("Auto rescue: rescues she needs again wait longer, to eight times", "[cardian][rescue]")
{
    Watch       w;
    const Point here{};
    double      now = 0.0;
    for (const double wait : { 10.0, 20.0, 40.0, 80.0, 80.0 })
    {
        const auto t = firstCaught(w, now, now + 200.0, [&](double) { return here; });
        REQUIRE(t > 0.0);
        CHECK(t - now >= wait);
        CHECK(t - now < wait + kTick + 1e-9);
        rescued(w, t);
        now = t + kTick;
    }
}

TEST_CASE("Auto rescue: walking out of her circle makes the next rescue a first one", "[cardian][rescue]")
{
    Watch w;
    REQUIRE(firstCaught(w, 0.0, 20.0, [](double) { return Point{}; }) > 0.0);
    rescued(w, 10.0);
    CHECK(w.again == 1);
    // she walks five yalms on her own, then is caught again
    CHECK_FALSE(caught(w, 11.0, Point{}, true, kRules));
    CHECK_FALSE(caught(w, 12.0, Point{ 5.0f, 0.0f, 0.0f }, true, kRules));
    CHECK(w.again == 0);
    const auto t = firstCaught(w, 12.4, 40.0, [](double) { return Point{ 5.0f, 0.0f, 0.0f }; });
    CHECK(t - 12.0 >= 10.0);
    CHECK(t - 12.0 < 10.0 + kTick + 1e-9);
}

TEST_CASE("Auto rescue: a rescue long past no longer doubles the wait", "[cardian][rescue]")
{
    Watch w;
    rescued(w, 0.0);
    rescued(w, 1.0);
    CHECK(w.again == 2);
    // caught again, three minutes later: the first wait, not four times it
    const auto t = firstCaught(w, 180.0, 260.0, [](double) { return Point{}; });
    CHECK(t - 180.0 >= 10.0);
    CHECK(t - 180.0 < 10.0 + kTick + 1e-9);
}

TEST_CASE("Auto rescue: a spot where she stands starts her clock again", "[cardian][rescue]")
{
    Watch w;
    REQUIRE(firstCaught(w, 0.0, 20.0, [](double) { return Point{}; }) > 0.0);
    restart(w, 10.0, Point{});
    CHECK(w.running);
    CHECK(w.again == 0);
    CHECK_FALSE(caught(w, 15.0, Point{}, true, kRules));
    CHECK(caught(w, 20.0, Point{}, true, kRules));
}
