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

#include "common/logging.h"

#include <cmath>
#include <cstdint>
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
        uint16     zone{};    // the zone it leaves
        uint16     to{};      // the zone it lands in
        position_t origin{};  // its trigger, as the zone's data places it
        position_t arrival{}; // where it sets her down, as the zone's data places it
        LineEnd    leaves;    // the trigger's piece of `zone`, and the spot she walks to to take it (subzones.h lineEnd)
        LineEnd    lands;     // the arrival's piece of `to`, and the spot it sets her down on
    };

    // The search's fixed half, built by `join` from the world's lines and
    // drops: every place a walk stands after a crossing or a drop, and each
    // piece's ways out to them
    struct Graph
    {
        struct Place
        {
            uint16      zone{};
            Piece       piece{};
            position_t  at{};
            std::size_t line = SIZE_MAX; // the line that sets her down here; SIZE_MAX at a drop's foot
        };

        // A way out of a piece: a walk to `at`, then a line's crossing or a
        // drop's fall to the place `to`
        struct Exit
        {
            position_t  at{};
            float       cost{};  // past the walk: kLineCost for a line, the fall for a drop
            std::size_t to{};    // its place, in `places`
            int         lines{}; // the zone lines it crosses
        };

        uint64_t                                               version = UINT64_MAX; // the world's version it was joined at; UINT64_MAX before a join
        std::vector<Place>                                     places;
        std::vector<Exit>                                      exits;
        std::unordered_map<uint64_t, std::vector<std::size_t>> byPiece; // each piece's ways out, by `key`; piece 0's are open to every piece of its zone
        std::unordered_map<uint16, std::vector<std::size_t>>   byZone;  // every way out of a zone, for a body on piece 0

        static auto key(const uint16 zone, const Piece piece) -> uint64_t
        {
            return (static_cast<uint64_t>(zone) << 32) | piece;
        }

        // What the tables hold in memory, their maps' own nodes not counted
        auto bytes() const -> std::size_t
        {
            std::size_t total = places.capacity() * sizeof(Place) + exits.capacity() * sizeof(Exit);
            for (const auto& [_, ways] : byPiece)
            {
                total += sizeof(uint64_t) + ways.capacity() * sizeof(std::size_t);
            }
            for (const auto& [_, ways] : byZone)
            {
                total += sizeof(uint16) + ways.capacity() * sizeof(std::size_t);
            }
            return total;
        }
    };

    struct World
    {
        std::vector<Line>                              lines;
        std::unordered_map<uint16, std::vector<Drop>> drops; // by zone

        // Raised by whoever changes `lines` or `drops`, who then joins them
        // again: `plan` warns when its tables were joined at another version
        uint64_t version = 0;
        Graph    graph; // `join` builds it, after any change to the lines or drops
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

    // The search's fixed half from the world's lines and drops: built once
    // the world is placed, and again after any change to it, so a plan
    // across the whole world reads its tables and builds none
    inline void join(World& world)
    {
        Graph graph;
        graph.version      = world.version;
        const auto addExit = [&](const uint16 zone, const Piece piece, const Graph::Exit& exit)
        {
            graph.byPiece[Graph::key(zone, piece)].push_back(graph.exits.size());
            graph.byZone[zone].push_back(graph.exits.size());
            graph.exits.push_back(exit);
        };
        for (std::size_t i = 0; i < world.lines.size(); ++i)
        {
            const Line& line = world.lines[i];
            addExit(line.zone, line.leaves.piece, { line.leaves.at, kLineCost, graph.places.size(), 1 });
            graph.places.push_back({ line.to, line.lands.piece, line.lands.at, i });
        }
        for (const auto& [zone, drops] : world.drops)
        {
            for (const auto& drop : drops)
            {
                addExit(zone, drop.from, { drop.top, detail::crow(drop.top, drop.bottom), graph.places.size(), 0 });
                graph.places.push_back({ zone, drop.to, drop.bottom });
            }
        }
        world.graph = std::move(graph);
    }

    // The shortest walk from `at` (in `zone`, on `piece`) to `toZone` -- on
    // `toPiece` when given, and as near `toPoint` as it can end when given
    inline auto plan(const World& world, const uint16 zone, const Piece piece, const position_t& at, const uint16 toZone, const std::optional<Piece> toPiece,
                     const std::optional<position_t> toPoint) -> Plan
    {
        if (world.graph.version != world.version)
        {
            // Changed and never joined again: a fault in whoever changed it.
            // The plan is made on a copy joined now, at the cost of the join
            ShowWarningFmt("travel: the walk's tables were joined at version {} of the world, which is at version {}; planning on a copy joined now",
                           world.graph.version, world.version);
            World joined = world;
            join(joined);
            return plan(joined, zone, piece, at, toZone, toPiece, toPoint);
        }
        const Graph& graph = world.graph;

        // The states: 0 the start, 1 + i at graph.places[i]. Their tables are
        // kept from one plan to the next -- the world's drops make them some
        // 365,000 long -- and a plan puts back only the entries it touched
        constexpr float kNever = std::numeric_limits<float>::infinity();
        struct Scratch
        {
            std::vector<float>       cost;
            std::vector<std::size_t> via; // the state each was reached from
            std::vector<int>         crossed;
            std::vector<std::size_t> touched;
        };
        thread_local Scratch scratch;
        const std::size_t    states = 1 + graph.places.size();
        if (scratch.cost.size() < states)
        {
            scratch.cost.resize(states, kNever);
            scratch.via.resize(states, SIZE_MAX);
            scratch.crossed.resize(states, 0);
        }
        struct PutBack
        {
            Scratch& s;
            ~PutBack()
            {
                for (const std::size_t t : s.touched)
                {
                    s.cost[t]    = std::numeric_limits<float>::infinity();
                    s.via[t]     = SIZE_MAX;
                    s.crossed[t] = 0;
                }
                s.touched.clear();
            }
        } putBack{ scratch };
        auto& cost    = scratch.cost;
        auto& via     = scratch.via;
        auto& crossed = scratch.crossed;
        auto& touched = scratch.touched;

        const auto placeOf = [&](const std::size_t s) -> Graph::Place
        {
            return s == 0 ? Graph::Place{ zone, piece, at } : graph.places[s - 1];
        };
        using Entry = std::pair<float, std::size_t>;
        std::priority_queue<Entry, std::vector<Entry>, std::greater<>> open;
        cost[0] = 0.0f;
        touched.push_back(0);
        open.push({ 0.0f, 0 });

        float       best    = kNever;
        std::size_t bestEnd = SIZE_MAX;
        while (!open.empty())
        {
            const auto [c, s] = open.top();
            open.pop();
            if (c > cost[s] || c >= best)
            {
                continue;
            }
            const Graph::Place here = placeOf(s);
            if (here.zone == toZone && (!toPiece.has_value() || detail::joins(here.piece, *toPiece)))
            {
                const float total = c + (toPoint.has_value() ? detail::crow(here.at, *toPoint) : 0.0f);
                if (total < best)
                {
                    best    = total;
                    bestEnd = s;
                }
            }
            const auto take = [&](const auto& ways, const auto key)
            {
                const auto it = ways.find(key);
                if (it == ways.end())
                {
                    return;
                }
                for (const std::size_t e : it->second)
                {
                    const Graph::Exit& exit = graph.exits[e];
                    const std::size_t  next = 1 + exit.to;
                    const float        step = c + detail::crow(here.at, exit.at) + exit.cost;
                    if (step < cost[next])
                    {
                        if (cost[next] == kNever)
                        {
                            touched.push_back(next);
                        }
                        cost[next]    = step;
                        via[next]     = s;
                        crossed[next] = crossed[s] + exit.lines;
                        open.push({ step, next });
                    }
                }
            };
            if (here.piece == 0)
            {
                take(graph.byZone, here.zone);
            }
            else
            {
                take(graph.byPiece, Graph::key(here.zone, here.piece));
                take(graph.byPiece, Graph::key(here.zone, 0));
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
            if (const auto line = graph.places[s - 1].line; line != SIZE_MAX)
            {
                out.firstLine = line;
            }
        }
        return out;
    }
} // namespace cardian::subzones
