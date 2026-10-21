// Cardian: a navmesh built for the player's own client (pawn/client_navmesh.h)
// -- the chocobo porter's: the client's collision ported (client_collision.h),
// its walker judging a navmesh's joins and its drop links (client_joins.h) --
// a drop link off a ledge kept, one whose way down a tree's trunk blocks
// taken out -- and the strip past the body's margin, out to the rider's
// berth, kept and marked tight (erode).
#include "pawn/client_collision.h"
#include "pawn/client_joins.h"
#include "pawn/client_navmesh.h"

#include <Recast.h>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <algorithm>
#include <cmath>
#include <utility>
#include <vector>

using namespace cardian::client;
using Catch::Matchers::WithinAbs;

namespace
{
    namespace cc = cardian::client::collision;

    auto tri(const cc::Vec& a, const cc::Vec& b, const cc::Vec& c) -> cc::Tri
    {
        cc::Tri t;
        REQUIRE(cc::makeTri(a, b, c, t));
        return t;
    }

    // A level floor at height y (y down), its normal up
    void floorAt(std::vector<cc::Tri>& out, const double x0, const double x1, const double z0, const double z1, const double y)
    {
        out.push_back(tri({ x0, y, z0 }, { x1, y, z1 }, { x1, y, z0 }));
        out.push_back(tri({ x0, y, z0 }, { x0, y, z1 }, { x1, y, z1 }));
    }

    // A face across x from its foot at ylo to its top at yhi, leaning `run`
    // toward +x over its height, facing -x
    void faceAt(std::vector<cc::Tri>& out, const double x, const double z0, const double z1, const double ylo, const double yhi, const double run)
    {
        out.push_back(tri({ x, ylo, z0 }, { x, ylo, z1 }, { x + run, yhi, z1 }));
        out.push_back(tri({ x, ylo, z0 }, { x + run, yhi, z1 }, { x + run, yhi, z0 }));
    }

    // Ground at y 0 up to x 1, then a face of height h at `deg` degrees (90: a riser), then ground at -h
    auto rise(const double h, const double deg) -> std::vector<cc::Tri>
    {
        std::vector<cc::Tri> out;
        const double         run = deg >= 90.0 ? 0.0 : h / std::tan(deg * 3.14159265358979323846 / 180.0);
        floorAt(out, -3, 1, -3, 3, 0.0);
        faceAt(out, 1, -3, 3, 0.0, -h, run);
        floorAt(out, 1 + run, 5, -3, 3, -h);
        return out;
    }

    // The feet after settling three frames, then pressing +x a walk's frame `presses` times
    auto pressed(const std::vector<cc::Tri>& tris, const cc::Vec& start, const int presses) -> std::pair<cc::Vec, bool>
    {
        cc::Player p;
        p.pos = start;
        for (int i = 0; i < 3; ++i)
        {
            p.frame(tris, 0.0, 0.0, 2.0);
        }
        for (int i = 0; i < presses; ++i)
        {
            p.frame(tris, 0.1667, 0.0, 2.0);
        }
        return { p.pos, p.grounded };
    }
} // namespace

TEST_CASE("Client collision: the port walks as tools/walls/clientsim.py walks", "[cardian][porter]")
{
    // The reference feet, from clientsim.py on the same scenes and frames
    // (the scratch script that made them pressed the same moves)
    struct Case
    {
        const char* what;
        double      h;
        double      deg;
        cc::Vec     start;
        int         presses;
        cc::Vec     feet;
    };
    const Case cases[] = {
        { "a step of 0.4, climbed", 0.4, 90.0, { 0.5, -0.2, 0.0 }, 8, { 1.8336, -0.4, 0.0 } },
        { "a step of 0.6, climbed: a floor within 0.3 of the centre is taken", 0.6, 90.0, { 0.5, -0.2, 0.0 }, 8, { 1.459828525, -0.6, 0.0 } },
        { "a bank of 62 degrees and 1.3, held at its foot", 1.3, 62.0, { 0.5, -0.2, 0.0 }, 12, { 0.622251865, 0.0, 0.0 } },
        { "a slope of 38 degrees and 1.3, climbed", 1.3, 38.0, { 0.5, -0.2, 0.0 }, 12, { 2.5004, -1.3, 0.0 } },
    };
    for (const auto& c : cases)
    {
        INFO(c.what);
        const auto [feet, grounded] = pressed(rise(c.h, c.deg), c.start, c.presses);
        CHECK(grounded);
        CHECK_THAT(feet[0], WithinAbs(c.feet[0], 1e-6));
        CHECK_THAT(feet[1], WithinAbs(c.feet[1], 1e-6));
        CHECK_THAT(feet[2], WithinAbs(c.feet[2], 1e-6));
    }

    // flat ground: on as far as it is pressed
    std::vector<cc::Tri> flat;
    floorAt(flat, -3, 3, -3, 3, 0.0);
    const auto [feet, grounded] = pressed(flat, { 0.0, -0.2, 0.0 }, 6);
    CHECK(grounded);
    CHECK_THAT(feet[0], WithinAbs(1.0002, 1e-6));
}

TEST_CASE("Client collision: a step across a navmesh's cells, judged by the client's walker", "[cardian][porter]")
{
    // a quarter of a yalm across ground, up a small step, up a steep bank
    std::vector<cc::Tri> flat;
    floorAt(flat, -3, 3, -3, 3, 0.0);
    CHECK(cc::walks(flat, { 0.0, -0.2, 0.0 }, { 0.25, 0.0, 0.0 }));

    const auto step = rise(0.4, 90.0);
    CHECK(cc::walks(step, { 0.9, -0.2, 0.0 }, { 1.2, -0.4, 0.0 }));
    CHECK(cc::walks(step, { 1.2, -0.6, 0.0 }, { 0.9, 0.0, 0.0 })); // and down

    const auto bank = rise(1.3, 62.0);
    // from the bank's foot to its middle, a quarter of a yalm in: held
    CHECK_FALSE(cc::walks(bank, { 0.9, -0.2, 0.0 }, { 1.15, -0.3, 0.0 }));
}

namespace
{
    // Drop links as the navmesh builder hands them over
    struct Links
    {
        std::vector<float>          verts;
        std::vector<float>          rad;
        std::vector<unsigned short> flags;
        std::vector<unsigned char>  areas;
        std::vector<unsigned char>  dir;
        int                         count{};

        // a link from s to e, both in FFXI's axes (y down), stored in Detour's
        void add(const cc::Vec& s, const cc::Vec& e)
        {
            verts.insert(verts.end(), { static_cast<float>(s[0]), static_cast<float>(-s[1]), static_cast<float>(-s[2]), static_cast<float>(e[0]),
                                        static_cast<float>(-e[1]), static_cast<float>(-e[2]) });
            rad.push_back(1.0f);
            flags.push_back(1);
            areas.push_back(63);
            dir.push_back(0);
            ++count;
        }
    };

    // Triangles as the navmesh builder gathers them: in Detour's space,
    // their corners the other way round from the client's
    void gathered(const std::vector<cc::Tri>& tris, std::vector<float>& verts, std::vector<int>& indices)
    {
        for (const auto& t : tris)
        {
            for (const int k : { 0, 2, 1 })
            {
                indices.push_back(static_cast<int>(verts.size() / 3));
                verts.insert(verts.end(), { static_cast<float>(t.v[k][0]), static_cast<float>(-t.v[k][1]), static_cast<float>(-t.v[k][2]) });
            }
        }
    }
} // namespace

TEST_CASE("Client navmesh: a drop link the client's walker makes is kept, one a tree's trunk blocks taken out", "[cardian][porter]")
{
    // An upper floor at y 0 to x 2, a face down to a lower floor at y 1.1
    // (y down), and on the lower floor at z 0 a trunk's face rising 4 yalms
    // across the way at x 3.5
    std::vector<cc::Tri> scene;
    floorAt(scene, -3, 2, -6, 6, 0.0);
    faceAt(scene, 2, -6, 6, 1.1, 0.0, 0.0);
    floorAt(scene, 2, 9, -6, 6, 1.1);
    faceAt(scene, 3.5, -0.4, 0.4, 1.1, -3.0, 0.0);
    std::vector<float> verts;
    std::vector<int>   indices;
    gathered(scene, verts, indices);

    // off the lip and down, clear of the trunk at z 4; then the same drop
    // with the trunk in the way, at z 0
    Links links;
    links.add({ 0.5, 0.0, 4.0 }, { 5.0, 1.1, 4.0 });
    links.add({ 0.5, 0.0, 0.0 }, { 5.0, 1.1, 0.0 });
    CHECK(clearDrops(links, verts, indices, scene.size(), true) == 1);
    REQUIRE(links.count == 1);
    CHECK_THAT(links.verts[2], WithinAbs(-4.0f, 0.001f)); // the one at z 4 (Detour's z is -z)
    CHECK(links.dir.size() == 1);

    // a landing close below the lip, 0.75 past it: the walker is pressed on
    // past the lip and comes down there
    Links close;
    close.add({ 1.25, 0.0, -3.0 }, { 2.75, 1.1, -3.0 });
    CHECK(clearDrops(close, verts, indices, scene.size(), true) == 0);
    CHECK(close.count == 1);

    // a triangle past the zone's own (a wall's) is not the client's
    Links past;
    past.add({ 0.5, 0.0, 0.0 }, { 5.0, 1.1, 0.0 });
    CHECK(clearDrops(past, verts, indices, scene.size() - 2, true) == 0);
    CHECK(past.count == 1);

    // any other build's links are left alone
    Links upstream;
    upstream.add({ 0.5, 0.0, 0.0 }, { 5.0, 1.1, 0.0 });
    CHECK(clearDrops(upstream, verts, indices, scene.size(), false) == 0);
    CHECK(upstream.count == 1);
}

TEST_CASE("Client navmesh: past the body's margin, the berth is kept and marked tight", "[cardian][porter]")
{
    // Two floors on a quarter-yalm grid: a field 4 yalms across (z 0 to 4)
    // and a bridge 1.5 across (z 6 to 7.5), each 6 long
    rcContext     ctx(false);
    rcHeightfield hf;
    const float   bmin[3]{ 0.0f, -1.0f, 0.0f };
    const float   bmax[3]{ 6.0f, 1.0f, 8.0f };
    REQUIRE(rcCreateHeightfield(&ctx, hf, 24, 32, bmin, bmax, kCellSize, kCellHeight));
    const auto floor = [&](const float z0, const float z1)
    {
        const float a[3]{ 0.01f, 0.0f, z0 + 0.01f }; // a hair inside the cells' lines, so no row past them takes a span
        const float b[3]{ 0.01f, 0.0f, z1 - 0.01f };
        const float c[3]{ 5.99f, 0.0f, z1 - 0.01f };
        const float d[3]{ 5.99f, 0.0f, z0 + 0.01f };
        REQUIRE(rcRasterizeTriangle(&ctx, a, b, c, RC_WALKABLE_AREA, hf));
        REQUIRE(rcRasterizeTriangle(&ctx, a, c, d, RC_WALKABLE_AREA, hf));
    };
    floor(0.0f, 4.0f);
    floor(6.0f, 7.5f);
    rcCompactHeightfield chf;
    REQUIRE(rcBuildCompactHeightfield(&ctx, 16, 4, hf, chf));
    std::vector<unsigned char> untouched(chf.areas, chf.areas + chf.spanCount);
    REQUIRE(erode(&ctx, 2, true, 1.0f, chf)); // the body's half yalm, a cell short of it, and the berth to a yalm

    // the area of the floor's cell at column x 12, row z
    const auto areaAt = [&](const int z) -> int
    {
        const rcCompactCell& cell = chf.cells[12 + z * chf.width];
        return cell.count == 0 ? -1 : chf.areas[cell.index];
    };
    // the field: its edge's cell gone, the rest of the yalm tight, open inside
    CHECK(areaAt(0) == RC_NULL_AREA);
    CHECK(areaAt(1) == kTightArea);
    CHECK(areaAt(3) == kTightArea);
    CHECK(areaAt(4) == RC_WALKABLE_AREA);
    CHECK(areaAt(11) == RC_WALKABLE_AREA);
    CHECK(areaAt(12) == kTightArea);
    CHECK(areaAt(14) == kTightArea);
    CHECK(areaAt(15) == RC_NULL_AREA);
    // the bridge, a yalm and a half across: its edges' cells gone, the rest kept, all tight
    CHECK(areaAt(24) == RC_NULL_AREA);
    CHECK(areaAt(25) == kTightArea);
    CHECK(areaAt(28) == kTightArea);
    CHECK(areaAt(29) == RC_NULL_AREA);
    CHECK(walkableArea(kTightArea));
    CHECK_FALSE(walkableArea(RC_NULL_AREA));

    // any other build: Recast's own erosion, nothing marked
    std::copy(untouched.begin(), untouched.end(), chf.areas);
    REQUIRE(erode(&ctx, 2, false, 1.0f, chf));
    CHECK(areaAt(1) == RC_NULL_AREA);
    CHECK(areaAt(2) == RC_WALKABLE_AREA);
    CHECK(areaAt(26) == RC_WALKABLE_AREA);
}

TEST_CASE("Client navmesh: a corridor between tall walls that the body passes stays open", "[cardian][porter]")
{
    // A floor 1.2 yalms across (x -0.6 to 0.6, z 0 to 6, y 0: y down) between
    // two walls 2 yalms tall, each facing in
    std::vector<cc::Tri> scene;
    floorAt(scene, -0.6, 0.6, 0, 6, 0.0);
    faceAt(scene, 0.6, 0, 6, 0.0, -2.0, 0.0);  // facing -x
    faceAt(scene, -0.6, 6, 0, 0.0, -2.0, 0.0); // facing +x
    std::vector<float> verts;
    std::vector<int>   indices;
    gathered(scene, verts, indices);

    // Recast's own steps: the floor walkable, the walls not, in Detour's space
    rcContext     ctx(false);
    rcHeightfield hf;
    const float   bmin[3]{ -2.0f, -1.0f, -7.0f };
    const float   bmax[3]{ 2.0f, 3.0f, 1.0f };
    REQUIRE(rcCreateHeightfield(&ctx, hf, 16, 32, bmin, bmax, kCellSize, kCellHeight));
    for (std::size_t t = 0; t < scene.size(); ++t)
    {
        const float* a    = &verts[static_cast<std::size_t>(indices[t * 3 + 0]) * 3];
        const float* b    = &verts[static_cast<std::size_t>(indices[t * 3 + 1]) * 3];
        const float* c    = &verts[static_cast<std::size_t>(indices[t * 3 + 2]) * 3];
        const bool   wall = t >= 2;
        REQUIRE(rcRasterizeTriangle(&ctx, a, b, c, wall ? RC_NULL_AREA : RC_WALKABLE_AREA, hf, 4));
    }
    rcCompactHeightfield chf;
    REQUIRE(rcBuildCompactHeightfield(&ctx, 16, 4, hf, chf));

    // the joins judged by the client, then the body's margin and the berth
    judgeJoins(chf, verts, indices, scene.size(), true);
    REQUIRE(erode(&ctx, 2, true, 1.0f, chf));

    // halfway along it (z 3, Detour's -3: row 16), its middle two cells still ground
    const auto areaAt = [&](const int x) -> int
    {
        const rcCompactCell& cell = chf.cells[x + 16 * chf.width];
        return cell.count == 0 ? -1 : chf.areas[cell.index];
    };
    CHECK(areaAt(7) == kTightArea); // x -0.25 to 0
    CHECK(areaAt(8) == kTightArea); // x 0 to 0.25
    CHECK(areaAt(6) == RC_NULL_AREA);
    CHECK(areaAt(9) == RC_NULL_AREA);
}

TEST_CASE("Client navmesh: a drop link is looked for square to its ledge's edge", "[cardian][porter]")
{
    // an edge along x; the polygon's centre off to its west, so centre to
    // middle points north-east
    const float a[3]{ 0.0f, 0.0f, 0.0f };
    const float b[3]{ 4.0f, 0.0f, 0.0f };
    float       outX = 1.0f;
    float       outZ = -1.0f;
    squareOut(true, a, b, outX, outZ);
    CHECK_THAT(outX, WithinAbs(0.0f, 1e-6f));
    CHECK(outZ < 0.0f); // square to it, on the side it pointed
    float upX = 1.0f;
    float upZ = -1.0f;
    squareOut(false, a, b, upX, upZ);
    CHECK(upX == 1.0f); // any other build's left as it was
}
