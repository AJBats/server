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

#include "formation_math.h"

#include "common/types/fn.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <functional>
#include <numbers>
#include <optional>
#include <queue>
#include <unordered_map>
#include <utility>
#include <vector>

// The local planner: this tick's step, planned over a one-yalm grid round
// the cardian against a field of soft shapes -- the player's body and wake
// today, other things tomorrow -- with the navmesh saying which cells can
// be walked at all. The navmesh's own nodes are polygons seven yalms
// across and more, too coarse to bend a walk a lane's width; this grid is
// the dense layer under it. Where there is room the cheap way round wins;
// where the walls leave nothing cheaper the walk goes through. The field
// is a suggestion: the danger map and the mesh still rule, above and
// below. Pure -- the grid, the field and a walkability callback -- so
// xi_test pins it (src/test/tests/cardian_planner_tests.cpp).
namespace cardian::planner
{
    // A shape of the field: extra cost per yalm walked inside it. A
    // capsule is a disc swept along a segment (the player's wake).
    struct Disc
    {
        float x      = 0.0f;
        float z      = 0.0f;
        float radius = 0.0f;
        float cost   = 0.0f;
    };

    struct Capsule
    {
        float ax     = 0.0f;
        float az     = 0.0f;
        float bx     = 0.0f;
        float bz     = 0.0f;
        float radius = 0.0f;
        float cost   = 0.0f;
    };

    struct Field
    {
        std::vector<Disc>    discs;
        std::vector<Capsule> capsules;

        auto empty() const -> bool
        {
            return discs.empty() && capsules.empty();
        }

        // The extra cost per yalm at a point: every shape it lies in, summed
        auto at(const float x, const float z) const -> float
        {
            float extra = 0.0f;
            for (const auto& d : discs)
            {
                if (formation::planarDistance(d.x, d.z, x, z) < d.radius)
                {
                    extra += d.cost;
                }
            }
            for (const auto& c : capsules)
            {
                if (formation::segmentClosest(formation::Circle{ x, z, 0.0f }, c.ax, c.az, c.bx, c.bz) < c.radius)
                {
                    extra += c.cost;
                }
            }
            return extra;
        }

        // Does the straight walk a->b pass through any shape? The cheap
        // question before planning at all. A capsule is asked as discs
        // along its axis half a radius apart, so their union covers all
        // but a sliver of its edge.
        auto reaches(const float ax, const float az, const float bx, const float bz) const -> bool
        {
            for (const auto& d : discs)
            {
                if (formation::segmentClosest(formation::Circle{ d.x, d.z, d.radius }, ax, az, bx, bz) < d.radius)
                {
                    return true;
                }
            }
            for (const auto& c : capsules)
            {
                const int samples = std::max(1, static_cast<int>(std::ceil(formation::planarDistance(c.ax, c.az, c.bx, c.bz) / std::max(c.radius * 0.5f, 0.25f))));
                for (int i = 0; i <= samples; ++i)
                {
                    const float t = static_cast<float>(i) / static_cast<float>(samples);
                    if (formation::segmentClosest(formation::Circle{ c.ax + (c.bx - c.ax) * t, c.az + (c.bz - c.az) * t, c.radius }, ax, az, bx, bz) < c.radius)
                    {
                        return true;
                    }
                }
            }
            return false;
        }
    };

    struct Query
    {
        float sx            = 0.0f; // where she stands
        float sz            = 0.0f;
        float gx            = 0.0f; // the local goal; beyond the window it is planned to the window's edge toward it
        float gz            = 0.0f;
        int   window        = 12;   // cells either side of her
        float sideBias      = 0.0f; // +1 / -1: last tick's side of the start->goal line, kept a little cheaper (0: none)
        float biasCost      = 0.1f; // per yalm on the other side
        int   maxExpansions = 1500;
    };

    // Which cells can be walked: the navmesh, asked at a cell's centre.
    // Each cell is asked at most once per plan.
    using Walkable = FnRef<bool(float x, float z)>;

    struct Plan
    {
        std::vector<std::pair<float, float>> points;          // from where she stands to the goal, cell centres between
        float                                cost     = 0.0f; // the walk with its extras
        float                                length   = 0.0f; // yalms
        float                                straight = 0.0f; // the straight line, for the record
    };

    namespace detail
    {
        inline auto key(const int cx, const int cz) -> int64_t
        {
            return (static_cast<int64_t>(cx) << 32) ^ static_cast<int64_t>(static_cast<uint32_t>(cz));
        }

        inline auto cellOf(const float v) -> int
        {
            return static_cast<int>(std::floor(v));
        }

        inline auto centre(const int c) -> float
        {
            return static_cast<float>(c) + 0.5f;
        }
    } // namespace detail

    // A* over the grid: eight neighbours, a step's cost its length times one
    // plus the field's extra at the cell entered, no cutting of corners past
    // a cell that cannot be walked, the octile distance as the heuristic
    // (admissible: extras are never negative).
    inline auto plan(const Query& q, const Field& field, Walkable walkable) -> std::optional<Plan>
    {
        using namespace detail;
        constexpr float kSqrt2 = std::numbers::sqrt2_v<float>;

        const int  sx         = cellOf(q.sx);
        const int  sz         = cellOf(q.sz);
        const int  gx         = std::clamp(cellOf(q.gx), sx - q.window, sx + q.window);
        const int  gz         = std::clamp(cellOf(q.gz), sz - q.window, sz + q.window);
        const bool goalInside = gx == cellOf(q.gx) && gz == cellOf(q.gz);

        const float lx   = q.gx - q.sx;
        const float lz   = q.gz - q.sz;
        const auto  side = [&](const float x, const float z) -> float
        {
            const float c = lx * (z - q.sz) - lz * (x - q.sx);
            return c > 0.0f ? 1.0f : (c < 0.0f ? -1.0f : 0.0f);
        };

        std::unordered_map<int64_t, bool> walk;
        const auto                        canWalk = [&](const int cx, const int cz) -> bool
        {
            if (cx == sx && cz == sz)
            {
                return true; // she stands there, on the mesh or not
            }
            const auto k = key(cx, cz);
            if (const auto it = walk.find(k); it != walk.end())
            {
                return it->second;
            }
            const bool ok = walkable(centre(cx), centre(cz));
            walk.emplace(k, ok);
            return ok;
        };

        struct Node
        {
            float   g      = 0.0f;
            int     cx     = 0;
            int     cz     = 0;
            int64_t parent = 0;
        };
        struct Open
        {
            float f  = 0.0f;
            float g  = 0.0f;
            int   cx = 0;
            int   cz = 0;
            auto  operator>(const Open& o) const -> bool
            {
                return f > o.f;
            }
        };
        const auto h = [&](const int cx, const int cz) -> float
        {
            const float dx = static_cast<float>(std::abs(cx - gx));
            const float dz = static_cast<float>(std::abs(cz - gz));
            return dx + dz + (kSqrt2 - 2.0f) * std::min(dx, dz);
        };

        std::unordered_map<int64_t, Node>                                 best;
        std::priority_queue<Open, std::vector<Open>, std::greater<Open>> open;
        best[key(sx, sz)] = Node{ 0.0f, sx, sz, key(sx, sz) };
        open.push(Open{ h(sx, sz), 0.0f, sx, sz });

        int  expansions = 0;
        bool found      = false;
        while (!open.empty())
        {
            const Open cur = open.top();
            open.pop();
            const auto ck = key(cur.cx, cur.cz);
            if (cur.g > best[ck].g + 1e-4f)
            {
                continue; // a stale entry: the cell was reached cheaper since
            }
            if (cur.cx == gx && cur.cz == gz)
            {
                found = true;
                break;
            }
            if (++expansions > q.maxExpansions)
            {
                break;
            }
            for (int dz = -1; dz <= 1; ++dz)
            {
                for (int dx = -1; dx <= 1; ++dx)
                {
                    if (dx == 0 && dz == 0)
                    {
                        continue;
                    }
                    const int nx = cur.cx + dx;
                    const int nz = cur.cz + dz;
                    if (std::abs(nx - sx) > q.window || std::abs(nz - sz) > q.window || !canWalk(nx, nz))
                    {
                        continue;
                    }
                    const bool diagonal = dx != 0 && dz != 0;
                    if (diagonal && (!canWalk(cur.cx + dx, cur.cz) || !canWalk(cur.cx, cur.cz + dz)))
                    {
                        continue;
                    }
                    const float len  = diagonal ? kSqrt2 : 1.0f;
                    float       step = len * (1.0f + field.at(centre(nx), centre(nz)));
                    if (q.sideBias != 0.0f && side(centre(nx), centre(nz)) == -q.sideBias)
                    {
                        step += len * q.biasCost;
                    }
                    const float g  = cur.g + step;
                    const auto  nk = key(nx, nz);
                    if (const auto it = best.find(nk); it == best.end() || g < it->second.g - 1e-4f)
                    {
                        best[nk] = Node{ g, nx, nz, ck };
                        open.push(Open{ g + h(nx, nz), g, nx, nz });
                    }
                }
            }
        }
        if (!found)
        {
            return std::nullopt;
        }

        std::vector<std::pair<int, int>> cells;
        for (auto k = key(gx, gz);;)
        {
            const auto& n = best[k];
            cells.emplace_back(n.cx, n.cz);
            if (n.cx == sx && n.cz == sz)
            {
                break;
            }
            k = n.parent;
        }
        std::reverse(cells.begin(), cells.end());

        Plan p;
        p.cost = best[key(gx, gz)].g;
        p.points.reserve(cells.size());
        p.points.emplace_back(q.sx, q.sz);
        for (std::size_t i = 1; i + 1 < cells.size(); ++i)
        {
            p.points.emplace_back(centre(cells[i].first), centre(cells[i].second));
        }
        if (cells.size() > 1)
        {
            p.points.emplace_back(goalInside ? q.gx : centre(gx), goalInside ? q.gz : centre(gz));
        }
        for (std::size_t i = 1; i < p.points.size(); ++i)
        {
            p.length += formation::planarDistance(p.points[i - 1].first, p.points[i - 1].second, p.points[i].first, p.points[i].second);
        }
        p.straight = formation::planarDistance(q.sx, q.sz, q.gx, q.gz);
        return p;
    }

    // The point `reach` yalms along the plan from its start; its end when
    // the plan is shorter
    inline auto along(const Plan& p, const float reach) -> std::pair<float, float>
    {
        float walked = 0.0f;
        for (std::size_t i = 1; i < p.points.size(); ++i)
        {
            const auto& a   = p.points[i - 1];
            const auto& b   = p.points[i];
            const float seg = formation::planarDistance(a.first, a.second, b.first, b.second);
            if (walked + seg >= reach)
            {
                const float t = seg > 0.0f ? (reach - walked) / seg : 0.0f;
                return { a.first + (b.first - a.first) * t, a.second + (b.second - a.second) * t };
            }
            walked += seg;
        }
        return p.points.empty() ? std::pair{ 0.0f, 0.0f } : p.points.back();
    }
} // namespace cardian::planner
