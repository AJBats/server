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

class CZone;

// A wild cardian re-dressed at the auction house. When the player stands by
// an auction counter, each of the world's adventurers in his party who stands
// by it too, and whose level has risen since she was last dressed there, is
// re-geared, taught her spells and has her skills brought up to her level, by
// the census's own rules (tools/world/census.py): her wardrobe, her spellbook,
// her skill values. The census is asked through a row of cardian_redress and
// answers through it (cardian_redress.sql has the life of a row); the map
// puts the answer on her while she stands, since the census never writes the
// character tables of a body the map holds. An alt or a recruit is the
// player's, and is never re-dressed.
namespace pawn::redress
{
    // cardian_redress as this build reads it: made at boot when missing, so a
    // database whose module SQL dbtool skipped still has it
    void ensureTable();

    // From the zone tick: the counters of this zone looked at every few
    // seconds, and every few seconds the census's answers put on whoever
    // they are for that stands
    void tick(CZone* PZone);
} // namespace pawn::redress
