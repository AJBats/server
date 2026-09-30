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

#include "common/cbasetypes.h"
#include "data/enums/job.h"

#include <algorithm>
#include <cstddef>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

// The party's roles (RESEARCH §15): who tanks, who heals, who deals damage
// and who pulls, as the party screen shows them. A party role is the
// party's word for a member; it is not the Role row of her gambits
// (gambit_ids.h pawn::Role), whose values are gambits of their own.
//
// The join rule says every member's role until the player says otherwise:
// it is settled again whenever the roster changes, a role the player chose
// is never moved, and a role the rule gave goes to a better candidate who
// joins later. The rule is pure, over plain facts about each member, so
// xi_test pins the user's order (cardian_party_roles_tests.cpp); the pawn
// code gathers the facts and asks.
namespace cardian::party
{
    enum class Role : uint8
    {
        None   = 0,
        Tank   = 1,
        Healer = 2,
        Damage = 3,
        Puller = 4,
    };
    constexpr std::size_t kRoleCount = 5;

    constexpr auto roleName(const Role role) -> std::string_view
    {
        switch (role)
        {
            case Role::Tank:
                return "Tank";
            case Role::Healer:
                return "Healer";
            case Role::Damage:
                return "Damage";
            case Role::Puller:
                return "Puller";
            default:
                return "none";
        }
    }

    // Puller is one member's. Tank, Healer and Damage can be held by
    // several: parties ran heal teams and two tanks
    constexpr auto heldByOne(const Role role) -> bool
    {
        return role == Role::Puller;
    }

    // What the rule reads of a member. The party's members are handed to it
    // in the order they joined, the player first
    struct Member
    {
        uint32              id        = 0;
        xi::Job             job       = xi::Job::NONE;
        bool                shield    = false; // a shield in her off hand
        bool                castsNi   = false; // she can cast Utsusemi: Ni: learned, and her jobs reach it
        bool                castsIchi = false; // the same of Utsusemi: Ichi
        std::optional<Role> chosen;            // the player's own choice for her, None included; empty while the rule decides
    };

    // A member's role as settled, and whether the player chose it
    struct Held
    {
        uint32 id       = 0;
        Role   role     = Role::None;
        bool   byPlayer = false;
    };

    // The user's order for each role: the lower rank is the better
    // candidate, and no rank is no candidate at all.
    //
    // Tank: a Paladin; a Ninja with Utsusemi: Ni; a Warrior with a shield
    // and Utsusemi: Ichi (a Ninja support job's); any other Warrior with a
    // shield. The shadows are asked of the spell, never of her level: a
    // Ninja who has not learned it yet is no tank
    constexpr auto tankRank(const Member& m) -> std::optional<int>
    {
        if (m.job == xi::Job::PLD)
        {
            return 0;
        }
        if (m.job == xi::Job::NIN && m.castsNi)
        {
            return 1;
        }
        if (m.job == xi::Job::WAR && m.shield)
        {
            return m.castsIchi ? 2 : 3;
        }
        return std::nullopt;
    }

    // Healer: a White Mage, a Red Mage, a Summoner, and nobody past them
    constexpr auto healerRank(const Member& m) -> std::optional<int>
    {
        switch (m.job)
        {
            case xi::Job::WHM:
                return 0;
            case xi::Job::RDM:
                return 1;
            case xi::Job::SMN:
                return 2;
            default:
                return std::nullopt;
        }
    }

    // Puller: a Thief, a Ranger, a Bard, then any other melee, who pulls
    // with a ranged attack or pebbles. A mage never pulls by the rule
    constexpr auto pullerRank(const Member& m) -> std::optional<int>
    {
        switch (m.job)
        {
            case xi::Job::THF:
                return 0;
            case xi::Job::RNG:
                return 1;
            case xi::Job::BRD:
                return 2;
            case xi::Job::NONE:
                return std::nullopt;
            default:
                return pawn::isMageJob(m.job) ? std::nullopt : std::optional<int>(3);
        }
    }

    // Every member's role, in the order the members were given.
    //  - A member the player chose for keeps his choice, None included.
    //  - Tank goes to the best tank among the rest, and Puller to the best
    //    puller among those still without a role, each only while no member
    //    holds it by the player's choice. Within a rank, the first to have
    //    joined.
    //  - Healer goes to every healer among the rest.
    //  - Everyone the rule gave nothing else is Damage.
    inline auto settle(const std::span<const Member> members) -> std::vector<Held>
    {
        std::vector<Held> out;
        out.reserve(members.size());
        for (const auto& m : members)
        {
            out.push_back({ m.id, m.chosen.value_or(Role::None), m.chosen.has_value() });
        }

        const auto chosenByPlayer = [&](const Role role)
        {
            return std::ranges::any_of(members, [role](const Member& m)
                                       {
                                           return m.chosen == role;
                                       });
        };
        // The rule's to decide: not the player's, and nothing given yet
        const auto undecided = [&](const std::size_t i)
        {
            return !out[i].byPlayer && out[i].role == Role::None;
        };
        const auto best = [&](auto&& rankOf) -> std::optional<std::size_t>
        {
            std::optional<std::size_t> pick;
            int                        pickRank = 0;
            for (std::size_t i = 0; i < members.size(); ++i)
            {
                const auto rank = undecided(i) ? rankOf(members[i]) : std::nullopt;
                if (rank.has_value() && (!pick.has_value() || *rank < pickRank))
                {
                    pick     = i;
                    pickRank = *rank;
                }
            }
            return pick;
        };

        if (!chosenByPlayer(Role::Tank))
        {
            if (const auto i = best(tankRank); i.has_value())
            {
                out[*i].role = Role::Tank;
            }
        }
        for (std::size_t i = 0; i < members.size(); ++i)
        {
            if (undecided(i) && healerRank(members[i]).has_value())
            {
                out[i].role = Role::Healer;
            }
        }
        if (!chosenByPlayer(Role::Puller))
        {
            if (const auto i = best(pullerRank); i.has_value())
            {
                out[*i].role = Role::Puller;
            }
        }
        for (std::size_t i = 0; i < members.size(); ++i)
        {
            if (undecided(i))
            {
                out[i].role = Role::Damage;
            }
        }
        return out;
    }

    // The player's press on the party screen: this member's role is his
    // choice from here on. A role only one member holds is taken from
    // whoever else he had chosen for it, who is left with none; one the
    // rule had given it to goes back to the rule at the next settle. A
    // press for someone no longer in the party changes nothing
    inline void choose(const std::span<Member> members, const uint32 id, const Role role)
    {
        const bool present = std::ranges::any_of(members, [id](const Member& m)
                                                 {
                                                     return m.id == id;
                                                 });
        if (!present)
        {
            return;
        }
        for (auto& m : members)
        {
            if (m.id == id)
            {
                m.chosen = role;
            }
            else if (heldByOne(role) && m.chosen == role)
            {
                m.chosen = Role::None;
            }
        }
    }
} // namespace cardian::party
