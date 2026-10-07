// Cardian: the party finder's rules (pawn/finder_rules.h, RESEARCH.md §18.6):
// who hears a shout, how one who hears it answers, which of those in reach
// are heard, and in what order.
#include "pawn/finder_rules.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <limits>
#include <optional>
#include <random>
#include <set>
#include <vector>

using namespace pawn::finder;
using namespace pawn::finder::rules;

namespace
{
    const Goal kExp{ Goal::Kind::Experience, 0 };
    const Goal kQuest{ Goal::Kind::Quest, 0 };
    const Goal kBastokMission{ Goal::Kind::Mission, 1 };
    const Goal kZilartMission{ Goal::Kind::Mission, 3 };

    // Her yeses over every threshold her seed can give her and every mood a
    // shout can roll, as a share of all of them: the rate a crowd of
    // strangers answers at
    auto yesRate(const Goal& goal, const MissionFit fit, const uint8 fame, const int rankDiff) -> double
    {
        int yes = 0;
        int all = 0;
        for (uint32 seed = 0; seed < kSeedSpread; ++seed)
        {
            for (int mood = kMoodMin; mood <= kMoodMax; ++mood)
            {
                yes += willing(goal, fit, fame, rankDiff, mood, seed) ? 1 : 0;
                ++all;
            }
        }
        return static_cast<double>(yes) / all;
    }

    auto stranger(const std::size_t index, const bool yes, const uint32 hops = 0) -> Seat
    {
        return Seat{ .index = index, .yes = yes, .hops = hops };
    }

    auto indices(const Heard& heard) -> std::set<std::size_t>
    {
        std::set<std::size_t> out;
        for (const auto& s : heard.seats)
        {
            out.insert(s.index);
        }
        return out;
    }
} // namespace

TEST_CASE("Finder: the yell is heard in every city and town, never in the field", "[cardian][finder]")
{
    CHECK(hears(true, 0, kExp));
    CHECK(hears(true, 2, kQuest));
    CHECK_FALSE(hears(false, 0, kExp)); // in the field or a dungeon: busy
    CHECK_FALSE(hears(false, 1, kBastokMission));
}

TEST_CASE("Finder: a nation's mission is heard by its own people only", "[cardian][finder]")
{
    CHECK(hears(true, 1, kBastokMission));
    CHECK_FALSE(hears(true, 0, kBastokMission));
    CHECK_FALSE(hears(true, 2, kBastokMission));
    // A log no nation owns is anyone's
    CHECK(hears(true, 0, kZilartMission));
    CHECK(hears(true, 2, kZilartMission));
}

TEST_CASE("Finder: the band is the player's level plus or minus the setting", "[cardian][finder]")
{
    CHECK(bandSide(20, 17, 3) == 0);
    CHECK(bandSide(20, 23, 3) == 0);
    CHECK(bandSide(20, 16, 3) == -1);
    CHECK(bandSide(20, 24, 3) == 1);
}

TEST_CASE("Finder: a stranger the player's fame has not reached says yes to exp one time in two", "[cardian][finder]")
{
    CHECK(yesRate(kExp, MissionFit::Free, 1, 0) == 0.5);
}

TEST_CASE("Finder: a stranger says yes to a quest about one time in three", "[cardian][finder]")
{
    const double rate = yesRate(kQuest, MissionFit::Free, 1, 0);
    CHECK(rate == 13.0 / 40.0);
    CHECK(rate > 0.30);
    CHECK(rate < 0.36);
}

TEST_CASE("Finder: fame and rank still move the answer", "[cardian][finder]")
{
    CHECK(yesRate(kExp, MissionFit::Free, 2, 0) > yesRate(kExp, MissionFit::Free, 1, 0));
    CHECK(yesRate(kExp, MissionFit::Free, 1, 2) > yesRate(kExp, MissionFit::Free, 1, 0));
    CHECK(yesRate(kExp, MissionFit::Free, 1, -2) < yesRate(kExp, MissionFit::Free, 1, 0));
    CHECK(yesRate(kExp, MissionFit::Free, 9, 3) == 1.0);
}

TEST_CASE("Finder: on the player's mission a stranger comes whatever his standing", "[cardian][finder]")
{
    CHECK(yesRate(kBastokMission, MissionFit::On, 1, -9) == 1.0);
    CHECK(yesRate(kZilartMission, MissionFit::On, 1, 0) == 1.0);
}

TEST_CASE("Finder: a mission she has done, or is free to take, answers at the exp rate", "[cardian][finder]")
{
    for (const uint8 fame : std::initializer_list<uint8>{ 1, 3, 6 })
    {
        for (const int rankDiff : { -2, 0, 2 })
        {
            CHECK(yesRate(kBastokMission, MissionFit::Done, fame, rankDiff) == yesRate(kExp, MissionFit::Free, fame, rankDiff));
            CHECK(yesRate(kBastokMission, MissionFit::Free, fame, rankDiff) == yesRate(kExp, MissionFit::Free, fame, rankDiff));
        }
    }
}

TEST_CASE("Finder: a mission she has not reached is always a no", "[cardian][finder]")
{
    CHECK(yesRate(kBastokMission, MissionFit::Behind, 9, 3) == 0.0);
}

TEST_CASE("Finder: sixteen to twenty hear a shout when the crowd has them", "[cardian][finder]")
{
    std::mt19937 rng(7);
    std::vector<Seat> pool;
    for (std::size_t i = 0; i < 200; ++i)
    {
        pool.push_back(stranger(i, i % 2 == 0));
    }
    for (std::size_t heard = kHeardMin; heard <= kHeardMax; ++heard)
    {
        const auto picked = pickHeard(pool, heard, 5, rng);
        CHECK(picked.seats.size() == heard);
        CHECK(indices(picked).size() == heard); // nobody twice
    }
}

TEST_CASE("Finder: a crowd smaller than the shout is heard whole", "[cardian][finder]")
{
    std::mt19937      rng(11);
    std::vector<Seat> pool{ stranger(0, true), stranger(1, false), stranger(2, false) };
    const auto        picked = pickHeard(pool, 18, 5, rng);
    CHECK(indices(picked) == std::set<std::size_t>{ 0, 1, 2 });
    CHECK_FALSE(picked.friendSeat.has_value());
    CHECK_FALSE(picked.affinitySeat.has_value());
}

TEST_CASE("Finder: the friend seat goes to the friend longest unheard", "[cardian][finder]")
{
    std::mt19937      rng(3);
    std::vector<Seat> pool;
    for (std::size_t i = 0; i < 100; ++i)
    {
        pool.push_back(stranger(i, true));
    }
    pool[40].friendly = true;
    pool[40].heardAt  = 500;
    pool[41].friendly = true;
    pool[41].heardAt  = 100; // heard longest ago
    pool[42].friendly = true;
    pool[42].heardAt  = 900;
    for (int run = 0; run < 20; ++run)
    {
        const auto picked = pickHeard(pool, 16, 5, rng);
        REQUIRE(picked.friendSeat.has_value());
        CHECK(*picked.friendSeat == 41);
    }
}

TEST_CASE("Finder: the affinity seat goes to the highest affinity besides the friend seat", "[cardian][finder]")
{
    std::mt19937      rng(5);
    std::vector<Seat> pool;
    for (std::size_t i = 0; i < 100; ++i)
    {
        pool.push_back(stranger(i, false));
    }
    // Two friends: the longest unheard takes the friend seat, the one with
    // more affinity of those left takes the affinity seat
    pool[10].friendly = true;
    pool[10].affinity = 6;
    pool[10].heardAt  = 10;
    pool[20].friendly = true;
    pool[20].affinity = 4;
    pool[20].heardAt  = 50;
    pool[30].friendly = true;
    pool[30].affinity = 2;
    pool[30].heardAt  = 0;
    for (int run = 0; run < 20; ++run)
    {
        const auto picked = pickHeard(pool, 16, 5, rng);
        REQUIRE(picked.friendSeat.has_value());
        REQUIRE(picked.affinitySeat.has_value());
        CHECK(*picked.friendSeat == 30);
        CHECK(*picked.affinitySeat == 10);
    }
}

TEST_CASE("Finder: with no affinity in reach the affinity seat is filled like the rest", "[cardian][finder]")
{
    std::mt19937      rng(13);
    std::vector<Seat> pool;
    for (std::size_t i = 0; i < 50; ++i)
    {
        pool.push_back(stranger(i, i % 3 == 0));
    }
    pool[7].friendly = true; // partied before, no affinity yet
    const auto picked = pickHeard(pool, 16, 5, rng);
    CHECK(picked.friendSeat == std::optional<std::size_t>(7));
    CHECK_FALSE(picked.affinitySeat.has_value());
    CHECK(picked.seats.size() == 16);
}

TEST_CASE("Finder: strangers make up most of a shout, and a static is still heard", "[cardian][finder]")
{
    std::mt19937      rng(17);
    std::vector<Seat> pool;
    for (std::size_t i = 0; i < 300; ++i)
    {
        pool.push_back(stranger(i, i % 2 == 0));
    }
    // A static of five the player knows well
    for (std::size_t i = 0; i < 5; ++i)
    {
        pool[i].friendly = true;
        pool[i].affinity = static_cast<uint32>(3 + i);
        pool[i].heardAt  = static_cast<int64>(i);
    }
    for (int run = 0; run < 50; ++run)
    {
        const auto picked  = pickHeard(pool, 16, 5, rng);
        const auto friends = std::count_if(picked.seats.begin(), picked.seats.end(), [](const Seat& s) { return s.friendly; });
        CHECK(friends >= 2); // the two kept seats
        CHECK(static_cast<std::size_t>(friends) * 2 < picked.seats.size());
    }
}

TEST_CASE("Finder: enough yeses to fill the party when the crowd has them", "[cardian][finder]")
{
    std::mt19937      rng(19);
    std::vector<Seat> pool;
    for (std::size_t i = 0; i < 200; ++i)
    {
        pool.push_back(stranger(i, i % 20 == 0)); // ten willing in two hundred
    }
    for (int run = 0; run < 20; ++run)
    {
        const auto picked = pickHeard(pool, 16, 5, rng);
        const auto yes    = std::count_if(picked.seats.begin(), picked.seats.end(), [](const Seat& s) { return s.yes; });
        CHECK(yes >= 5);
    }
}

TEST_CASE("Finder: the mix after the sure yeses answers as the crowd does", "[cardian][finder]")
{
    // Half the crowd willing: five sure yeses, then fifteen more at one in
    // two each, about twelve and a half yeses in twenty on average
    std::mt19937      rng(41);
    std::vector<Seat> pool;
    for (std::size_t i = 0; i < 400; ++i)
    {
        pool.push_back(stranger(i, i % 2 == 0));
    }
    constexpr int kRuns = 400;
    std::size_t   yes   = 0;
    for (int run = 0; run < kRuns; ++run)
    {
        const auto picked = pickHeard(pool, 20, 5, rng);
        yes += static_cast<std::size_t>(std::count_if(picked.seats.begin(), picked.seats.end(), [](const Seat& s) { return s.yes; }));
    }
    const double mean = static_cast<double>(yes) / kRuns;
    CHECK(mean > 12.0);
    CHECK(mean < 13.0);
}

TEST_CASE("Finder: no more than three who cannot come, however many the towns hold", "[cardian][finder]")
{
    // Out of the band or short of the mission: most of the world's towns,
    // to a player of any one level
    std::mt19937      rng(23);
    std::vector<Seat> pool;
    for (std::size_t i = 0; i < 300; ++i)
    {
        Seat s   = stranger(i, i >= 240 && i % 2 == 0);
        s.cannot = i < 240;
        pool.push_back(s);
    }
    for (int run = 0; run < 20; ++run)
    {
        const auto picked = pickHeard(pool, 20, 5, rng);
        const auto cannot = std::count_if(picked.seats.begin(), picked.seats.end(), [](const Seat& s) { return s.cannot; });
        CHECK(cannot == static_cast<std::ptrdiff_t>(kCannotMax));
        CHECK(picked.seats.size() == 20);
    }
}

TEST_CASE("Finder: kept seats that cannot come count among the three", "[cardian][finder]")
{
    // A level-30 player whose only friends are level 10: both kept seats go
    // to people out of his band, and leave room for one more such no
    std::mt19937      rng(43);
    std::vector<Seat> pool;
    for (std::size_t i = 0; i < 200; ++i)
    {
        Seat s   = stranger(i, i >= 100 && i % 2 == 0);
        s.cannot = i < 100;
        pool.push_back(s);
    }
    pool[10].friendly = true;
    pool[10].affinity = 2;
    pool[20].friendly = true;
    pool[20].affinity = 5;
    for (int run = 0; run < 20; ++run)
    {
        const auto picked = pickHeard(pool, 20, 5, rng);
        REQUIRE(picked.friendSeat.has_value());
        REQUIRE(picked.affinitySeat.has_value());
        const auto cannot = std::count_if(picked.seats.begin(), picked.seats.end(), [](const Seat& s) { return s.cannot; });
        CHECK(cannot == static_cast<std::ptrdiff_t>(kCannotMax));
        CHECK(picked.seats.size() == 20);
    }
}

TEST_CASE("Finder: with few who could come, those who cannot stay at three", "[cardian][finder]")
{
    std::mt19937      rng(31);
    std::vector<Seat> pool;
    for (std::size_t i = 0; i < 100; ++i)
    {
        Seat s   = stranger(i, i < 4);
        s.cannot = i >= 6;
        pool.push_back(s);
    }
    const auto picked = pickHeard(pool, 20, 5, rng);
    CHECK(picked.seats.size() == 6 + kCannotMax); // the six who could, and three who cannot
}

TEST_CASE("Finder: a kept seat goes to one who could come before one who cannot", "[cardian][finder]")
{
    std::mt19937      rng(37);
    std::vector<Seat> pool;
    for (std::size_t i = 0; i < 50; ++i)
    {
        pool.push_back(stranger(i, false));
    }
    pool[1].friendly = true; // heard longest ago, but out of the band
    pool[1].cannot   = true;
    pool[1].heardAt  = 0;
    pool[1].affinity = 9;
    pool[2].friendly = true;
    pool[2].heardAt  = 100;
    pool[2].affinity = 1;
    pool[3].affinity = 2;
    const auto picked = pickHeard(pool, 16, 5, rng);
    CHECK(picked.friendSeat == std::optional<std::size_t>(2));
    CHECK(picked.affinitySeat == std::optional<std::size_t>(3));
}

TEST_CASE("Finder: the nearest are heard first", "[cardian][finder]")
{
    std::mt19937      rng(29);
    std::vector<Seat> pool;
    for (std::size_t i = 0; i < 60; ++i)
    {
        pool.push_back(stranger(i, i % 2 == 0, static_cast<uint32>(i % 7)));
    }
    pool[3].friendly = true;
    pool[3].hops     = 6; // a kept seat speaks in its place too
    pool[5].hops     = std::numeric_limits<uint32>::max(); // no line reaches her: last
    const auto picked = pickHeard(pool, 20, 5, rng);
    CHECK(std::is_sorted(picked.seats.begin(), picked.seats.end(), [](const Seat& a, const Seat& b) { return a.hops < b.hops; }));
}
