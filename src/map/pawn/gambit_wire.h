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

#include "cardian_link_messages.h"
#include "gambit_ids.h"

#include "ai/helpers/gambits_container.h"

#include <optional>
#include <vector>

// A gambit row as the Cardian Link carries it (cl_gambit,
// cardian_link_protocol.h): the gambit engine's own fields, its conditions
// listed group by group. The row grammar (gambit_text.h) stays the database's.
// Header-only, so xi_test pins the round trip (cardian_link_protocol_tests.cpp).
namespace pawn::wire
{
    constexpr std::size_t kConditions = sizeof(cl_gambit::conditions) / sizeof(cl_gambit_condition);
    constexpr std::size_t kActions    = sizeof(cl_gambit::actions) / sizeof(cl_gambit_action);
    constexpr std::size_t kGroups     = sizeof(cl_gambit::orGroups) * 8; // one bit each

    // The row into its fields; false, and the fields empty, when it holds
    // more groups, conditions or actions than they carry, or a group with
    // no condition (the fields name a group only by its conditions)
    inline auto toWire(const gambits::Gambit_t& g, cl_gambit& out) -> bool
    {
        out = cl_gambit{};
        if (g.predicate_groups.size() > kGroups || g.actions.size() > kActions)
        {
            return false;
        }
        std::size_t count = 0;
        for (std::size_t group = 0; group < g.predicate_groups.size(); ++group)
        {
            const auto& predicates = g.predicate_groups[group];
            if (predicates.predicates.empty())
            {
                out = cl_gambit{};
                return false;
            }
            if (predicates.logic == gambits::G_LOGIC::OR)
            {
                out.orGroups = static_cast<uint8_t>(out.orGroups | (1u << group));
            }
            for (const auto& p : predicates.predicates)
            {
                if (count >= kConditions)
                {
                    out = cl_gambit{};
                    return false;
                }
                out.conditions[count++] = cl_gambit_condition{ static_cast<uint16_t>(p.condition), static_cast<uint8_t>(group), 0, p.condition_arg };
            }
        }
        out.conditionCount = static_cast<uint8_t>(count);
        out.target         = static_cast<uint16_t>(g.target_selector);
        out.retry          = g.retry_delay;
        for (const auto& a : g.actions)
        {
            out.actions[out.actionCount++] = cl_gambit_action{ static_cast<uint16_t>(a.reaction), static_cast<uint16_t>(a.select), a.select_arg };
        }
        return true;
    }

    // The fields back into a row; nullopt for what the row grammar refuses
    // too (gambit_text.h's parseRow) -- no conditions, no actions, a retired
    // behaviour -- and for fields no row makes: counts past the arrays, a
    // group listed out of order, an any-of bit on a group that is not there
    inline auto fromWire(const cl_gambit& w) -> std::optional<gambits::Gambit_t>
    {
        if (w.conditionCount == 0 || w.conditionCount > kConditions || w.actionCount == 0 || w.actionCount > kActions)
        {
            return std::nullopt;
        }

        gambits::Gambit_t g;
        g.target_selector = static_cast<gambits::G_TARGET>(w.target);
        g.retry_delay     = w.retry;
        for (std::size_t i = 0; i < w.conditionCount; ++i)
        {
            const auto& c = w.conditions[i];
            if (c.group == g.predicate_groups.size() && c.group < kGroups)
            {
                const auto logic = (w.orGroups >> c.group) & 1u ? gambits::G_LOGIC::OR : gambits::G_LOGIC::AND;
                g.predicate_groups.emplace_back(logic, std::vector<gambits::Predicate_t>{});
            }
            else if (c.group + 1u != g.predicate_groups.size())
            {
                return std::nullopt;
            }
            g.predicate_groups.back().predicates.emplace_back(static_cast<gambits::G_CONDITION>(c.condition), c.arg);
        }
        if ((static_cast<uint32>(w.orGroups) >> g.predicate_groups.size()) != 0)
        {
            return std::nullopt;
        }

        for (std::size_t i = 0; i < w.actionCount; ++i)
        {
            const auto& a = w.actions[i];
            // A retired behaviour never returns, saved, imported or sent (gambit_ids.h)
            if (a.reaction == static_cast<uint16>(G_REACTION_BEHAVIOR) && isRetiredBehavior(a.select))
            {
                return std::nullopt;
            }
            g.actions.emplace_back(static_cast<gambits::G_REACTION>(a.reaction), static_cast<gambits::G_SELECT>(a.select), a.arg);
        }
        return g;
    }
} // namespace pawn::wire
