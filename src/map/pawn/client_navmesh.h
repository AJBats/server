/*
===========================================================================

  Cardian: a navmesh built for the player's own client

===========================================================================
*/

#pragma once

// The zone's navmesh is built for the server's walkers -- monsters and
// cardians, whom the server moves and the client only draws -- and climbs a
// step of a yalm. A route the player's own client runs, as the chocobo
// porter's does (pawn/porter_mesh.h), must keep to what his client lets him
// do, and the client collides by its own rule (tools/walls/clientsim.py): a
// body a sphere half a yalm wide, floors up to 45 degrees, and anything lower
// than half a yalm stepped over. A navmesh built with those numbers keeps to
// that rule itself: Recast parts every ledge over half a yalm, and the
// builder's drop links, made from the climb, go down each one way. Recast's
// ledge filter stays off: it cuts a cell whose neighbours' floors spread by
// more than the climb, which under a climb this low cuts hillsides the client
// walks, and the body's margin already keeps a route off every real edge (the
// zone's border, a ledge's lip, is no join).
//
// The body's own margin, kBody, is all the navmesh takes off its edges --
// eroded a cell short of it, since Recast's cells already block that much
// (kTrimCells): a gap the client's body passes stays open, a bridge, a neck
// between rocks or a squeeze beside a log among them. A rider keeps a wider berth where there is room, as a
// person riding would -- trees and cliffs given a wide berth -- so the strip
// beyond the body's margin, out to the berth (the porter's width), is kept
// but marked tight (erode): its polygons carry kTightArea and kTightFlag,
// and the route search crosses them only where going round costs more
// (pawn/route_search.h, kTightCost), threading their middle. Open ground is
// kept only where Recast can make polygons of it (keepOpenWhole), and two
// regions' outlines keep the same points where they meet (matchOutlines),
// or the navmesh parts between them. A drop link is searched for straight
// out from its ledge's edge (squareOut).
// Header-only: the navmesh builder (map core) reads it, and xi_test links it.

#include "navmesh_dump.h"

#include <Recast.h>
#include <RecastAlloc.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <unordered_map>
#include <utility>
#include <vector>

namespace cardian::client
{

constexpr float kStep       = 0.5f;   // yalms: the client steps up this much; a ledge higher he drops off and never climbs
constexpr float kSlope      = 44.92f; // degrees: the steepest floor the client stands on, a normal's y above 0.708 (cos 44.92) -- a bank of 45 exactly is a wall
constexpr float kCellHeight = 0.125f; // yalms: the build's height steps, kStep a whole number of them, exactly
// yalms: the build's cells. Recast joins two cells whose floors differ by no
// more than the climb, so on a slope of 45 degrees a cell's rise to the next
// -- its side, plus a height step of rounding -- must stay under kStep: a
// quarter of a yalm keeps every slope the client walks whole, where the
// zone's half-yalm cells, under a half-yalm climb, cut a hillside of 38
constexpr float kCellSize = 0.25f;
constexpr float kBody     = 0.5f; // yalms: the client's body, a sphere this far round its centre: never nearer an edge than this

// The strip between the body's margin and the berth: Recast's area for it
// (walkable, and not RC_WALKABLE_AREA), and the flag its polygons carry
constexpr unsigned char  kTightArea = 62;
constexpr unsigned short kTightFlag = 0x20;

// Whether a polygon's area is ground: the open, or the tight strip
inline auto walkableArea(const unsigned char area) -> bool
{
    return area == RC_WALKABLE_AREA || area == kTightArea;
}

// cells: the body's margin is eroded this much short of it. Recast blocks
// every cell a collision triangle touches, as much as a cell nearer the
// triangle than the collision itself, so the cell short keeps the body's
// margin from the collision near enough, and opens the squeezes the client's
// body slides through (a strip of dirt beside a fallen log in Yuhtunga
// Jungle, which a full erosion pinched to a point)
constexpr int kTrimCells = 1;

// The open ground left where the berth fits, kept only where Recast can make
// polygons of it; the rest goes into the tight strip round it. Open and
// tight ground are never one region, so each open patch is a region of its
// own, and one Recast cannot outline -- a single cell, or a line one cell
// wide, which its outline's simplification takes down to fewer than three
// points -- makes no polygon and leaves a hole; and where such a patch
// touches the edge between two tight regions at a corner, the outline of the
// region round it bends there and the one across does not, so the two share
// no edge and their polygons are not joined (#561). So an open cell stays
// open only in a square of four open cells joined to each other, and an
// open patch smaller than `minOpenCells` -- Recast's merge size
// (navmesh_config.h mergeRegionArea), under which it merges a region into a
// neighbour of the same area -- goes too: too small to matter to a route, and
// too small to stand on its own. Only the tile's own cells count: the
// `borderSize` cells round them are the next tile's, Recast gives them
// regions of their own, and a square or a patch they complete is cut at the
// seam into one that does not stand; they are left as they are
inline void keepOpenWhole(rcCompactHeightfield& chf, const int borderSize, const int minOpenCells)
{
    const auto inTile = [&](const int c)
    {
        const int x = c % chf.width;
        const int z = c / chf.width;
        return x >= borderSize && x < chf.width - borderSize && z >= borderSize && z < chf.height - borderSize;
    };
    const auto neighbour = [&](const int x, const int z, const int i, const int dir) -> std::pair<int, int>
    {
        const rcCompactSpan& s = chf.spans[i];
        if (rcGetCon(s, dir) == RC_NOT_CONNECTED)
        {
            return { -1, -1 };
        }
        const int nx = x + rcGetDirOffsetX(dir);
        const int nz = z + rcGetDirOffsetY(dir);
        return { nx + nz * chf.width, static_cast<int>(chf.cells[nx + nz * chf.width].index) + rcGetCon(s, dir) };
    };
    std::vector<int> column(chf.spanCount, -1);
    for (int c = 0; c < chf.width * chf.height; ++c)
    {
        for (int i = static_cast<int>(chf.cells[c].index), ni = static_cast<int>(chf.cells[c].index + chf.cells[c].count); i < ni; ++i)
        {
            column[i] = c;
        }
    }
    // open ground of the tile's own
    const auto isOpen = [&](const int i)
    {
        return i >= 0 && chf.areas[i] == RC_WALKABLE_AREA && inTile(column[i]);
    };

    // in a square of four: from the span, one step along each of two
    // neighbouring directions, and the corner across from both ways
    std::vector<unsigned char> inSquare(chf.spanCount, 0);
    for (int i = 0; i < chf.spanCount; ++i)
    {
        if (!isOpen(i))
        {
            continue;
        }
        const int x = column[i] % chf.width;
        const int z = column[i] / chf.width;
        for (int d = 0; d < 4 && !inSquare[i]; ++d)
        {
            const int  e = (d + 1) & 3;
            const auto a = neighbour(x, z, i, d);
            const auto b = neighbour(x, z, i, e);
            if (!isOpen(a.second) || !isOpen(b.second))
            {
                continue;
            }
            const int c  = neighbour(a.first % chf.width, a.first / chf.width, a.second, e).second;
            const int c2 = neighbour(b.first % chf.width, b.first / chf.width, b.second, d).second;
            if (isOpen(c) && c == c2)
            {
                inSquare[i] = 1;
            }
        }
    }
    for (int i = 0; i < chf.spanCount; ++i)
    {
        if (isOpen(i) && !inSquare[i])
        {
            chf.areas[i] = kTightArea;
        }
    }

    // the patches left, each as a whole
    std::vector<int> patch(chf.spanCount, -1);
    std::vector<int> members;
    for (int i = 0; i < chf.spanCount; ++i)
    {
        if (!isOpen(i) || patch[i] >= 0)
        {
            continue;
        }
        members.clear();
        members.push_back(i);
        patch[i] = i;
        for (std::size_t k = 0; k < members.size(); ++k)
        {
            const int m = members[k];
            for (int dir = 0; dir < 4; ++dir)
            {
                const int n = neighbour(column[m] % chf.width, column[m] / chf.width, m, dir).second;
                if (isOpen(n) && patch[n] < 0)
                {
                    patch[n] = i;
                    members.push_back(n);
                }
            }
        }
        if (static_cast<int>(members.size()) < minOpenCells)
        {
            for (const int m : members)
            {
                chf.areas[m] = kTightArea;
            }
        }
    }
}

// The erosion of a navmesh built for the client: the body's margin,
// `bodyCells` (Recast's walkableRadius) less kTrimCells, and the berth past
// it, out to `berth` from the edges, kept and marked kTightArea, the open
// ground left kept only where Recast can make polygons of it (keepOpenWhole,
// given the tile's `borderSize` and Recast's `mergeRegionArea`). Recast
// measures each erosion from the edges as they stand, so a second erosion of
// the rest reaches the berth. Any other build: Recast's own erosion
inline auto erode(rcContext* ctx, const int bodyCells, const bool forClient, const float berth, const int borderSize, const int mergeRegionArea,
                  rcCompactHeightfield& chf) -> bool
{
    if (!forClient)
    {
        return rcErodeWalkableArea(ctx, bodyCells, chf);
    }
    const int body = std::max(0, bodyCells - kTrimCells);
    if (!rcErodeWalkableArea(ctx, body, chf))
    {
        return false;
    }
    const bool dumping = dump::holds(chf); // the development tool's (navmesh_dump.h)
    if (dumping)
    {
        dump::stage("body", chf);
    }
    const int berthCells = static_cast<int>(std::ceil(berth / chf.cs));
    if (berthCells <= body)
    {
        return true;
    }
    std::vector<unsigned char> kept(chf.areas, chf.areas + chf.spanCount);
    if (!rcErodeWalkableArea(ctx, berthCells - body, chf))
    {
        return false;
    }
    for (int i = 0; i < chf.spanCount; ++i)
    {
        if (chf.areas[i] == RC_NULL_AREA && kept[i] != RC_NULL_AREA)
        {
            chf.areas[i] = kTightArea;
        }
    }
    if (dumping)
    {
        dump::stage("berth", chf);
    }
    keepOpenWhole(chf, borderSize, mergeRegionArea);
    if (dumping)
    {
        dump::stage("open", chf);
    }
    return true;
}

// The outlines Recast traced round a tile's regions (rcBuildContours), made
// to agree where two regions meet. Recast keeps a point of a region's
// outline where the region across it changes, so where a third region
// touches the edge between two others at a single corner, the outline of the
// one that meets it there keeps that corner and the outline across does not:
// the two no longer share their edge point for point, rcBuildPolyMesh joins
// two polygons only along an edge both have, and the navmesh parts there
// (#561). So any point one outline keeps that lies on another region's
// traced boundary -- a corner of its own cells, the same spot within two
// height steps, as rcBuildPolyMesh joins two points -- is kept by that
// outline too, in its place along it, until no outline gains another. A
// point of a traced boundary moves no outline off its own cells; an outline
// that would cross itself all the same is kept as it was, and said in the
// map's log, as is one whose points cannot be laid along its boundaries.
// Any other build's outlines are left as Recast traced them. Returns how
// many points were added.
inline auto matchOutlines(rcContourSet& cset, const bool forClient) -> int
{
    if (!forClient)
    {
        return 0;
    }
    const auto sameColumn = [](const int* a, const int* b)
    {
        return a[0] == b[0] && a[2] == b[2];
    };
    const auto columnOf = [](const int* p)
    {
        return (static_cast<int64_t>(p[0]) << 32) | static_cast<uint32_t>(p[2]);
    };
    // a point in yalms, for the map's log: x and z in FFXI's axes
    const auto where = [&](const int* p)
    {
        return std::make_pair(cset.bmin[0] + p[0] * cset.cs, -(cset.bmin[2] + p[2] * cset.cs));
    };

    // The boundaries an outline's points lie on: its own, and those of the
    // holes Recast joined into it (rcBuildContours' mergeRegionHoles: the
    // outline runs round each hole between two bridges, and the hole's
    // contour keeps its traced boundary and no points)
    const auto loopsOf = [&](const int c)
    {
        std::vector<const rcContour*> loops{ &cset.conts[c] };
        for (int h = 0; h < cset.nconts; ++h)
        {
            if (h != c && cset.conts[h].reg == cset.conts[c].reg && cset.conts[h].nverts == 0 && cset.conts[h].nrverts > 0)
            {
                loops.push_back(&cset.conts[h]);
            }
        }
        return loops;
    };
    // Where each of an outline's points lies: on which boundary, and where
    // along it, in the boundary's order, a bridge's two ends on different
    // boundaries; each boundary walked round exactly once. Empty when they
    // cannot be laid so
    struct Place
    {
        int loop  = 0;
        int index = 0;
    };
    const auto placesOf = [](const rcContour& c, const std::vector<const rcContour*>& loops) -> std::vector<Place>
    {
        std::vector<std::vector<Place>> candidates(c.nverts);
        for (int v = 0; v < c.nverts; ++v)
        {
            const int* q = &c.verts[v * 4];
            for (int l = 0; l < static_cast<int>(loops.size()); ++l)
            {
                for (int r = 0; r < loops[l]->nrverts; ++r)
                {
                    const int* p = &loops[l]->rverts[r * 4];
                    if (p[0] == q[0] && p[1] == q[1] && p[2] == q[2])
                    {
                        candidates[v].push_back({ l, r });
                    }
                }
            }
            if (candidates[v].empty())
            {
                return {};
            }
        }
        const auto ahead = [&](const Place& a, const Place& b)
        {
            const int n = loops[a.loop]->nrverts;
            return (b.index - a.index + n) % n;
        };
        // the next place along the same boundary, nearest ahead; else a bridge
        const auto step = [&](const Place& a, const Place& b)
        {
            const int d = ahead(a, b);
            return d == 0 ? loops[a.loop]->nrverts : d;
        };
        for (const Place& start : candidates[0])
        {
            std::vector<Place> places{ start };
            for (int v = 1; v < c.nverts; ++v)
            {
                const Place& last = places.back();
                const Place* best = nullptr;
                for (const Place& p : candidates[v])
                {
                    const bool along = p.loop == last.loop;
                    if (best == nullptr || (along && (best->loop != last.loop || step(last, p) < step(last, *best))))
                    {
                        best = &p;
                    }
                }
                places.push_back(*best);
            }
            // each boundary used walked round once, no more and no less
            std::vector<int> walked(loops.size(), 0);
            std::vector<int> used(loops.size(), 0);
            for (int v = 0; v < c.nverts; ++v)
            {
                const Place& a = places[v];
                const Place& b = places[(v + 1) % c.nverts];
                used[a.loop] = 1;
                if (a.loop == b.loop)
                {
                    walked[a.loop] += ahead(a, b);
                }
            }
            bool once = true;
            for (std::size_t l = 0; l < loops.size(); ++l)
            {
                once = once && (!used[l] || walked[l] == loops[l]->nrverts);
            }
            if (once)
            {
                return places;
            }
        }
        return {};
    };
    // Whether two segments cross, each through the other's inside, on the ground
    const auto crosses = [](const int* a, const int* b, const int* c, const int* d)
    {
        const auto side = [](const int* p, const int* q, const int* r)
        {
            const int64_t s = static_cast<int64_t>(q[0] - p[0]) * (r[2] - p[2]) - static_cast<int64_t>(q[2] - p[2]) * (r[0] - p[0]);
            return s > 0 ? 1 : s < 0 ? -1 : 0;
        };
        return side(a, b, c) * side(a, b, d) < 0 && side(c, d, a) * side(c, d, b) < 0;
    };

    int               added = 0;
    std::vector<char> said(cset.nconts, 0); // an outline the map's log has spoken of, once
    for (bool again = true; again;)
    {
        again = false;
        // every kept point, by its column, with the outline keeping it
        std::unordered_map<int64_t, std::vector<std::pair<int, const int*>>> kept;
        for (int c = 0; c < cset.nconts; ++c)
        {
            for (int v = 0; v < cset.conts[c].nverts; ++v)
            {
                const int* p = &cset.conts[c].verts[v * 4];
                kept[columnOf(p)].emplace_back(c, p);
            }
        }
        const auto keptElsewhere = [&](const int c, const int* p)
        {
            const auto it = kept.find(columnOf(p));
            return it != kept.end() && std::any_of(it->second.begin(), it->second.end(), [&](const auto& k)
            {
                return k.first != c && std::abs(k.second[1] - p[1]) <= 2;
            });
        };

        std::vector<std::pair<int, std::vector<int>>> grown; // outline -> its new points, 4 ints each
        for (int c = 0; c < cset.nconts; ++c)
        {
            const rcContour& cont = cset.conts[c];
            if (cont.nverts < 3 || cont.nrverts == 0)
            {
                continue;
            }
            if (said[c])
            {
                continue;
            }
            const auto loops  = loopsOf(c);
            const auto places = placesOf(cont, loops);
            if (places.empty())
            {
                // said only where it leaves out a point another outline keeps
                const int* missing = nullptr;
                for (const rcContour* loop : loops)
                {
                    for (int r = 0; r < loop->nrverts && missing == nullptr; ++r)
                    {
                        const int* p        = &loop->rverts[r * 4];
                        const auto own      = kept.find(columnOf(p));
                        const bool ownKeeps = own != kept.end() && std::any_of(own->second.begin(), own->second.end(), [&](const auto& k) { return k.first == c; });
                        if (keptElsewhere(c, p) && !ownKeeps)
                        {
                            missing = p;
                        }
                    }
                }
                if (missing != nullptr)
                {
                    const auto [x, z] = where(missing);
                    ShowWarningFmt("navmesh: the outline of region {} cannot be laid along its boundaries; ({:.2f}, {:.2f}), which the region across keeps, is left out of it", cont.reg, x, z);
                    said[c] = 1;
                }
                continue;
            }
            std::vector<int> verts;
            std::vector<int> fresh; // the points added, by their index in verts
            for (int v = 0; v < cont.nverts; ++v)
            {
                verts.insert(verts.end(), &cont.verts[v * 4], &cont.verts[v * 4 + 4]);
                const Place& from = places[v];
                const Place& to   = places[(v + 1) % cont.nverts];
                if (from.loop != to.loop)
                {
                    continue; // a bridge between two boundaries
                }
                const rcContour& loop = *loops[from.loop];
                for (int r = (from.index + 1) % loop.nrverts; r != to.index && r != from.index; r = (r + 1) % loop.nrverts)
                {
                    const int* p = &loop.rverts[r * 4];
                    if (keptElsewhere(c, p) && !sameColumn(p, &verts[verts.size() - 4]) && !sameColumn(p, &cont.verts[((v + 1) % cont.nverts) * 4]))
                    {
                        // the edge's region from the kept point before it; the
                        // tile border's mark, by which rcBuildPolyMesh lets go of
                        // a point, its own
                        fresh.push_back(static_cast<int>(verts.size() / 4));
                        verts.insert(verts.end(), { p[0], p[1], p[2], (cont.verts[v * 4 + 3] & (RC_CONTOUR_REG_MASK | RC_AREA_BORDER)) | (p[3] & RC_BORDER_VERTEX) });
                    }
                }
            }
            if (fresh.empty())
            {
                continue;
            }
            // no edge to or from a point added crossing another edge of the
            // outline (edge i runs from point i to the next)
            const int  n       = static_cast<int>(verts.size() / 4);
            const auto wrap    = [n](const int i) { return ((i % n) + n) % n; };
            const auto point   = [&](const int i) { return &verts[wrap(i) * 4]; };
            bool       crossed = false;
            for (const int f : fresh)
            {
                for (const int e : { wrap(f - 1), f })
                {
                    for (int k = 0; k < n && !crossed; ++k)
                    {
                        if (k == e || k == wrap(e - 1) || k == wrap(e + 1))
                        {
                            continue; // the edge itself, or one sharing a point with it
                        }
                        crossed = crosses(point(e), point(e + 1), point(k), point(k + 1));
                    }
                }
            }
            if (crossed)
            {
                const auto [x, z] = where(point(fresh.front()));
                ShowWarningFmt("navmesh: the outline of region {} is kept as Recast traced it: a point the region across keeps, at ({:.2f}, {:.2f}), would make it cross itself", cont.reg, x, z);
                said[c] = 1;
                continue;
            }
            grown.emplace_back(c, std::move(verts));
        }
        for (auto& [c, verts] : grown)
        {
            rcContour& cont = cset.conts[c];
            auto*      next = static_cast<int*>(rcAlloc(sizeof(int) * verts.size(), RC_ALLOC_PERM));
            if (next == nullptr)
            {
                continue;
            }
            std::copy(verts.begin(), verts.end(), next);
            added += static_cast<int>(verts.size() / 4) - cont.nverts;
            rcFree(cont.verts);
            cont.verts  = next;
            cont.nverts = static_cast<int>(verts.size() / 4);
            again       = true;
        }
    }
    return added;
}

// A drop link's way out from a ledge's edge a to b, for the client: square
// to the edge, on the side `outX`, `outZ` (from the polygon's centre to the
// edge's middle) points -- which, for a long, thin polygon whose edge lies
// off to one side, can stray far from straight out, and miss the ground
// below. Any other build's is left as it is
inline void squareOut(const bool forClient, const float* a, const float* b, float& outX, float& outZ)
{
    if (!forClient)
    {
        return;
    }
    float nx = b[2] - a[2];
    float nz = -(b[0] - a[0]);
    if (nx * outX + nz * outZ < 0.0f)
    {
        nx = -nx;
        nz = -nz;
    }
    outX = nx;
    outZ = nz;
}

} // namespace cardian::client
