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
// walks, and the body's width already keeps a route off every real edge (the
// zone's border, a ledge's lip, is no join). Its links
// reach past the margin a body's width keeps on both sides of a ledge, and a
// link whose way down meets the zone's collision -- a tree's trunk in the
// hole the navmesh leaves round it -- is taken out (clearDrops).
// Header-only: the navmesh builder (map core) reads it, and xi_test links it.

#include <algorithm>
#include <cmath>
#include <cstddef>
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

// Whether the segment p-q passes through the triangle a, b, c
inline auto segmentHits(const float* p, const float* q, const float* a, const float* b, const float* c) -> bool
{
    const float d[3]{ q[0] - p[0], q[1] - p[1], q[2] - p[2] };
    const float e1[3]{ b[0] - a[0], b[1] - a[1], b[2] - a[2] };
    const float e2[3]{ c[0] - a[0], c[1] - a[1], c[2] - a[2] };
    const float h[3]{ d[1] * e2[2] - d[2] * e2[1], d[2] * e2[0] - d[0] * e2[2], d[0] * e2[1] - d[1] * e2[0] };
    const float det = e1[0] * h[0] + e1[1] * h[1] + e1[2] * h[2];
    if (std::fabs(det) < 1e-8f)
    {
        return false;
    }

    const float inv = 1.0f / det;
    const float s[3]{ p[0] - a[0], p[1] - a[1], p[2] - a[2] };
    const float u   = inv * (s[0] * h[0] + s[1] * h[1] + s[2] * h[2]);
    if (u < 0.0f || u > 1.0f)
    {
        return false;
    }

    const float r[3]{ s[1] * e1[2] - s[2] * e1[1], s[2] * e1[0] - s[0] * e1[2], s[0] * e1[1] - s[1] * e1[0] };
    const float v = inv * (d[0] * r[0] + d[1] * r[1] + d[2] * r[2]);
    if (v < 0.0f || u + v > 1.0f)
    {
        return false;
    }

    const float t = inv * (e2[0] * r[0] + e2[1] * r[1] + e2[2] * r[2]);
    return t >= 0.0f && t <= 1.0f;
}

// Whether a triangle is ground (or a ceiling): its normal within kSlope of
// up or down, either winding
inline auto flatish(const float* a, const float* b, const float* c) -> bool
{
    const float e1[3]{ b[0] - a[0], b[1] - a[1], b[2] - a[2] };
    const float e2[3]{ c[0] - a[0], c[1] - a[1], c[2] - a[2] };
    const float n[3]{ e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2], e1[0] * e2[1] - e1[1] * e2[0] };
    const float len = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
    return len > 1e-8f && std::fabs(n[1]) / len > std::cos(kSlope * 3.14159265f / 180.0f);
}

// Whether a body dropping by a link from `start` to `end` (Detour's space, y
// up) meets the zone's own collision on its way: it walks off at its
// sphere's height, kStep over the start, straight out to above the landing,
// and falls there. A tree's trunk or a rock the link spans is in the way; a
// ledge's own face is under it, and ground he walks on toward the lip --
// rising, it may reach the walk's height -- is never in it. `verts` and
// `indices` are a tile's gathered triangles in Detour's space, the first
// `count` the zone's own
inline auto dropBlocked(const float* start, const float* end, const std::vector<float>& verts, const std::vector<int>& indices, const std::size_t count) -> bool
{
    const float from[3]{ start[0], start[1] + kStep, start[2] };
    const float over[3]{ end[0], start[1] + kStep, end[2] };
    const float land[3]{ end[0], end[1] + kStep, end[2] };
    const float lo[3]{ std::min(from[0], over[0]), land[1], std::min(from[2], over[2]) };
    const float hi[3]{ std::max(from[0], over[0]), from[1], std::max(from[2], over[2]) };
    for (std::size_t t = 0; t < count && t * 3 + 2 < indices.size(); ++t)
    {
        const float* a     = &verts[static_cast<std::size_t>(indices[t * 3 + 0]) * 3];
        const float* b     = &verts[static_cast<std::size_t>(indices[t * 3 + 1]) * 3];
        const float* c     = &verts[static_cast<std::size_t>(indices[t * 3 + 2]) * 3];
        bool         apart = false;
        for (int axis = 0; axis < 3 && !apart; ++axis)
        {
            apart = std::max({ a[axis], b[axis], c[axis] }) < lo[axis] || std::min({ a[axis], b[axis], c[axis] }) > hi[axis];
        }

        if (!apart && !flatish(a, b, c) && (segmentHits(from, over, a, b, c) || segmentHits(over, land, a, b, c)))
        {
            return true;
        }
    }

    return false;
}

// The drop links of a navmesh built for the client (the builder's, one
// tile's) whose way down meets the zone's collision (dropBlocked), taken out;
// any other build's left alone
template <typename Links>
inline void clearDrops(Links& links, const std::vector<float>& verts, const std::vector<int>& indices, const std::size_t count, const bool forClient)
{
    if (!forClient)
    {
        return;
    }

    int kept = 0;
    for (int i = 0; i < links.count; ++i)
    {
        const float* l = &links.verts[static_cast<std::size_t>(i) * 6];
        if (dropBlocked(l, l + 3, verts, indices, count))
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

    links.count = kept;
    links.verts.resize(static_cast<std::size_t>(kept) * 6);
    links.rad.resize(kept);
    links.flags.resize(kept);
    links.areas.resize(kept);
    links.dir.resize(kept);
}

} // namespace cardian::client
