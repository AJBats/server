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

#include <chrono>

// Drills: travel tests with no client (ROADMAP T, "Drills"). A drill sends a
// cardian on a trek with nobody online, watches her, and reports how it
// went. Dev only: the map reads nothing while cardian.DRILLS is off, which
// it is everywhere but the dev profile.
//
// tools/drills/*.yaml names the drills of a run -- each one's character,
// the zone and spot she starts at, the zone she walks to, her time limit,
// and a level for the drill if not her own -- and `./dev-servers.ps1 drill
// <file>` hands one to the map as log/drill/run.yaml with a run id on top.
// The map answers in log/drill/results.jsonl, lines for this run's id: one
// as it takes the run (or why it refuses it), one per drill as it ends,
// then one saying the run is done. It stands each character up with no
// player (pawn::standForDrill), gives her a travel order, and watches her,
// the clock stopped while a pause holds. A run file written before the
// map's first look at it, a moment after the map starts, is a stale one
// and is never run.
namespace pawn::drill
{
    // The zone tick's look at the run file, and each drill's watch: at most
    // twice a second, whichever zone ticks
    void tick();

    // What her trek tells of itself (TravelTick, TrekSafeWalk, the rest
    // across a line), counted for a drill's walker and ignored for anyone
    // else: each route planned, a hold at a choke, stuck by aggro there, a
    // sneak-by set out on, a chase begun, an "I'm stuck!" with no walk at
    // all, a rest across a zone line
    void notePlan(uint32 charid, std::chrono::microseconds took, int searched, bool stretch);
    void noteHold(uint32 charid);
    void noteBlocked(uint32 charid);
    void noteHop(uint32 charid);
    void noteChase(uint32 charid);
    void noteStuck(uint32 charid);
    void noteRest(uint32 charid);
} // namespace pawn::drill
