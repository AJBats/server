// Cardian: a cardian on a chocobo (pawn/riding_math.h, and the trek's plan
// in pawn/subzone_graph.h): a body of the world holds the Chocobo License
// once any of her jobs stands at 20 or above, and rides on all of them
// then; a rider gets down in a fight, and once the player is on foot and she
// has reached him, never on her way to him; and a trek takes a city's stable
// when she may rent and it is the quicker way, rides on through the field,
// and walks once a zone that allows no mount gets her down; and the chocobo
// porter's ride times, one search to every place through the zones that
// allow a mount.
#include "pawn/riding_math.h"
#include "pawn/subzone_graph.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <array>

using namespace cardian::riding;
using Catch::Matchers::WithinAbs;

TEST_CASE("A job at 20 or above holds the Chocobo License", "[cardian][riding]")
{
    std::array<uint8_t, 22> levels{};
    levels.fill(1);
    CHECK_FALSE(licensedByJobs(levels));

    levels[5] = 19; // a Red Mage one short
    CHECK_FALSE(licensedByJobs(levels));

    levels[5] = 20; // the Red Mage at 20, every other job at 1: she rides on all of them
    CHECK(licensedByJobs(levels));

    levels[5] = 1;
    levels[1] = 37; // a Warrior well past it
    CHECK(licensedByJobs(levels));
}

TEST_CASE("A rider gets down in a fight, and beside the player on foot", "[cardian][riding]")
{
    // he rides: she rides on, near him or not
    CHECK_FALSE(dismounts(false, true, true));
    CHECK_FALSE(dismounts(false, true, false));

    // he is on foot: she rides on until she has reached him
    CHECK_FALSE(dismounts(false, false, false));
    CHECK(dismounts(false, false, true));

    // a fight gets her down wherever she is
    CHECK(dismounts(true, true, false));
    CHECK(dismounts(true, false, false));
}

namespace
{
    namespace sz = cardian::subzones;

    auto at(const float x, const float z) -> position_t
    {
        position_t p{};
        p.x = x;
        p.z = z;
        return p;
    }

    // A city (zone 1, no mount) whose gate at x 0 opens on a field (zone 2,
    // a mount allowed), whose far line at x 1000 leads into a dungeon (zone
    // 3, no mount); with the city's stable at x 10, setting a rider down
    // 800 yalms into the field
    auto town(const bool stable) -> sz::World
    {
        sz::World world;
        world.lines.push_back({ .zone = 1, .to = 2, .origin = at(0, 0), .arrival = at(0, 0), .leaves = { 1, at(0, 0) }, .lands = { 1, at(0, 0) } });
        world.lines.push_back({ .zone = 2, .to = 3, .origin = at(1000, 0), .arrival = at(0, 0), .leaves = { 1, at(1000, 0) }, .lands = { 1, at(0, 0) } });
        if (stable)
        {
            world.rentals.push_back({ .zone = 1, .piece = 1, .at = at(10, 0), .to = 2, .lands = 1, .landsAt = at(800, 0) });
        }
        world.mountZones = { 2 };
        sz::join(world);
        return world;
    }

    const sz::Ride kMayRent{ .mayRent = true, .mounted = false, .rideFactor = 0.5f };
} // namespace

TEST_CASE("Chocobos on the trek: a city's stable, taken when she may rent and it is quicker", "[cardian][riding]")
{
    const auto world = town(true);

    // on foot, or with no license: the gate, and the field walked
    const auto walk = sz::plan(world, 1, 1, at(20, 0), 2, std::nullopt, at(900, 0));
    CHECK(walk.firstLine == 0u);
    CHECK_FALSE(walk.firstRental.has_value());
    CHECK_THAT(walk.length, WithinAbs(20.0f + sz::kLineCost + 900.0f, 0.01f));

    // licensed: the stable, and the last 100 yalms ridden
    const auto ride = sz::plan(world, 1, 1, at(20, 0), 2, std::nullopt, at(900, 0), kMayRent);
    CHECK(ride.firstRental == 0u);
    CHECK_FALSE(ride.firstLine.has_value());
    CHECK_THAT(ride.length, WithinAbs(10.0f + sz::kRentCost + 100.0f * 0.5f, 0.01f));

    // the player just past the gate: the stable would set her down far
    // beyond him, and she walks
    const auto nearby = sz::plan(world, 1, 1, at(20, 0), 2, std::nullopt, at(30, 0), kMayRent);
    CHECK(nearby.firstLine == 0u);
    CHECK_FALSE(nearby.firstRental.has_value());

    // already riding, she rents no second chocobo (she is in the city here
    // only on foot, so no stable is open to a rider)
    const auto rider = sz::plan(world, 1, 1, at(20, 0), 2, std::nullopt, at(900, 0), sz::Ride{ .mayRent = false, .mounted = true, .rideFactor = 0.5f });
    CHECK(rider.firstLine == 0u);
    CHECK_FALSE(rider.firstRental.has_value());
}

TEST_CASE("Chocobos on the trek: she rides on through the field and walks once a dungeon gets her down", "[cardian][riding]")
{
    const auto world = town(false);
    const auto ride  = sz::Ride{ .mayRent = false, .mounted = true, .rideFactor = 0.5f };

    // mounted across the field, on foot in the dungeon
    const auto rides = sz::plan(world, 2, 1, at(0, 0), 3, std::nullopt, at(100, 0), ride);
    CHECK(rides.firstLine == 1u);
    CHECK_THAT(rides.length, WithinAbs(1000.0f * 0.5f + sz::kLineCost + 100.0f, 0.01f));

    // the same on foot, every yalm walked
    const auto walks = sz::plan(world, 2, 1, at(0, 0), 3, std::nullopt, at(100, 0));
    CHECK_THAT(walks.length, WithinAbs(1000.0f + sz::kLineCost + 100.0f, 0.01f));

    // a rider set down in a zone that allows no mount walks it from the start
    const auto inCity = sz::plan(world, 1, 1, at(0, 0), 1, std::nullopt, at(40, 0), ride);
    CHECK_THAT(inCity.length, WithinAbs(40.0f, 0.01f));
}

TEST_CASE("The chocobo porter's ride times: one search to every place, ridden through the zones that allow a mount alone", "[cardian][riding][porter]")
{
    // A field (zone 2, a mount allowed) whose line at x 0 leads into a city
    // (zone 1), and whose line at x 1000 into a dungeon (zone 3, no mount),
    // past which lies a far field (zone 4, a mount allowed)
    sz::World world;
    world.lines.push_back({ .zone = 2, .to = 1, .origin = at(0, 0), .arrival = at(0, 0), .leaves = { 1, at(0, 0) }, .lands = { 1, at(0, 0) } });
    world.lines.push_back({ .zone = 2, .to = 3, .origin = at(1000, 0), .arrival = at(0, 0), .leaves = { 1, at(1000, 0) }, .lands = { 1, at(0, 0) } });
    world.lines.push_back({ .zone = 3, .to = 4, .origin = at(200, 0), .arrival = at(0, 0), .leaves = { 1, at(200, 0) }, .lands = { 1, at(0, 0) } });
    world.mountZones = { 2, 4 };
    sz::join(world);

    const auto costs = sz::rideCosts(world, 2, 1, at(100, 0),
                                     {
                                         { .zone = 2, .piece = 1, .point = at(400, 0) }, // a crag in his own field
                                         { .zone = 1 },                                  // the city, entered through its gate
                                         { .zone = 3 },                                  // the dungeon, entered
                                         { .zone = 4, .point = at(50, 0) },              // the far field: no ride goes through the dungeon
                                         { .zone = 2, .piece = 2, .point = at(120, 0) }, // a piece of his field no line or drop reaches
                                     });
    REQUIRE(costs.size() == 5);
    REQUIRE(costs[0].has_value());
    CHECK_THAT(*costs[0], WithinAbs(300.0f, 0.01f));
    REQUIRE(costs[1].has_value());
    CHECK_THAT(*costs[1], WithinAbs(100.0f + sz::kLineCost, 0.01f));
    REQUIRE(costs[2].has_value());
    CHECK_THAT(*costs[2], WithinAbs(900.0f + sz::kLineCost, 0.01f));
    CHECK_FALSE(costs[3].has_value());
    CHECK_FALSE(costs[4].has_value());

    // the same search with every target reached ends as soon as nothing nearer can come
    const auto cityOnly = sz::rideCosts(world, 2, 1, at(100, 0), { { .zone = 1 } });
    REQUIRE(cityOnly.front().has_value());
    CHECK_THAT(*cityOnly.front(), WithinAbs(100.0f + sz::kLineCost, 0.01f));
}
