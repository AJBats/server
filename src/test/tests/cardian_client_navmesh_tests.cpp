// Cardian: a navmesh built for the player's own client (pawn/client_navmesh.h)
// -- the chocobo porter's -- whose drop links are taken out where a body's
// way down meets the zone's collision: a tree's trunk in the hole the
// navmesh leaves round it, never a ledge's own face.
#include "pawn/client_navmesh.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <vector>

using namespace cardian::client;
using Catch::Matchers::WithinAbs;

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

        void add(const float* s, const float* e)
        {
            verts.insert(verts.end(), { s[0], s[1], s[2], e[0], e[1], e[2] });
            rad.push_back(1.0f);
            flags.push_back(1);
            areas.push_back(63);
            dir.push_back(0);
            ++count;
        }
    };
} // namespace

TEST_CASE("Client navmesh: a drop link through a tree's trunk is taken out, one off a ledge kept", "[cardian][porter]")
{
    // Detour's space, y up. A ledge's face at x 2, from the lip at height 0
    // down to the foot at -1.1; a trunk at x 2 on z 10, rising from -1.1 to 3
    const std::vector<float> verts{
        2, 0, -1, 2, 0, 1, 2, -1.1f, 0,      // the ledge's face, under the lip
        2, -1.1f, 9, 2, -1.1f, 11, 2, 3, 10, // the trunk
    };
    const std::vector<int> indices{ 0, 1, 2, 3, 4, 5 };

    // off the lip, 1.1 down, 4 yalms across: the face is under the way
    const float lip[3]{ 0, 0, 0 };
    const float foot[3]{ 4, -1.1f, 0 };
    CHECK_FALSE(dropBlocked(lip, foot, verts, indices, 2));

    // across the trunk's hole, the same drop: the trunk is in the way
    const float hole[3]{ 0, 0, 10 };
    const float beyond[3]{ 4, -1.1f, 10 };
    CHECK(dropBlocked(hole, beyond, verts, indices, 2));
    // a triangle past the zone's own (a wall's quad) is not looked at
    CHECK_FALSE(dropBlocked(hole, beyond, verts, indices, 1));

    // ground rising toward a lip at z 20, a yalm over two (some 27 degrees),
    // crosses the walk's height: ground is never in the way
    const std::vector<float> rise{ 0, 0, 19, 0, 0, 21, 2, 1, 20 };
    const std::vector<int>   one{ 0, 1, 2 };
    const float              back[3]{ 0, 0, 20 };
    const float              off[3]{ 4, -1.1f, 20 };
    CHECK_FALSE(dropBlocked(back, off, rise, one, 1));
    CHECK(flatish(&rise[0], &rise[3], &rise[6]));
    CHECK_FALSE(flatish(&verts[9], &verts[12], &verts[15])); // the trunk

    Links links;
    links.add(hole, beyond);
    links.add(lip, foot);
    clearDrops(links, verts, indices, 2, true);
    REQUIRE(links.count == 1);
    CHECK_THAT(links.verts[0], WithinAbs(0.0f, 0.001f));
    CHECK_THAT(links.verts[2], WithinAbs(0.0f, 0.001f)); // the ledge's, z 0
    CHECK(links.dir.size() == 1);

    // any other build's links are left alone
    Links upstream;
    upstream.add(hole, beyond);
    clearDrops(upstream, verts, indices, 2, false);
    CHECK(upstream.count == 1);
}
