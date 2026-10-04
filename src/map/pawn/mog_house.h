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

class CBaseEntity;
class CCharEntity;

// The Mog House by proxy: a cardian cannot walk into the player's Mog House,
// but what it does for her (her jobs) is done while he stands in his own, or
// within reach of a Nomad Moogle, as the conquest exchange is done while he
// stands by a gate guard (gate_guards.h) -- for a cardian nearby: in his own
// Mog House, one anywhere in that city (where she stands inside it cannot be
// known: she never enters one); by a Nomad Moogle, one within 20 yalms of
// the moogle, which stands in one spot. The roster tells the addon which
// cardians it reaches, and the Mog House's messages (JOB_CHANGE) ask again
// as they act.
namespace pawn::moghouse
{
    // What the player stands in or by: neither, his own Mog House, or the
    // nearest Nomad Moogle in sight within his reach (8 yalms, a gate guard's).
    // Good for the tick it is asked in
    struct Place
    {
        bool               mogHouse = false;
        const CBaseEntity* moogle   = nullptr;
    };

    auto placeOf(const CCharEntity* PPlayer) -> Place;

    // Whether the place he stands in or by reaches her: CL_S_OK, or the
    // refusal (CL_S_NO_MOG_HOUSE, CL_S_NOT_IN_CITY, CL_S_OTHER_ZONE,
    // CL_S_FAR_FROM_MOOGLE)
    auto reaches(const Place& place, const CCharEntity* PPlayer, const CCharEntity* PPawn) -> uint16;

    // Her main job, her support job, or both, changed by the game's own job
    // change (0 keeps the one she has): CL_S_OK, or the refusal. The caller
    // has checked that she is his to manage and that he is in reach
    auto changeJobs(CCharEntity* PPawn, uint8 mainJob, uint8 subJob) -> uint16;
} // namespace pawn::moghouse
