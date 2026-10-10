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
#include <string_view>

class CBaseEntity;
class CCharEntity;
class CZone;

// The conquest exchange by proxy: a cardian cannot talk to a gate guard, but
// her player can stand beside one, and she buys from that guard's stock out of
// her own conquest points. These are the guards that sell -- the city and
// embassy guards (xi.conquest.guard CITY and FOREIGN; the outpost and border
// overseers sell nothing), found by grepping overseerOnTrigger in
// scripts/zones/*/npcs. The roster tells the addon whether one stands within
// the player's reach, and the exchange's messages (CP_SHOP, CP_BUY) sell from
// the nearest.
namespace pawn::guards
{
    struct Guard
    {
        std::string_view name;   // the NPC's name
        uint8            nation; // xi.nation
        uint8            type;   // xi.conquest.guard: 1 city, 2 foreign
        std::string_view zone;   // the zone it stands in, by name
    };

    // The nearest gate guard within the player's reach (8 yalms), nullptr for
    // none -- nearest, because a consulate stands its guards together
    auto guardNear(const CCharEntity* PPlayer) -> const Guard*;

    // A guard who sells stands in the zone, by its name
    auto zoneHasGuard(std::string_view zoneName) -> bool;

    // The guard a cardian of this nation shops at in her zone, and the NPC
    // standing there: her own nation's nearest her, else a guard who sells
    // to every nation's (Jeuno's), else any; nothing when none stands there
    struct Standing
    {
        const Guard*       guard = nullptr;
        const CBaseEntity* npc   = nullptr;
    };
    auto guardFor(CZone* PZone, uint8 nation, const position_t& from) -> std::optional<Standing>;

    // Her own nation's guard in another zone of her city -- a consulate:
    // San d'Oria's Bastok and Windurst guards stand in Northern San d'Oria
    // -- and the zone it stands in; nothing when her zone has one of hers,
    // or her city none
    struct Elsewhere
    {
        const Guard* guard = nullptr;
        uint16       zone  = 0;
    };
    auto consulateFor(CZone* PZone, uint8 nation) -> std::optional<Elsewhere>;

    // Whether this guard sells to one of this nation: her own nation's, one
    // who sells to every nation's (Jeuno's), or another nation's while hers
    // outranks his in the conquest tally -- the exchange's own rule
    // (conquest_exchange.lua refusalOf), asked before she sets out
    auto sellsTo(const Guard& guard, uint8 nation) -> bool;
} // namespace pawn::guards
