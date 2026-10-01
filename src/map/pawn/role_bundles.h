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

#include "party_roles.h"

#include <string>
#include <utility>
#include <vector>

// The rows a party role lends a member while she holds it (RESEARCH §15.5,
// §15.11): a small bundle of ordinary gambit rows, every one a row the
// editor could make, in the grammar of gambit_text.h with the ids of
// gambit_ids.h. The bundle is written as a default set is: its orders
// first, then its line row where it has one, then the rows for below the
// line; the fit (gambit_layers.h) lays each part onto hers. Never saved:
// the role comes off and the rows go with it. Header-only, so xi_test pins
// the bundles (cardian_gambit_layers_tests.cpp).
namespace pawn::bundles
{
    // A role's rows, as (row, checkbox) pairs in the bundle's order.
    //  - Healer lends the Support Mage gambit and the two rows it needs to
    //    cure and to take ailments off: on a default mage every one of them
    //    is hers already, and the fit adds nothing.
    //  - Tank lends the Tank gambit, her line, and below it the pull (the
    //    mob an ally is on, the player as much as a cardian: her
    //    tactician's melee) and Provoke as her tactician's choice
    //    (tank_calls.h paces it).
    //  - Damage lends the Damage gambit and the melee defaults' trio, all
    //    orders: on a default melee every one is hers already; a mage gets
    //    what makes her fight.
    //  - Puller is a seat only.
    inline auto bundleFor(const cardian::party::Role role) -> const std::vector<std::pair<std::string, bool>>&
    {
        static const std::vector<std::pair<std::string, bool>> healer{
            { "0|0:0|100:11:1|0", true }, // Self -> Role: Support Mage, the line
            { "1|101:0|2:0:1|0", true },  // Ally: tactician's choice -> Cure (best)
            { "1|101:0|2:0:4|0", true },  // Ally: tactician's choice -> -na (best)
        };
        static const std::vector<std::pair<std::string, bool>> tank{
            { "0|0:0|100:11:2|0", true }, // Self -> Role: Tank, the line
            { "101|0:0|0:0:0|0", true },  // Foe: targeted by ally -> Attack
            { "2|101:0|3:2:35|0", true }, // Foe: tactician's choice -> Provoke
        };
        static const std::vector<std::pair<std::string, bool>> damage{
            { "100|0:0|0:0:0|0", true },  // Foe: party leader's target -> Attack
            { "101|0:0|0:0:0|0", true },  // Foe: targeted by ally -> Attack
            { "102|0:0|0:0:0|0", true },  // Foe: targeting ally -> Attack
            { "0|0:0|100:11:3|0", true }, // Self -> Role: Damage
        };
        static const std::vector<std::pair<std::string, bool>> none{};
        switch (role)
        {
            case cardian::party::Role::Healer:
                return healer;
            case cardian::party::Role::Tank:
                return tank;
            case cardian::party::Role::Damage:
                return damage;
            default:
                return none;
        }
    }
} // namespace pawn::bundles
