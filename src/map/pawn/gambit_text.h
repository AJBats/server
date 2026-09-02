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

#include "ai/helpers/gambits_container.h"

#include <charconv>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// The row grammar (M3.85): one line per gambit row, the same text on the
// wire and in the database, numbers only so it can never drift from the
// enums it encodes:
//   <target>|<group>[&<group>...]|<action>[+<action>...]|<retry>
//   group  = [?]<condition>:<arg>[,<condition>:<arg>...]   ? marks an OR group
//   action = <reaction>:<select>:<arg>
namespace pawn::text
{
    inline auto formatRow(const gambits::Gambit_t& g) -> std::string
    {
        std::string out = std::to_string(static_cast<uint16>(g.target_selector)) + "|";
        for (std::size_t i = 0; i < g.predicate_groups.size(); ++i)
        {
            const auto& group = g.predicate_groups[i];
            if (i != 0)
            {
                out += '&';
            }
            if (group.logic == gambits::G_LOGIC::OR)
            {
                out += '?';
            }
            for (std::size_t j = 0; j < group.predicates.size(); ++j)
            {
                if (j != 0)
                {
                    out += ',';
                }
                out += std::to_string(static_cast<uint16>(group.predicates[j].condition)) + ":" + std::to_string(group.predicates[j].condition_arg);
            }
        }
        out += '|';
        for (std::size_t i = 0; i < g.actions.size(); ++i)
        {
            const auto& a = g.actions[i];
            if (i != 0)
            {
                out += '+';
            }
            out += std::to_string(static_cast<uint16>(a.reaction)) + ":" + std::to_string(static_cast<uint16>(a.select)) + ":" + std::to_string(a.select_arg);
        }
        out += "|" + std::to_string(g.retry_delay);
        return out;
    }

    namespace detail
    {
        inline auto split(std::string_view s, const char sep) -> std::vector<std::string_view>
        {
            std::vector<std::string_view> out;
            std::size_t                   start = 0;
            while (true)
            {
                const auto at = s.find(sep, start);
                out.push_back(s.substr(start, at == std::string_view::npos ? std::string_view::npos : at - start));
                if (at == std::string_view::npos)
                {
                    return out;
                }
                start = at + 1;
            }
        }

        inline auto number(std::string_view s, uint32& out) -> bool
        {
            if (s.empty())
            {
                return false;
            }
            const auto [end, ec] = std::from_chars(s.data(), s.data() + s.size(), out);
            return ec == std::errc{} && end == s.data() + s.size();
        }
    } // namespace detail

    // nullopt for anything malformed: a missing field, a non-number, no
    // conditions, no actions, or a value outside 16 bits where one is needed
    inline auto parseRow(std::string_view text) -> std::optional<gambits::Gambit_t>
    {
        const auto fields = detail::split(text, '|');
        if (fields.size() != 4)
        {
            return std::nullopt;
        }

        gambits::Gambit_t g;
        uint32            value = 0;
        if (!detail::number(fields[0], value) || value > 0xFFFF)
        {
            return std::nullopt;
        }
        g.target_selector = static_cast<gambits::G_TARGET>(value);

        for (auto groupText : detail::split(fields[1], '&'))
        {
            gambits::G_LOGIC logic = gambits::G_LOGIC::AND;
            if (!groupText.empty() && groupText.front() == '?')
            {
                logic = gambits::G_LOGIC::OR;
                groupText.remove_prefix(1);
            }
            std::vector<gambits::Predicate_t> predicates;
            for (const auto predicateText : detail::split(groupText, ','))
            {
                const auto parts = detail::split(predicateText, ':');
                uint32     condition = 0;
                uint32     arg       = 0;
                if (parts.size() != 2 || !detail::number(parts[0], condition) || condition > 0xFFFF || !detail::number(parts[1], arg))
                {
                    return std::nullopt;
                }
                predicates.emplace_back(static_cast<gambits::G_CONDITION>(condition), arg);
            }
            if (predicates.empty())
            {
                return std::nullopt;
            }
            g.predicate_groups.emplace_back(logic, std::move(predicates));
        }

        for (const auto actionText : detail::split(fields[2], '+'))
        {
            const auto parts = detail::split(actionText, ':');
            uint32     reaction = 0;
            uint32     select   = 0;
            uint32     arg      = 0;
            if (parts.size() != 3 || !detail::number(parts[0], reaction) || reaction > 0xFFFF ||
                !detail::number(parts[1], select) || select > 0xFFFF || !detail::number(parts[2], arg))
            {
                return std::nullopt;
            }
            g.actions.emplace_back(static_cast<gambits::G_REACTION>(reaction), static_cast<gambits::G_SELECT>(select), arg);
        }
        if (g.actions.empty())
        {
            return std::nullopt;
        }

        if (!detail::number(fields[3], value) || value > 0xFFFF)
        {
            return std::nullopt;
        }
        g.retry_delay = static_cast<uint16>(value);
        return g;
    }
} // namespace pawn::text
