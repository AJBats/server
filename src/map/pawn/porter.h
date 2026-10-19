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

class CCharEntity;

// The chocobo porter (ROADMAP T, "The chocobo porter"): the player rents a
// chocobo at a city's stable, the game's own rental, and is asked where to
// as it sets him down in its field (the Link's PORTER_PLACES, the addon's
// Ride to, each place with its ride time); he picks a city, a conquest
// outpost or a teleport crag (PORTER_GO), and is ridden there.
//
// The server plans; his addon drives. His body stays his client's -- no
// charm, no controller of the porter's, no step of the server's -- so his
// menus, chat and macros work as ever, and the server reads where he is from
// his client's own position packets. In each zone the trek's planner picks
// his next zone line, or the place itself in its zone (pawn_travel.h
// nextHop, a rider's), the trek's route search plans his way there over the
// zone's mesh (travel::routeAvoiding, round nothing: no monster aggroes a
// rider), and the way goes to his addon a stretch at a time (PORTER_ROUTE),
// which runs his client along it with the client's own auto-run, on through
// each zone line so his client crosses it itself. Sent again as he enters
// each zone, as he strays from it (planned from where his client has him),
// and as he nears the end of the stretch sent. At a city's gate he is got
// down short of its line, the game taking no rented chocobo into a city,
// and the addon runs him through on foot; an outpost or a crag he rides up
// to.
//
// His client can catch on ground the mesh would have let a body the server
// walks clip past: the addon tells of a run that makes no headway
// (PORTER_STUCK), and the server plans round it from where he stands, then,
// at the same spot again, sets him down a few yalms on along his way, the
// game's own same-zone position set (porter_math.h, "Snags"). Every snag is
// in the map log with where he was and where he was going.
//
// The ride ends there; or when he takes the reins back -- his movement,
// which the addon sees and answers with PORTER_STOP, or the Ride to row --;
// when his chocobo is gone (its rental run out, or anything else gets him
// down); when a monster is on him; with no way on from where he is; or
// snagged past mending. His addon is told why (PORTER_STATE). A held pause
// holds the ride.
namespace pawn::porter
{
    // His rental has just set him down in its field (modules/cardian/lua/
    // chocobo_ride.lua): once he has landed and his client is in, his addon
    // is asked where to (PORTER_PLACES, one-way)
    void askAfterRental(CCharEntity* PChar);
} // namespace pawn::porter
