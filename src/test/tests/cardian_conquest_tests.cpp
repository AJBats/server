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

// The conquest simulation (RESEARCH §20): the tally's schedule on the game clock
// (common/cardian_conquest_clock.h) -- its boundaries at Vana'diel midnight, the
// client's countdown, the trigger xi_world fires by, a pause holding it -- and the
// crowd's sum (map/pawn/conquest_math.h): seats and dials in, influence, kills and
// deaths out, the tide, the period's campaigns (the thumb round Jeuno, each nation's
// focus, the far crowd), and the dials file with its borders checked against the zone
// lines.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "common/cardian_conquest_clock.h"
#include "common/earth_time.h"
#include "common/vana_time.h"
#include "map/data/datasets/zones/settings/dataset.h"
#include "map/data/loader.h"
#include "map/pawn/conquest_math.h"
#include "map/utils/zoneutils.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <random>
#include <set>
#include <string>
#include <utility>
#include <vector>

using namespace std::chrono_literals;
using Catch::Matchers::WithinAbs;

namespace cc = cardian::conquest_clock;
namespace cq = cardian::conquest;

namespace
{

// The Earth clock's offset and the calendar's state are process-global, and the Lua
// suite runs after these cases in the same process
struct CalendarGuard
{
    earth_time::duration      savedOffset = earth_time::time_offset;
    earth_time::duration::rep savedState  = earth_time::calendar_state.load();

    CalendarGuard()                                = default;
    CalendarGuard(const CalendarGuard&)            = delete;
    CalendarGuard& operator=(const CalendarGuard&) = delete;

    ~CalendarGuard()
    {
        earth_time::calendar_state.store(savedState);
        earth_time::time_offset = savedOffset;
    }
};

constexpr auto kThreeDays = std::chrono::seconds(3 * 3456);

// A game instant this far past a tally of the three-day schedule
auto pastATally(const earth_time::duration into) -> earth_time::time_point
{
    const auto anchor = earth_time::vanadiel_epoch + std::chrono::duration_cast<earth_time::duration>(kThreeDays) * 100000;
    return anchor + into;
}

// The client's byte: whole Vana'diel days to the tally, rounded up, as conquest::GetNextTally says it
auto countdown(const earth_time::time_point game, const uint32 days) -> int64
{
    return std::chrono::ceil<xi::vanadiel_clock::days>(cc::nextTally(game, days) - game).count();
}

auto vanaOf(const earth_time::time_point game) -> vanadiel_time::time_point
{
    return vanadiel_time::time_point{ std::chrono::duration_cast<vanadiel_time::duration>(game - earth_time::vanadiel_epoch) };
}

// A flat experience table: every level earns this much a real hour
auto flatDials(const double expPerHour) -> cq::Dials
{
    auto dials       = cq::evenDials();
    dials.expPerHour = { { 1, expPerHour }, { 75, expPerHour } };
    return dials;
}

auto nobodyHolds() -> std::array<uint8, cq::kRegions>
{
    std::array<uint8, cq::kRegions> owners{};
    owners.fill(5); // NATION_NEUTRAL
    return owners;
}

constexpr std::array<double, cq::kNations> kStillTide{ 0.0, 0.0, 0.0 };

constexpr uint8 kRonfaure   = 0;
constexpr uint8 kZulkheim   = 1;
constexpr uint8 kNorvallen  = 2;
constexpr uint8 kGustaberg  = 3;
constexpr uint8 kDerfland   = 4;
constexpr uint8 kSaruta     = 5;
constexpr uint8 kKolshushu  = 6;
constexpr uint8 kAragoneu   = 7;
constexpr uint8 kFauregandi = 8;
constexpr uint8 kQufim      = 10;
constexpr uint8 kLitelor    = 11;
constexpr uint8 kElshimoLow = 14;
constexpr uint8 kSandoria   = 0;
constexpr uint8 kBastok     = 1;
constexpr uint8 kWindurst   = 2;

// A draw at the bottom of the odds: the first nation
constexpr auto alwaysFirst = [] { return 0.0; };

// xi_world's addition of a nation's points to a region (src/world/conquest_system.cpp,
// updateInfluencePoints): three times while under half the leader, twice while under it,
// then a tenth, at least one
void worldAdds(std::array<int32, cq::kNations>& influence, const uint8 nation, int32 points)
{
    const int32 first = std::ranges::max(influence);
    if (influence[nation] < first / 2)
    {
        points *= 3;
    }
    else if (influence[nation] < first)
    {
        points *= 2;
    }
    if (points > 0)
    {
        influence[nation] += std::max(points / 10, 1);
    }
}

// Of `periods` periods of 72 Vana'diel hours of a region's crowd, sent in a random order each
// hour as the map sends them, how many end with this nation strictly first
auto homeWins(const cq::Dials& dials, const uint8 region, const uint8 nation, const uint32 seatCount, const uint32 periods, const uint32 seed) -> uint32
{
    std::mt19937                           rng(seed);
    std::uniform_real_distribution<double> uniform(0.0, 1.0);
    const std::vector<cq::Seat>            seats(seatCount, cq::Seat{ .region = region, .level = 20 });
    uint32                                 wins = 0;
    for (uint32 p = 0; p < periods; ++p)
    {
        std::array<int32, cq::kNations> influence{};
        cq::Carry                       carry{};
        for (int h = 0; h < 72; ++h)
        {
            const auto hour  = cq::sumHour(seats, dials, nobodyHolds(), kStillTide, [&] { return uniform(rng); });
            const auto sends = cq::settle(hour, carry);
            std::array<uint8, cq::kNations> order{ 0, 1, 2 };
            std::shuffle(order.begin(), order.end(), rng);
            for (const auto n : order)
            {
                if (sends.points[region][n] > 0)
                {
                    worldAdds(influence, n, sends.points[region][n]);
                }
            }
        }
        bool first = true;
        for (uint8 n = 0; n < cq::kNations; ++n)
        {
            first = first && (n == nation || influence[n] < influence[nation]);
        }
        wins += first ? 1 : 0;
    }
    return wins;
}

// Of `periods` periods of 72 Vana'diel hours of a region's crowd, with San d'Oria alone
// focusing on it at `pull`, growing over the first half of the period, how many show San
// d'Oria's up arrow -- half the three's influence or more, the client's band 2 -- at
// `checkHour` of the period
auto focusArrows(const cq::Dials& dials, const uint8 region, const double pull, const uint32 seatCount, const uint32 periods, const int64 checkHour,
                 const uint32 seed) -> uint32
{
    std::mt19937                           rng(seed);
    std::uniform_real_distribution<double> uniform(0.0, 1.0);
    const std::vector<cq::Seat>            seats(seatCount, cq::Seat{ .region = region, .level = 20 });
    cq::Campaign                           campaign{};
    campaign.focus[kSandoria][region] = pull;
    uint32 arrows                     = 0;
    for (uint32 p = 0; p < periods; ++p)
    {
        std::array<int32, cq::kNations> influence{};
        cq::Carry                       carry{};
        for (int64 h = 0; h <= checkHour; ++h)
        {
            campaign.grown   = cq::focusGrown(h, 72, 0.5);
            const auto hour  = cq::sumHour(seats, dials, nobodyHolds(), kStillTide, [&] { return uniform(rng); }, campaign);
            const auto sends = cq::settle(hour, carry);
            std::array<uint8, cq::kNations> order{ 0, 1, 2 };
            std::shuffle(order.begin(), order.end(), rng);
            for (const auto n : order)
            {
                if (sends.points[region][n] > 0)
                {
                    worldAdds(influence, n, sends.points[region][n]);
                }
            }
        }
        const int64 total = static_cast<int64>(influence[0]) + influence[1] + influence[2];
        arrows += total > 0 && 2 * static_cast<int64>(influence[kSandoria]) >= total ? 1 : 0;
    }
    return arrows;
}

auto readDialsFile() -> cq::files::DialFile
{
    std::ifstream     in("./modules/cardian/conquest.yaml", std::ios::binary);
    const std::string text{ std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>() };
    REQUIRE_FALSE(text.empty());
    std::string error;
    const auto  file = cq::parseDials(text, error);
    INFO(error);
    REQUIRE(file.has_value());
    return *file;
}

} // namespace

// -- The schedule -----------------------------------------------------------------

TEST_CASE("conquest clock: a tally every three Vana'diel days falls at Vana'diel midnight", "[cardian][conquest]")
{
    REQUIRE(cc::periodLength(3) == std::chrono::duration_cast<earth_time::duration>(2h + 52min + 48s));

    const auto game = pastATally(5h);
    const auto next = cc::nextTally(game, 3);
    CHECK(next > game);
    CHECK(next - game <= cc::periodLength(3));
    CHECK((next - earth_time::vanadiel_epoch) % cc::periodLength(3) == earth_time::duration::zero());
    CHECK(vanadiel_time::get_hour(vanaOf(next)) == 0);
    CHECK(vanadiel_time::get_minute(vanaOf(next)) == 0);

    // On a boundary the next tally is a whole period on, and the last is the one just struck
    const auto boundary = pastATally(0s);
    CHECK(cc::nextTally(boundary, 3) == boundary + cc::periodLength(3));
    CHECK(cc::lastTally(boundary, 3) == boundary);
    CHECK(cc::lastTally(game, 3) == next - cc::periodLength(3));

    // A day's period, the testing one, is 57 min 36 s
    CHECK(cc::periodLength(1) == std::chrono::duration_cast<earth_time::duration>(57min + 36s));
}

TEST_CASE("conquest clock: the client counts the days down, and 0 keeps the JST week", "[cardian][conquest]")
{
    const auto day = std::chrono::duration_cast<earth_time::duration>(cc::kVanaDay);
    CHECK(countdown(pastATally(day / 2), 3) == 3);
    CHECK(countdown(pastATally(day + day / 2), 3) == 2);
    CHECK(countdown(pastATally(2 * day + day / 2), 3) == 1);

    CHECK(cc::hoursIntoPeriod(pastATally(0s), 3) == 0);
    CHECK(cc::hoursIntoPeriod(pastATally(std::chrono::duration_cast<earth_time::duration>(cc::kVanaHour * 3 / 2)), 3) == 1);
    CHECK(cc::hoursIntoPeriod(pastATally(day + 1s), 3) == 24);

    // Retail's: the next Monday midnight JST of the game clock, as upstream counts it
    const auto game = pastATally(7h);
    CHECK(cc::nextTally(game, 0) == earth_time::get_next_game_week(game));
    CHECK(cc::periodLength(0) == std::chrono::duration_cast<earth_time::duration>(std::chrono::days(7)));
}

TEST_CASE("conquest clock: xi_world's trigger fires once per tally, never at boot, never twice", "[cardian][conquest]")
{
    const auto     day = std::chrono::duration_cast<earth_time::duration>(cc::kVanaDay);
    cc::TallyWatch watch;
    const auto     start = pastATally(2 * day + std::chrono::duration_cast<earth_time::duration>(cc::kVanaHour * 20)); // late in a period
    CHECK_FALSE(watch.due(start, 3));                         // the first look only sets it: the tallies of the time off do not happen
    CHECK_FALSE(watch.due(start + 1s, 3));
    CHECK(watch.due(pastATally(3 * day + 1s), 3)); // the boundary passed
    CHECK_FALSE(watch.due(pastATally(3 * day + 3s), 3));

    // Set back across that tally and carried forward again: the tally is done
    CHECK_FALSE(watch.due(pastATally(3 * day - 2s), 3));
    CHECK_FALSE(watch.due(pastATally(3 * day + 5s), 3));

    // Many periods at once still tally once
    CHECK(watch.due(pastATally(30 * day), 3));
    CHECK_FALSE(watch.due(pastATally(30 * day + 1s), 3));

    // The week is the Monday tick's, never this trigger's
    cc::TallyWatch weekly;
    CHECK_FALSE(weekly.due(start, 0));
    CHECK_FALSE(weekly.due(pastATally(30 * day), 0));
}

TEST_CASE("conquest clock: a pause holds the tally and the countdown", "[cardian][conquest]")
{
    const CalendarGuard guard;
    earth_time::set_calendar_drift(earth_time::duration::zero());

    const auto before      = earth_time::game_now();
    const auto tallyBefore = cc::nextTally(before, 3);
    const auto daysBefore  = countdown(before, 3);

    earth_time::hold_calendar();
    earth_time::add_offset(std::chrono::duration_cast<earth_time::duration>(10min));
    const auto held = earth_time::game_now();
    CHECK(std::chrono::abs(held - before) < 1s);
    CHECK(cc::nextTally(held, 3) == tallyBefore);
    CHECK(countdown(held, 3) == daysBefore);

    // Released, the game clock runs on behind real time by the hold, so on the real clock the
    // tally falls the hold's length later than it would have (the drift was zero before)
    earth_time::release_calendar();
    CHECK(cc::nextTally(earth_time::game_now(), 3) == tallyBefore);
    const auto realTallyNow = tallyBefore + (earth_time::now() - earth_time::game_now());
    CHECK(std::chrono::abs((realTallyNow - tallyBefore) - std::chrono::duration_cast<earth_time::duration>(10min)) < 1s);
}

// -- The sum ------------------------------------------------------------------------

TEST_CASE("conquest sum: a seat's hour is a twenty-fifth of a real hour, at the seats' flat rate, for the nation drawn", "[cardian][conquest]")
{
    const auto dials = flatDials(2500.0);
    const auto hour  = cq::sumHour({ cq::Seat{ .region = kZulkheim, .level = 20 } }, dials, nobodyHolds(), kStillTide, alwaysFirst);
    // 2,500 experience a real hour, 100 a Vana'diel hour; 12.5 % of it conquest points, half of them influence
    CHECK_THAT(hour.influence[kZulkheim][kSandoria], WithinAbs(6.25, 1e-9));
    CHECK_THAT(hour.influence[kZulkheim][kBastok], WithinAbs(0.0, 1e-9));
    CHECK_THAT(hour.kills[kZulkheim], WithinAbs(1.2, 1e-9));
    CHECK_THAT(hour.deaths[kZulkheim], WithinAbs(0.002, 1e-9));
    CHECK(hour.seats[kZulkheim] == 1);
    CHECK(hour.drawn[kZulkheim][kSandoria] == 1);

    // A draw at the top of the odds lands on the last nation
    const auto last = cq::sumHour({ cq::Seat{ .region = kZulkheim, .level = 20 } }, dials, nobodyHolds(), kStillTide, [] { return 0.999; });
    CHECK_THAT(last.influence[kZulkheim][kWindurst], WithinAbs(6.25, 1e-9));

    // Outside the conquest regions a seat earns nothing
    const auto none = cq::sumHour({ cq::Seat{ .region = 22, .level = 20 } }, dials, nobodyHolds(), kStillTide, alwaysFirst);
    CHECK(none.seats[kZulkheim] == 0);
    CHECK_THAT(none.kills[kZulkheim], WithinAbs(0.0, 1e-9));
}

TEST_CASE("conquest crowd: every seat the world has reached counts, filled or not, at its band's middle", "[cardian][conquest]")
{
    // The world's top is the census's highest target, never under 18
    CHECK(cq::worldTop(5) == 18);
    CHECK(cq::worldTop(40) == 40);

    const std::vector<cq::Slot> slots{
        cq::Slot{ .region = kRonfaure, .low = 1, .high = 5, .seats = 4 },   // reached: level 3
        cq::Slot{ .region = kRonfaure, .low = 20, .high = 30, .seats = 3 }, // above the world: nobody
        cq::Slot{ .region = kZulkheim, .low = 10, .high = 30, .seats = 2 }, // reached to 18: level 14
        cq::Slot{ .region = 20, .low = 1, .high = 75, .seats = 9 },         // a city's region: nobody
    };
    const auto seats = cq::crowd(slots, 18);
    REQUIRE(seats.size() == 6);
    CHECK(std::ranges::count_if(seats, [](const cq::Seat& s) { return s.region == kRonfaure && s.level == 3; }) == 4);
    CHECK(std::ranges::count_if(seats, [](const cq::Seat& s) { return s.region == kZulkheim && s.level == 14; }) == 2);

    // The frontier follows the world's level: at 25 the second Ronfaure slot is reached too
    CHECK(cq::crowd(slots, 25).size() == 9);
}

TEST_CASE("conquest sum: the experience table runs in straight lines between its rows", "[cardian][conquest]")
{
    auto dials       = cq::evenDials();
    dials.expPerHour = { { 10, 2000.0 }, { 20, 3000.0 } };
    CHECK_THAT(cq::expPerHour(dials, 1), WithinAbs(2000.0, 1e-9));
    CHECK_THAT(cq::expPerHour(dials, 15), WithinAbs(2500.0, 1e-9));
    CHECK_THAT(cq::expPerHour(dials, 75), WithinAbs(3000.0, 1e-9));
}

TEST_CASE("conquest shares: the dials weigh each nation -- strength, tide, home, neighbours to the cap, a hand", "[cardian][conquest]")
{
    std::string error;
    auto        dials = cq::toDials(readDialsFile(), error);
    REQUIRE(dials.has_value());
    const auto owners = nobodyHolds();

    // Home: San d'Oria weighs eight times in Ronfaure, so four in five of its crowd are San d'Orian
    CHECK_THAT(cq::weight(*dials, kSandoria, kRonfaure, owners, kStillTide), WithinAbs(8.0, 1e-9));
    CHECK_THAT(cq::weight(*dials, kBastok, kRonfaure, owners, kStillTide), WithinAbs(1.0, 1e-9));
    const auto ronfaure = cq::shares(*dials, kRonfaure, owners, kStillTide);
    CHECK_THAT(ronfaure[kSandoria], WithinAbs(0.80, 1e-9));
    CHECK_THAT(ronfaure[kBastok], WithinAbs(0.10, 1e-9));
    CHECK_THAT(ronfaure[kWindurst], WithinAbs(0.10, 1e-9));
    const auto norvallen = cq::shares(*dials, kNorvallen, owners, kStillTide);
    CHECK_THAT(norvallen[kBastok], WithinAbs(1.0 / 3.0, 1e-9));

    // Neighbours, off in the file, still weigh by their dial. Norvallen borders Ronfaure,
    // Zulkheim, Derfland and Fauregandi
    CHECK(dials->neighbourBonus == 0.0);
    dials->neighbourBonus = 0.10;
    auto held             = owners;
    held[kRonfaure]       = kSandoria;
    held[kZulkheim]       = kSandoria;
    CHECK_THAT(cq::weight(*dials, kSandoria, kNorvallen, held, kStillTide), WithinAbs(1.2, 1e-9));
    held[kDerfland] = kSandoria;
    held[kFauregandi] = kSandoria;
    CHECK_THAT(cq::weight(*dials, kSandoria, kNorvallen, held, kStillTide), WithinAbs(1.3, 1e-9)); // four held, capped at +30 %
    CHECK_THAT(cq::shares(*dials, kNorvallen, held, kStillTide)[kSandoria], WithinAbs(1.3 / 3.3, 1e-9));

    // Strength, tide and a hand on the scale, each a factor
    dials->strength[kSandoria]         = 1.5;
    dials->hand[kSandoria][kNorvallen] = 2.0;
    const std::array<double, cq::kNations> rising{ 0.1, 0.0, 0.0 };
    CHECK_THAT(cq::weight(*dials, kSandoria, kNorvallen, owners, rising), WithinAbs(1.5 * 1.1 * 2.0, 1e-9));

    // A draw picks by the shares, in nation order
    CHECK(cq::pick(ronfaure, 0.0) == kSandoria);
    CHECK(cq::pick(ronfaure, 0.79) == kSandoria);
    CHECK(cq::pick(ronfaure, 0.81) == kBastok);
    CHECK(cq::pick(ronfaure, 0.91) == kWindurst);
    CHECK(cq::pick(ronfaure, 0.9999) == kWindurst);
}

TEST_CASE("conquest heat map: the region next to a nation's home leans its way, gently", "[cardian][conquest]")
{
    std::string error;
    const auto  dials = cq::toDials(readDialsFile(), error);
    REQUIRE(dials.has_value());
    const auto owners = nobodyHolds();

    // San d'Oria and Bastok go on to Zulkheim, Windurst to Kolshushu
    CHECK_THAT(cq::weight(*dials, kSandoria, kZulkheim, owners, kStillTide), WithinAbs(1.1, 1e-9));
    CHECK_THAT(cq::weight(*dials, kBastok, kZulkheim, owners, kStillTide), WithinAbs(1.1, 1e-9));
    CHECK_THAT(cq::weight(*dials, kWindurst, kZulkheim, owners, kStillTide), WithinAbs(1.0, 1e-9));
    CHECK_THAT(cq::weight(*dials, kWindurst, kKolshushu, owners, kStillTide), WithinAbs(1.1, 1e-9));
    CHECK_THAT(cq::weight(*dials, kSandoria, kKolshushu, owners, kStillTide), WithinAbs(1.0, 1e-9));

    // Past them, with no campaign, every outer region is even
    for (const auto region : dials->outer)
    {
        const auto odds = cq::shares(*dials, region, owners, kStillTide);
        CHECK_THAT(odds[kSandoria], WithinAbs(1.0 / 3.0, 1e-9));
        CHECK_THAT(odds[kWindurst], WithinAbs(1.0 / 3.0, 1e-9));
    }
}

TEST_CASE("conquest focus: each nation picks a third of the open outer regions, apart from the others, its pull jittered round the dial", "[cardian][conquest]")
{
    // The world past every outer region's level: all fourteen open
    std::string error;
    const auto  dials = cq::toDials(readDialsFile(), error);
    REQUIRE(dials.has_value());
    REQUIRE(dials->outer.size() == 14);
    REQUIRE(cq::focusPicks(*dials, 99) == 5);
    double lowest  = 99.0;
    double highest = 0.0;

    std::mt19937                                           rng(5u);
    std::uniform_real_distribution<double>                 uniform(0.0, 1.0);
    std::array<std::array<uint32, cq::kRegions>, cq::kNations> pickedTimes{};
    uint32                                                 shared    = 0; // tallies two nations focused on one region
    uint32                                                 untouched = 0; // tallies some outer region drew no nation
    for (int tally = 0; tally < 300; ++tally)
    {
        const auto focus = cq::drawFocus(*dials, 99, [&] { return uniform(rng); });
        std::array<uint32, cq::kRegions> pickers{};
        for (uint8 n = 0; n < cq::kNations; ++n)
        {
            uint32 picks = 0;
            for (uint8 r = 0; r < cq::kRegions; ++r)
            {
                if (focus[n][r] <= 0.0)
                {
                    continue;
                }
                ++picks;
                ++pickers[r];
                ++pickedTimes[n][r];
                CHECK(std::ranges::find(dials->outer, r) != dials->outer.end());
                CHECK(focus[n][r] >= 8.0 * 0.75 - 1e-9);
                CHECK(focus[n][r] <= 8.0 * 1.25 + 1e-9);
                lowest  = std::min(lowest, focus[n][r]);
                highest = std::max(highest, focus[n][r]);
            }
            CHECK(picks == 5);
        }
        shared += std::ranges::any_of(pickers, [](const uint32 count) { return count >= 2; }) ? 1 : 0;
        untouched += std::ranges::any_of(dials->outer, [&](const uint8 region) { return pickers[region] == 0; }) ? 1 : 0;
    }
    // Every nation reaches every outer region in time; the nations fight over some regions
    // and leave others to nobody
    for (uint8 n = 0; n < cq::kNations; ++n)
    {
        for (const auto region : dials->outer)
        {
            CHECK(pickedTimes[n][region] > 0);
        }
    }
    CHECK(shared > 0);
    CHECK(untouched > 0);
    // The jitter reaches across its range
    CHECK(lowest < 6.5);
    CHECK(highest > 9.5);
}

TEST_CASE("conquest focus: the nations pick only open regions, and one the world opens takes its place without reshuffling the rest", "[cardian][conquest]")
{
    std::string error;
    const auto  dials = cq::toDials(readDialsFile(), error);
    REQUIRE(dials.has_value());
    const std::array<uint8, 4> jeuno{ kNorvallen, kDerfland, kAragoneu, kQufim };
    for (uint32 seed = 0; seed < 100; ++seed)
    {
        // The world at 27: the four round Jeuno open, one pick each. At 30 Elshimo Lowlands
        // opens too: two picks each, the first still among them, at the same pull
        std::mt19937                           first(seed);
        std::mt19937                           second(seed);
        std::uniform_real_distribution<double> uniform(0.0, 1.0);
        const auto                             at27 = cq::drawFocus(*dials, 27, [&] { return uniform(first); });
        const auto                             at30 = cq::drawFocus(*dials, 30, [&] { return uniform(second); });
        for (uint8 n = 0; n < cq::kNations; ++n)
        {
            uint32 picks27 = 0;
            uint32 picks30 = 0;
            for (uint8 r = 0; r < cq::kRegions; ++r)
            {
                if (at27[n][r] > 0.0)
                {
                    ++picks27;
                    CHECK(std::ranges::find(jeuno, r) != jeuno.end());
                    CHECK(at30[n][r] == at27[n][r]);
                }
                if (at30[n][r] > 0.0)
                {
                    ++picks30;
                    CHECK((std::ranges::find(jeuno, r) != jeuno.end() || r == kElshimoLow));
                }
            }
            CHECK(picks27 == 1);
            CHECK(picks30 == 2);
        }
    }

    // A fresh server, the world at its floor of 18: nothing past the next regions to pick
    CHECK(cq::focusPicks(*dials, 18) == 0);
    std::mt19937                           rng(3u);
    std::uniform_real_distribution<double> uniform(0.0, 1.0);
    const auto                             fresh = cq::drawFocus(*dials, 18, [&] { return uniform(rng); });
    for (const auto& row : fresh)
    {
        CHECK(std::ranges::all_of(row, [](const double pull) { return pull == 0.0; }));
    }

    // The same draws give the same picks: a restart within a period draws them again
    std::mt19937 again(9u);
    std::mt19937 twice(9u);
    CHECK(cq::drawFocus(*dials, 99, [&] { return uniform(again); }) == cq::drawFocus(*dials, 99, [&] { return uniform(twice); }));
}

TEST_CASE("conquest focus: the pull grows from nothing at the tally to its full by the ramp, then holds", "[cardian][conquest]")
{
    CHECK(cq::focusGrown(0, 72, 0.5) == 0.0);
    CHECK_THAT(cq::focusGrown(18, 72, 0.5), WithinAbs(0.5, 1e-9));
    CHECK(cq::focusGrown(36, 72, 0.5) == 1.0);
    CHECK(cq::focusGrown(71, 72, 0.5) == 1.0);
    CHECK(cq::focusGrown(0, 72, 0.0) == 1.0); // no ramp: full at once

    auto dials = cq::evenDials();
    cq::Campaign campaign{};
    campaign.focus[kSandoria][kLitelor] = 8.0;
    campaign.grown                      = 0.0;
    CHECK_THAT(cq::weight(dials, kSandoria, kLitelor, nobodyHolds(), kStillTide, campaign), WithinAbs(1.0, 1e-9));
    campaign.grown = 0.5;
    CHECK_THAT(cq::weight(dials, kSandoria, kLitelor, nobodyHolds(), kStillTide, campaign), WithinAbs(4.5, 1e-9));
    campaign.grown = 1.0;
    CHECK_THAT(cq::weight(dials, kSandoria, kLitelor, nobodyHolds(), kStillTide, campaign), WithinAbs(8.0, 1e-9));
    CHECK_THAT(cq::weight(dials, kBastok, kLitelor, nobodyHolds(), kStillTide, campaign), WithinAbs(1.0, 1e-9));
    CHECK_THAT(cq::weight(dials, kSandoria, kAragoneu, nobodyHolds(), kStillTide, campaign), WithinAbs(1.0, 1e-9));
}

TEST_CASE("conquest focus: a region one nation alone focuses on shows its up arrow by the tally, not at the start", "[cardian][conquest]")
{
    // 20 seats through xi_world's own catch-up: the arrow is earned through the window
    auto dials       = cq::evenDials();
    dials.expPerHour = { { 1, 2500.0 }, { 75, 2500.0 } };
    CHECK(focusArrows(dials, kLitelor, 8.0, 20, 20, 6, 21u) <= 2);   // a quarter of a real hour in
    CHECK(focusArrows(dials, kLitelor, 8.0, 20, 20, 71, 22u) >= 16); // by the tally
    CHECK(focusArrows(dials, kLitelor, 1.0, 20, 20, 71, 23u) == 0);  // with no focus, nobody's up
}

TEST_CASE("conquest thumb: the favour passes to the nation holding the fewest regions, never back to the one it leaves", "[cardian][conquest]")
{
    auto owners       = nobodyHolds();
    owners[0]         = kSandoria;
    owners[1]         = kSandoria;
    owners[2]         = kSandoria;
    owners[3]         = kBastok;
    owners[5]         = kWindurst;
    owners[6]         = kWindurst;
    const auto bottom = [] { return 0.0; };
    const auto top    = [] { return 0.999; };
    CHECK(cq::nextFavoured(kSandoria, owners, bottom) == kBastok);   // Bastok holds one, Windurst two
    CHECK(cq::nextFavoured(kBastok, owners, bottom) == kWindurst);   // never back to Bastok: Windurst, two, before San d'Oria's three
    CHECK(cq::nextFavoured(cq::kNations, owners, bottom) == kBastok); // nobody favoured yet: all three asked
    owners[4] = kBastok;                                             // Bastok two, Windurst two
    CHECK(cq::nextFavoured(kSandoria, owners, bottom) == kBastok);   // a draw between equals
    CHECK(cq::nextFavoured(kSandoria, owners, top) == kWindurst);

    // The thumb weighs only round Jeuno, only for the nation it favours
    std::string error;
    const auto  dials = cq::toDials(readDialsFile(), error);
    REQUIRE(dials.has_value());
    cq::Campaign campaign{};
    campaign.favoured = kBastok;
    CHECK_THAT(cq::weight(*dials, kBastok, kDerfland, nobodyHolds(), kStillTide, campaign), WithinAbs(1.5, 1e-9));
    CHECK_THAT(cq::weight(*dials, kSandoria, kDerfland, nobodyHolds(), kStillTide, campaign), WithinAbs(1.0, 1e-9));
    CHECK_THAT(cq::weight(*dials, kBastok, kLitelor, nobodyHolds(), kStillTide, campaign), WithinAbs(1.0, 1e-9));
}

TEST_CASE("conquest thumb: it counts each tally once, from the period's second hour, and passes on after its tallies", "[cardian][conquest]")
{
    // San d'Oria holds one region, Bastok two, Windurst three
    auto owners       = nobodyHolds();
    owners[0]         = kSandoria;
    owners[2]         = kBastok;
    owners[3]         = kBastok;
    owners[5]         = kWindurst;
    owners[6]         = kWindurst;
    owners[7]         = kWindurst;
    const auto bottom = [] { return 0.0; };

    // A fresh table: nobody favoured, and the period's first hour waits for the tally's owners
    cq::Thumb thumb{};
    CHECK_FALSE(cq::advanceThumb(thumb, 100, 0, 4, owners, bottom));
    CHECK(thumb.favoured == cq::kNations);
    CHECK(cq::advanceThumb(thumb, 100, 1, 4, owners, bottom));
    CHECK(thumb.favoured == kSandoria);
    CHECK(thumb.held == 0);

    // The same period again -- a later step, or a restart -- counts nothing
    CHECK_FALSE(cq::advanceThumb(thumb, 100, 30, 4, owners, bottom));
    CHECK(thumb.held == 0);

    // Three more tallies: San d'Oria keeps it
    for (int64 period = 200; period <= 400; period += 100)
    {
        CHECK(cq::advanceThumb(thumb, period, 1, 4, owners, bottom));
        CHECK(thumb.favoured == kSandoria);
    }
    CHECK(thumb.held == 3);

    // The fifth: on to the other nation holding the fewest regions
    CHECK(cq::advanceThumb(thumb, 500, 1, 4, owners, bottom));
    CHECK(thumb.favoured == kBastok);
    CHECK(thumb.held == 0);
    CHECK(thumb.period == 500);
}

TEST_CASE("conquest open regions: home and the next region from a fresh server's first day, the outer regions with the world's level", "[cardian][conquest]")
{
    std::string error;
    const auto  dials = cq::toDials(readDialsFile(), error);
    REQUIRE(dials.has_value());

    // A fresh server, the world at its floor of 18: each nation's home and the region next to it
    for (uint8 r = 0; r < cq::kRegions; ++r)
    {
        const bool homeOrNext = r == kRonfaure || r == kGustaberg || r == kSaruta || r == kZulkheim || r == kKolshushu;
        INFO(cq::kRegionNames[r]);
        CHECK(cq::inPlay(*dials, r, 18) == homeOrNext);
    }
    // The world at 27: the four round Jeuno too, nothing past them
    CHECK(cq::inPlay(*dials, kNorvallen, 27));
    CHECK(cq::inPlay(*dials, kQufim, 27));
    CHECK_FALSE(cq::inPlay(*dials, kFauregandi, 27));
    CHECK_FALSE(cq::inPlay(*dials, kLitelor, 27));
    CHECK(cq::focusPicks(*dials, 27) == 1);

    // The crowd: a slot in a region not open seats nobody, whatever its band, and the far
    // crowd comes with its region, at the world's level, on top of its tables
    const std::vector<cq::Slot> slots{
        cq::Slot{ .region = kRonfaure, .low = 1, .high = 2, .seats = 1 },
        cq::Slot{ .region = kNorvallen, .low = 15, .high = 29, .seats = 8 },
        cq::Slot{ .region = kFauregandi, .low = 3, .high = 18, .seats = 4 },
    };
    const auto seatsIn = [](const std::vector<cq::Seat>& seats, const uint8 region)
    {
        return std::ranges::count_if(seats, [&](const cq::Seat& seat) { return seat.region == region; });
    };
    const auto fresh = cq::fieldCrowd(slots, *dials, 18);
    CHECK(fresh.size() == 1);
    CHECK(seatsIn(fresh, kRonfaure) == 1);
    const auto at27 = cq::fieldCrowd(slots, *dials, 27);
    CHECK(at27.size() == 9);
    CHECK(seatsIn(at27, kNorvallen) == 8);
    CHECK(seatsIn(at27, kFauregandi) == 0);
    const auto at35 = cq::fieldCrowd(slots, *dials, 35);
    CHECK(seatsIn(at35, kFauregandi) == 4 + 12);
    CHECK(seatsIn(at35, kElshimoLow) == 12);
    CHECK(seatsIn(at35, kLitelor) == 0);
    for (const auto& seat : at35)
    {
        if (seat.region == kElshimoLow)
        {
            CHECK(seat.level == 35);
        }
    }
}

TEST_CASE("conquest shares: a nation with no adventurers of its own still holds its home region", "[cardian][conquest]")
{
    // The crowd's nations come from the dials alone: the sum takes no census and no nation from
    // any seat. Each home region's crowd of 20 seats, through periods of 72 Vana'diel hours and
    // xi_world's own catch-up for the trailing nations: the home bonus holds it
    std::string error;
    auto        dials = cq::toDials(readDialsFile(), error);
    REQUIRE(dials.has_value());
    dials->expPerHour = { { 1, 2500.0 }, { 75, 2500.0 } };
    for (uint8 nation = 0; nation < cq::kNations; ++nation)
    {
        INFO(cq::kNationNames[nation] << " in its home region");
        CHECK(homeWins(*dials, cq::kHomeRegion[nation], nation, 20, 20, 7u + nation) >= 18);
    }

    // Without the home bonus the home nation is one of three, and wins about a third of the time
    dials->homeBonus = 1.0;
    CHECK(homeWins(*dials, kGustaberg, kBastok, 20, 20, 11u) <= 12);
}

TEST_CASE("conquest sum: whole units go out, the fractions carry to the next hour", "[cardian][conquest]")
{
    const auto dials = flatDials(2500.0);
    const auto hour  = cq::sumHour({ cq::Seat{ .region = kZulkheim, .level = 20 } }, dials, nobodyHolds(), kStillTide, alwaysFirst);
    cq::Carry  carry{};
    int32      points = 0;
    int32      kills  = 0;
    for (int i = 0; i < 4; ++i)
    {
        const auto sends = cq::settle(hour, carry);
        points += sends.points[kZulkheim][kSandoria];
        kills += sends.kills[kZulkheim];
        CHECK(sends.points[kZulkheim][kSandoria] % cq::kPointsPerInfluence == 0); // xi_world divides by ten
    }
    CHECK(points == 25 * cq::kPointsPerInfluence);        // 4 x 6.25 influence, sent as ten points each
    CHECK(kills == 4);                                     // 4 x 1.2, the 0.8 waiting
    CHECK(cq::settle(hour, carry).kills[kZulkheim] == 2); // the fifth hour's 1.2 makes the 0.8 whole

    // Several hours at once are the same hour that many times
    auto late = hour;
    cq::scale(late, 3.0);
    CHECK_THAT(late.influence[kZulkheim][kSandoria], WithinAbs(18.75, 1e-9));
    CHECK_THAT(late.kills[kZulkheim], WithinAbs(3.6, 1e-9));
}

TEST_CASE("conquest sum: a busy region's period is about 11,000 influence, as §20.5 reckons it", "[cardian][conquest]")
{
    const auto                             dials = flatDials(2500.0);
    const std::vector<cq::Seat>            seats(25, cq::Seat{ .region = kZulkheim, .level = 20 });
    std::mt19937                           rng(3u);
    std::uniform_real_distribution<double> uniform(0.0, 1.0);
    const auto                             hour  = cq::sumHour(seats, dials, nobodyHolds(), kStillTide, [&] { return uniform(rng); });
    double                                 total = 0.0;
    for (const auto value : hour.influence[kZulkheim])
    {
        total += value;
    }
    CHECK_THAT(total * 72.0, WithinAbs(11250.0, 1e-6)); // three Vana'diel days of 24 hours, however the nations fell
}

TEST_CASE("conquest sum: an empty region owes the beastmen one death a period, from its second hour", "[cardian][conquest]")
{
    CHECK(cq::owesEmptyDeath(0, 7, 6, 1));
    CHECK_FALSE(cq::owesEmptyDeath(0, 7, 6, 0)); // the tally that opens the period might still be on its way
    CHECK_FALSE(cq::owesEmptyDeath(0, 7, 7, 5)); // paid this period
    CHECK_FALSE(cq::owesEmptyDeath(3, 7, 6, 5)); // seated: its own pressure
}

TEST_CASE("conquest sum: the tide drifts round the dial, back toward it, never past its amplitude", "[cardian][conquest]")
{
    CHECK(cq::tideStep(0.15, 0.0, 24.0, 2.0) == 0.0); // amplitude 0 holds it still
    const double pulled = cq::tideStep(0.15, 0.2, 24.0, 0.0);
    CHECK(pulled < 0.15);
    CHECK(pulled > 0.14); // a Vana'diel hour against a day's period: a small pull
    CHECK(cq::tideStep(0.19, 0.2, 24.0, 1000.0) == 0.2);
    CHECK(cq::tideStep(-0.19, 0.2, 24.0, -1000.0) == -0.2);
}

// -- The dials file -----------------------------------------------------------------

TEST_CASE("conquest dials: the file reads, and a name that is not a region or a nation is refused", "[cardian][conquest]")
{
    std::string error;
    const auto  dials = cq::toDials(readDialsFile(), error);
    INFO(error);
    REQUIRE(dials.has_value());
    CHECK(dials->homeBonus == 8.0);
    CHECK(dials->nextPull == 1.1);
    CHECK(dials->nextRegion[kWindurst][kKolshushu]);
    CHECK_FALSE(dials->nextRegion[kWindurst][kZulkheim]);
    CHECK(dials->thumbRegion[kNorvallen]);
    CHECK_FALSE(dials->thumbRegion[kLitelor]);
    CHECK(dials->thumbTallies == 4);
    CHECK(dials->focusPull == 8.0);
    CHECK(dials->focusRamp == 0.5);
    CHECK(dials->opensAt[kNorvallen] == 20);
    CHECK(dials->opensAt[kRonfaure] == 0);
    CHECK(dials->farSeats == 12);
    CHECK(dials->cpRate == 0.125);
    CHECK_FALSE(dials->expPerHour.empty());

    cq::files::DialFile bad{};
    bad.borders["atlantis"] = { "ronfaure" };
    CHECK_FALSE(cq::toDials(bad, error).has_value());
    cq::files::DialFile badHand{};
    badHand.hands.push_back(cq::files::Hand{ .nation = "jeuno", .region = "ronfaure", .times = 2.0 });
    CHECK_FALSE(cq::toDials(badHand, error).has_value());
    cq::files::DialFile badTable{};
    badTable.exp_per_hour = { { 10.0, 100.0 }, { 5.0, 200.0 } };
    CHECK_FALSE(cq::toDials(badTable, error).has_value());

    // The new keys, each refused on its own: a file of defaults reads, so each refusal is its key's
    CHECK(cq::toDials(cq::files::DialFile{}, error).has_value());
    const auto refused = [&error](const auto& spoil)
    {
        cq::files::DialFile file{};
        spoil(file);
        return !cq::toDials(file, error).has_value();
    };
    CHECK(refused([](auto& file) { file.outer = { { "norvallen", 20 }, { "jeuno", 20 } }; }));
    CHECK(refused([](auto& file) { file.outer = { { "norvallen", 300 } }; }));
    CHECK(refused([](auto& file) { file.next_regions["jeuno"] = { "zulkheim" }; }));
    CHECK(refused([](auto& file) { file.next_regions["bastok"] = { "atlantis" }; }));
    CHECK(refused([](auto& file) { file.next_pull = -1.0; }));
    CHECK(refused([](auto& file) { file.thumb.regions = { "jeuno" }; }));
    CHECK(refused([](auto& file) { file.thumb.tallies = 0; }));
    CHECK(refused([](auto& file) { file.focus.share = 1.5; }));
    CHECK(refused([](auto& file) { file.focus.pull = 0.0; }));
    CHECK(refused([](auto& file) { file.focus.jitter = 1.0; }));
    CHECK(refused([](auto& file) { file.focus.ramp = 1.5; }));
    CHECK(refused([](auto& file) { file.far_crowd.regions = { "jeuno" }; }));
    CHECK(refused([](auto& file) { file.far_crowd.seats = cq::kMostFarSeats + 1; }));
}

TEST_CASE("conquest dials: the land borders are the zone lines between the regions", "[cardian][conquest]")
{
    const auto file = readDialsFile();

    // The file's land borders, each listed both ways
    std::set<std::pair<uint8, uint8>> listed;
    for (const auto& [name, others] : file.borders)
    {
        const auto region = cq::regionByName(name);
        REQUIRE(region.has_value());
        for (const auto& other : others)
        {
            const auto next = cq::regionByName(other);
            REQUIRE(next.has_value());
            listed.emplace(*region, *next);
        }
    }
    for (const auto& [a, b] : listed)
    {
        INFO(cq::kRegionNames[a] << " lists " << cq::kRegionNames[b] << ", not the other way round");
        CHECK(listed.contains({ b, a }));
    }

    // Every zone line from a zone of one conquest region into a zone of another
    std::set<std::pair<uint8, uint8>> lines;
    for (uint16 id = 0; id < 1024; ++id)
    {
        const auto zoneId = static_cast<xi::ZoneId>(id);
        const auto region = static_cast<uint8>(zoneutils::GetCurrentRegion(zoneId));
        if (region >= cq::kRegions)
        {
            continue;
        }
        const auto zoneFile = xi::data::loadZoneFile<xi::data::datasets::zones::settings::Dataset>(zoneId);
        if (!zoneFile.has_value())
        {
            continue;
        }
        for (const auto& line : zoneFile->ZoneLines)
        {
            const auto there = static_cast<uint8>(zoneutils::GetCurrentRegion(line.DestinationZone));
            if (there < cq::kRegions && there != region)
            {
                lines.emplace(region, there);
                lines.emplace(there, region);
            }
        }
    }
    REQUIRE_FALSE(lines.empty());
    for (const auto& [a, b] : lines)
    {
        INFO("a zone line joins " << cq::kRegionNames[a] << " to " << cq::kRegionNames[b]);
        CHECK(listed.contains({ a, b }));
    }
    for (const auto& [a, b] : listed)
    {
        INFO(cq::kRegionNames[a] << " lists " << cq::kRegionNames[b] << " with no zone line between them");
        CHECK(lines.contains({ a, b }));
    }

    // The sea links are not land: the ferry joins Zulkheim to Kolshushu
    for (const auto& [a, b] : file.sea)
    {
        const auto ra = cq::regionByName(a);
        const auto rb = cq::regionByName(b);
        REQUIRE(ra.has_value());
        REQUIRE(rb.has_value());
        CHECK_FALSE(lines.contains({ *ra, *rb }));
    }
}
