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

#include "subzones.h"

#include <cmath>
#include <functional>
#include <limits>
#include <optional>
#include <queue>
#include <unordered_map>
#include <vector>

// The world's sub-zones joined (the travel walk): every zone line from the
// piece its trigger stands on to the piece it lands her on, and every drop
// from one piece of a zone to another. A trek's route is the shortest walk
// across them -- the crow's distance from where she enters a piece to where
// she leaves it, and a zone line's crossing -- so she leaves by the right
// gate. A piece of 0 is one no navmesh knows (a zone without one): it takes
// and reaches every line of its zone, as travel did before sub-zones.
namespace cardian::subzones
{
    constexpr float kLineCost = 50.0f; // a zone line's crossing, in yalms: the load, and the gate

    struct Line
    {
        uint16     zone{};   // the zone it leaves
        uint16     to{};     // the zone it lands in
        position_t origin{}; // its trigger
        position_t arrival{};
        Piece      originPiece{};  // the trigger's piece of `zone`
        Piece      arrivalPiece{}; // the arrival's piece of `to`
    };

    struct World
    {
        std::vector<Line>                              lines;
        std::unordered_map<uint16, std::vector<Drop>> drops; // by zone
    };

    // Where a trek goes next: the first zone line of the shortest walk, or
    // none when the goal is in her own zone and piece already, or reached by
    // a drop she walks to; `reachable` false when no walk joins them at all
    struct Plan
    {
        bool                       reachable{};
        std::optional<std::size_t> firstLine;
        float                      length{};
        int                        lines{}; // the zone lines crossed
    };

    namespace detail
    {
        inline auto crow(const position_t& a, const position_t& b) -> float
        {
            return std::sqrt((a.x - b.x) * (a.x - b.x) + (a.y - b.y) * (a.y - b.y) + (a.z - b.z) * (a.z - b.z));
        }

        inline auto joins(const Piece a, const Piece b) -> bool
        {
            return a == 0 || b == 0 || a == b;
        }
    } // namespace detail

    // The shortest walk from `at` (in `zone`, on `piece`) to `toZone` -- on
    // `toPiece` when given, and as near `toPoint` as it can end when given
    inline auto plan(const World& world, const uint16 zone, const Piece piece, const position_t& at, const uint16 toZone, const std::optional<Piece> toPiece,
                     const std::optional<position_t> toPoint) -> Plan
    {
        // the states: 0 the start, 1 + i landed by line i, 1 + lines + j at the foot of drop j
        struct Where
        {
            uint16     zone{};
            Piece      piece{};
            position_t at{};
        };
        std::vector<Where>                                         where{ { zone, piece, at } };
        std::vector<std::pair<uint16, const Drop*>>                dropOf;
        for (const auto& line : world.lines)
        {
            where.push_back({ line.to, line.arrivalPiece, line.arrival });
        }
        for (const auto& [z, drops] : world.drops)
        {
            for (const auto& drop : drops)
            {
                where.push_back({ z, drop.to, drop.bottom });
                dropOf.emplace_back(z, &drop);
            }
        }
        const std::size_t lineBase = 1;
        const std::size_t dropBase = 1 + world.lines.size();
        std::unordered_map<uint16, std::vector<std::size_t>> linesFrom; // each zone's lines out
        std::unordered_map<uint16, std::vector<std::size_t>> dropsIn;   // each zone's drops
        for (std::size_t i = 0; i < world.lines.size(); ++i)
        {
            linesFrom[world.lines[i].zone].push_back(i);
        }
        for (std::size_t j = 0; j < dropOf.size(); ++j)
        {
            dropsIn[dropOf[j].first].push_back(j);
        }

        constexpr float          kNever = std::numeric_limits<float>::infinity();
        std::vector<float>       cost(where.size(), kNever);
        std::vector<std::size_t> via(where.size(), SIZE_MAX); // the state each was reached from
        std::vector<int>         crossed(where.size(), 0);
        using Entry = std::pair<float, std::size_t>;
        std::priority_queue<Entry, std::vector<Entry>, std::greater<>> open;
        cost[0] = 0.0f;
        open.push({ 0.0f, 0 });

        float       best     = kNever;
        std::size_t bestEnd  = SIZE_MAX;
        while (!open.empty())
        {
            const auto [c, s] = open.top();
            open.pop();
            if (c > cost[s] || c >= best)
            {
                continue;
            }
            const Where& here = where[s];
            if (here.zone == toZone && (!toPiece.has_value() || detail::joins(here.piece, *toPiece)))
            {
                const float total = c + (toPoint.has_value() ? detail::crow(here.at, *toPoint) : 0.0f);
                if (total < best)
                {
                    best    = total;
                    bestEnd = s;
                }
            }
            const auto relax = [&](const std::size_t next, const float step, const int lines)
            {
                if (c + step < cost[next])
                {
                    cost[next]    = c + step;
                    via[next]     = s;
                    crossed[next] = crossed[s] + lines;
                    open.push({ cost[next], next });
                }
            };
            if (const auto it = linesFrom.find(here.zone); it != linesFrom.end())
            {
                for (const std::size_t i : it->second)
                {
                    const Line& line = world.lines[i];
                    if (detail::joins(here.piece, line.originPiece))
                    {
                        relax(lineBase + i, detail::crow(here.at, line.origin) + kLineCost, 1);
                    }
                }
            }
            if (const auto it = dropsIn.find(here.zone); it != dropsIn.end())
            {
                for (const std::size_t j : it->second)
                {
                    const Drop* drop = dropOf[j].second;
                    if (detail::joins(here.piece, drop->from))
                    {
                        relax(dropBase + j, detail::crow(here.at, drop->top) + detail::crow(drop->top, drop->bottom), 0);
                    }
                }
            }
        }

        Plan out;
        if (bestEnd == SIZE_MAX)
        {
            return out;
        }
        out.reachable = true;
        out.length    = best;
        out.lines     = crossed[bestEnd];
        // the first zone line on the way back from the end to the start
        for (std::size_t s = bestEnd; s != 0 && s != SIZE_MAX; s = via[s])
        {
            if (s >= lineBase && s < dropBase)
            {
                out.firstLine = s - lineBase;
            }
        }
        return out;
    }
} // namespace cardian::subzones
