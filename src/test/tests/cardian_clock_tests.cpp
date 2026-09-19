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

// The two clock domains (common/timer.h).
//
// timer:: carries an offset so gameplay time can be moved as a body; realtime::
// is the same steady clock without it, for work that must keep measuring real
// elapsed time. These tests pin the properties the pause feature stands on, using
// the two offset entries alone -- no pause, no server, no client:
//   timer::add_simulation_offset  moves the simulation clock only
//   timer::add_offset             the test harness's "time went by": both domains
//   timer::hold / timer::release  stop the simulation clock and restart it

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <thread>
#include <type_traits>
#include <vector>

#include "cardian_clock_guard.h"
#include "common/timer.h"
#include "map/entities/char_entity.h"

using namespace std::chrono_literals;

// The compile-time half of the split. The instants of the two domains must not
// convert into each other in either direction -- that is what turns a simulation
// timestamp in a liveness slot into a build error -- while durations stay shared,
// so a measured real interval can be handed to the simulation clock as an offset.
// If realtime::clock is ever "simplified" into an alias of the steady clock, these
// fail the build.
static_assert(!std::is_same_v<realtime::time_point, timer::time_point>);
static_assert(!std::is_constructible_v<realtime::time_point, timer::time_point>);
static_assert(!std::is_constructible_v<timer::time_point, realtime::time_point>);
static_assert(!std::is_assignable_v<realtime::time_point&, timer::time_point>);
static_assert(!std::is_assignable_v<timer::time_point&, realtime::time_point>);
static_assert(std::is_same_v<realtime::duration, timer::duration>);

// One consumer pinned by name. Packet flood control keeps its instants in the real
// domain; if a merge rewrites that member, or moves it back to the simulation clock
// together with its reader, this stops the build where the pair alone would not.
static_assert(std::is_same_v<decltype(CCharEntity::m_PacketRecievedTimestamps)::mapped_type, realtime::time_point>);

TEST_CASE("clock domains: a simulation offset does not move real time", "[cardian][clock]")
{
    const ClockGuard guard;

    const auto realBefore = realtime::now();
    const auto simBefore  = timer::now();

    timer::add_simulation_offset(1h);

    // Real time only advanced by however long those few statements took.
    REQUIRE(std::chrono::abs(realtime::now() - realBefore) < 1s);

    // Simulation time moved by the whole hour.
    REQUIRE(std::chrono::abs((timer::now() - simBefore) - 1h) < 1s);
}

TEST_CASE("clock domains: the watchdog cannot be fooled by a time shift", "[cardian][clock]")
{
    const ClockGuard guard;

    // What MapEngine::watchdogWatcher does: remember when the main thread last
    // checked in, then ask how long ago that was. A simulation shift in either
    // direction -- a pause, a resume, sim:skipTime -- must not answer that question.
    const auto lastCheckIn = realtime::now();

    timer::add_simulation_offset(-2h);
    REQUIRE(std::chrono::abs(realtime::now() - lastCheckIn) < 1s);

    timer::add_simulation_offset(4h);
    REQUIRE(std::chrono::abs(realtime::now() - lastCheckIn) < 1s);
}

TEST_CASE("clock domains: a deadline keeps its remaining time across an offset round trip", "[cardian][clock]")
{
    const ClockGuard guard;

    // Gameplay deadlines are absolute points on the simulation clock, so an offset
    // that is later given back leaves every one of them where it was.
    const auto castFinishesAt  = timer::now() + 60s;
    const auto remainingBefore = castFinishesAt - timer::now();

    timer::add_simulation_offset(10min);
    timer::add_simulation_offset(-10min);

    const auto remainingAfter = castFinishesAt - timer::now();

    // Integer nanosecond arithmetic: the offset itself comes back exactly. The clock
    // reads around it are only as close as the machine was quick.
    REQUIRE(timer::get_offset() == guard.savedOffset);
    REQUIRE(std::chrono::abs(remainingBefore - remainingAfter) < 1s);
}

TEST_CASE("clock domains: the harness's time going by moves both domains", "[cardian][clock]")
{
    const ClockGuard guard;

    const auto realBefore = realtime::now();
    const auto simBefore  = timer::now();

    // sim:skipTime and every simulated tick come through here. Real-time work such as
    // packet flood control has to see those skips, or a test that fast-forwards past
    // a limit finds the limit still standing.
    timer::add_offset(1h);

    REQUIRE(std::chrono::abs((realtime::now() - realBefore) - 1h) < 1s);
    REQUIRE(std::chrono::abs((timer::now() - simBefore) - 1h) < 1s);
}

TEST_CASE("clock domains: concurrent offset writes are not lost and reads stay whole", "[cardian][clock]")
{
    const ClockGuard guard;

    // Two writers race add_simulation_offset while readers ask the time. With a plain
    // read-modify-write the writers lose updates and the offset does not come back
    // to where it started; with the atomic one every +step meets its -step.
    // Readers check that whatever they observe is a whole number of steps from the
    // starting offset (on x86-64 a torn 64-bit read cannot happen in practice; the
    // check pins the contract and gives a sanitizer something to find).
    constexpr auto             step        = timer::duration{ 1s };
    constexpr int              writes      = 250000;
    constexpr int              readerCount = 4;
    constexpr int              writerCount = 2;
    constexpr uint64           minOverlap  = 1000;
    constexpr int              maxRounds   = 200000000;
    const timer::duration::rep stepTicks   = step.count();
    const timer::duration::rep baseTicks   = guard.savedOffset.count();

    std::atomic<int>    ready{ 0 };
    std::atomic<bool>   go{ false };
    std::atomic<bool>   stop{ false };
    std::atomic<bool>   partial{ false };
    std::atomic<int>    writersActive{ 0 };
    std::atomic<uint64> readsWhileWriting{ 0 };

    std::vector<std::thread> threads;

    for (int i = 0; i < readerCount; ++i)
    {
        threads.emplace_back(
            [&]
            {
                ready.fetch_add(1);
                while (!go.load())
                {
                    std::this_thread::yield();
                }

                while (!stop.load(std::memory_order_relaxed))
                {
                    if ((timer::get_offset().count() - baseTicks) % stepTicks != 0)
                    {
                        partial.store(true);
                    }
                    (void)timer::now();
                    if (writersActive.load(std::memory_order_relaxed) > 0)
                    {
                        readsWhileWriting.fetch_add(1, std::memory_order_relaxed);
                    }
                }
            });
    }

    std::vector<std::thread> writers;
    for (int i = 0; i < writerCount; ++i)
    {
        writers.emplace_back(
            [&]
            {
                ready.fetch_add(1);
                while (!go.load())
                {
                    std::this_thread::yield();
                }

                // At least `writes` rounds, and then on until the readers have seen
                // plenty of them -- bounded, so a starved reader fails the test
                // instead of hanging it. Every round is balanced, so stopping on any
                // boundary leaves the offset where it began.
                writersActive.fetch_add(1);
                for (int n = 0; n < writes || (readsWhileWriting.load(std::memory_order_relaxed) < minOverlap && n < maxRounds); ++n)
                {
                    timer::add_simulation_offset(step);
                    timer::add_simulation_offset(-step);
                }
                writersActive.fetch_sub(1);
            });
    }

    // Nobody starts until everybody is running, so the reads overlap the writes.
    while (ready.load() < readerCount + writerCount)
    {
        std::this_thread::yield();
    }
    go.store(true);

    for (auto& writer : writers)
    {
        writer.join();
    }
    stop.store(true);
    for (auto& reader : threads)
    {
        reader.join();
    }

    REQUIRE(readsWhileWriting.load() >= minOverlap);
    REQUIRE_FALSE(partial.load());
    REQUIRE(timer::get_offset() == guard.savedOffset);
}

//
// The hold: timer::hold() and timer::release().
//

TEST_CASE("clock hold: a held clock stands still while time goes by", "[cardian][clock]")
{
    const ClockGuard guard;

    const auto realBefore = realtime::now();

    timer::hold();
    REQUIRE(timer::is_held());
    const auto heldInstant = timer::now();

    timer::add_offset(10min);

    // Ten minutes went by for the real clock and not one tick for the held one.
    REQUIRE(timer::now() == heldInstant);
    REQUIRE(std::chrono::abs((realtime::now() - realBefore) - 10min) < 1s);

    timer::release();
    REQUIRE_FALSE(timer::is_held());
}

TEST_CASE("clock hold: a deadline keeps its time across holds", "[cardian][clock]")
{
    const ClockGuard guard;

    const auto castFinishesAt = timer::now() + 60s;

    // Two holds in a row, ten minutes each: far longer than the cast had left.
    for (int i = 0; i < 2; ++i)
    {
        timer::hold();
        timer::add_offset(10min);
        REQUIRE(timer::now() < castFinishesAt);
        timer::release();
    }

    REQUIRE(std::chrono::abs((castFinishesAt - timer::now()) - 60s) < 1s);

    // Released, the clock runs again: a skip past the deadline reaches it.
    timer::add_offset(61s);
    REQUIRE(timer::now() >= castFinishesAt);
}

TEST_CASE("clock hold: the clock stops and starts but never jumps", "[cardian][clock]")
{
    const ClockGuard guard;

    const auto before = timer::now();

    timer::hold();
    const auto held = timer::now();
    timer::add_offset(10min);
    const auto stillHeld = timer::now();
    timer::release();
    const auto after = timer::now();

    REQUIRE(before <= held);
    REQUIRE(held == stillHeld);
    REQUIRE(after >= held);
    REQUIRE(after - held < 1s);
}

TEST_CASE("clock hold: holding twice and releasing twice change nothing", "[cardian][clock]")
{
    const ClockGuard guard;

    timer::hold();
    const auto held = timer::now();
    timer::add_offset(5min);
    timer::hold();
    REQUIRE(timer::now() == held);

    timer::release();
    const auto offsetAfterRelease = timer::get_offset();
    timer::release();
    REQUIRE(timer::get_offset() == offsetAfterRelease);
    REQUIRE_FALSE(timer::is_held());
}

TEST_CASE("clock hold: a deadline's wall-clock time does not jump at release", "[cardian][clock]")
{
    const ClockGuard guard;

    // to_utc is how a deadline is persisted (status effects) and how time_server
    // reads the wall clock. It converts through timer::now(), so a clock that jumped
    // at release would move every converted deadline by the length of the hold. The
    // harness's fast-forward leaves the wall clock alone, so here the answer must
    // not move at all.
    const auto castFinishesAt = timer::now() + 60s;
    const auto wallBefore     = timer::to_utc(castFinishesAt);

    timer::hold();
    timer::add_offset(10min);
    REQUIRE(std::chrono::abs(timer::to_utc(castFinishesAt) - wallBefore) < 1s);

    timer::release();
    REQUIRE(std::chrono::abs(timer::to_utc(castFinishesAt) - wallBefore) < 1s);
}

TEST_CASE("clock hold: other threads never see the clock jump", "[cardian][clock]")
{
    const ClockGuard guard;

    // hold() and release() belong to the main thread, but any thread may be reading
    // while they run. A reader may see the clock stand still and may see it run; it
    // must never get a wild value. The bound is loose because hold() can be preempted
    // between its read of the running clock and its store. (What a hold does to time
    // is pinned single-threaded above: the harness's fast-forward is not for racing.)
    constexpr int cycles = 20000;

    std::atomic<bool> go{ false };
    std::atomic<bool> stop{ false };
    std::atomic<bool> jumped{ false };
    std::atomic<int>  ready{ 0 };

    std::vector<std::thread> readers;
    for (int i = 0; i < 4; ++i)
    {
        readers.emplace_back(
            [&]
            {
                ready.fetch_add(1);
                while (!go.load())
                {
                    std::this_thread::yield();
                }

                auto last = timer::now();
                while (!stop.load(std::memory_order_relaxed))
                {
                    const auto current = timer::now();
                    if (std::chrono::abs(current - last) > 1s)
                    {
                        jumped.store(true);
                    }
                    last = current;
                }
            });
    }

    while (ready.load() < 4)
    {
        std::this_thread::yield();
    }
    go.store(true);

    for (int n = 0; n < cycles; ++n)
    {
        timer::hold();
        timer::release();
    }

    stop.store(true);
    for (auto& reader : readers)
    {
        reader.join();
    }

    REQUIRE_FALSE(jumped.load());
    REQUIRE_FALSE(timer::is_held());
}
