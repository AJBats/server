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
#include <span>
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

    constexpr float kPortalSpacing = 3.0f;    // yalms between the points tried along a polygon's edge near danger; its two ends elsewhere
    constexpr float kNearDanger    = 3.0f;    // a polygon whose box comes this near a hazard's sphere is near danger
    constexpr float kDangerCost    = 5000.0f; // a step through danger, in yalms of walk: a way round shorter than this is taken (less the greed's quarter at worst)
    constexpr float kInsideCost    = 100.0f;  // each yalm walked inside the reaches she set out in (once where they overlap), in yalms of walk: she leaves them by the shortest way, then goes on
    constexpr float kShortcutReach = 250.0f;  // the longest straight a route is drawn tight over
    constexpr int   kShortcutTries = 4;       // points ahead tried past a straight that fails, before the last good one is kept
    constexpr float kGreed         = 1.25f;   // how much harder the search runs toward the goal than a shortest way allows: far fewer points looked at, a route a quarter longer at worst (under 1% in the zones measured)
    constexpr int   kAvoidNodes    = 400000;  // the search's bound, past every zone's edge points
    constexpr int   kRayPolys      = 1024;

    // The steepest ground a player walks up, as a climb over run: 30 degrees.
    // The meshes are built for monsters and let them up steeper; measured on
    // Valkurm Dunes' goblin hill (the user, 2026-10-12), a player made 29
    // degrees and stopped at 31. A rise of kClimbStep or less is a step,
    // whatever its slope; a run of steeper ground shorter than kClimbRun is
    // a bump, and walked (a lone yalm of 35 degrees he walked, not two of 31
    // to 34, nor a band of 38 to 44). Down, any slope is walked
    constexpr float kClimbSlope = 0.57735f; // tan(30 degrees)
    constexpr float kClimbStep  = 0.5f;
    constexpr float kClimbRun   = 1.5f;

    // The ground a player cannot climb, worked out once for a mesh -- the
    // ground never moves: every triangle of its fine ground (the detail
    // mesh, the heights Detour reads under a point) steeper than
    // kClimbSlope, kept by polygon with the rise it makes per yalm along x
    // and along z. A walk across one is judged by the way it goes
    // (detail::climbsFaces): down a face is walked
    struct Face
    {
        float ax{}, az{}, bx{}, bz{}, cx{}, cz{}; // its corners across the ground (Detour's x and z)
        float sx{}, sz{};                         // its rise per yalm along x and along z
    };

    struct Faces
    {
        std::vector<uint32_t> base;  // each tile's first polygon in `first`
        std::vector<uint32_t> first; // per polygon, its first face; the next polygon's first is where its faces end
        std::vector<Face>     faces;

        // A polygon's faces: none on a mesh's ground climbable everywhere
        auto of(const dtNavMesh& mesh, const dtPolyRef ref) const -> std::span<const Face>
        {
            unsigned int salt = 0;
            unsigned int it   = 0;
            unsigned int ip   = 0;
            mesh.decodePolyId(ref, salt, it, ip);
            if (it >= base.size())
            {
                return {};
            }
            const size_t i = static_cast<size_t>(base[it]) + ip;
            if (i + 1 >= first.size())
            {
                return {};
            }
            return { faces.data() + first[i], faces.data() + first[i + 1] };
        }
    };

    inline auto facesOf(const dtNavMesh& mesh) -> Faces
    {
        Faces     out;
        const int maxTiles = mesh.getMaxTiles();
        out.base.resize(static_cast<size_t>(maxTiles));
        uint32_t total = 0;
        for (int t = 0; t < maxTiles; ++t)
        {
            out.base[t]            = total;
            const dtMeshTile* tile = mesh.getTile(t);
            total += tile != nullptr && tile->header != nullptr ? static_cast<uint32_t>(tile->header->polyCount) : 0;
        }
        out.first.reserve(static_cast<size_t>(total) + 1);
        for (int t = 0; t < maxTiles; ++t)
        {
            const dtMeshTile* tile = mesh.getTile(t);
            if (tile == nullptr || tile->header == nullptr)
            {
                continue;
            }
            for (int p = 0; p < tile->header->polyCount; ++p)
            {
                out.first.push_back(static_cast<uint32_t>(out.faces.size()));
                const dtPoly& poly = tile->polys[p];
                if (poly.getType() != DT_POLYTYPE_GROUND || tile->detailMeshes == nullptr)
                {
                    continue;
                }
                const dtPolyDetail& pd   = tile->detailMeshes[p];
                const auto          vert = [&](const unsigned char v) -> const float*
                {
                    return v < poly.vertCount ? &tile->verts[poly.verts[v] * 3] : &tile->detailVerts[(pd.vertBase + v - poly.vertCount) * 3];
                };
                for (int j = 0; j < pd.triCount; ++j)
                {
                    const unsigned char* tri = &tile->detailTris[(pd.triBase + j) * 4];
                    const float*         a   = vert(tri[0]);
                    const float*         b   = vert(tri[1]);
                    const float*         c   = vert(tri[2]);
                    const float          ux  = b[0] - a[0];
                    const float          uy  = b[1] - a[1];
                    const float          uz  = b[2] - a[2];
                    const float          vx  = c[0] - a[0];
                    const float          vy  = c[1] - a[1];
                    const float          vz  = c[2] - a[2];
                    const float          nx  = uy * vz - uz * vy;
                    const float          ny  = uz * vx - ux * vz;
                    const float          nz  = ux * vy - uy * vx;
                    if (std::fabs(ny) <= 1e-6f)
                    {
                        continue; // upright: it covers no ground
                    }
                    // the plane's height over the ground rises -n/ny along x and z
                    const float sx = -nx / ny;
                    const float sz = -nz / ny;
                    if (sx * sx + sz * sz > kClimbSlope * kClimbSlope)
                    {
                        out.faces.push_back({ a[0], a[2], b[0], b[2], c[0], c[2], sx, sz });
                    }
                }
            }
        }
        out.first.push_back(static_cast<uint32_t>(out.faces.size()));
        return out;
    }

    namespace detail
    {
        using Vec = std::array<float, 3>; // Detour's coordinates

        inline auto apart(const Vec& a, const Vec& b) -> float
        {
            return std::sqrt((a[0] - b[0]) * (a[0] - b[0]) + (a[1] - b[1]) * (a[1] - b[1]) + (a[2] - b[2]) * (a[2] - b[2]));
        }

        // Whether going from a to b climbs steeper than a player can
        // (kClimbSlope, past a step of kClimbStep); Detour's y is up
        inline auto tooSteep(const Vec& a, const Vec& b) -> bool
        {
            const float rise = b[1] - a[1];
            const float run  = std::sqrt((b[0] - a[0]) * (b[0] - a[0]) + (b[2] - a[2]) * (b[2] - a[2]));
            return rise > kClimbStep && rise > run * kClimbSlope;
        }

        // Whether a walk from a to b climbs faces for kClimbRun yalms on
        // end: the stretches of it across faces that climb past kClimbSlope
        // the way it goes, each the walk clipped to its triangle on the
        // ground, joined where they meet. `forEachFace(visit)` hands every
        // face the walk may cross to `visit`
        template <typename ForEachFace>
        inline auto climbsOver(const ForEachFace& forEachFace, const Vec& a, const Vec& b) -> bool
        {
            const float dx  = b[0] - a[0];
            const float dz  = b[2] - a[2];
            const float run = std::sqrt(dx * dx + dz * dz);
            if (run < kClimbRun)
            {
                return false; // too short to hold such a climb
            }
            const float ux = dx / run;
            const float uz = dz / run;

            thread_local std::vector<std::pair<float, float>> spans;
            spans.clear();
            forEachFace([&](const Face& f)
            {
                if (f.sx * ux + f.sz * uz <= kClimbSlope)
                {
                    return; // across it or down it: walked
                }
                // facesOf keeps no upright triangle, so the corners always turn one way
                const float turn = (f.bx - f.ax) * (f.cz - f.az) - (f.bz - f.az) * (f.cx - f.ax);
                const float side = turn > 0.0f ? 1.0f : -1.0f;
                float       lo   = 0.0f;
                float       hi   = run;
                // the walk a + t u (t in yalms) kept on the inner side of each edge
                const auto edge = [&](const float px, const float pz, const float qx, const float qz)
                {
                    const float ex  = qx - px;
                    const float ez  = qz - pz;
                    const float at0 = side * (ex * (a[2] - pz) - ez * (a[0] - px));
                    const float per = side * (ex * uz - ez * ux);
                    if (std::fabs(per) <= 1e-9f)
                    {
                        if (at0 < 0.0f)
                        {
                            hi = lo - 1.0f; // parallel, and outside
                        }
                        return;
                    }
                    const float cut = -at0 / per;
                    if (per > 0.0f)
                    {
                        lo = std::max(lo, cut);
                    }
                    else
                    {
                        hi = std::min(hi, cut);
                    }
                };
                edge(f.ax, f.az, f.bx, f.bz);
                edge(f.bx, f.bz, f.cx, f.cz);
                edge(f.cx, f.cz, f.ax, f.az);
                if (hi > lo)
                {
                    spans.emplace_back(lo, hi);
                }
            });
            if (spans.empty())
            {
                return false;
            }
            std::sort(spans.begin(), spans.end());
            float from = spans.front().first;
            float to   = spans.front().second;
            for (const auto& [lo, hi] : spans)
            {
                if (lo > to + 0.1f)
                {
                    from = lo; // a gap: a new climb
                }
                to = std::max(to, hi);
                if (to - from >= kClimbRun)
                {
                    return true;
                }
            }
            return false;
        }

        // The same for a walk crossing the polygons `polys` of a mesh, over
        // their faces (Faces)
        inline auto climbsFaces(const Faces& faces, const dtNavMesh& mesh, const dtPolyRef* polys, const int count, const Vec& a, const Vec& b) -> bool
        {
            return climbsOver([&](const auto& visit)
            {
                for (int i = 0; i < count; ++i)
                {
                    for (const Face& f : faces.of(mesh, polys[i]))
                    {
                        visit(f);
                    }
                }
            }, a, b);
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

        // The part of the segment a-b inside the sphere round c of radius r,
        // as the fractions of the way from a to b it enters and leaves at;
        // the two equal when none of it is
        inline auto insideSpan(const Vec& a, const Vec& b, const Vec& c, const float r) -> std::pair<float, float>
        {
            const Vec   d{ b[0] - a[0], b[1] - a[1], b[2] - a[2] };
            const Vec   f{ a[0] - c[0], a[1] - c[1], a[2] - c[2] };
            const float qa   = d[0] * d[0] + d[1] * d[1] + d[2] * d[2];
            const float qb   = 2.0f * (f[0] * d[0] + f[1] * d[1] + f[2] * d[2]);
            const float qc   = f[0] * f[0] + f[1] * f[1] + f[2] * f[2] - r * r;
            const float disc = qb * qb - 4.0f * qa * qc;
            if (qa <= 0.0f || disc <= 0.0f)
            {
                return { 0.0f, 0.0f };
            }
            const float root = std::sqrt(disc);
            const float t1   = std::clamp((-qb - root) / (2.0f * qa), 0.0f, 1.0f);
            const float t2   = std::clamp((-qb + root) / (2.0f * qa), 0.0f, 1.0f);
            return { t1, std::max(t1, t2) };
        }

        // The yalms of the segment a-b inside the sphere round c of radius r
        inline auto insideLength(const Vec& a, const Vec& b, const Vec& c, const float r) -> float
        {
            const auto [enters, leaves] = insideSpan(a, b, c, r);
            return (leaves - enters) * apart(a, b);
        }

        // The hazards filed by the cells of a flat grid over the ground their
        // spheres cover, each cell's hazards one run of `items_`, so a step
        // asks only those near it
        class HazardGrid
        {
        public:
            HazardGrid(const std::vector<Vec>& centres, const std::vector<Hazard>& hazards)
            {
                if (hazards.empty())
                {
                    return;
                }
                minX_ = minZ_ = std::numeric_limits<int>::max();
                maxX_ = maxZ_ = std::numeric_limits<int>::min();
                for (size_t h = 0; h < hazards.size(); ++h)
                {
                    const float r = hazards[h].radius;
                    minX_         = std::min(minX_, cell(centres[h][0] - r));
                    maxX_         = std::max(maxX_, cell(centres[h][0] + r));
                    minZ_         = std::min(minZ_, cell(centres[h][2] - r));
                    maxZ_         = std::max(maxZ_, cell(centres[h][2] + r));
                }
                width_ = maxX_ - minX_ + 1;
                start_.assign(static_cast<size_t>(width_) * (maxZ_ - minZ_ + 1) + 1, 0);
                const auto eachCell = [&](const size_t h, const auto& fn)
                {
                    const float r = hazards[h].radius;
                    for (int cz = cell(centres[h][2] - r); cz <= cell(centres[h][2] + r); ++cz)
                    {
                        for (int cx = cell(centres[h][0] - r); cx <= cell(centres[h][0] + r); ++cx)
                        {
                            fn(indexOf(cx, cz));
                        }
                    }
                };
                for (size_t h = 0; h < hazards.size(); ++h)
                {
                    eachCell(h, [&](const size_t i) { ++start_[i + 1]; });
                }
                for (size_t i = 1; i < start_.size(); ++i)
                {
                    start_[i] += start_[i - 1];
                }
                items_.resize(start_.back());
                std::vector<uint32_t> fill(start_.begin(), start_.end() - 1);
                for (size_t h = 0; h < hazards.size(); ++h)
                {
                    eachCell(h, [&](const size_t i) { items_[fill[i]++] = static_cast<uint32_t>(h); });
                }
            }

            // Each hazard filed near the step a-b, until `fn` answers true;
            // true when it did
            template <typename Fn>
            auto any(const Vec& a, const Vec& b, Fn&& fn) const -> bool
            {
                if (items_.empty())
                {
                    return false;
                }
                const int x0 = std::max(cell(std::min(a[0], b[0])), minX_);
                const int x1 = std::min(cell(std::max(a[0], b[0])), maxX_);
                const int z0 = std::max(cell(std::min(a[2], b[2])), minZ_);
                const int z1 = std::min(cell(std::max(a[2], b[2])), maxZ_);
                for (int cz = z0; cz <= z1; ++cz)
                {
                    for (int cx = x0; cx <= x1; ++cx)
                    {
                        const size_t i = indexOf(cx, cz);
                        for (uint32_t k = start_[i]; k < start_[i + 1]; ++k)
                        {
                            if (fn(items_[k]))
                            {
                                return true;
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

            auto indexOf(const int cx, const int cz) const -> size_t
            {
                return static_cast<size_t>(cz - minZ_) * width_ + (cx - minX_);
            }

            int                   minX_  = 0;
            int                   minZ_  = 0;
            int                   maxX_  = -1;
            int                   maxZ_  = -1;
            int                   width_ = 0;
            std::vector<uint32_t> start_;
            std::vector<uint32_t> items_;
        };

        // A point of the search: on the edge two polygons share, at an
        // off-mesh link's end, or the route's own start or goal
        struct Node
        {
            Vec       at{};
            dtPolyRef polys[2]{}; // the polygons whose shared edge it lies on; one for the start and the goal
            dtPolyRef via    = 0; // the polygon the step to it from its parent crosses
            int       parent = -1;
            float     g      = std::numeric_limits<float>::max();
            float     walk   = 0.0f; // the yalms walked to it, danger's cost left out
            bool      closed = false;
            bool      danger = false; // the step to it from its parent passes through danger
            bool      link   = false; // the step to it from its parent crosses an off-mesh link
            int8_t    end    = -1;    // an off-mesh link's end it stands on: 0 where the link starts, 1 where it ends
        };

        constexpr uint8_t kLink   = 1; // a polygon that is an off-mesh link
        constexpr uint8_t kOneWay = 2; // ... walked from its start to its end only

        // The search's tables, kept from one search to the next and grown to
        // the largest mesh searched: every trek plans on the map's main
        // thread, one search at a time. A polygon's entries count only when
        // stamped with the search under way, so nothing is cleared between
        // searches
        struct Workspace
        {
            std::vector<uint32_t>              base;   // each tile's first polygon in the tables below
            std::vector<uint32_t>              madeIn; // per polygon: the search that made its points
            std::vector<uint32_t>              listIn; // ... the search its list of points belongs to
            std::vector<std::vector<int>>      points; // ... its points, as indices into `nodes`
            std::vector<uint8_t>               kind;   // ... kLink, kOneWay
            std::vector<uint32_t>              nearIn; // ... the search that judged it near danger or not
            std::vector<uint8_t>               nearby; // ... near danger (kNearDanger)
            std::vector<Node>                  nodes;
            std::vector<std::pair<float, int>> open; // the open list, a heap
            uint32_t                           stamp = 0;
        };

        inline auto workspace() -> Workspace&
        {
            static Workspace w;
            return w;
        }

        // A search that grew the points or the open list past what a zone's
        // whole route needs (a failed search walks all it can reach) hands
        // the memory back rather than holding it for good
        inline void trim(Workspace& ws)
        {
            constexpr size_t kKeep = 1u << 18;
            if (ws.nodes.capacity() > kKeep)
            {
                std::vector<Node>().swap(ws.nodes);
            }
            if (ws.open.capacity() > kKeep)
            {
                std::vector<std::pair<float, int>>().swap(ws.open);
            }
        }
    } // namespace detail

    // The rule a route kept out of danger is held to, one straight step at a
    // time: a step is in danger when it passes inside a hazard's sphere at a
    // point its mob sees -- where the step comes nearest, or where it enters
    // or leaves the sphere, so a pillar hiding one of them hides no more --
    // or inside its unseen sphere. Inside a sphere she stood in as she set
    // out (`origin`), only a step that takes her deeper is; the yalms she
    // walks inside one whose mob sees where she set out -- the reaches she
    // is in -- are no choke, and are counted (insideOrigin) so the search
    // can price them
    class DangerTest
    {
    public:
        DangerTest(const std::vector<Hazard>& hazards, const detail::Vec& origin, HazardSight sight)
        : hazards_(hazards)
        , sight_(std::move(sight))
        , centres_(centresOf(hazards))
        , holdsOrigin_(holding(hazards, centres_, origin))
        , grid_(centres_, hazards)
        , origins_(exposing(origin))
        {
        }

        // The yalms of the step a-b inside the reaches she set out in, each
        // yalm counted once however many of them overlap (Detour's
        // coordinates)
        auto insideOrigin(const detail::Vec& a, const detail::Vec& b) const -> float
        {
            if (origins_.empty())
            {
                return 0.0f;
            }
            thread_local std::vector<std::pair<float, float>> spans;
            spans.clear();
            for (const uint32_t h : origins_)
            {
                const auto span = detail::insideSpan(a, b, centres_[h], hazards_[h].radius);
                if (span.second > span.first)
                {
                    spans.push_back(span);
                }
            }
            std::sort(spans.begin(), spans.end());
            float inside = 0.0f;
            float upTo   = 0.0f;
            for (const auto& [enters, leaves] : spans)
            {
                if (leaves > upTo)
                {
                    inside += leaves - std::max(enters, upTo);
                    upTo = leaves;
                }
            }
            return inside * detail::apart(a, b);
        }

        // Whether she set out inside any reach
        auto startsInside() const -> bool
        {
            return !origins_.empty();
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
                if (holdsOrigin_[h] && fromA < hazards_[h].radius && d >= fromA - 0.01f && detail::apart(b, c) >= fromA - 0.01f)
                {
                    return false; // on her way out of the danger she set out in, never nearer its middle than where the step starts
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

        // The hazard whose reach holds the point -- inside its sphere where
        // its mob sees the point, or inside its unseen sphere -- or -1
        auto exposes(const detail::Vec& p) const -> int
        {
            int found = -1;
            grid_.any(p, p, [&](const uint32_t h)
            {
                const float d = detail::apart(p, centres_[h]);
                if (d >= hazards_[h].radius)
                {
                    return false;
                }
                if (d < hazards_[h].unseen || !sight_ || sight_(h, fromDetour(p.data())))
                {
                    found = static_cast<int>(h);
                    return true;
                }
                return false;
            });
            return found;
        }

        // Whether a hazard's sphere comes within `margin` of the box lo-hi
        // (Detour's coordinates), seen or not
        auto reaches(const detail::Vec& lo, const detail::Vec& hi, const float margin) const -> bool
        {
            const detail::Vec wideLo{ lo[0] - margin, lo[1] - margin, lo[2] - margin };
            const detail::Vec wideHi{ hi[0] + margin, hi[1] + margin, hi[2] + margin };
            return grid_.any(wideLo, wideHi, [&](const uint32_t h)
            {
                const auto& c     = centres_[h];
                const float dx    = std::max({ lo[0] - c[0], 0.0f, c[0] - hi[0] });
                const float dy    = std::max({ lo[1] - c[1], 0.0f, c[1] - hi[1] });
                const float dz    = std::max({ lo[2] - c[2], 0.0f, c[2] - hi[2] });
                const float reach = hazards_[h].radius + margin;
                return dx * dx + dy * dy + dz * dz < reach * reach;
            });
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

        // The spheres she set out in whose mobs see where she set out: inside
        // the unseen sphere, or where its sight reaches
        auto exposing(const detail::Vec& origin) const -> std::vector<uint32_t>
        {
            std::vector<uint32_t> out;
            for (size_t h = 0; h < hazards_.size(); ++h)
            {
                if (holdsOrigin_[h] &&
                    (detail::apart(origin, centres_[h]) < hazards_[h].unseen || !sight_ || sight_(h, fromDetour(origin.data()))))
                {
                    out.push_back(static_cast<uint32_t>(h));
                }
            }
            return out;
        }

        const std::vector<Hazard>& hazards_;
        HazardSight                sight_;
        std::vector<detail::Vec>   centres_;
        std::vector<char>          holdsOrigin_;
        detail::HazardGrid         grid_;
        std::vector<uint32_t>      origins_; // the reaches she set out in
    };

    // The route from `from` to `to` that keeps out of danger: an A* of its
    // own over points along the edges the mesh's polygons share, every step
    // a straight walk inside one polygon -- so on the game's walkable ground
    // -- or across one of the mesh's off-mesh links (a ledge, a step) from
    // the end it lands on to the other, never back along a one-way link nor up
    // one that rises more than the mesh's own climb (dropped off, never
    // climbed: the meshes mark ledges of up to 5 yalms two-way), never up
    // ground steeper than a player climbs (kClimbSlope), and
    // every step tested as a segment against the hazards' spheres, a
    // wall between a mob and the step hiding it but for its unseen sphere.
    // A step through danger costs kDangerCost more, so the route takes a
    // way round shorter than that (less a quarter, kGreed's bound, at worst)
    // and, where the mobs hold every way, the
    // fewest steps through them: `firstDanger` names the first, a choke known
    // before she takes a step. Danger she stands in as she sets out holds
    // only a step that takes her deeper, and every yalm she walks inside it
    // costs kInsideCost, so she leaves it by the shortest way before going
    // on -- not along its inside toward her goal. The route is then drawn
    // tight, a straight kept wherever it stays on the mesh and out of
    // danger, and keeps her inside no longer than the way it replaces. Not
    // complete when no walk joins the two points; none when either is off the
    // mesh. With a `reach`, only the ground within it looks for the way --
    // every point whose distances to the two ends add up to no more, an
    // ellipse round them -- so a stretch of a route is planned again for
    // the cost of the stretch. Detour's own search takes no such rule here
    // (its filter is not virtual in this build), so the search is ours
    //
    // The search behind searchAvoiding and searchSafeGround: to `to` when
    // given; without one, to the nearest point out of every reach, walking at
    // most `within` yalms (Dijkstra, the greed off)
    inline auto searchCore(const dtNavMesh& mesh, const position_t& from, const std::optional<position_t>& to, const float* fromExtents, const float* toExtents,
                           const std::vector<Hazard>& hazards, const HazardSight& sight, const float reach, const float within, const Faces* faces)
        -> std::optional<Route>
    {
        using detail::apart;
        using detail::Node;
        using detail::Vec;

        dtNavMeshQuery query;
        if (dtStatusFailed(query.init(&mesh, 64)))
        {
            return std::nullopt;
        }
        dtQueryFilter filter;
        filter.setIncludeFlags(0xFFFF);
        filter.setExcludeFlags(kPolyDisabled);
        const bool safeGround = !to.has_value();
        const auto start      = toDetour(from);
        const auto goal       = toDetour(to.value_or(from));
        dtPolyRef  startRef{};
        dtPolyRef  goalRef{};
        float      startAt[3]{};
        float      goalAt[3]{};
        if (dtStatusFailed(query.findNearestPoly(start.data(), fromExtents, &filter, &startRef, startAt)) || !startRef)
        {
            return std::nullopt;
        }
        if (!safeGround && (dtStatusFailed(query.findNearestPoly(goal.data(), toExtents, &filter, &goalRef, goalAt)) || !goalRef))
        {
            return std::nullopt;
        }
        const Vec   origin{ startAt[0], startAt[1], startAt[2] };
        const Vec   target = safeGround ? origin : Vec{ goalAt[0], goalAt[1], goalAt[2] };
        const float greed  = safeGround ? 0.0f : kGreed;

        // The highest step the mesh's ground takes (its walkableClimb): an
        // off-mesh link rising more is a ledge, dropped off and never climbed
        float climb = 1.0f;
        {
            const dtMeshTile* tile = nullptr;
            const dtPoly*     poly = nullptr;
            mesh.getTileAndPolyByRefUnsafe(startRef, &tile, &poly);
            climb = tile->header->walkableClimb;
        }

        const DangerTest inDanger(hazards, origin, sight);
        if (safeGround && inDanger.exposes(origin) < 0)
        {
            Route here;
            here.complete = true;
            return here; // out of every reach where she stands
        }

        // The tables, indexed by polygon tile by tile, grown to this mesh
        auto& ws = detail::workspace();
        if (++ws.stamp == 0)
        {
            ws.stamp = 1;
            std::fill(ws.madeIn.begin(), ws.madeIn.end(), 0);
            std::fill(ws.listIn.begin(), ws.listIn.end(), 0);
            std::fill(ws.nearIn.begin(), ws.nearIn.end(), 0);
        }
        const uint32_t stamp    = ws.stamp;
        const int      maxTiles = mesh.getMaxTiles();
        ws.base.resize(static_cast<size_t>(maxTiles));
        uint32_t total = 0;
        for (int t = 0; t < maxTiles; ++t)
        {
            ws.base[t]             = total;
            const dtMeshTile* tile = mesh.getTile(t);
            total += tile != nullptr && tile->header != nullptr ? static_cast<uint32_t>(tile->header->polyCount) : 0;
        }
        if (ws.madeIn.size() < total)
        {
            ws.madeIn.resize(total, 0);
            ws.listIn.resize(total, 0);
            ws.points.resize(total);
            ws.kind.resize(total, 0);
            ws.nearIn.resize(total, 0);
            ws.nearby.resize(total, 0);
        }
        const auto indexOf = [&](const dtPolyRef ref) -> uint32_t
        {
            unsigned int salt = 0;
            unsigned int it   = 0;
            unsigned int ip   = 0;
            mesh.decodePolyId(ref, salt, it, ip);
            return ws.base[it] + ip;
        };
        const auto listOf = [&](const uint32_t i) -> std::vector<int>&
        {
            if (ws.listIn[i] != stamp)
            {
                ws.points[i].clear();
                ws.listIn[i] = stamp;
            }
            return ws.points[i];
        };

        // Whether a polygon lies near danger, judged once a search: its edges
        // are tried every kPortalSpacing there, at their two ends elsewhere
        const auto nearOf = [&](const uint32_t i, const dtMeshTile* tile, const dtPoly* poly) -> bool
        {
            if (ws.nearIn[i] != stamp)
            {
                Vec lo{ FLT_MAX, FLT_MAX, FLT_MAX };
                Vec hi{ -FLT_MAX, -FLT_MAX, -FLT_MAX };
                for (int v = 0; v < poly->vertCount; ++v)
                {
                    const float* p = &tile->verts[poly->verts[v] * 3];
                    for (int a = 0; a < 3; ++a)
                    {
                        lo[a] = std::min(lo[a], p[a]);
                        hi[a] = std::max(hi[a], p[a]);
                    }
                }
                ws.nearIn[i] = stamp;
                ws.nearby[i] = !hazards.empty() && inDanger.reaches(lo, hi, kNearDanger) ? 1 : 0;
            }
            return ws.nearby[i] != 0;
        };

        // Whether a walk from a to b, crossing `polys`, climbs a face a
        // player cannot (detail::climbsFaces); with no faces, the caller's,
        // only the climb overall is judged (tooSteep)
        const auto climbsCliff = [&](const dtPolyRef* polys, const int count, const Vec& a, const Vec& b) -> bool
        {
            return faces != nullptr && detail::climbsFaces(*faces, mesh, polys, count, a, b);
        };

        auto& nodes = ws.nodes;
        nodes.clear();
        nodes.push_back(Node{ .at = origin, .polys = { startRef, 0 }, .g = 0.0f }); // the start
        nodes.push_back(Node{ .at = target, .polys = { goalRef, 0 } });            // the goal
        constexpr int kGoal = 1;

        // An off-mesh link's end that lands on the ground polygon: 0 its
        // start, 1 its end, -1 none
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

        // A polygon's points, made the first time the search stands on it:
        // an edge's belong to both its polygons, so a neighbour already made
        // has made them. An off-mesh link is a polygon of two vertices, its
        // ends, each set by Detour on the ground polygon it lands on: its
        // points are those two ends
        const auto make = [&](const dtPolyRef ref)
        {
            const uint32_t r = indexOf(ref);
            if (ws.madeIn[r] == stamp)
            {
                return;
            }
            ws.madeIn[r] = stamp;
            listOf(r);
            const dtMeshTile* tile = nullptr;
            const dtPoly*     poly = nullptr;
            mesh.getTileAndPolyByRefUnsafe(ref, &tile, &poly);
            const bool isLink = poly->getType() == DT_POLYTYPE_OFFMESH_CONNECTION;
            ws.kind[r]        = 0;
            if (isLink)
            {
                const auto& con = tile->offMeshCons[static_cast<int>(poly - tile->polys) - tile->header->offMeshBase];
                ws.kind[r]      = detail::kLink | ((con.flags & DT_OFFMESH_CON_BIDIR) == 0 ? detail::kOneWay : 0);
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
                // A disabled ground polygon (one the builder found buried) is
                // never walked. A disabled off-mesh link is: the builder
                // switches off every drop whose two ends a walk already joins,
                // however long that walk (navmesh_builder.cpp,
                // disableRedundantOffMeshLinks), and for her it is the short
                // way down a ledge or a slope -- dropped off, never climbed,
                // as every link rising past the mesh's climb is
                if (((nextPoly->flags & kPolyDisabled) && !nextLink) || (isLink && nextLink))
                {
                    continue;
                }
                const uint32_t n = indexOf(next);

                // An off-mesh link's end where it lands on the ground: one
                // point, the link's own vertex, made once whichever side
                // comes first (a one-way link's far end has no link back)
                if (isLink || nextLink)
                {
                    const dtPolyRef linkRef = isLink ? ref : next;
                    const dtPolyRef ground  = isLink ? next : ref;
                    const int       end     = linkEnd(linkRef, ground);
                    if (end < 0)
                    {
                        continue;
                    }
                    const auto& ends = listOf(isLink ? r : n);
                    if (std::any_of(ends.begin(), ends.end(), [&](const int id) { return nodes[id].end == end; }))
                    {
                        continue;
                    }
                    const dtMeshTile* linkTile = isLink ? tile : nextTile;
                    const dtPoly*     linkPoly = isLink ? poly : nextPoly;
                    const float*      v        = &linkTile->verts[linkPoly->verts[end] * 3];
                    nodes.push_back(Node{ .at = { v[0], v[1], v[2] }, .polys = { ground, linkRef }, .end = static_cast<int8_t>(end) });
                    const int id = static_cast<int>(nodes.size()) - 1;
                    listOf(r).push_back(id);
                    listOf(n).push_back(id);
                    continue;
                }
                // Made already when the neighbour, made first, linked back
                // to this polygon; across a tile border Detour links an
                // edge to four neighbours at most, so a fifth may not
                const auto& mine = listOf(r);
                if (ws.madeIn[n] == stamp &&
                    std::any_of(mine.begin(), mine.end(), [&](const int id) { return nodes[id].end < 0 && (nodes[id].polys[0] == next || nodes[id].polys[1] == next); }))
                {
                    continue;
                }

                // The edge as Detour's own portal reads it: the polygon's
                // side, narrowed to the part a neighbour across a tile
                // border shares
                const float* v0   = &tile->verts[poly->verts[link.edge] * 3];
                const float* v1   = &tile->verts[poly->verts[(link.edge + 1) % poly->vertCount] * 3];
                float        tmin = 0.0f;
                float        tmax = 1.0f;
                if (link.side != 0xff && (link.bmin != 0 || link.bmax != 255))
                {
                    tmin = link.bmin / 255.0f;
                    tmax = link.bmax / 255.0f;
                }
                const Vec left{ v0[0] + (v1[0] - v0[0]) * tmin, v0[1] + (v1[1] - v0[1]) * tmin, v0[2] + (v1[2] - v0[2]) * tmin };
                const Vec right{ v0[0] + (v1[0] - v0[0]) * tmax, v0[1] + (v1[1] - v0[1]) * tmax, v0[2] + (v1[2] - v0[2]) * tmax };

                // Points along it, kPortalSpacing apart where either polygon
                // lies near danger, the end ones a little in from the
                // corners; elsewhere its two ends alone, where a shortest way
                // turns (its middle, on an edge too short for two)
                const float len   = apart(left, right);
                const bool  fine  = nearOf(r, tile, poly) || nearOf(n, nextTile, nextPoly);
                const int   count = fine ? std::max(1, static_cast<int>(std::ceil(len / kPortalSpacing))) : (len > 1.0f ? 2 : 1);
                const float inset = len > 0.0f ? std::min(0.3f, len * 0.1f) / len : 0.0f;
                for (int k = 0; k < count; ++k)
                {
                    const float t = count == 1 ? 0.5f : inset + (1.0f - 2.0f * inset) * static_cast<float>(k) / static_cast<float>(count - 1);
                    nodes.push_back(Node{ .at = { left[0] + (right[0] - left[0]) * t, left[1] + (right[1] - left[1]) * t, left[2] + (right[2] - left[2]) * t },
                                          .polys = { ref, next } });
                    const int id = static_cast<int>(nodes.size()) - 1;
                    listOf(r).push_back(id);
                    listOf(n).push_back(id);
                }
            }
        };

        auto&      open = ws.open;
        const auto push = [&](const float f, const int id)
        {
            open.emplace_back(f, id);
            std::push_heap(open.begin(), open.end(), std::greater<>{});
        };
        open.clear();
        push(greed * apart(origin, target), 0);
        int  expanded = 0;
        bool reached  = false;
        int  endsAt   = kGoal; // the point the route ends at
        while (!open.empty())
        {
            std::pop_heap(open.begin(), open.end(), std::greater<>{});
            const int here = open.back().second;
            open.pop_back();
            if (nodes[here].closed)
            {
                continue;
            }
            nodes[here].closed = true;
            if (here == kGoal || (safeGround && here != 0 && inDanger.exposes(nodes[here].at) < 0))
            {
                reached = true;
                endsAt  = here;
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
            const Vec    at    = nodes[here].at;
            const float  g     = nodes[here].g;
            const int8_t end   = nodes[here].end;
            const auto   relax = [&](const int there, const dtPolyRef via, const bool viaLink)
            {
                Node& next = nodes[there];
                if (there == here || next.closed)
                {
                    return;
                }
                if (viaLink && next.at[1] > at[1] + climb)
                {
                    return; // up a ledge: never climbed
                }
                if (reach > 0.0f && there != kGoal && apart(next.at, origin) + apart(next.at, target) > reach)
                {
                    return; // past the ground the stretch is planned on
                }
                const float step   = apart(at, next.at);
                const float walk   = nodes[here].walk + step;
                const float walked = g + step;
                if (walked >= next.g || (within > 0.0f && walk > within))
                {
                    return; // danger only adds; past the yalms safe ground is looked for within
                }
                // up ground too steep for a player, overall or up a face: read
                // last, the dearest of the step's tests
                if (!viaLink && (detail::tooSteep(at, next.at) || climbsCliff(&via, 1, at, next.at)))
                {
                    return;
                }
                const bool  danger = inDanger(at, next.at);
                const float cost   = walked + (danger ? kDangerCost : 0.0f) + kInsideCost * inDanger.insideOrigin(at, next.at);
                if (cost < next.g)
                {
                    next.g      = cost;
                    next.parent = here;
                    next.via    = via;
                    next.danger = danger;
                    next.link   = viaLink;
                    next.walk   = walk;
                    push(cost + greed * apart(next.at, target), there);
                }
            };
            for (const dtPolyRef ref : polys)
            {
                if (!ref)
                {
                    continue;
                }
                const uint32_t r = indexOf(ref);
                if ((ws.kind[r] & detail::kOneWay) && end == 1)
                {
                    continue; // a one-way link is never walked back
                }
                const bool viaLink = (ws.kind[r] & detail::kLink) != 0;
                for (const int there : ws.points[r])
                {
                    relax(there, ref, viaLink);
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
            detail::trim(ws);
            return route; // not complete: no walk joins them
        }
        std::vector<int> raw;
        for (int at = endsAt; at != -1; at = nodes[at].parent)
        {
            raw.push_back(at);
        }
        std::reverse(raw.begin(), raw.end());

        // Drawn tight: from each kept point, the points ahead in turn whose
        // straight stays on the mesh -- a ray along the ground from the
        // polygon she leaves by, ending on the polygon that point lies on --
        // and out of danger; the farthest found that climbs nothing a player
        // cannot is kept, the search going kShortcutTries points past one
        // that fails. The climb up faces is read only of the straights that
        // pass the rest, farthest first. A step through danger, or across an
        // off-mesh link, is kept as it is. A straight keeps her no longer
        // inside the reaches she set out in than the way it replaces: the
        // yalms inside them, added up along the route to each point
        std::vector<float> insideTo(raw.size(), 0.0f);
        if (inDanger.startsInside())
        {
            for (size_t i = 1; i < raw.size(); ++i)
            {
                insideTo[i] = insideTo[i - 1] + inDanger.insideOrigin(nodes[raw[i - 1]].at, nodes[raw[i]].at);
            }
        }
        const auto straight = [&](const int a, const int k, const bool climb) -> bool
        {
            const Node& from_ = nodes[raw[a]];
            const Node& to_   = nodes[raw[k]];
            if (nodes[raw[a + 1]].link || apart(from_.at, to_.at) > kShortcutReach)
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
            const dtPolyRef last = path[count - 1];
            if (last != to_.polys[0] && last != to_.polys[1] && last != to_.via)
            {
                return false;
            }
            if (detail::tooSteep(from_.at, to_.at))
            {
                return false; // climbs past a player's slope overall
            }
            if (climb)
            {
                return !climbsCliff(path, count, from_.at, to_.at);
            }
            if (inDanger.startsInside() && inDanger.insideOrigin(from_.at, to_.at) > insideTo[k] - insideTo[a] + 0.1f)
            {
                return false; // longer inside the reaches she set out in than the way it replaces
            }
            return !inDanger(from_.at, to_.at); // the danger's sight tests last
        };
        std::vector<int> kept;
        std::vector<int> straights; // an anchor's straights that pass all but the climb, nearest first
        const int        last = static_cast<int>(raw.size()) - 1;
        for (int anchor = 0; anchor < last;)
        {
            int best = anchor + 1;
            if (!nodes[raw[best]].danger && !nodes[raw[best]].link)
            {
                straights.clear();
                int misses = 0;
                for (int k = anchor + 2; k <= last && !nodes[raw[k]].danger && misses <= kShortcutTries; ++k)
                {
                    if (straight(anchor, k, false))
                    {
                        straights.push_back(k);
                        misses = 0;
                    }
                    else
                    {
                        ++misses;
                    }
                }
                for (auto it = straights.rbegin(); it != straights.rend(); ++it)
                {
                    if (straight(anchor, *it, true))
                    {
                        best = *it;
                        break;
                    }
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
        detail::trim(ws);
        return route;
    }

    // The route from `from` to `to` that keeps out of danger (searchCore)
    inline auto searchAvoiding(const dtNavMesh& mesh, const position_t& from, const position_t& to, const float* fromExtents, const float* toExtents,
                               const std::vector<Hazard>& hazards, const HazardSight& sight, const float reach = 0.0f, const Faces* faces = nullptr)
        -> std::optional<Route>
    {
        return searchCore(mesh, from, to, fromExtents, toExtents, hazards, sight, reach, 0.0f, faces);
    }

    // The way from `from` to the nearest ground out of every reach, walking
    // at most `within` yalms: the same points and steps, the first point out
    // of every reach the goal, a way through no danger before one through
    // the fewest steps of it. Complete and empty when `from` is out of every
    // reach already; not complete when no such ground lies within
    inline auto searchSafeGround(const dtNavMesh& mesh, const position_t& from, const float* fromExtents, const std::vector<Hazard>& hazards,
                                 const HazardSight& sight, const float within, const Faces* faces = nullptr) -> std::optional<Route>
    {
        return searchCore(mesh, from, std::nullopt, fromExtents, fromExtents, hazards, sight, 0.0f, within, faces);
    }
} // namespace cardian::route
