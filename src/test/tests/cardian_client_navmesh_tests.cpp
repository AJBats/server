// Cardian: a navmesh built for the player's own client (pawn/client_navmesh.h)
// -- the chocobo porter's: the client's collision ported (client_collision.h),
// its walker judging a navmesh's joins and its drop links (client_joins.h) --
// a drop link off a ledge kept, one whose way down a tree's trunk blocks
// taken out -- and the strip past the body's margin, out to the rider's
// berth, kept and marked tight (erode), with open ground too thin or too
// small to stand as a region of its own given to it (keepOpenWhole).
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

    // a landing within a body of the lip, half a yalm past it: his body,
    // half over the edge, rests on it until he walks on past the landing
    // (Palborough Mines' refiner shelf, whose drops were all taken out)
    Links edge;
    edge.add({ 1.0, 0.0, -3.0 }, { 2.5, 1.1, -3.0 });
    CHECK(clearDrops(edge, verts, indices, scene.size(), true) == 0);
    CHECK(edge.count == 1);

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
    REQUIRE(erode(&ctx, 2, true, 1.0f, 0, 20, chf)); // the body's half yalm, a cell short of it, and the berth to a yalm

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
    REQUIRE(erode(&ctx, 2, false, 1.0f, 0, 20, chf));
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
    REQUIRE(erode(&ctx, 2, true, 1.0f, 0, 20, chf));

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

namespace
{
    // A level floor of `side` by `side` quarter-yalm cells, every cell one
    // span joined to its neighbours, all of it the tight strip
    struct TightField
    {
        rcContext            ctx{ false };
        rcCompactHeightfield chf;

        explicit TightField(const int side)
        {
            rcHeightfield hf;
            const float   span = side * kCellSize;
            const float   bmin[3]{ 0.0f, -1.0f, 0.0f };
            const float   bmax[3]{ span, 1.0f, span };
            REQUIRE(rcCreateHeightfield(&ctx, hf, side, side, bmin, bmax, kCellSize, kCellHeight));
            const float a[3]{ 0.01f, 0.0f, 0.01f };
            const float b[3]{ 0.01f, 0.0f, span - 0.01f };
            const float c[3]{ span - 0.01f, 0.0f, span - 0.01f };
            const float d[3]{ span - 0.01f, 0.0f, 0.01f };
            REQUIRE(rcRasterizeTriangle(&ctx, a, b, c, RC_WALKABLE_AREA, hf));
            REQUIRE(rcRasterizeTriangle(&ctx, a, c, d, RC_WALKABLE_AREA, hf));
            REQUIRE(rcBuildCompactHeightfield(&ctx, 16, 4, hf, chf));
            std::fill(chf.areas, chf.areas + chf.spanCount, kTightArea);
        }

        auto span(const int x, const int z) -> int
        {
            const rcCompactCell& cell = chf.cells[x + z * chf.width];
            REQUIRE(cell.count == 1);
            return static_cast<int>(cell.index);
        }

        // the cells x0..x1, z0..z1 (inclusive) made open ground
        void open(const int x0, const int x1, const int z0, const int z1)
        {
            for (int z = z0; z <= z1; ++z)
            {
                for (int x = x0; x <= x1; ++x)
                {
                    chf.areas[span(x, z)] = RC_WALKABLE_AREA;
                }
            }
        }

        auto area(const int x, const int z) -> int
        {
            return chf.areas[span(x, z)];
        }
    };
} // namespace

TEST_CASE("Client navmesh: open ground too thin or too small for a region goes into the tight strip", "[cardian][porter]")
{
    SECTION("a single open cell amid the tight strip")
    {
        TightField f(24);
        f.open(12, 12, 12, 12);
        keepOpenWhole(f.chf, 0, 20);
        CHECK(f.area(12, 12) == kTightArea);
    }
    SECTION("a line one cell wide, however long")
    {
        TightField f(24);
        f.open(2, 21, 12, 12);
        keepOpenWhole(f.chf, 0, 20);
        CHECK(f.area(2, 12) == kTightArea);
        CHECK(f.area(12, 12) == kTightArea);
        CHECK(f.area(21, 12) == kTightArea);
    }
    SECTION("a square of four alone is under the merge size; a patch of twenty-five stays")
    {
        TightField f(24);
        f.open(2, 3, 2, 3);
        f.open(10, 14, 10, 14);
        keepOpenWhole(f.chf, 0, 20);
        CHECK(f.area(2, 2) == kTightArea);
        CHECK(f.area(3, 3) == kTightArea);
        CHECK(f.area(10, 10) == RC_WALKABLE_AREA);
        CHECK(f.area(14, 14) == RC_WALKABLE_AREA);
        CHECK(f.area(12, 12) == RC_WALKABLE_AREA);
        CHECK(f.area(9, 12) == kTightArea); // the tight strip round it untouched
    }
    SECTION("a strip two cells wide stands, until the joins between its rows are cut")
    {
        TightField f(24);
        f.open(2, 21, 11, 12);
        TightField g(24);
        g.open(2, 21, 11, 12);
        for (int x = 2; x <= 21; ++x)
        {
            auto& lower = g.chf.spans[g.span(x, 11)];
            auto& upper = g.chf.spans[g.span(x, 12)];
            rcSetCon(lower, 1, RC_NOT_CONNECTED); // +z, as the client's walker cuts a join (client_joins.h)
            rcSetCon(upper, 3, RC_NOT_CONNECTED);
        }
        keepOpenWhole(f.chf, 0, 20);
        keepOpenWhole(g.chf, 0, 20);
        CHECK(f.area(2, 11) == RC_WALKABLE_AREA);
        CHECK(f.area(21, 12) == RC_WALKABLE_AREA);
        CHECK(g.area(2, 11) == kTightArea);
        CHECK(g.area(21, 12) == kTightArea);
    }
    SECTION("nothing but open ground changes")
    {
        TightField f(24);
        f.open(10, 14, 10, 14);
        f.chf.areas[f.span(0, 0)] = RC_NULL_AREA;
        keepOpenWhole(f.chf, 0, 20);
        CHECK(f.area(0, 0) == RC_NULL_AREA);
        CHECK(f.area(5, 5) == kTightArea);
    }
    SECTION("the next tile's cells round the tile complete no square and count to no patch")
    {
        // a border of 2 cells: the tile is cells 2 to 21. Open ground two
        // rows deep from the border into the tile's first row; and a patch
        // of 25 whose first rows lie in the border
        TightField f(24);
        f.open(4, 19, 0, 2);
        f.open(8, 12, 21, 23); // rows 21 the tile's, 22 and 23 the border's: 5 of the tile's own
        keepOpenWhole(f.chf, 2, 20);
        CHECK(f.area(10, 2) == kTightArea);       // the tile's row alone is a line one cell wide
        CHECK(f.area(10, 1) == RC_WALKABLE_AREA); // the border's left as it was
        CHECK(f.area(10, 21) == kTightArea);      // five of the tile's own, under the merge size
        CHECK(f.area(10, 23) == RC_WALKABLE_AREA);
    }
}

TEST_CASE("Client navmesh: two regions' outlines keep the same points along the edge they share", "[cardian][porter]")
{
    // Two regions side by side, 1 at x 0 to 4 and 2 at x 4 to 8 (z 0 to 4),
    // their traced boundaries on the cells' corners. Region 1's outline keeps
    // a point halfway along the edge they share (x 4, z 2), where a third
    // region touches it at a corner; region 2's runs straight past it
    const auto alloc = [](const std::vector<int>& v)
    {
        auto* out = static_cast<int*>(rcAlloc(sizeof(int) * v.size(), RC_ALLOC_PERM));
        std::copy(v.begin(), v.end(), out);
        return out;
    };
    const auto pts = [](const std::vector<std::pair<int, int>>& xz, const int reg)
    {
        std::vector<int> out;
        for (const auto& [x, z] : xz)
        {
            out.insert(out.end(), { x, 0, z, reg });
        }
        return out;
    };
    std::vector<std::pair<int, int>> raw2;
    for (int x = 4; x <= 8; ++x) raw2.emplace_back(x, 0);
    for (int z = 1; z <= 4; ++z) raw2.emplace_back(8, z);
    for (int x = 7; x >= 4; --x) raw2.emplace_back(x, 4);
    for (int z = 3; z >= 1; --z) raw2.emplace_back(4, z);
    std::vector<std::pair<int, int>> raw1;
    for (int z = 0; z <= 4; ++z) raw1.emplace_back(4, z);
    for (int x = 3; x >= 0; --x) raw1.emplace_back(x, 4);
    for (int z = 3; z >= 0; --z) raw1.emplace_back(0, z);
    for (int x = 1; x <= 3; ++x) raw1.emplace_back(x, 0);

    rcContourSet cset;
    cset.nconts = 2;
    cset.conts  = static_cast<rcContour*>(rcAlloc(sizeof(rcContour) * 2, RC_ALLOC_PERM));
    const auto set = [&](rcContour& c, const std::vector<int>& raw, const std::vector<int>& kept, const unsigned short reg)
    {
        c.rverts  = alloc(raw);
        c.nrverts = static_cast<int>(raw.size() / 4);
        c.verts   = alloc(kept);
        c.nverts  = static_cast<int>(kept.size() / 4);
        c.reg     = reg;
        c.area    = RC_WALKABLE_AREA;
    };
    set(cset.conts[0], pts(raw1, 2), pts({ { 4, 0 }, { 4, 2 }, { 4, 4 }, { 0, 4 }, { 0, 0 } }, 2), 1);
    set(cset.conts[1], pts(raw2, 1), pts({ { 4, 0 }, { 8, 0 }, { 8, 4 }, { 4, 4 } }, 1), 2);

    CHECK(matchOutlines(cset, false) == 0); // any other build's left as Recast traced them
    CHECK(cset.conts[1].nverts == 4);
    CHECK(matchOutlines(cset, true) == 1);
    REQUIRE(cset.conts[1].nverts == 5);
    // region 2 now goes (4,4) -> (4,2) -> (4,0), as region 1 does the other way
    const int* added = &cset.conts[1].verts[4 * 4];
    CHECK(added[0] == 4);
    CHECK(added[2] == 2);
    CHECK(cset.conts[0].nverts == 5); // region 1 unchanged
    CHECK(matchOutlines(cset, true) == 0);
}

TEST_CASE("Client navmesh: where a third region meets two at a corner, their polygons still join", "[cardian][porter]")
{
    // Region 1 (the tight strip) fills x 0 to 7, z 0 to 7; region 3 (open
    // ground) the two by two cells beyond its far corner, x 8 to 9, z 8 to 9;
    // region 2 (the tight strip too) the rest, round both. Region 2's outline
    // meets region 3 at region 1's corner (8, 8) and keeps it; region 1's
    // meets only region 2 along that edge and runs straight past it
    const auto build = [](const bool matched, rcPolyMesh& mesh)
    {
        TightField f(16);
        for (int z = 0; z < 16; ++z)
        {
            for (int x = 0; x < 16; ++x)
            {
                const int  i     = f.span(x, z);
                const bool one   = x < 8 && z < 8;
                const bool three = x >= 8 && x < 10 && z >= 8 && z < 10;
                f.chf.spans[i].reg = static_cast<unsigned short>(one ? 1 : three ? 3 : 2);
                f.chf.areas[i]     = three ? RC_WALKABLE_AREA : kTightArea;
            }
        }
        f.chf.maxRegions = 4;
        rcContourSet cset;
        REQUIRE(rcBuildContours(&f.ctx, f.chf, 1.3f, 12, cset));
        CHECK(matchOutlines(cset, matched) >= 0);
        REQUIRE(rcBuildPolyMesh(&f.ctx, cset, 6, mesh));
    };
    // whether a polygon of region 1 has one of region 2 across an edge
    const auto joined = [](const rcPolyMesh& mesh)
    {
        for (int p = 0; p < mesh.npolys; ++p)
        {
            if (mesh.regs[p] != 1)
            {
                continue;
            }
            const unsigned short* poly = &mesh.polys[p * mesh.nvp * 2];
            for (int j = 0; j < mesh.nvp && poly[j] != RC_MESH_NULL_IDX; ++j)
            {
                const unsigned short across = poly[mesh.nvp + j];
                if (across != RC_MESH_NULL_IDX && (across & 0x8000) == 0 && mesh.regs[across] == 2)
                {
                    return true;
                }
            }
        }
        return false;
    };
    rcPolyMesh traced;
    build(false, traced);
    CHECK_FALSE(joined(traced)); // as Recast traced them, the navmesh parts there
    rcPolyMesh matched;
    build(true, matched);
    CHECK(joined(matched));
}
