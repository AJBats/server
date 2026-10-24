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

// Cardian: a zone's lifts, for a cardian to ride (OPEN_ISSUES #572,
// ROADMAP T, "Lifts"). The game moves nobody on a lift: the client draws
// the platform and carries its own player with it. So the cardians are
// carried by the server -- her height following the platform as it travels
// -- and a cardian bound for another floor walks to a lift's landing, waits
// for the platform, boards and rides (CPawnController::LiftTick, pawn_lift.cpp).
//
// Each lift is read once per zone from the zone's data (a platform NPC with
// an `elevator` block names its two doors) and its navmesh: an end of the
// shaft is a door, its floor the mesh's height there, its landing the mesh
// a step beyond the door. The platform's state is read off what the game
// tells the client (lift_math.h).
#pragma once

#include "common/types/position.h"

#include "lift_math.h"

#include <cstdint>
#include <string>
#include <vector>

class CZone;
class CNpcEntity;

namespace pawn::lifts
{
    struct End
    {
        position_t door{};    // the door NPC at this end of the shaft
        position_t landing{}; // on the floor's mesh, a step beyond the door
        float      floor = 0; // the floor's height, which the platform stands level with here
    };

    struct Lift
    {
        uint32      platformId = 0;
        std::string name;    // the platform's, for the map log
        float       cx = 0;  // the platform's centre
        float       cz = 0;
        End         ends[2]; // the data's lower door's end, its upper door's
        uint32      periodMs    = 0;
        uint32      doorDelayMs = 0;
        bool        reversed    = false;
        bool        timed       = false; // runs itself off the clock; a lever's waits to be pulled
    };

    // A rider stands within this of the platform's centre
    constexpr float kRadius = 2.2f;

    // The zone's lifts, read once per zone; empty where it has none, or none
    // whose ends the mesh reaches
    auto in(CZone* PZone) -> const std::vector<Lift>&;

    // The Vana'diel clock the lifts run on, in milliseconds
    auto nowMs() -> int64_t;

    // The platform as the game has it now, and its state and height
    auto platformOf(const Lift& lift) -> cardian::lift::Platform;
    auto stateOf(const Lift& lift) -> cardian::lift::State;
    auto heightOf(const Lift& lift) -> float;

    // The lift whose platform `at` stands on, level with it: nullptr when none
    auto underFoot(CZone* PZone, const position_t& at) -> const Lift*;

    // The end whose floor `at` stands on, near enough to its landing to
    // board from it: -1 when neither
    auto endFor(const Lift& lift, const position_t& at) -> int;

    // A rider's spot on the platform at an end: the `i`th of six round the
    // centre, at that end's floor
    auto spotOf(const Lift& lift, int end, int i) -> position_t;

    // The lift by its platform
    auto byPlatform(CZone* PZone, uint32 platformId) -> const Lift*;
} // namespace pawn::lifts
