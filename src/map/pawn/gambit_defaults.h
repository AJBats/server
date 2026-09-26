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

#include "gambit_ids.h"

#include "data/enums/job.h"

#include <string>
#include <utility>
#include <vector>

// The rows a character starts with, by job (RESEARCH §14.12 decision 4,
// ROADMAP K): every cardian, every alt, and the main character while the
// player drives someone else. Rows are in the grammar of gambit_text.h,
// with the ids of gambit_ids.h. Header-only, so xi_test pins the sets
// (cardian_gambit_defaults_tests.cpp).
namespace pawn
{
    // A mage takes the mage defaults; every other job is melee. The one
    // list of mage jobs, so a world body's layer and her own Role row agree.
    constexpr auto isMageJob(const xi::Job job) -> bool
    {
        switch (job)
        {
            case xi::Job::WHM:
            case xi::Job::BLM:
            case xi::Job::RDM:
            case xi::Job::SMN:
            case xi::Job::SCH:
            case xi::Job::GEO:
                return true;
            default:
                return false;
        }
    }

    // A job's default set, as (row, checkbox) pairs in list order.
    //  - Melee: the assist trio, her best weapon skill on a target with half
    //    its HP or more (the user, 2026-09-26: TP not spent on a mob about to
    //    fall), then rest with the player, then the Damage role. The trio
    //    takes the party leader's fight first, and another cardian's only
    //    after it, so a cardian whose mob has died joins the player's fight
    //    before anyone else's.
    //  - Mage: a Support Mage who attends fights without engaging monsters
    //    and cures. Her weapon skill row and Rest with the player are
    //    orders, above her role row (her tactician line, tactician_line.h,
    //    where a weapon skill means something); below it, her allow-list:
    //    the Cure left to her judgement, and her Attack row, shipped
    //    unchecked -- checking it makes her a melee mage whose tactician
    //    leaves a fight to rest (RESEARCH §14.12 decisions 19 and 20).
    // The weapon skill row is in every set (the user, 2026-09-26): it costs
    // nothing, and comes up only once she is engaged with the TP for one.
    // Neither set avoids aggro (that is the world's, not the party's): the
    // player adds that as a row of his own.
    inline auto defaultRowsFor(const xi::Job job) -> const std::vector<std::pair<std::string, bool>>&
    {
        static const std::vector<std::pair<std::string, bool>> melee{
            { "100|0:0|0:0:0|0", true },  // Foe: party leader's target -> Attack
            { "101|0:0|0:0:0|0", true },  // Foe: targeted by ally -> Attack
            { "102|0:0|0:0:0|0", true },  // Foe: targeting ally -> Attack
            { "2|2:50|4:0:0|0", true },   // Foe: HP >= 50% -> Weapon skill (best)
            { "0|0:0|100:6:1|0", true },  // Self -> Rest with the player
            { "0|0:0|100:11:3|0", true }, // Self -> Role: Damage
        };
        static const std::vector<std::pair<std::string, bool>> mage{
            { "2|2:50|4:0:0|0", true },   // Foe: HP >= 50% -> Weapon skill (best)
            { "0|0:0|100:6:1|0", true },  // Self -> Rest with the player
            { "0|0:0|100:11:1|0", true }, // Self -> Role: Support Mage
            { "1|101:0|2:0:1|0", true },  // Ally: tactician's choice -> Cure (best)
            { "102|0:0|0:0:0|0", false }, // Foe: targeting ally -> Attack
        };
        return isMageJob(job) ? mage : melee;
    }
} // namespace pawn
