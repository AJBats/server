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
#include "common/types/position.h"

#include <optional>
#include <string>
#include <vector>

class CBaseEntity;
class CCharEntity;
class CZone;

// Doors on a cardian's way. The collision data the navmesh is built from
// carries no door slabs, so a path runs straight through a closed door;
// the client stops a player there and opens the door for them as they
// walk up. This does the same for a cardian from the server side: a
// generic door she opens, and a closed door she may not open is a wall
// her walks end short of -- or, where the client's own event walks a
// player through it, she crosses it as he would be walked
// (CPawnController::DoorTick, pawn_door_cross.cpp).
//
// The doors' planes and their crossings are read off the client's own
// files by tools/walls/doors.py, into .cache/cardian_doors.json: a door the
// table lacks is no wall to her.
namespace pawn::doors
{
    // The table read, said in the map log; a missing file is said loudly.
    // Read at the pawn module's start, or at first use
    void loadTable();

    // A crossing of a door: where she stands to start it, on the `from` side
    // (+1 or -1 of the door's plane), and where the client's event leaves a
    // player on its other side; whether the event is the door's own -- one
    // the server's scripts start for a player at it, offered anyone --
    // whether it asks him first, and whether `stand` is where the event
    // places him, not a step out from the door. Event 0: made from the
    // door's plane, a step out on each side
    struct Crossing
    {
        int        event = 0;
        int        from  = 0;
        position_t stand{};
        position_t land{};
        bool       own    = false;
        bool       menu   = false;
        bool       placed = false;
    };

    // Open every closed generic door within `lane` yalms of the way ahead
    // of her -- the segment from where she stands to `reach` yalms along
    // her facing -- exactly as the game opens one for a player: the open
    // animation to everyone in range, closing again seven seconds later.
    // A door with a script of its own, or a handler of the interaction
    // framework's (a key, a quest, a cutscene), is left to the player; a
    // ferry gate or an elevator door keeps to its timetable; one the game
    // makes untargetable -- a lever's door, a drawbridge, a barrier -- opens
    // only by its own mechanism. Returns how many doors opened this call.
    auto openAhead(CCharEntity* PPawn, float reach, float lane) -> int;

    // A closed door she may not open -- one with a scene or a script of its
    // own, a lift's -- is a wall to her as it is to the player, though the
    // mesh has none. The point `to`, seen from `from` (the player), brought
    // back to `from`'s side, a yalm short of the first such door the way
    // crosses; nullopt when it crosses none
    auto keepToNearSide(CCharEntity* PPawn, const position_t& from, const position_t& to) -> std::optional<position_t>;

    // The first closed door she may not open that a walk's route meets --
    // its points in order, her own spot first: the door, the route's point
    // the leg meeting it ends at, the spot a yalm short of it on that leg,
    // and her side of the door
    struct Ahead
    {
        uint32     npc  = 0;
        size_t     leg  = 0;
        position_t stop{};
        int        side = 0;
    };
    auto firstOnRoute(const CBaseEntity* PWalker, const std::vector<position_t>& route) -> std::optional<Ahead>;
    auto firstOnRoute(const CZone* PZone, const std::vector<position_t>& route) -> std::optional<Ahead>;

    // Whether the door still stands closed in a cardian's way
    auto blocks(CZone* PZone, uint32 npc) -> bool;

    // The crossing she takes from her side of the door: the table's from
    // that side, or the reverse of the other side's -- the way in a door
    // walks a player is a way back out. Off her way to the player only the
    // door's own counts, the one the game offers anyone at it: a scene's
    // walk through it is a mission's, and a door with none holds her. On
    // her way to him (`towardHim`) the door is always open (the user,
    // 2026-10-23): any event's crossing, and with none a step out on each
    // side of its plane. Its two spots stand on the zone's mesh; nullopt
    // when either has no ground
    auto crossingFor(CZone* PZone, uint32 npc, int side, bool towardHim) -> std::optional<Crossing>;

    // The side of a door's plane a point stands on, +1 or -1; 0 for a door
    // the table lacks
    auto sideOf(uint32 npc, const position_t& at) -> int;

    // The door's name for the map log: its client name, or its script name
    auto nameOf(uint32 npc) -> std::string;
} // namespace pawn::doors
