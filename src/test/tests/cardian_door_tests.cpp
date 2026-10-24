// Cardian: a closed door as a wall on the ground (pawn/door_math.h): which
// side of its plane a point stands on, where a straight walk meets it -- in
// the doorway and on its storey -- and where a walk that meets it stops.
#include "map/pawn/door_math.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

using namespace cardian::door;
using Catch::Matchers::WithinAbs;

namespace
{
    // The President's Office door in Metalworks, roughly: a doorway along z,
    // its + side the office (x past the door)
    constexpr Plane kOffice{ 95.6f, -19.0f, 0.0f, 1.0f, 0.0f, 2.0f };

    struct P
    {
        float x;
        float y;
        float z;
    };
} // namespace

TEST_CASE("Door: a point's side is the sign of its distance in front of the plane", "[cardian][door]")
{
    CHECK(sideOf(kOffice, 97.3f, 0.0f) == 1);
    CHECK(sideOf(kOffice, 94.3f, 0.0f) == -1);
    CHECK(sideOf(kOffice, 94.3f, 50.0f) == -1); // however far along the plane
    CHECK_THAT(ahead(kOffice, 97.6f, 3.0f), WithinAbs(2.0f, 1e-4f));
}

TEST_CASE("Door: a walk through the doorway meets the door where it crosses the plane", "[cardian][door]")
{
    const auto t = meets(kOffice, 93.6f, -20.2f, 0.0f, 97.6f, -20.2f, 0.0f);
    REQUIRE(t.has_value());
    CHECK_THAT(*t, WithinAbs(0.5f, 1e-4f));

    // ... from either side
    const auto back = meets(kOffice, 97.6f, -20.2f, 0.5f, 93.6f, -20.2f, 0.5f);
    REQUIRE(back.has_value());
    CHECK_THAT(*back, WithinAbs(0.5f, 1e-4f));
}

TEST_CASE("Door: a walk that stays on one side, passes beside the doorway or on another storey, never meets it", "[cardian][door]")
{
    // Along the corridor in front of it
    CHECK_FALSE(meets(kOffice, 94.0f, -20.2f, -10.0f, 94.0f, -20.2f, 10.0f).has_value());
    // Across the plane, but past the doorway's edge and the body's margin
    CHECK_FALSE(meets(kOffice, 93.6f, -20.2f, 2.0f, 97.6f, -20.2f, 2.0f).has_value());
    // ... and just within the margin it meets
    CHECK(meets(kOffice, 93.6f, -20.2f, 1.4f, 97.6f, -20.2f, 1.4f).has_value());
    // A storey below
    CHECK_FALSE(meets(kOffice, 93.6f, -24.0f, 0.0f, 97.6f, -24.0f, 0.0f).has_value());
}

TEST_CASE("Door: the first leg of a route that meets it", "[cardian][door]")
{
    const std::vector<P> route{ { 80.0f, -20.0f, 5.0f }, { 90.0f, -20.0f, 0.0f }, { 98.0f, -20.0f, 0.0f }, { 104.0f, -20.0f, 4.0f } };
    const auto           meeting = firstMeeting(kOffice, route);
    REQUIRE(meeting.has_value());
    CHECK(meeting->leg == 2);
    CHECK_THAT(meeting->t, WithinAbs(5.6f / 8.0f, 1e-4f));

    const std::vector<P> clear{ { 80.0f, -20.0f, 5.0f }, { 90.0f, -20.0f, 0.0f } };
    CHECK_FALSE(firstMeeting(kOffice, clear).has_value());
}

TEST_CASE("Door: a walk that meets it stops a yalm short, never behind its leg's start", "[cardian][door]")
{
    CHECK_THAT(stopShort(8.0f, 0.7f, 1.0f), WithinAbs(4.6f / 8.0f, 1e-4f));
    CHECK_THAT(stopShort(8.0f, 0.1f, 1.0f), WithinAbs(0.0f, 1e-6f));
    CHECK_THAT(stopShort(0.0f, 0.5f, 1.0f), WithinAbs(0.0f, 1e-6f));
}
