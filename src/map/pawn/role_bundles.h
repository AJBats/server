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

#include "gambit_defaults.h"
#include "party_roles.h"

#include <string>
#include <utility>
#include <vector>

// The rows a party role lends a member while she holds it (RESEARCH §17.5,
// §17.11, §17.12, §17.13): a small bundle of ordinary gambit rows, every
// one a row the editor could make, in the grammar of gambit_text.h with the
// ids of gambit_ids.h, in the bundle's order of priority; the fit
// (gambit_layers.h) puts them ahead of hers. Never saved: the role comes
// off and the rows go with it. Header-only, so xi_test pins the bundles
// (cardian_gambit_layers_tests.cpp).
namespace pawn::bundles
{
    // A role's rows for a member of that job, as (row, checkbox) pairs in
    // the bundle's order. What a role lends depends on her job (RESEARCH
    // §17.12).
    //  - Healer lends the marked rows the tactician cures and takes
    //    ailments off through, and her marked Rest, its MP pacing: on a
    //    default mage every one is hers already, and the fit adds nothing.
    //  - Tank lends the pull, marked (the mob an ally is on, the player as
    //    much as a cardian: her tactician's melee) and Provoke, marked
    //    (tank_calls.h paces it; the Tank seat is what runs the tank
    //    tactician).
    //  - Damage lends a melee job the melee defaults' trio, all orders: on
    //    a default melee every one is hers already. A mage job is lent
    //    nothing, the seat only: her damage is a judgement of its own (the
    //    Black Mage's nuking and MP pacing), and until it exists the melee
    //    trio would only stand her in the fight with her staff (the user,
    //    2026-10-01).
    //  - Puller is a seat only.
    inline auto bundleFor(const cardian::party::Role role, const xi::Job job) -> const std::vector<std::pair<std::string, bool>>&
    {
        static const std::vector<std::pair<std::string, bool>> healer{
            { "1|101:0|2:0:1|0", true },    // * Ally -> Cure (best)
            { "1|101:0|2:0:4|0", true },    // * Ally -> -na (best)
            { "0|101:0|100:14:1|0", true }, // * Self -> Rest
        };
        static const std::vector<std::pair<std::string, bool>> tank{
            { "101|101:0|0:0:0|0", true }, // * Foe: targeted by ally -> Attack
            { "2|101:0|3:2:35|0", true },  // * Foe -> Provoke
        };
        static const std::vector<std::pair<std::string, bool>> damage{
            { "100|0:0|0:0:0|0", true }, // Foe: party leader's target -> Attack
            { "101|0:0|0:0:0|0", true }, // Foe: targeted by ally -> Attack
            { "102|0:0|0:0:0|0", true }, // Foe: targeting ally -> Attack
        };
        static const std::vector<std::pair<std::string, bool>> none{};
        switch (role)
        {
            case cardian::party::Role::Healer:
                return healer;
            case cardian::party::Role::Tank:
                return tank;
            case cardian::party::Role::Damage:
                return pawn::isMageJob(job) ? none : damage;
            default:
                return none;
        }
    }
} // namespace pawn::bundles
