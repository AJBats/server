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

#include "food_math.h"

#include "common/cbasetypes.h"

#include <optional>
#include <set>
#include <string>

class CCharEntity;

// Food on the map (RESEARCH §19; the rules are food_math.h's). What a
// character eats for her party role: a body of the world the census's pick
// for her (cardian_food, written by tools/world/census.py with her
// wardrobe), an owned cardian -- a recruit, or one of the player's own --
// the best food for her role her own bags hold. A body of the world in a
// real player's party never runs out: her picks are topped up to her stock.
namespace pawn::food
{
    // cardian_food as this build reads it: made at boot when missing, so a
    // database whose module SQL dbtool skipped still has it
    void ensureTable();

    // A food item's facts, read from its script once and kept; nullptr for
    // an item that is no food
    auto factsOf(uint16 itemId) -> const cardian::food::Facts*;

    // What she eats in a party role, and whether it is a cookie; nullopt
    // when she has nothing to eat for it
    auto pickFor(CCharEntity* PChar, cardian::party::Role role) -> std::optional<cardian::food::Pick>;

    // The food she has on now, by its item; 0 for none
    auto eating(const CCharEntity* PChar) -> uint16;

    // The census's foods for a body of the world, every role's, by item
    auto plannedIds(const std::string& name) -> std::set<uint16>;

    // The census's picks for her read again at the next ask (a re-dress
    // has written new ones)
    void forget(uint32 charid);

    // A body of the world's picks topped up to her stock in her bag. How
    // many items she was given
    auto topUp(CCharEntity* PChar) -> uint32;

    // She eats a stack of this food: from her inventory, or one fetched
    // from her bags first. Her own use, which keeps a rest order she is on.
    // CL_S_OK when the use began, else why not
    auto eat(CCharEntity* PChar, uint16 itemId) -> uint16;
} // namespace pawn::food
