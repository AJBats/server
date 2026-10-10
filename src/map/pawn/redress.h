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
class CZone;

// A wild cardian re-dressed at the auction house. When the player stands by
// an auction counter, each of the world's adventurers in his party who stands
// by it too, and whose level has risen since she was last dressed there, is
// re-geared, given her food, taught her spells and has her skills brought up
// to her level, by the census's own rules (tools/world/census.py): her
// wardrobe, her food, her spellbook, her skill values. The census is asked through a row of cardian_redress and
// answers through it (cardian_redress.sql has the life of a row); the map
// puts the answer on her while she stands, since the census never writes the
// character tables of a body the map holds. An alt or a recruit is the
// player's, and is never re-dressed.
//
// The live census (RESEARCH §11.11) shares the road. Out of sight -- in a
// zone no real player is in, and not in a real player's party -- a body of
// the world gains the levels her cohort's pace gives her, and in a city
// changes her gear. The census watcher decides (census.py, the pace) and
// plans a ding in a row of cardian_ding (cardian_ding.sql has the life of a
// row). A body the map holds takes it here when she is out of sight and
// free: her level, her support job and her skills, and in a city her gear,
// spells and food; one whose seat farms keeps the experience she earns
// instead. A body the map does not hold is claimed for the census, which
// writes her rows, and she stands for nobody until it has.
namespace pawn::redress
{
    // cardian_redress and cardian_ding as this build reads them: made at boot
    // when missing, so a database whose module SQL dbtool skipped still has
    // them; and every body a census write was under way for when the map
    // last stopped claimed again, until the census says it is done
    void ensureTable();

    // From the zone tick: the counters of this zone looked at every few
    // seconds, and every few seconds the census's answers put on whoever
    // they are for that stands, and its dings on whoever is out of sight
    void tick(CZone* PZone);

    // The census is writing her rows: she does not stand until it is done
    auto isClaimed(uint32 charid) -> bool;
    // The gear-up errand at a counter (errands.h): she asks the census for
    // her level as she would at the counter beside him -- when she has risen
    // past the level she was last dressed for, and nothing is on its way.
    // True when a request is on its way now (hers just asked, or one before)
    auto askFor(const CCharEntity* PPawn) -> bool;
    // Nothing of hers waits on the census: never asked, or her answer put on her
    auto settled(uint32 charid) -> bool;
} // namespace pawn::redress
