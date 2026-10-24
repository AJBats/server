/*
===========================================================================

  Cardian: the client's own collision judges a navmesh's joins and drops

===========================================================================
*/

#pragma once

// A navmesh built for the player's own client (client_navmesh.h) is Recast's
// cells, joined by Recast's rule: neighbouring floors within the climb of each
// other. The client judges by its own collision (client_collision.h), a
// sphere swept against each triangle, and the two disagree at a short, steep
// bank, which Recast's quarter-yalm cells climb a step at a time and the
// client calls a wall (#543). So, between Recast's cells and its polygons,
// every pair of neighbouring cells Recast joined where the two rules could
// differ -- floors more than a height step apart, and a client wall near them,
// tall enough to count from their floors -- stays joined only if the client's
// own walker steps from each to where the other's walker comes to rest; a
// pair on one floor, or on a gentle slope, keeps Recast's join untouched, the
// body's width beside a wall being the erosion's to keep (judgeJoins). A cut edge is then a
// border like any other: the body's margin is eroded from it, and where the
// client drops down it and cannot climb back, the builder's drop links go
// down it, one way. And each of those links stays only if the client's
// walker, walking off its start toward its landing, gets there (clearDrops):
// a link that crosses the hole the navmesh leaves round a tree's trunk, or
// passes it nearer than the body, is taken out (#548).
// Header-only: the navmesh builder (map core) calls it after building its
// compact heightfield, and again on each tile's drop links.

#include "client_collision.h"
#include "client_navmesh.h"
#include "navmesh_dump.h"

#include <Recast.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <optional>
#include <unordered_map>
#include <vector>

namespace cardian::client
{

namespace joins
{
    constexpr double kLift   = 0.2; // yalms: a walker is set down this far over a cell's floor, and falls to it
    constexpr double kGather = 2.0; // yalms round a pair whose triangles its walks are judged against
    constexpr double kNear   = 1.0; // yalms: a wall this near a pair makes it one to judge
    constexpr double kHead   = 2.0; // yalms over the feet a wall still meets the body
    constexpr double kCounts = 0.5; // yalms: a wall counts only rising further than this over the feet
    constexpr int    kBucket = 1;   // yalms: the side of the ground grid the triangles are sorted into
    constexpr double kLands      = 0.5; // yalms: a drop's walker that comes down this near its landing on the ground has made it ...
    constexpr double kPressTo    = 0.1; // ... pressed on until this near it, past the lip of a ledge whose landing lies close below
    constexpr int    kFallFrames = 90;  // frames at 30 a second a drop's walker is given to come down once over its landing
    constexpr double kOvershoot  = 0.7; // yalms: ... walked on past it at most this far while he stands on the lip: his body, half over the edge, rests on it

    inline auto bucketKey(const int bx, const int bz) -> int64_t
    {
        return (static_cast<int64_t>(bx) << 32) ^ static_cast<int64_t>(static_cast<uint32_t>(bz));
    }

    // A tile's gathered triangles as the client holds them, sorted into a
    // grid on the ground: `verts` and `indices` in Detour's space, the
    // first `count` the zone's own (the collision walls after them are never
    // the client's)
    struct ClientTris
    {
        std::vector<collision::Tri>                   tris;
        std::vector<bool>                             wall; // a normal's y at most the client's floor limit
        std::unordered_map<int64_t, std::vector<int>> grid;
        std::vector<int>                              stamp;
        int                                           stampNow = 0;

        ClientTris(const std::vector<float>& verts, const std::vector<int>& indices, const std::size_t count)
        {
            using namespace collision;
            // Back in FFXI's axes (Detour's is x, -y, -z: a turn, which keeps a
            // winding). The builder gathers each with its corners the other
            // way round from the client's (for Recast's winding: flipWinding,
            // set for every placement the client takes as stored), so they are
            // taken back in the client's turn, and the normal is the one the
            // client stores -- its floors face up, and its walls face the body
            tris.reserve(count);
            for (std::size_t t = 0; t < count && t * 3 + 2 < indices.size(); ++t)
            {
                Vec c[3];
                for (int k = 0; k < 3; ++k)
                {
                    const float* v = &verts[static_cast<std::size_t>(indices[t * 3 + k]) * 3];
                    c[k]           = { v[0], -v[1], -v[2] };
                }
                Tri tri;
                if (makeTri(c[0], c[2], c[1], tri))
                {
                    tris.push_back(tri);
                    wall.push_back(std::fabs(tri.n[1]) <= kFloorNy);
                }
            }
            // each in every bucket its box meets
            for (int i = 0; i < static_cast<int>(tris.size()); ++i)
            {
                const auto& v   = tris[i].v;
                const int   bx0 = static_cast<int>(std::floor(std::min({ v[0][0], v[1][0], v[2][0] }) / kBucket));
                const int   bx1 = static_cast<int>(std::floor(std::max({ v[0][0], v[1][0], v[2][0] }) / kBucket));
                const int   bz0 = static_cast<int>(std::floor(std::min({ v[0][2], v[1][2], v[2][2] }) / kBucket));
                const int   bz1 = static_cast<int>(std::floor(std::max({ v[0][2], v[1][2], v[2][2] }) / kBucket));
                for (int bx = bx0; bx <= bx1; ++bx)
                {
                    for (int bz = bz0; bz <= bz1; ++bz)
                    {
                        grid[bucketKey(bx, bz)].push_back(i);
                    }
                }
            }
            stamp.assign(tris.size(), 0);
        }

        // The triangles whose buckets meet the box (x0, z0) to (x1, z1) on the ground, each once
        void inBox(const double x0, const double z0, const double x1, const double z1, std::vector<int>& out)
        {
            ++stampNow;
            out.clear();
            for (int bx = static_cast<int>(std::floor(x0 / kBucket)); bx <= static_cast<int>(std::floor(x1 / kBucket)); ++bx)
            {
                for (int bz = static_cast<int>(std::floor(z0 / kBucket)); bz <= static_cast<int>(std::floor(z1 / kBucket)); ++bz)
                {
                    const auto it = grid.find(bucketKey(bx, bz));
                    if (it == grid.end())
                    {
                        continue;
                    }
                    for (const int i : it->second)
                    {
                        if (stamp[i] != stampNow)
                        {
                            stamp[i] = stampNow;
                            out.push_back(i);
                        }
                    }
                }
            }
        }

        // The triangles within `r` of (x, z) on the ground
        void around(const double x, const double z, const double r, std::vector<int>& out)
        {
            inBox(x - r, z - r, x + r, z + r, out);
        }
    };
} // namespace joins

// The joins of `chf` judged by the client's walker: `verts` and `indices`
// are the tile's gathered triangles in Detour's space, the first `count` the
// zone's own. Returns how many joins it cut; nothing is done unless `forClient`
inline auto judgeJoins(rcCompactHeightfield& chf, const std::vector<float>& verts, const std::vector<int>& indices, const std::size_t count, const bool forClient)
    -> int
{
    using namespace collision;
    if (!forClient)
    {
        return 0;
    }
    joins::ClientTris ct(verts, indices, count);

    // the development tool's dump (navmesh_dump.h): Recast's joins, the cuts and why
    std::optional<dump::Cuts> cuts;
    if (dump::holds(chf))
    {
        dump::stage("compact", chf);
        cuts.emplace(chf);
    }

    // A cell's floor, its feet in FFXI's axes
    const auto feetOf = [&](const int x, const int z, const rcCompactSpan& s) -> Vec
    {
        return { chf.bmin[0] + (x + 0.5) * chf.cs, -(chf.bmin[1] + s.y * chf.ch), -(chf.bmin[2] + (z + 0.5) * chf.cs) };
    };

    int              cut = 0;
    std::vector<int> nearby;
    std::vector<Tri> local;
    for (int z = 0; z < chf.height; ++z)
    {
        for (int x = 0; x < chf.width; ++x)
        {
            const rcCompactCell& cell = chf.cells[x + z * chf.width];
            for (int i = static_cast<int>(cell.index), ni = static_cast<int>(cell.index + cell.count); i < ni; ++i)
            {
                rcCompactSpan& s = chf.spans[i];
                if (chf.areas[i] == RC_NULL_AREA)
                {
                    continue;
                }
                for (const int dir : { 1, 2 }) // +z and +x: each pair once
                {
                    if (rcGetCon(s, dir) == RC_NOT_CONNECTED)
                    {
                        continue;
                    }
                    const int nx = x + rcGetDirOffsetX(dir);
                    const int nz = z + rcGetDirOffsetY(dir);
                    if (nx < 0 || nz < 0 || nx >= chf.width || nz >= chf.height)
                    {
                        continue;
                    }
                    const int      nIdx = static_cast<int>(chf.cells[nx + nz * chf.width].index) + rcGetCon(s, dir);
                    rcCompactSpan& ns   = chf.spans[nIdx];
                    const Vec      a    = feetOf(x, z, s);
                    const Vec      b    = feetOf(nx, nz, ns);
                    const double   mx   = (a[0] + b[0]) * 0.5;
                    const double   mz   = (a[2] + b[2]) * 0.5;

                    // floors a height step apart or less: one floor, or a slope
                    // the client walks, where the two rules agree but for the
                    // width -- a walker set down beside a wall is pushed off a
                    // cell's middle, and would cut joins the erosion keeps
                    if (std::fabs(a[1] - b[1]) <= kCellHeight + 1e-4)
                    {
                        continue;
                    }

                    // a client wall near, rising more than a step over the
                    // lower floor and reaching down past the higher one's head
                    // (y is down: the lower floor has the greater y)
                    const double lowFeet  = std::max(a[1], b[1]);
                    const double highFeet = std::min(a[1], b[1]);
                    ct.around(mx, mz, joins::kNear, nearby);
                    const bool judge = std::any_of(nearby.begin(), nearby.end(), [&](const int k)
                    {
                        if (!ct.wall[k])
                        {
                            return false;
                        }
                        const auto&  v   = ct.tris[k].v;
                        const double top = std::min({ v[0][1], v[1][1], v[2][1] });
                        const double bot = std::max({ v[0][1], v[1][1], v[2][1] });
                        return top < lowFeet - joins::kCounts && bot > highFeet - joins::kHead;
                    });
                    if (!judge)
                    {
                        continue;
                    }

                    ct.around(mx, mz, joins::kGather, nearby);
                    local.clear();
                    for (const int k : nearby)
                    {
                        local.push_back(ct.tris[k]);
                    }
                    // each walked to where the other's walker comes to rest: by a
                    // wall, that is off its cell's middle
                    const Vec  aLift{ a[0], a[1] - joins::kLift, a[2] };
                    const Vec  bLift{ b[0], b[1] - joins::kLift, b[2] };
                    const auto aRest = settle(local, aLift);
                    const auto bRest = settle(local, bLift);
                    if (aRest.has_value() && bRest.has_value() && walks(local, aLift, *bRest) && walks(local, bLift, *aRest))
                    {
                        continue;
                    }
                    if (cuts.has_value())
                    {
                        cuts->cut(x, z, static_cast<float>(a[1]), nx, nz, static_cast<float>(b[1]), dir, aRest.has_value() && bRest.has_value() ? "walk" : "settle");
                    }
                    rcSetCon(s, dir, RC_NOT_CONNECTED);
                    rcSetCon(ns, (dir + 2) & 3, RC_NOT_CONNECTED);
                    ++cut;
                }
            }
        }
    }
    if (cuts.has_value())
    {
        dump::stage("joins", chf);
    }
    return cut;
}

// The drop links of a navmesh built for the client (the builder's, one
// tile's: Detour's space, y up) whose way down the client's walker does not
// make, taken out: set down a little over the link's start and pressed
// straight toward its landing until within kPressTo of it, and on past it
// while he still stands on the lip (kOvershoot), he walks off the lip and
// falls, and must come down within kLands of the landing, and of how far he
// was pressed past it, on its floor. A trunk or a rock in the
// way stops him or turns him aside; a ledge's own face is under him. Any
// other build's links are left alone. Returns how many it took out
template <typename Links>
auto clearDrops(Links& links, const std::vector<float>& verts, const std::vector<int>& indices, const std::size_t count, const bool forClient) -> int
{
    using namespace collision;
    if (!forClient || links.count == 0)
    {
        return 0;
    }
    joins::ClientTris ct(verts, indices, count);

    int              kept = 0;
    std::vector<int> nearby;
    std::vector<Tri> local;
    for (int i = 0; i < links.count; ++i)
    {
        const float* l = &links.verts[static_cast<std::size_t>(i) * 6];
        const Vec    from{ l[0], -l[1] - joins::kLift, -l[2] };
        const Vec    to{ l[3], -l[4], -l[5] };
        ct.inBox(std::min(from[0], to[0]) - joins::kGather, std::min(from[2], to[2]) - joins::kGather, std::max(from[0], to[0]) + joins::kGather,
                 std::max(from[2], to[2]) + joins::kGather, nearby);
        local.clear();
        for (const int k : nearby)
        {
            local.push_back(ct.tris[k]);
        }
        if (!walks(local, from, to, joins::kLands, 0.6, 5.0, 30.0, joins::kFallFrames, joins::kPressTo, joins::kOvershoot))
        {
            continue;
        }

        if (kept != i)
        {
            std::copy_n(links.verts.begin() + i * 6, 6, links.verts.begin() + kept * 6);
            links.rad[kept]   = links.rad[i];
            links.flags[kept] = links.flags[i];
            links.areas[kept] = links.areas[i];
            links.dir[kept]   = links.dir[i];
        }
        ++kept;
    }

    const int removed = links.count - kept;
    links.count       = kept;
    links.verts.resize(static_cast<std::size_t>(kept) * 6);
    links.rad.resize(kept);
    links.flags.resize(kept);
    links.areas.resize(kept);
    links.dir.resize(kept);
    return removed;
}

} // namespace cardian::client
