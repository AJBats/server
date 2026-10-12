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

#pragma once

#include "common/mmo.h"

#include <DetourNavMesh.h>
#include <DetourNavMeshQuery.h>

#include <algorithm>
#include <array>
#include <cfloat>
#include <cmath>
#include <cstdint>
#include <functional>
#include <limits>
#include <memory>
#include <optional>
#include <queue>
#include <unordered_map>
#include <utility>
#include <vector>

// A route across a whole zone, searched in one go (the travel walk). The
// game's own path query expands at most 2,048 polygons, and on the fine
// meshes built from a zone's collision a long walk can need more: Detour
// then answers with a partial path to the polygon it reached nearest the
// goal as the crow flies, which can be a dead end behind a wall. This search
// runs with a node pool large enough for any zone, so it either reaches the
// goal or proves that no walkable way joins the two points; a trek walks it
// where the game's own query cannot reach her zone line.
namespace cardian::route
{
    // Detour's polygon flags, as the game's navmeshes are built (RecastDemo's)
    constexpr unsigned short kPolyDisabled = 0x10;

    constexpr int kSearchNodes     = 65535; // Detour's largest node pool
    constexpr int kCorridorPolys   = 16384;
    constexpr int kStraightPoints  = 4096;
    constexpr float kPickExtents[] = { 2.5f, 5.0f, 2.5f };

    struct Route
    {
        std::vector<position_t> points;            // waypoints after the start, the last the goal or the nearest point reached
        bool                    complete{};        // the goal reached: false, the two points are not joined by walkable mesh
        float                   length{};          // along the waypoints, in yalms
        int                     polys{};           // the corridor's polygons
        int                     firstDanger{ -1 }; // kept out of danger: the first waypoint whose step to it passes through danger, -1 when none does
        int                     searched{};        // kept out of danger: the points its search took up
        int                     links{};           // kept out of danger: the off-mesh links it crosses
    };

    // FFXI is left-handed Y-up and Detour right-handed Y-up: Y and Z flip
    inline auto toDetour(const position_t& p) -> std::array<float, 3>
    {
        return { p.x, -p.y, -p.z };
    }

    inline auto fromDetour(const float* p) -> position_t
    {
        return { p[0], -p[1], -p[2], 0, 0 };
    }

    // The route from `from` to `to` over `mesh`, or none when either point is
    // off the mesh: each is looked for within its extents (a zone line's
    // trigger often lies some yalms past the mesh's edge)
    inline auto search(const dtNavMesh& mesh, const position_t& from, const position_t& to, const float* fromExtents = kPickExtents,
                       const float* toExtents = kPickExtents) -> std::optional<Route>
    {
        auto query = std::unique_ptr<dtNavMeshQuery, decltype(&dtFreeNavMeshQuery)>(dtAllocNavMeshQuery(), &dtFreeNavMeshQuery);
        if (!query || dtStatusFailed(query->init(&mesh, kSearchNodes)))
        {
            return std::nullopt;
        }
        dtQueryFilter filter;
        filter.setIncludeFlags(0xFFFF);
        filter.setExcludeFlags(kPolyDisabled);

        const auto start = toDetour(from);
        const auto goal  = toDetour(to);
        dtPolyRef  startRef{};
        dtPolyRef  goalRef{};
        float      startAt[3]{};
        float      goalAt[3]{};
        if (dtStatusFailed(query->findNearestPoly(start.data(), fromExtents, &filter, &startRef, startAt)) || !startRef ||
            dtStatusFailed(query->findNearestPoly(goal.data(), toExtents, &filter, &goalRef, goalAt)) || !goalRef)
        {
            return std::nullopt;
        }

        std::vector<dtPolyRef> corridor(kCorridorPolys);
        int                    corridorCount = 0;
        const dtStatus         found         = query->findPath(startRef, goalRef, startAt, goalAt, &filter, corridor.data(), &corridorCount, kCorridorPolys);
        if (dtStatusFailed(found) || corridorCount == 0)
        {
            return std::nullopt;
        }

        // a partial corridor ends on the polygon nearest the goal: its own
        // nearest point is where the walk would stop
        float end[3]{ goalAt[0], goalAt[1], goalAt[2] };
        if (corridor[corridorCount - 1] != goalRef)
        {
            query->closestPointOnPoly(corridor[corridorCount - 1], goalAt, end, nullptr);
        }

        std::vector<float> straight(kStraightPoints * 3);
        int                straightCount = 0;
        if (dtStatusFailed(query->findStraightPath(startAt, end, corridor.data(), corridorCount, straight.data(), nullptr, nullptr, &straightCount, kStraightPoints)))
        {
            return std::nullopt;
        }

        Route route;
        route.complete = corridor[corridorCount - 1] == goalRef;
        route.polys    = corridorCount;
        position_t last = fromDetour(startAt);
        for (int i = 1; i < straightCount; ++i)
        {
            const auto point = fromDetour(&straight[i * 3]);
            route.length += std::sqrt((point.x - last.x) * (point.x - last.x) + (point.y - last.y) * (point.y - last.y) + (point.z - last.z) * (point.z - last.z));
            route.points.push_back(point);
            last = point;
        }
        return route;
    }

    // A danger a route keeps out of: the sphere round a mob that would come
    // for her (pawn/pawn_danger.h spheres), and the smaller one it notices
    // her in without sight (0 when none)
    struct Hazard
    {
        position_t at{};
        float      radius = 0.0f;
        float      unseen = 0.0f;
    };

    // Whether a hazard's mob sees a point: the game's line of sight, which a
    // wall cuts. Empty, every mob sees every point
    using HazardSight = std::function<bool(size_t hazard, const position_t& point)>;

    constexpr float kPortalSpacing = 3.0f;    // yalms between the points tried along a polygon's edge
    constexpr float kDangerCost    = 5000.0f; // a step through danger, in yalms of walk: any way round shorter than this is taken
    constexpr float kShortcutReach = 60.0f;   // the longest straight a route is drawn tight over
    constexpr int   kAvoidNodes    = 400000;  // the search's bound, past every zone's edge points
    constexpr int   kRayPolys      = 1024;

    namespace detail
    {
        using Vec = std::array<float, 3>; // Detour's coordinates

        inline auto apart(const Vec& a, const Vec& b) -> float
        {
            return std::sqrt((a[0] - b[0]) * (a[0] - b[0]) + (a[1] - b[1]) * (a[1] - b[1]) + (a[2] - b[2]) * (a[2] - b[2]));
        }

        // The point of the segment a-b nearest c
        inline auto nearestOn(const Vec& a, const Vec& b, const Vec& c) -> Vec
        {
            const Vec   ab{ b[0] - a[0], b[1] - a[1], b[2] - a[2] };
            const float len = ab[0] * ab[0] + ab[1] * ab[1] + ab[2] * ab[2];
            const float t   = len > 0.0f ? std::clamp(((c[0] - a[0]) * ab[0] + (c[1] - a[1]) * ab[1] + (c[2] - a[2]) * ab[2]) / len, 0.0f, 1.0f) : 0.0f;
            return { a[0] + ab[0] * t, a[1] + ab[1] * t, a[2] + ab[2] * t };
        }

        // Where the segment a-b enters and leaves the sphere round c of
        // radius r, each held to the segment's own ends
        inline auto chordOf(const Vec& a, const Vec& b, const Vec& c, const float r) -> std::pair<Vec, Vec>
        {
            const Vec   d{ b[0] - a[0], b[1] - a[1], b[2] - a[2] };
            const Vec   f{ a[0] - c[0], a[1] - c[1], a[2] - c[2] };
            const float qa   = d[0] * d[0] + d[1] * d[1] + d[2] * d[2];
            const float qb   = 2.0f * (f[0] * d[0] + f[1] * d[1] + f[2] * d[2]);
            const float qc   = f[0] * f[0] + f[1] * f[1] + f[2] * f[2] - r * r;
            const float disc = qb * qb - 4.0f * qa * qc;
            if (qa <= 0.0f || disc < 0.0f)
            {
                return { a, b };
            }
            const float root = std::sqrt(disc);
            const float t1   = std::clamp((-qb - root) / (2.0f * qa), 0.0f, 1.0f);
            const float t2   = std::clamp((-qb + root) / (2.0f * qa), 0.0f, 1.0f);
            return { Vec{ a[0] + d[0] * t1, a[1] + d[1] * t1, a[2] + d[2] * t1 }, Vec{ a[0] + d[0] * t2, a[1] + d[1] * t2, a[2] + d[2] * t2 } };
        }

        // The hazards filed by the cells of a coarse grid their spheres
        // touch, so a step asks only those near it
        class HazardGrid
        {
        public:
            HazardGrid(const std::vector<Vec>& centres, const std::vector<Hazard>& hazards)
            {
                for (size_t h = 0; h < hazards.size(); ++h)
                {
                    const float r = hazards[h].radius;
                    for (int cx = cell(centres[h][0] - r); cx <= cell(centres[h][0] + r); ++cx)
                    {
                        for (int cz = cell(centres[h][2] - r); cz <= cell(centres[h][2] + r); ++cz)
                        {
                            cells_[key(cx, cz)].push_back(static_cast<uint32_t>(h));
                        }
                    }
                }
            }

            // Each hazard filed near the step a-b, until `fn` answers true;
            // true when it did
            template <typename Fn>
            auto any(const Vec& a, const Vec& b, Fn&& fn) const -> bool
            {
                if (cells_.empty())
                {
                    return false;
                }
                for (int cx = cell(std::min(a[0], b[0])); cx <= cell(std::max(a[0], b[0])); ++cx)
                {
                    for (int cz = cell(std::min(a[2], b[2])); cz <= cell(std::max(a[2], b[2])); ++cz)
                    {
                        if (const auto it = cells_.find(key(cx, cz)); it != cells_.end())
                        {
                            for (const uint32_t h : it->second)
                            {
                                if (fn(h))
                                {
                                    return true;
                                }
                            }
                        }
                    }
                }
                return false;
            }

        private:
            static constexpr float kCell = 32.0f;

            static auto cell(const float v) -> int
            {
                return static_cast<int>(std::floor(v / kCell));
            }

            static auto key(const int x, const int z) -> int64_t
            {
                return (static_cast<int64_t>(x) << 32) ^ static_cast<uint32_t>(z);
            }

            std::unordered_map<int64_t, std::vector<uint32_t>> cells_;
        };
    } // namespace detail

    // The rule a route kept out of danger is held to, one straight step at a
    // time: a step is in danger when it passes inside a hazard's sphere at a
    // point its mob sees -- where the step comes nearest, or where it enters
    // or leaves the sphere, so a pillar hiding one of them hides no more --
    // or inside its unseen sphere. Inside a sphere she stood in as she set
    // out (`origin`), only a step that takes her deeper is
    class DangerTest
    {
    public:
        DangerTest(const std::vector<Hazard>& hazards, const detail::Vec& origin, HazardSight sight)
        : hazards_(hazards)
        , sight_(std::move(sight))
        , centres_(centresOf(hazards))
        , holdsOrigin_(holding(hazards, centres_, origin))
        , grid_(centres_, hazards)
        {
        }

        // The hazard the step a-b passes through, or -1 (Detour's coordinates)
        auto holder(const detail::Vec& a, const detail::Vec& b) const -> int
        {
            int found = -1;
            grid_.any(a, b, [&](const uint32_t h)
            {
                const auto& c       = centres_[h];
                const auto  closest = detail::nearestOn(a, b, c);
                const float d       = detail::apart(closest, c);
                if (d >= hazards_[h].radius)
                {
                    return false;
                }
                const float fromA = detail::apart(a, c);
                if (holdsOrigin_[h] && fromA < hazards_[h].radius && detail::apart(b, c) >= fromA - 0.01f)
                {
                    return false; // on her way out of the danger she set out in
                }
                const auto seen = [&](const detail::Vec& p) { return sight_(h, fromDetour(p.data())); };
                bool       held = d < hazards_[h].unseen || !sight_ || seen(closest);
                if (!held)
                {
                    const auto [enters, leaves] = detail::chordOf(a, b, c, hazards_[h].radius);
                    held                        = seen(enters) || seen(leaves);
                }
                if (held)
                {
                    found = static_cast<int>(h);
                }
                return held;
            });
            return found;
        }

        auto operator()(const detail::Vec& a, const detail::Vec& b) const -> bool
        {
            return holder(a, b) >= 0;
        }

        // The same, in the zone's coordinates
        auto holder(const position_t& a, const position_t& b) const -> int
        {
            return holder(toDetour(a), toDetour(b));
        }

    private:
        static auto centresOf(const std::vector<Hazard>& hazards) -> std::vector<detail::Vec>
        {
            std::vector<detail::Vec> out;
            out.reserve(hazards.size());
            for (const auto& h : hazards)
            {
                out.push_back(toDetour(h.at));
            }
            return out;
        }

        static auto holding(const std::vector<Hazard>& hazards, const std::vector<detail::Vec>& centres, const detail::Vec& origin) -> std::vector<char>
        {
            std::vector<char> out(hazards.size());
            for (size_t h = 0; h < hazards.size(); ++h)
            {
                out[h] = detail::apart(origin, centres[h]) < hazards[h].radius ? 1 : 0;
            }
            return out;
        }

        const std::vector<Hazard>& hazards_;
        HazardSight                sight_;
        std::vector<detail::Vec>   centres_;
        std::vector<char>          holdsOrigin_;
        detail::HazardGrid         grid_;
    };

    // The route from `from` to `to` that keeps out of danger: an A* of its
    // own over points along the edges the mesh's polygons share, every step
    // a straight walk inside one polygon -- so on the game's walkable ground
    // -- or across one of the mesh's off-mesh links (a ledge, a step) from
    // the end it lands on to the other, never back along a one-way link, and
    // every step tested as a segment against the hazards' spheres, a
    // wall between a mob and the step hiding it but for its unseen sphere.
    // A step through danger costs kDangerCost more, so the route takes any
    // way round shorter than that and, where the mobs hold every way, the
    // fewest steps through them: `firstDanger` names the first, a choke known
    // before she takes a step. Danger she stands in as she sets out holds
    // only a step that takes her deeper. The route is then drawn tight, a
    // straight kept wherever it stays on the mesh and out of danger. Not
    // complete when no walk joins the two points; none when either is off the
    // mesh. Detour's own search takes no such rule here (its filter is not
    // virtual in this build), so the search is ours
    inline auto searchAvoiding(const dtNavMesh& mesh, const position_t& from, const position_t& to, const float* fromExtents, const float* toExtents,
                               const std::vector<Hazard>& hazards, const HazardSight& sight) -> std::optional<Route>
    {
        using detail::apart;
        using detail::Vec;

        dtNavMeshQuery query;
        if (dtStatusFailed(query.init(&mesh, 64)))
        {
            return std::nullopt;
        }
        dtQueryFilter filter;
        filter.setIncludeFlags(0xFFFF);
        filter.setExcludeFlags(kPolyDisabled);
        const auto start = toDetour(from);
        const auto goal  = toDetour(to);
        dtPolyRef  startRef{};
        dtPolyRef  goalRef{};
        float      startAt[3]{};
        float      goalAt[3]{};
        if (dtStatusFailed(query.findNearestPoly(start.data(), fromExtents, &filter, &startRef, startAt)) || !startRef ||
            dtStatusFailed(query.findNearestPoly(goal.data(), toExtents, &filter, &goalRef, goalAt)) || !goalRef)
        {
            return std::nullopt;
        }
        const Vec origin{ startAt[0], startAt[1], startAt[2] };
        const Vec target{ goalAt[0], goalAt[1], goalAt[2] };

        const DangerTest inDanger(hazards, origin, sight);

        struct Node
        {
            Vec       at{};
            dtPolyRef polys[2]{}; // the polygons whose shared edge it lies on; one for the start and the goal
            dtPolyRef via    = 0; // the polygon the step to it from its parent crosses
            int       parent = -1;
            float     g      = std::numeric_limits<float>::max();
            bool      closed = false;
            bool      danger = false; // the step to it from its parent passes through danger
            bool      link   = false; // the step to it from its parent crosses an off-mesh link
            int8_t    end    = -1;    // an off-mesh link's end it stands on: 0 where the link starts, 1 where it ends
        };
        std::vector<Node> nodes;
        nodes.reserve(4096);
        nodes.push_back(Node{ .at = origin, .polys = { startRef, 0 }, .g = 0.0f }); // the start
        nodes.push_back(Node{ .at = target, .polys = { goalRef, 0 } });            // the goal
        constexpr int kGoal = 1;

        // Each polygon's edge points, made the first time the search stands
        // on it; an edge's points belong to both its polygons. An off-mesh
        // link is a polygon of two vertices, its ends, each set by Detour on
        // the ground polygon it lands on: its points are those two ends
        struct PolyPoints
        {
            std::vector<int> nodes;
            bool             made   = false;
            bool             link   = false; // an off-mesh link
            bool             oneWay = false; // ... walked from its start to its end only
        };
        const auto linkEnd = [&](const dtPolyRef linkRef, const dtPolyRef ground) -> int
        {
            const dtMeshTile* tile = nullptr;
            const dtPoly*     poly = nullptr;
            mesh.getTileAndPolyByRefUnsafe(linkRef, &tile, &poly);
            for (unsigned int l = poly->firstLink; l != DT_NULL_LINK; l = tile->links[l].next)
            {
                if (tile->links[l].ref == ground && tile->links[l].edge < 2)
                {
                    return tile->links[l].edge;
                }
            }
            return -1;
        };
        std::unordered_map<dtPolyRef, PolyPoints> pointsOf;
        std::unordered_map<uint64_t, char>        edgesMade;
        const auto                                make = [&](const dtPolyRef ref)
        {
            if (pointsOf[ref].made)
            {
                return;
            }
            pointsOf[ref].made     = true;
            const dtMeshTile* tile = nullptr;
            const dtPoly*     poly = nullptr;
            mesh.getTileAndPolyByRefUnsafe(ref, &tile, &poly);
            const bool isLink = poly->getType() == DT_POLYTYPE_OFFMESH_CONNECTION;
            if (isLink)
            {
                const auto& con      = tile->offMeshCons[static_cast<int>(poly - tile->polys) - tile->header->offMeshBase];
                pointsOf[ref].link   = true;
                pointsOf[ref].oneWay = (con.flags & DT_OFFMESH_CON_BIDIR) == 0;
            }
            for (unsigned int l = poly->firstLink; l != DT_NULL_LINK; l = tile->links[l].next)
            {
                const dtLink&   link = tile->links[l];
                const dtPolyRef next = link.ref;
                if (!next)
                {
                    continue;
                }
                const dtMeshTile* nextTile = nullptr;
                const dtPoly*     nextPoly = nullptr;
                mesh.getTileAndPolyByRefUnsafe(next, &nextTile, &nextPoly);
                const bool nextLink = nextPoly->getType() == DT_POLYTYPE_OFFMESH_CONNECTION;
                if ((nextPoly->flags & kPolyDisabled) || (isLink && nextLink))
                {
                    continue;
                }
                const uint64_t edgeKey = (static_cast<uint64_t>(std::min(ref, next)) << 32) | static_cast<uint64_t>(std::max(ref, next));
                if (!edgesMade.emplace(edgeKey, 1).second)
                {
                    continue;
                }

                // An off-mesh link's end where it lands on the ground: one
                // point, the link's own vertex
                if (isLink || nextLink)
                {
                    const dtPolyRef linkRef = isLink ? ref : next;
                    const dtPolyRef ground  = isLink ? next : ref;
                    const int       end     = linkEnd(linkRef, ground);
                    if (end < 0)
                    {
                        continue;
                    }
                    const dtMeshTile* linkTile = isLink ? tile : nextTile;
                    const dtPoly*     linkPoly = isLink ? poly : nextPoly;
                    const float*      v        = &linkTile->verts[linkPoly->verts[end] * 3];
                    nodes.push_back(Node{ .at = { v[0], v[1], v[2] }, .polys = { ground, linkRef }, .end = static_cast<int8_t>(end) });
                    const int id = static_cast<int>(nodes.size()) - 1;
                    pointsOf[ref].nodes.push_back(id);
                    pointsOf[next].nodes.push_back(id);
                    continue;
                }

                // The edge as Detour's own portal reads it: the polygon's
                // side, narrowed to the part a neighbour across a tile
                // border shares
                const float* v0 = &tile->verts[poly->verts[link.edge] * 3];
                const float* v1 = &tile->verts[poly->verts[(link.edge + 1) % poly->vertCount] * 3];
                float        tmin = 0.0f;
                float        tmax = 1.0f;
                if (link.side != 0xff && (link.bmin != 0 || link.bmax != 255))
                {
                    tmin = link.bmin / 255.0f;
                    tmax = link.bmax / 255.0f;
                }
                const Vec left{ v0[0] + (v1[0] - v0[0]) * tmin, v0[1] + (v1[1] - v0[1]) * tmin, v0[2] + (v1[2] - v0[2]) * tmin };
                const Vec right{ v0[0] + (v1[0] - v0[0]) * tmax, v0[1] + (v1[1] - v0[1]) * tmax, v0[2] + (v1[2] - v0[2]) * tmax };

                // Points along it, kPortalSpacing apart, the end ones a little
                // in from the corners
                const float len   = apart(left, right);
                const int   count = std::max(1, static_cast<int>(std::ceil(len / kPortalSpacing)));
                const float inset = len > 0.0f ? std::min(0.3f, len * 0.1f) / len : 0.0f;
                for (int k = 0; k < count; ++k)
                {
                    const float t = count == 1 ? 0.5f : inset + (1.0f - 2.0f * inset) * static_cast<float>(k) / static_cast<float>(count - 1);
                    nodes.push_back(Node{ .at = { left[0] + (right[0] - left[0]) * t, left[1] + (right[1] - left[1]) * t, left[2] + (right[2] - left[2]) * t },
                                          .polys = { ref, next } });
                    const int id = static_cast<int>(nodes.size()) - 1;
                    pointsOf[ref].nodes.push_back(id);
                    pointsOf[next].nodes.push_back(id);
                }
            }
        };

        using Open = std::pair<float, int>;
        std::priority_queue<Open, std::vector<Open>, std::greater<>> open;
        open.push({ apart(origin, target), 0 });
        int  expanded = 0;
        bool reached  = false;
        while (!open.empty())
        {
            const int here = open.top().second;
            open.pop();
            if (nodes[here].closed)
            {
                continue;
            }
            nodes[here].closed = true;
            if (here == kGoal)
            {
                reached = true;
                break;
            }
            if (++expanded > kAvoidNodes || static_cast<int>(nodes.size()) > kAvoidNodes * 4)
            {
                break;
            }

            const dtPolyRef polys[2] = { nodes[here].polys[0], nodes[here].polys[1] };
            for (const dtPolyRef ref : polys)
            {
                if (ref)
                {
                    make(ref);
                }
            }
            const Vec   at = nodes[here].at;
            const float g  = nodes[here].g;
            const auto  relax = [&](const int there, const dtPolyRef via, const bool viaLink)
            {
                Node& next = nodes[there];
                if (there == here || next.closed)
                {
                    return;
                }
                const float walked = g + apart(at, next.at);
                if (walked >= next.g)
                {
                    return; // danger only adds
                }
                const bool  danger = inDanger(at, next.at);
                const float cost   = walked + (danger ? kDangerCost : 0.0f);
                if (cost < next.g)
                {
                    next.g      = cost;
                    next.parent = here;
                    next.via    = via;
                    next.danger = danger;
                    next.link   = viaLink;
                    open.push({ cost + apart(next.at, target), there });
                }
            };
            for (const dtPolyRef ref : polys)
            {
                if (!ref)
                {
                    continue;
                }
                const auto& points = pointsOf.find(ref)->second;
                if (points.oneWay && nodes[here].end == 1)
                {
                    continue; // a one-way link is never walked back
                }
                for (const int there : points.nodes)
                {
                    relax(there, ref, points.link);
                }
                if (ref == goalRef)
                {
                    relax(kGoal, ref, false);
                }
            }
        }

        Route route;
        route.searched = expanded;
        if (!reached)
        {
            return route; // not complete: no walk joins them
        }
        std::vector<int> raw;
        for (int at = kGoal; at != -1; at = nodes[at].parent)
        {
            raw.push_back(at);
        }
        std::reverse(raw.begin(), raw.end());

        // Drawn tight: from each kept point, the points ahead in turn while
        // the straight to them stays on the mesh -- a ray along the ground
        // from the polygon she leaves by, ending on the polygon that point
        // lies on -- and out of danger; the last before the first that fails
        // is kept. A step through danger, or across an off-mesh link, is kept
        // as it is
        const auto straight = [&](const int a, const int k) -> bool
        {
            const Node& from_ = nodes[raw[a]];
            const Node& to_   = nodes[raw[k]];
            if (nodes[raw[a + 1]].link || apart(from_.at, to_.at) > kShortcutReach || inDanger(from_.at, to_.at))
            {
                return false;
            }
            float     t = 0.0f;
            float     normal[3]{};
            dtPolyRef path[kRayPolys]{};
            int       count = 0;
            if (dtStatusFailed(query.raycast(nodes[raw[a + 1]].via, from_.at.data(), to_.at.data(), &filter, &t, normal, path, &count, kRayPolys)) ||
                t != FLT_MAX || count == 0)
            {
                return false;
            }
            const dtPolyRef end = path[count - 1];
            return end == to_.polys[0] || end == to_.polys[1] || end == to_.via;
        };
        std::vector<int> kept;
        const int        last = static_cast<int>(raw.size()) - 1;
        for (int anchor = 0; anchor < last;)
        {
            int best = anchor + 1;
            if (!nodes[raw[best]].danger && !nodes[raw[best]].link)
            {
                for (int k = anchor + 2; k <= last && !nodes[raw[k]].danger && straight(anchor, k); ++k)
                {
                    best = k;
                }
            }
            kept.push_back(best);
            anchor = best;
        }

        route.complete = true;
        Vec previous   = origin;
        for (const int k : kept)
        {
            const Node& point = nodes[raw[k]];
            route.links += point.link ? 1 : 0;
            if (point.danger && route.firstDanger < 0)
            {
                route.firstDanger = static_cast<int>(route.points.size());
            }
            route.length += apart(previous, point.at);
            route.points.push_back(fromDetour(point.at.data()));
            previous = point.at;
        }
        return route;
    }
} // namespace cardian::route
