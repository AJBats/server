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
#include "tactician_line.h"

#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// The rows a party role lends a member while she holds it (RESEARCH §17.5,
// §17.11, §17.12, §17.13): a small bundle of ordinary gambit rows, every
// one a row the editor could make, in the grammar of gambit_text.h with the
// ids of gambit_ids.h, in the bundle's order of priority; the fit
// (gambit_layers.h) puts them ahead of hers. Never saved: the role comes
// off and the rows go with it. Header-only, so xi_test pins the bundles
// (cardian_gambit_layers_tests.cpp).
//
// A seat has its tools, and lends each one her two jobs can ever use on
// this server (the user, 2026-10-02): her main job by the server's level
// cap, her sub job by the sub job's level at that cap. Never by her level
// now, so the rows she is lent stay put as she levels -- a WHM 9 / WAR 4
// in the Tank seat is lent Provoke, and uses it from her sub's level 5 --
// and a tool neither job ever reaches is never lent (a sub Warrior's
// Aggressor, at 45, on a 75 server whose sub jobs stop at 37).
namespace pawn::bundles
{
    // What a seat's tool asks of her two jobs: nothing, or a job ability or
    // a spell one of them can ever learn; and, for a melee tool, a main job
    // that fights in melee on the Damage seat (meleesOnDamage), for a nuke
    // one that nukes there (nukesOnDamage)
    struct Need
    {
        enum class Kind : uint8
        {
            Anyone,
            Ability,
            Spell,
        };
        Kind   kind  = Kind::Anyone;
        uint16 id    = 0;     // the ability's or the spell's
        bool   melee = false; // only for a main job that melees
        bool   nuker = false; // only for a main job that nukes
    };

    // A row a seat lends, and what it needs
    struct Tool
    {
        std::string_view spec;
        Need             need;
    };

    // The highest levels her two jobs reach on this server: the main job's
    // cap (main.MAX_LEVEL: 75 on prod, 99 on dev) and the sub job's level
    // under it (subLevelAt)
    struct Caps
    {
        uint8 main = 0;
        uint8 sub  = 0;
    };

    // The sub job's level under a main job at mainLevel, by the server's
    // rule (map.SUBJOB_RATIO, as CBattleEntity::SetSLevel applies it): 0 no
    // sub job, 1 half, 2 two thirds, 3 equal
    constexpr auto subLevelAt(const uint8 mainLevel, const uint8 ratio) -> uint8
    {
        switch (ratio)
        {
            case 1:
                return mainLevel <= 1 ? mainLevel : static_cast<uint8>(mainLevel / 2);
            case 2:
                return mainLevel <= 1 ? mainLevel : static_cast<uint8>(mainLevel * 2 / 3);
            case 3:
                return mainLevel;
            default:
                return 0;
        }
    }

    // Whether a main job melees on the Damage seat. Case by case (the user,
    // 2026-10-02): the White and the Red Mage do, as orders -- they finish
    // the fight, and their own marked Rest keeps them down between fights
    // until they are ready; the Black Mage, the Scholar and the Geomancer
    // nuke and do not; the Summoner is lent nothing on the seat until her
    // avatar has gambits of its own
    constexpr auto meleesOnDamage(const xi::Job main) -> bool
    {
        switch (main)
        {
            case xi::Job::BLM:
            case xi::Job::SCH:
            case xi::Job::GEO:
            case xi::Job::SMN:
                return false;
            default:
                return true;
        }
    }

    // Whether a main job nukes on the Damage seat: the mages whose damage is
    // spells, the Summoner aside (the user, 2026-10-02) -- the Black, the
    // White (Banish) and the Red Mage, the Scholar and the Geomancer
    constexpr auto nukesOnDamage(const xi::Job main) -> bool
    {
        switch (main)
        {
            case xi::Job::BLM:
            case xi::Job::WHM:
            case xi::Job::RDM:
            case xi::Job::SCH:
            case xi::Job::GEO:
                return true;
            default:
                return false;
        }
    }

    // Whether her two jobs can ever use what a tool needs. levelOf(need,
    // job): the level that job learns the need's ability or spell at, 0
    // for never -- the game's tables on the server, a few known levels in
    // the tests
    template <typename LevelOf>
    auto reaches(const Need need, const xi::Job main, const xi::Job sub, const Caps caps, LevelOf&& levelOf) -> bool
    {
        if ((need.melee && !meleesOnDamage(main)) || (need.nuker && !nukesOnDamage(main)))
        {
            return false;
        }
        if (need.kind == Need::Kind::Anyone)
        {
            return true;
        }
        const auto within = [&](const xi::Job job, const uint8 cap)
        {
            if (job == xi::Job::NONE || cap == 0)
            {
                return false;
            }
            const uint8 level = levelOf(need, job);
            return level > 0 && level <= cap;
        };
        return within(main, caps.main) || within(sub, caps.sub);
    }

    // The game's ids the tools need (static_assert'ed against the game's
    // enums in pawn_gambits.cpp)
    inline constexpr uint16 kSpellCure    = 1;
    inline constexpr uint16 kSpellPoisona = 14; // the first -na a job learns
    inline constexpr uint16 kProvoke      = 35;

    // A seat's tools, every one, in the bundle's order of priority.
    //  - Healer: the marked rows the tactician cures and takes ailments off
    //    through, and her marked Rest, its MP pacing -- for a job that
    //    learns Cure (the -na, one that learns a -na): on a default mage
    //    every one is hers already, and the fit adds nothing.
    //  - Tank: the pull, marked (the mob an ally is on, the player as much
    //    as a cardian: her tactician's melee), for anyone; then Provoke
    //    (tank_calls.h paces it; the Tank seat is what runs the tank
    //    tactician), Defender, Focus and Dodge, marked.
    //  - Damage: the melee defaults' trio, orders, then Berserk, Aggressor,
    //    Boost, Sneak Attack and Focus, marked -- all of them melee tools,
    //    for a main job that melees (meleesOnDamage: a Black Mage / Warrior
    //    is lent no Berserk, a Summoner nothing). A buff acts
    //    where it sits in her think (CGambits::BuffNow), the seat choosing
    //    between Berserk and Defender (tactician_line.h wrongStance). Then
    //    Damage spell (any), marked -- her nukes -- and her marked Rest,
    //    the MP pacing that sits her down between fights, for a main job
    //    that nukes (nukesOnDamage: her tactician's when and which,
    //    CGambits::CastNuke). The Thief's Trick Attack joins when its
    //    judgement exists.
    //  - Puller is a seat only.
    inline auto toolsOf(const cardian::party::Role role) -> std::span<const Tool>
    {
        using K = Need::Kind;
        namespace t = cardian::tactician;
        static const std::vector<Tool> healer{
            { "1|101:0|2:0:1|0", { K::Spell, kSpellCure } },       // * Ally -> Cure (best)
            { "1|101:0|2:0:4|0", { K::Spell, kSpellPoisona } },    // * Ally -> -na (best)
            { "0|101:0|100:14:1|0", { K::Spell, kSpellCure } },    // * Self -> Rest
        };
        static const std::vector<Tool> tank{
            { "101|101:0|0:0:0|0", { K::Anyone, 0 } },          // * Foe: targeted by ally -> Attack
            { "2|101:0|3:2:35|0", { K::Ability, kProvoke } },   // * Foe -> Provoke
            { "0|101:0|3:2:33|0", { K::Ability, t::kDefender } }, // * Self -> Defender
            { "0|101:0|3:2:36|0", { K::Ability, t::kFocus } },    // * Self -> Focus
            { "0|101:0|3:2:37|0", { K::Ability, t::kDodge } },    // * Self -> Dodge
        };
        static const std::vector<Tool> damage{
            { "100|0:0|0:0:0|0", { K::Anyone, 0, true } },               // Foe: party leader's target -> Attack
            { "101|0:0|0:0:0|0", { K::Anyone, 0, true } },               // Foe: targeted by ally -> Attack
            { "102|0:0|0:0:0|0", { K::Anyone, 0, true } },               // Foe: targeting ally -> Attack
            { "0|101:0|3:2:31|0", { K::Ability, t::kBerserk, true } },   // * Self -> Berserk
            { "0|101:0|3:2:34|0", { K::Ability, t::kAggressor, true } }, // * Self -> Aggressor
            { "0|101:0|3:2:39|0", { K::Ability, t::kBoost, true } },     // * Self -> Boost
            { "0|101:0|3:2:44|0", { K::Ability, t::kSneakAttack, true } }, // * Self -> Sneak Attack
            { "0|101:0|3:2:36|0", { K::Ability, t::kFocus, true } },     // * Self -> Focus
            { "2|101:0|2:3:0|0", { K::Anyone, 0, false, true } },        // * Foe -> Damage spell (any)
            { "0|101:0|100:14:1|0", { K::Anyone, 0, false, true } },     // * Self -> Rest
        };
        switch (role)
        {
            case cardian::party::Role::Healer:
                return healer;
            case cardian::party::Role::Tank:
                return tank;
            case cardian::party::Role::Damage:
                return damage;
            default:
                return {};
        }
    }

    // The rows a seat lends her, as (row, checkbox) pairs in the bundle's
    // order: its tools her two jobs can ever use (reaches)
    template <typename LevelOf>
    auto bundleFor(const cardian::party::Role role, const xi::Job main, const xi::Job sub, const Caps caps, LevelOf&& levelOf)
        -> std::vector<std::pair<std::string, bool>>
    {
        std::vector<std::pair<std::string, bool>> out;
        for (const auto& tool : toolsOf(role))
        {
            if (reaches(tool.need, main, sub, caps, levelOf))
            {
                out.emplace_back(std::string(tool.spec), true);
            }
        }
        return out;
    }
} // namespace pawn::bundles
