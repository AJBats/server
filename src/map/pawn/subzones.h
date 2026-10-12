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

#include "route_search.h"

#include <cstdint>
#include <vector>

// A zone's sub-zones (the travel walk): the pieces of its navmesh no walk
// joins. North Gustaberg is two -- its lowlands, and the high ground with
// the line to Konschtat Highlands -- and a traveller takes the right gate
// out of South Gustaberg to reach the high ground; a dungeon's floors are
// pieces joined only by the drops between them. Polygons share a piece when
// a link joins them both ways: the ground's own edges and the mesh's
// off-mesh links walked both ways. An off-mesh link (a ledge, a step) is
// climbed only where it rises no more than the mesh's own climb (its
// walkableClimb, 1 yalm): a taller one is dropped down and never climbed,
// whatever the mesh marks it, as is a one-way link -- a drop, from one piece
// into another, never back. Every polygon's piece is kept in four bytes: the
// world's meshes hold millions.
namespace cardian::subzones
{
    using Piece = uint32_t; // 1 and up; 0 is none (off the mesh, or a zone no mesh knows)

    struct Drop
    {
        Piece      from{};
        Piece      to{};
        position_t top{};    // where she steps off
        position_t bottom{}; // where she lands
    };

    struct Pieces
    {
        std::vector<std::size_t> tileBase; // each tile's first polygon in `pieceOf`
        std::vector<Piece>       pieceOf;  // every polygon's piece, 0 for a disabled one or a link
        std::vector<int>         polys;    // each piece's polygon count, by piece
        std::vector<Drop>        drops;

        // The piece of a polygon of this mesh
        auto of(const dtNavMesh& mesh, const dtPolyRef ref) const -> Piece
        {
            unsigned int salt = 0;
            unsigned int tile = 0;
            unsigned int poly = 0;
            mesh.decodePolyId(ref, salt, tile, poly);
            if (tile + 1 >= tileBase.size() || tileBase[tile] + poly >= tileBase[tile + 1])
            {
                return 0;
            }
            return pieceOf[tileBase[tile] + poly];
        }
    };

    namespace detail
    {
        inline auto root(std::vector<uint32_t>& parent, uint32_t i) -> uint32_t
        {
            while (parent[i] != i)
            {
                parent[i] = parent[parent[i]];
                i         = parent[i];
            }
            return i;
        }
    } // namespace detail

    // The pieces of a zone's mesh
    inline auto piecesOf(const dtNavMesh& mesh) -> Pieces
    {
        Pieces out;
        out.tileBase.reserve(mesh.getMaxTiles() + 1);
        std::size_t total = 0;
        for (int t = 0; t < mesh.getMaxTiles(); ++t)
        {
            out.tileBase.push_back(total);
            const dtMeshTile* tile = mesh.getTile(t);
            total += tile != nullptr && tile->header != nullptr ? tile->header->polyCount : 0;
        }
        out.tileBase.push_back(total);

        constexpr uint32_t    kNone = UINT32_MAX;
        std::vector<uint32_t> parent(total, kNone); // ground polygons only; kNone for the rest
        const auto            flat = [&](const dtPolyRef ref) -> uint32_t
        {
            unsigned int salt = 0;
            unsigned int tile = 0;
            unsigned int poly = 0;
            mesh.decodePolyId(ref, salt, tile, poly);
            return tile + 1 < out.tileBase.size() ? static_cast<uint32_t>(out.tileBase[tile] + poly) : kNone;
        };
        const auto ground = [&](const uint32_t i) { return i != kNone && i < parent.size() && parent[i] != kNone; };

        for (int t = 0; t < mesh.getMaxTiles(); ++t)
        {
            const dtMeshTile* tile = mesh.getTile(t);
            if (tile == nullptr || tile->header == nullptr)
            {
                continue;
            }
            for (int p = 0; p < tile->header->polyCount; ++p)
            {
                const dtPoly& poly = tile->polys[p];
                if (poly.getType() == DT_POLYTYPE_GROUND && (poly.flags & route::kPolyDisabled) == 0)
                {
                    const auto i = static_cast<uint32_t>(out.tileBase[t] + p);
                    parent[i]    = i;
                }
            }
        }

        struct OneWay
        {
            uint32_t   from{}, to{};
            position_t top{}, bottom{};
        };
        std::vector<OneWay> oneWays;
        for (int t = 0; t < mesh.getMaxTiles(); ++t)
        {
            const dtMeshTile* tile = mesh.getTile(t);
            if (tile == nullptr || tile->header == nullptr)
            {
                continue;
            }
            for (int p = 0; p < tile->header->polyCount; ++p)
            {
                const dtPoly&  poly = tile->polys[p];
                const uint32_t self = static_cast<uint32_t>(out.tileBase[t] + p);
                if (poly.getType() == DT_POLYTYPE_GROUND)
                {
                    if (!ground(self))
                    {
                        continue;
                    }
                    for (unsigned int l = poly.firstLink; l != DT_NULL_LINK; l = tile->links[l].next)
                    {
                        const uint32_t other = flat(tile->links[l].ref);
                        if (ground(other))
                        {
                            parent[detail::root(parent, self)] = detail::root(parent, other);
                        }
                    }
                    continue;
                }
                if (poly.flags & route::kPolyDisabled)
                {
                    continue;
                }
                // an off-mesh link: its start is the land it links to by edge 0, its end by edge 1
                uint32_t start = kNone;
                uint32_t end   = kNone;
                for (unsigned int l = poly.firstLink; l != DT_NULL_LINK; l = tile->links[l].next)
                {
                    (tile->links[l].edge == 0 ? start : end) = flat(tile->links[l].ref);
                }
                if (!ground(start) || !ground(end))
                {
                    continue;
                }
                // walked start to end unless that climbs past the mesh's
                // climb; end to start only on a two-way link, and the same.
                // Its ends as the route search reads them: the link's own
                // vertices, which Detour snaps to the ground it lands on
                const dtOffMeshConnection& con     = tile->offMeshCons[p - tile->header->offMeshBase];
                const float*               atStart = &tile->verts[poly.verts[0] * 3];
                const float*               atEnd   = &tile->verts[poly.verts[1] * 3];
                const float                climb   = tile->header->walkableClimb;
                const bool                 forward = atEnd[1] - atStart[1] <= climb; // Detour's y is up
                const bool                 back    = (con.flags & DT_OFFMESH_CON_BIDIR) != 0 && atStart[1] - atEnd[1] <= climb;
                if (forward && back)
                {
                    parent[detail::root(parent, start)] = detail::root(parent, end);
                }
                else if (forward)
                {
                    oneWays.push_back({ start, end, route::fromDetour(atStart), route::fromDetour(atEnd) });
                }
                else if (back)
                {
                    oneWays.push_back({ end, start, route::fromDetour(atEnd), route::fromDetour(atStart) });
                }
            }
        }

        // number the pieces from 1, in the order their first polygon comes
        out.pieceOf.assign(total, 0);
        std::vector<Piece> number(total, 0);
        out.polys.push_back(0);
        for (uint32_t i = 0; i < total; ++i)
        {
            if (parent[i] == kNone)
            {
                continue;
            }
            const uint32_t r = detail::root(parent, i);
            if (number[r] == 0)
            {
                number[r] = static_cast<Piece>(out.polys.size());
                out.polys.push_back(0);
            }
            out.pieceOf[i] = number[r];
            ++out.polys[number[r]];
        }
        for (const auto& way : oneWays)
        {
            const Piece from = out.pieceOf[way.from];
            const Piece to   = out.pieceOf[way.to];
            if (from != to)
            {
                out.drops.push_back({ from, to, way.top, way.bottom });
            }
        }
        return out;
    }

    // The search reach for a body standing on the mesh, and for a zone line's
    // ends: a line's trigger often lies some yalms past the mesh's edge
    constexpr float kBodyExtents[] = { 2.5f, 5.0f, 2.5f };
    constexpr float kLineExtents[] = { 30.0f, 60.0f, 30.0f };

    // The piece a point stands on within `extents`, 0 off the mesh
    inline auto pieceAt(const dtNavMesh& mesh, const Pieces& pieces, const position_t& point, const float* extents) -> Piece
    {
        dtNavMeshQuery query;
        if (dtStatusFailed(query.init(&mesh, 64)))
        {
            return 0;
        }
        dtQueryFilter filter;
        filter.setExcludeFlags(route::kPolyDisabled);
        const auto at  = route::toDetour(point);
        dtPolyRef  ref = 0;
        if (dtStatusFailed(query.findNearestPoly(at.data(), extents, &filter, &ref, nullptr)) || !ref)
        {
            return 0;
        }
        return pieces.of(mesh, ref);
    }
} // namespace cardian::subzones
