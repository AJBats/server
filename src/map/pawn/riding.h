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

#include <string_view>
#include <vector>

class CCharEntity;

// A cardian on a chocobo (ROADMAP T, "Chocobos on the trek", #527). She
// rides only on a rental of her own: when the player rents at a city's
// stable, the cardians beside him rent too and come out where his rental
// sets him down (modules/cardian/lua/chocobo_ride.lua, through
// ridersBeside); mounted, she moves at pawn.RIDE_SPEED, and she gets down
// in a fight and once he is on foot and she has reached him
// (CPawnController::RideTick). The rules are riding_math.h's.
namespace pawn::riding
{
    // Whether she holds the Chocobo License: the key item. His alts hold it
    // through his account (account_wide.cpp copies it); a cardian his account
    // owns holds it when he does, and a body of the world once any of her jobs
    // stands at 20 or above -- each given the key item here the first time
    // it is asked
    auto licensed(CCharEntity* PChar) -> bool;

    // The cardians of his party who rent with him at the stable he rents at:
    // in his zone and within reach of him, alive, on foot, in no fight, free
    // of her errands in town, not holding on his order nor in his maneuver,
    // and licensed; each with whether she rides free (ridesFree)
    struct Rider
    {
        uint32 charid = 0;
        bool   free   = false;
    };
    auto ridersBeside(const CCharEntity* PPlayer) -> std::vector<Rider>;

    // A body of the world rides free (the world simulates no gil for her);
    // his alts and owned cardians pay their way
    auto ridesFree(const CCharEntity* PChar) -> bool;

    // Whether she holds the license, read without her body, for a venture's
    // way (venture_spots.h), as the game has it: a body of the world -- by
    // the census, errands.h ofTheWorld -- once any of her jobs stands at
    // kLicenseLevel or above, whatever key items she holds; anyone else only
    // by the account's license, the key item, his or her own. Asks, and
    // grants, nothing
    auto licensedFor(const CCharEntity* PPlayer, uint32 charid) -> bool;

    // Her base speed on her feet or on her chocobo: pawn.PAWN_SPEED, or
    // pawn.RIDE_SPEED while she rides
    auto runSpeed(const CCharEntity* PPawn) -> uint8;

    // Gets her down from her chocobo, said in the map log with why
    void dismount(CCharEntity* PPawn, std::string_view why);

    // Her own rental at the city's stable she stands by, on her trek (#528):
    // free for a body of the world; one of his own pays the stable's price
    // from her gil, `PPayer`'s purse behind hers when he is online (nullptr
    // else); then her chocobo, and the stable's set-down carries her
    // (modules/cardian/lua/chocobo_ride.lua, xi.cardian.chocobo.rent).
    // Whether she rented, the refusal said in the map log
    auto rent(CCharEntity* PPawn, CCharEntity* PPayer) -> bool;
    // (Her rental here carries her nowhere: her trek's crossing takes her to
    // the stable's set-down, as a zone line's would)
} // namespace pawn::riding
