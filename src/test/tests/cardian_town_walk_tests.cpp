// Cardian: a town walk walked one's own way (pawn/town_walk_math.h): her side
// of the street and her line's wander, long stretches given points to wander
// through, every corner a curve kept to the ground, and her beat before she
// sets out.
#include "map/pawn/town_walk_math.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <optional>
#include <vector>

using namespace cardian::townwalk;

namespace
{
    // Open ground everywhere: every point stands, every straight walk goes
    auto open()
    {
        return [](const Pt& p) -> std::optional<Pt> { return p; };
    }
    auto anyWay()
    {
        return [](const Pt&, const Pt&) { return true; };
    }

    // The nearest the point comes to the polyline
    auto offLine(const std::vector<Pt>& line, const Pt& p) -> float
    {
        float best = 1e9f;
        for (size_t i = 1; i < line.size(); ++i)
        {
            const Pt&   a  = line[i - 1];
            const Pt&   b  = line[i];
            const float dx = b.x - a.x, dz = b.z - a.z;
            const float l2 = dx * dx + dz * dz;
            const float t  = l2 > 0.0f ? std::clamp(((p.x - a.x) * dx + (p.z - a.z) * dz) / l2, 0.0f, 1.0f) : 0.0f;
            best           = std::min(best, std::hypot(a.x + dx * t - p.x, a.z + dz * t - p.z));
        }
        return best;
    }
} // namespace

TEST_CASE("Town walk: a long stretch gets points no further apart than the spacing", "[cardian][townwalk]")
{
    const std::vector<Pt> route{ { 0, 0, 0 }, { 23, 0, 0 } };
    const auto            dense = densify(route, 5.0f);
    REQUIRE(dense.front().x == 0.0f);
    REQUIRE(dense.back().x == 23.0f);
    for (size_t i = 1; i < dense.size(); ++i)
    {
        CHECK(flat(dense[i - 1], dense[i]) <= 5.0f + 1e-3f);
    }
    CHECK(densify(route, 0.0f).size() == 2);
}

TEST_CASE("Town walk: she keeps to the right of the way she walks, her ends where they were", "[cardian][townwalk]")
{
    // Walking east (x up): her right is south (z down)
    const std::vector<Pt> route{ { 0, 0, 0 }, { 30, 0, 0 } };
    Style                 style;
    style.right   = 1.0f;
    style.spacing = 5.0f;
    const auto walk = shape(route, style, open(), anyWay());
    REQUIRE(walk.size() > 2);
    CHECK(walk.front().x == 0.0f);
    CHECK(walk.front().z == 0.0f);
    CHECK(walk.back().x == 30.0f);
    CHECK(walk.back().z == 0.0f);
    for (size_t i = 1; i + 1 < walk.size(); ++i)
    {
        CHECK(std::abs(walk[i].z + 1.0f) < 1e-3f);
    }

    // Walking back west the same street, her right is north: two lanes
    const std::vector<Pt> back{ { 30, 0, 0 }, { 0, 0, 0 } };
    const auto            returning = shape(back, style, open(), anyWay());
    for (size_t i = 1; i + 1 < returning.size(); ++i)
    {
        CHECK(std::abs(returning[i].z - 1.0f) < 1e-3f);
    }
}

TEST_CASE("Town walk: her line wanders about her side, smoothly", "[cardian][townwalk]")
{
    Style style;
    style.right      = 0.8f;
    style.drift      = 0.5f;
    style.wavelength = 16.0f;
    CHECK(std::abs(offsetAt(style, 0.0f) - 0.8f) < 1e-3f);
    CHECK(std::abs(offsetAt(style, 4.0f) - 1.3f) < 1e-3f); // a quarter of the wander
    CHECK(std::abs(offsetAt(style, 12.0f) - 0.3f) < 1e-3f);
    // no wander without a length to it
    style.wavelength = 0.0f;
    CHECK(offsetAt(style, 4.0f) == style.right);
}

TEST_CASE("Town walk: a corner becomes a curve near it, and turns gently", "[cardian][townwalk]")
{
    const std::vector<Pt> route{ { 0, 0, 0 }, { 10, 0, 0 }, { 10, 0, 10 } };
    Style                 style;
    style.round     = 2.0f;
    const auto walk = shape(route, style, open(), anyWay());
    REQUIRE(walk.size() > route.size());
    CHECK(walk.front().x == 0.0f);
    CHECK(walk.back().z == 10.0f);
    // no point strays far from the route it rounds
    for (const auto& p : walk)
    {
        CHECK(offLine(route, p) < 1.0f);
    }
    // the hard corner itself is gone, and no turn along the curve is as hard
    for (size_t i = 1; i + 1 < walk.size(); ++i)
    {
        CHECK_FALSE((walk[i].x == 10.0f && walk[i].z == 0.0f));
        CHECK(turnAt(walk[i - 1], walk[i], walk[i + 1]) < 1.0f);
    }
}

TEST_CASE("Town walk: a gentle bend is left as it is", "[cardian][townwalk]")
{
    const std::vector<Pt> route{ { 0, 0, 0 }, { 10, 0, 0 }, { 20, 0, 1 } };
    Style                 style;
    style.round     = 2.0f;
    const auto walk = shape(route, style, open(), anyWay());
    CHECK(walk.size() == route.size());
}

TEST_CASE("Town walk: what the ground refuses is left as the route had it", "[cardian][townwalk]")
{
    const std::vector<Pt> route{ { 0, 0, 0 }, { 10, 0, 0 }, { 10, 0, 10 } };
    Style                 style;
    style.right   = 1.0f;
    style.round   = 2.0f;
    style.spacing = 0.0f;
    // no ground off the route's own points: nothing slides, no corner rounds
    const auto onlyRoute = [&](const Pt& p) -> std::optional<Pt>
    {
        for (const auto& r : route)
        {
            if (flat(r, p) < 1e-3f)
            {
                return r;
            }
        }
        return std::nullopt;
    };
    const auto walk = shape(route, style, onlyRoute, anyWay());
    REQUIRE(walk.size() == route.size());
    for (size_t i = 0; i < walk.size(); ++i)
    {
        CHECK(flat(walk[i], route[i]) < 1e-3f);
    }

    // a wall across the inside of the corner: no straight walk there, so
    // the corner stays a corner
    const auto noShortcut = [](const Pt& a, const Pt& b) { return !(a.x > 8.5f && a.z < 1.5f && b.x > 8.5f && b.z > 0.01f && b.x < 9.99f); };
    Style      round;
    round.round   = 2.0f;
    const auto kept = shape(route, round, open(), noShortcut);
    bool       hasCorner = false;
    for (const auto& p : kept)
    {
        hasCorner = hasCorner || (p.x == 10.0f && p.z == 0.0f);
    }
    CHECK(hasCorner);
}

TEST_CASE("Town walk: her beat is mostly short, now and then long, and in bounds", "[cardian][townwalk]")
{
    CHECK(startBeat(0.0f, 0.2f, 5.0f) == 0.2f);
    CHECK(std::abs(startBeat(1.0f, 0.2f, 5.0f) - 5.0f) < 1e-4f);
    CHECK(startBeat(0.5f, 0.2f, 5.0f) < 1.0f); // half the rolls under a second
    CHECK(startBeat(2.0f, 0.2f, 5.0f) <= 5.0f);
    for (uint32_t salt = 0; salt < 8; ++salt)
    {
        const float f = fraction(0xC0FFEEu, salt);
        CHECK(f >= 0.0f);
        CHECK(f < 1.0f);
    }
    CHECK(fraction(1u, 1u) != fraction(1u, 2u));
}
