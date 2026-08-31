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

#include "pawn_travel.h"

#include "common/logging.h"

#include "data/datasets/zones/settings/dataset.h"
#include "data/loader.h"
#include "utils/zoneutils.h"
#include "zone.h"

#include <deque>
#include <unordered_map>
#include <vector>

namespace
{
    struct Edge
    {
        xi::ZoneId to{};
        position_t walkTo{};
        position_t arriveAt{};
    };

    std::unordered_map<uint16, std::vector<Edge>> graph;
    bool                                          graphBuilt = false;

    void buildGraph()
    {
        graphBuilt = true;

        std::vector<xi::ZoneId> zoneIds;
        zoneutils::ForEachZone([&](CZone* PZone)
        {
            zoneIds.push_back(PZone->GetID());
        });

        uint32 edgeCount = 0;
        for (const auto zoneId : zoneIds)
        {
            const auto settings = xi::data::loadZoneFile<xi::data::datasets::zones::settings::Dataset>(zoneId);
            if (!settings)
            {
                continue;
            }

            for (const auto& line : settings->ZoneLines)
            {
                // Self-edges are mog-house doors and intra-zone teleport pads
                if (line.DestinationZone == zoneId)
                {
                    continue;
                }

                graph[static_cast<uint16>(zoneId)].emplace_back(Edge{ line.DestinationZone, line.Origin, line.Destination });
                ++edgeCount;
            }
        }

        ShowInfoFmt("pawn: travel graph built: {} zones, {} edges", graph.size(), edgeCount);
    }
} // namespace

namespace pawn::travel
{
    auto nextHop(const xi::ZoneId from, const xi::ZoneId to) -> std::optional<TravelHop>
    {
        if (from == to)
        {
            return std::nullopt;
        }

        if (!graphBuilt)
        {
            buildGraph();
        }

        const auto fromKey = static_cast<uint16>(from);
        const auto toKey   = static_cast<uint16>(to);

        // BFS recording the edge used to first reach each zone
        std::unordered_map<uint16, const Edge*> arrivedVia;
        std::unordered_map<uint16, uint16>      cameFrom;
        std::deque<uint16>                      frontier{ fromKey };
        arrivedVia[fromKey] = nullptr;

        while (!frontier.empty())
        {
            const uint16 current = frontier.front();
            frontier.pop_front();

            if (current == toKey)
            {
                break;
            }

            const auto it = graph.find(current);
            if (it == graph.end())
            {
                continue;
            }

            for (const auto& edge : it->second)
            {
                const auto next = static_cast<uint16>(edge.to);
                if (!arrivedVia.contains(next))
                {
                    arrivedVia[next] = &edge;
                    cameFrom[next]   = current;
                    frontier.push_back(next);
                }
            }
        }

        if (!arrivedVia.contains(toKey))
        {
            return std::nullopt;
        }

        // Walk back from the destination to find the edge leaving 'from'
        uint16 step = toKey;
        while (cameFrom.at(step) != fromKey)
        {
            step = cameFrom.at(step);
        }

        const Edge* first = arrivedVia.at(step);
        return TravelHop{ first->to, first->walkTo, first->arriveAt };
    }
} // namespace pawn::travel
