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

#include "common/cbasetypes.h"

#include <vector>

class CZone;
class dtNavMesh;

// The chocobo porter's own navmesh of a zone (ROADMAP T, "The chocobo
// porter"): the route a porter plans runs the player's own client, which
// collides by the client's rule, so it is planned on a navmesh built with
// that rule (pawn/client_navmesh.h) and the rider's width, pawn.PORTER_WIDTH,
// kept where there is room -- never on the zone's, which monsters walk and
// which stays upstream's. A town's is built the same way, with the town's
// props merged into its collision (tools/walls/props.py), and every route a
// cardian plans in a town keeps to it (ROADMAP W, "Town walks";
// pawn_travel.h townMesh). Each zone a ride can plan in, and each town, has
// its navmesh precalculated, by the map run as a tool (./dev-servers.ps1
// porter-navmeshes, which sets CARDIAN_PORTER_NAVMESH_PRECALC=1), into
// .cache/cardian_navmeshes/<zone>.porter.nav, beside a recipe of what it
// was built with (<zone>.porter.recipe). The map never builds one while it
// serves: a zone whose file is missing, or whose recipe is not today's
// settings', is said loudly, and its rides and walks keep to the zone's navmesh.
namespace pawn::porter::mesh
{
    // Whether `zone` is a town whose cardians walk this navmesh: a zone of
    // type City, or a ferry's deck between Selbina and Mhaura (the user,
    // 2026-10-23: the zone's own navmesh walked her into the ship's walls),
    // with collision and a navmesh of its own (a Mog House's residential
    // area has none)
    auto isTown(CZone* zone) -> bool;

    // Whether a cardian's walks in `zone` keep to this navmesh (pawn_travel.h
    // townMesh): a town's, with pawn.TOWN_CLIENT_NAVMESH; and, on trial with
    // pawn.CLIENT_NAVMESH_EVERYWHERE (the user, 2026-10-24), any other zone
    // one was built for. Safe from any thread
    auto walkedOnClient(CZone* zone) -> bool;

    // The zone's porter navmesh, read from its file the first time it is
    // asked for -- by a ride's trip check, on a worker (porter.cpp), so a
    // ride's own plan finds it in memory -- and kept; nullptr when it has
    // none for today's settings, said loudly, once a zone. Safe from any
    // thread; the mesh, read-only once read, outlives every reader
    auto of(CZone* zone) -> const dtNavMesh*;

    // At the map's start: which of `zones` have a porter navmesh for today's
    // settings, in one line, loudly for those that have none
    void check(const std::vector<uint16>& zones);

    // The precalculation run: builds the porter navmesh of each of `zones`
    // in turn that has none for today's settings, writes its file and its
    // recipe, says what came of it, and ends the map, with code 0 when every
    // zone's is written
    void precalc(const std::vector<uint16>& zones);
} // namespace pawn::porter::mesh
