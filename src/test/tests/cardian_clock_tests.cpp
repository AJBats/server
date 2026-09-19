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
//   timer::add_simulation_offset  moves the simulation clock only (a pause's resume)
//   timer::add_offset             the test harness's "time went by": both domains

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <thread>
#include <type_traits>
#include <vector>

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

namespace
{

// Both offsets are process-global and the Lua suite's sim:skipTime() shares them,
// so a test restores exactly what it found rather than zeroing them.
struct OffsetGuard
{
    timer::duration      saved     = timer::get_offset();
    realtime::clock::rep savedReal = realtime::test_offset.load();

    ~OffsetGuard()
    {
        timer::time_offset.store(saved.count());
        realtime::test_offset.store(savedReal);
    }
};

} // namespace

TEST_CASE("clock domains: a simulation offset does not move real time", "[cardian][clock]")
{
    const OffsetGuard guard;

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
    const OffsetGuard guard;

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
    const OffsetGuard guard;

    // Gameplay deadlines are absolute points on the simulation clock, so an offset
    // that is later given back leaves every one of them where it was.
    const auto castFinishesAt  = timer::now() + 60s;
    const auto remainingBefore = castFinishesAt - timer::now();

    timer::add_simulation_offset(10min);
    timer::add_simulation_offset(-10min);

    const auto remainingAfter = castFinishesAt - timer::now();

    // Integer nanosecond arithmetic: the offset itself comes back exactly. The clock
    // reads around it are only as close as the machine was quick.
    REQUIRE(timer::get_offset() == guard.saved);
    REQUIRE(std::chrono::abs(remainingBefore - remainingAfter) < 1s);
}

TEST_CASE("clock domains: the harness's time going by moves both domains", "[cardian][clock]")
{
    const OffsetGuard guard;

    const auto realBefore = realtime::now();
    const auto simBefore  = timer::now();

    // sim:skipTime and every simulated tick come through here. Real-time work such as
    // packet flood control has to see those skips, or a test that fast-forwards past
    // a limit finds the limit still standing.
    timer::add_offset(1h);

    REQUIRE(std::chrono::abs((realtime::now() - realBefore) - 1h) < 1s);
    REQUIRE(std::chrono::abs((timer::now() - simBefore) - 1h) < 1s);
}

TEST_CASE("clock domains: a hold gives a deadline its time back", "[cardian][clock]")
{
    const OffsetGuard guard;

    // The pause's arithmetic, done by hand: ten minutes go by -- far longer than the
    // cast had left -- and the simulation clock is then given back exactly what was
    // measured on the real clock. The cast must still have its minute.
    const auto castFinishesAt = timer::now() + 60s;

    const auto holdStart = realtime::now();
    timer::add_offset(10min);
    const auto heldFor = realtime::now() - holdStart;
    timer::add_simulation_offset(-heldFor);

    REQUIRE(std::chrono::abs(heldFor - 10min) < 1s);
    REQUIRE(std::chrono::abs((castFinishesAt - timer::now()) - 60s) < 1s);

    // The same ten minutes with nothing given back: the cast is long over.
    timer::add_offset(10min);
    REQUIRE(timer::now() >= castFinishesAt);
}

TEST_CASE("clock domains: to_utc of now ignores the offset on one thread", "[cardian][clock]")
{
    const OffsetGuard guard;

    // map/time_server.cpp schedules its JST work from to_utc(timer::now()) and relies
    // on this: to_utc subtracts a now() of its own, so with no writer in between the
    // offset cancels and that work keeps real wall time.
    //
    // The cancellation is NOT safe against a concurrent writer: to_utc reads the
    // offset a second time, and an offset written between the caller's read and
    // to_utc's shifts the result by the whole write. Nothing writes concurrently
    // today; the pause will, so closing that window is a prerequisite of the pause
    // and this test only covers the single-threaded half.
    const auto wallBefore = timer::to_utc(timer::now());

    timer::add_simulation_offset(6h);

    const auto wallAfter = timer::to_utc(timer::now());
    REQUIRE(std::chrono::abs(wallAfter - wallBefore) < 1s);
}

TEST_CASE("clock domains: concurrent offset writes are not lost and reads stay whole", "[cardian][clock]")
{
    const OffsetGuard guard;

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
    const timer::duration::rep baseTicks   = guard.saved.count();

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
    REQUIRE(timer::get_offset() == guard.saved);
}
