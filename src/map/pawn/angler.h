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
#include "common/mmo.h"

#include <functional>
#include <string>

class CCharEntity;

// A money venture's fishing, done by the map itself (RESEARCH §11.13, "The
// server's own fishing: the design"). While she is away her character is
// loaded as a login loads it and placed in no zone -- her body, the only copy
// of her anything writes while she is gone -- and on it the map runs
// upstream's own fishing as her client's packets would: StartFishing, the
// bite's wait the server sets, FishingAction's hook check, its fight's end,
// and its release, casting again the moment the server allows. The fight is
// timed, not played (fish_fight.h); a monster or a treasure chest that bites
// is let go, and an item too where the player chose so. Her rod and bait live in her first Mog Wardrobe with room, worn
// from there, so her inventory is kept for the catch; what she lacks is
// bought at the catalog's price, her gil first and then his
// (professions.h). The Venture Report and his log's news are written as she
// goes. Only the errand that sent her (errands.cpp) drives it.
namespace pawn::angler
{
    // Where and with what she fishes
    struct Trip
    {
        uint32      charid     = 0;
        uint32      accid      = 0; // the player's account: his gil and his news
        uint8       profession = 0;
        uint16      zone       = 0;
        uint16      area       = 0;
        std::string areaName;
        position_t  spot{};     // a point of the fishing area: her body's place
        uint16      rod    = 0;
        uint16      bait   = 0;
        uint32      leftAt = 0; // the game clock as she left the world
        uint32      minutes    = 0; // how long she was sent for
        bool        letItemsGo = false; // an item that bites is let go, as a monster is (the bait goes with it)
        bool        endOnSnap  = false; // a lost lure ends the venture, not bought again (the player's Line snap choice)
        bool        turbo      = false; // the dev profile's debug venture: an hour's casts in its minute, the waits taken out
    };

    // What she wore in the ranged and ammo slots before her kit went on, put
    // back on at the end. `known` once it is remembered, before the kit goes
    // on: a restart finds her kitted, and keeps what she wore from the first
    // start
    struct Worn
    {
        bool   known  = false;
        uint16 ranged = 0;
        uint16 ammo   = 0;
    };

    // Why she stops before her time is up
    enum class Stop : uint8
    {
        None,       // she fishes on
        RodBroke,   // her rod broke and she holds no spare
        BagsFull,   // her inventory has no room for a catch
        NoGil,      // neither purse could buy her more bait
        CannotFish, // the server would not let her cast, or she could not be kitted (his log says which; the map log why)
        LureLost,   // her lure was lost to a snapped line, and the player chose that it end the venture
        RodAndLure, // her rod broke and took her lure, and the player chose that a lost lure end the venture, or she holds no spare rod
    };

    // Her body loaded and kitted, her rod and bait worn from a wardrobe: on
    // a first start, the report begun and `worn` filled and handed to
    // `remember` to keep, before the kit goes on. Stop::None when she is
    // fishing
    auto start(const Trip& trip, Worn& worn, const std::function<void(const Worn&)>& remember) -> Stop;

    // She is fishing: her body is loaded
    auto fishing(uint32 charid) -> bool;

    // Her body while she fishes, else none
    auto bodyOf(uint32 charid) -> CCharEntity*;

    // One step of hers, called every second: Stop::None while she fishes
    // on. A stop is in his log already (VENTURE_NEWS)
    auto tick(uint32 charid) -> Stop;

    // The trip over: a fish on the line let go, her kit off and what she
    // wore back on, her body saved and released, the report done. Loads her
    // body for it when a restart left it unloaded
    void stop(uint32 charid, const Worn& worn);
} // namespace pawn::angler
