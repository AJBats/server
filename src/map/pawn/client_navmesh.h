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
// (pawn/route_search.h, kTightCost), threading their middle. A drop link
// is searched for straight out from its ledge's edge (squareOut).
// Header-only: the navmesh builder (map core) reads it, and xi_test links it.

#include <Recast.h>

#include <algorithm>
#include <cmath>
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

// The erosion of a navmesh built for the client: the body's margin,
// `bodyCells` (Recast's walkableRadius) less kTrimCells, and the berth past
// it, out to `berth` from the edges, kept and marked kTightArea. Recast
// measures each erosion from the edges as they stand, so a second erosion of
// the rest reaches the berth. Any other build: Recast's own erosion
inline auto erode(rcContext* ctx, const int bodyCells, const bool forClient, const float berth, rcCompactHeightfield& chf) -> bool
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
    return true;
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
