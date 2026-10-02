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
    // A mage takes the mage defaults; every other job is melee, the Monk
    // and the Warrior with their own tools (defaultRowsFor). The one list
    // of mage jobs, so a world body's layer and her own rows agree.
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

    // A job's default set, as (row, checkbox) pairs in list order. The
    // list's order is its priority, for the tactician's rows (marked, the
    // tactician deciding the when) and orders alike (RESEARCH §17.13).
    //  - Melee: the assist trio, her best weapon skill on a target with half
    //    its HP or more (the user, 2026-09-26: TP not spent on a mob about to
    //    fall), then rest with the player. The trio takes the party leader's
    //    fight first, and another cardian's only after it, so a cardian
    //    whose mob has died joins the player's fight before anyone else's.
    //    A Monk and a Warrior carry their tactician's tools between the trio
    //    and the weapon skill, so they go out first (RESEARCH §17.13): the
    //    Monk's Boost (right before her weapon skill), Focus and Dodge, the
    //    Warrior's Berserk (never as the party's Tank), Defender (only as
    //    the Tank) and Aggressor.
    //  - Mage: the tactician's tools first -- the Cure, the -na and the
    //    enfeebles left to its judgement, so curing outranks everything
    //    below, and her rest, the MP pacing -- then her weapon skill and
    //    Rest with the player as orders, and her Attack row, marked and
    //    shipped unchecked: checking it makes her a melee mage whose
    //    tactician leaves a fight to rest (RESEARCH §14.12 decisions 19
    //    and 20; an unmarked Attack row would be an order that keeps her
    //    in). With tools to offer a fight she attends it at cure range
    //    without engaging.
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
        };
        static const std::vector<std::pair<std::string, bool>> monk{
            { "100|0:0|0:0:0|0", true },   // Foe: party leader's target -> Attack
            { "101|0:0|0:0:0|0", true },   // Foe: targeted by ally -> Attack
            { "102|0:0|0:0:0|0", true },   // Foe: targeting ally -> Attack
            { "0|101:0|3:2:39|0", true },  // * Self -> Boost
            { "0|101:0|3:2:36|0", true },  // * Self -> Focus
            { "0|101:0|3:2:37|0", true },  // * Self -> Dodge
            { "2|2:50|4:0:0|0", true },    // Foe: HP >= 50% -> Weapon skill (best)
            { "0|0:0|100:6:1|0", true },   // Self -> Rest with the player
        };
        static const std::vector<std::pair<std::string, bool>> warrior{
            { "100|0:0|0:0:0|0", true },   // Foe: party leader's target -> Attack
            { "101|0:0|0:0:0|0", true },   // Foe: targeted by ally -> Attack
            { "102|0:0|0:0:0|0", true },   // Foe: targeting ally -> Attack
            { "0|101:0|3:2:31|0", true },  // * Self -> Berserk
            { "0|101:0|3:2:33|0", true },  // * Self -> Defender
            { "0|101:0|3:2:34|0", true },  // * Self -> Aggressor
            { "2|2:50|4:0:0|0", true },    // Foe: HP >= 50% -> Weapon skill (best)
            { "0|0:0|100:6:1|0", true },   // Self -> Rest with the player
        };
        static const std::vector<std::pair<std::string, bool>> mage{
            { "1|101:0|2:0:1|0", true },   // * Ally -> Cure (best)
            { "1|101:0|2:0:4|0", true },   // * Ally -> -na (best)
            { "2|101:0|2:100:0|0", true }, // * Foe -> Enfeeble
            { "0|101:0|100:14:1|0", true }, // * Self -> Rest
            { "2|2:50|4:0:0|0", true },    // Foe: HP >= 50% -> Weapon skill (best)
            { "0|0:0|100:6:1|0", true },    // Self -> Rest with the player
            { "102|101:0|0:0:0|0", false }, // * Foe: targeting ally -> Attack, off
        };
        if (isMageJob(job))
        {
            return mage;
        }
        return job == xi::Job::MNK ? monk : job == xi::Job::WAR ? warrior : melee;
    }
} // namespace pawn
