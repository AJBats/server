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

#include "venture_spots.h"

#include "pawn.h"

#include "common/database.h"
#include "common/logging.h"
#include "data/datasets/zones/settings/dataset.h"
#include "data/loader.h"
#include "entities/char_entity.h"
#include "entities/mob_entity.h"
#include "utils/zoneutils.h"
#include "zone.h"

#include <algorithm>
#include <chrono>
#include <tuple>
#include <unordered_map>
#include <utility>

namespace pawn::venturespots
{
    namespace
    {
        using ZoneSettingsDataset = xi::data::datasets::zones::settings::Dataset;

        // Where each zone's zone lines lead, from its settings file: read once
        auto links() -> const std::unordered_map<uint16, std::vector<uint16>>&
        {
            static const auto graph = []
            {
                const auto                                      began = std::chrono::steady_clock::now();
                std::unordered_map<uint16, std::vector<uint16>> out;
                zoneutils::ForEachZone(
                    [&](CZone* PZone)
                    {
                        const auto id = static_cast<uint16>(PZone->GetID());
                        if (const auto settings = xi::data::loadZoneFile<ZoneSettingsDataset>(PZone->GetID()))
                        {
                            auto& to = out[id];
                            for (const auto& line : settings->ZoneLines)
                            {
                                const auto dest = static_cast<uint16>(line.DestinationZone);
                                if (dest != id && std::ranges::find(to, dest) == to.end())
                                {
                                    to.push_back(dest);
                                }
                            }
                        }
                    });
                ShowInfoFmt("venturespots: the zone lines of {} zones read in {} ms", out.size(),
                            std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - began).count());
                return out;
            }();
            return graph;
        }

        // The level of a zone's usual monster: the median top level of the
        // monsters loaded there, notorious and scripted ones left out; none
        // for a zone with none, a town. Read once a zone
        auto usualLevel(const uint16 zone) -> std::optional<uint8>
        {
            static std::unordered_map<uint16, std::optional<uint8>> known;
            if (const auto it = known.find(zone); it != known.end())
            {
                return it->second;
            }
            std::vector<uint8> levels;
            if (auto* PZone = zoneutils::GetZone(static_cast<xi::ZoneId>(zone)); PZone != nullptr)
            {
                PZone->ForEachMob(
                    [&](CMobEntity* PMob)
                    {
                        if ((PMob->m_Type & xi::MobType::Notorious) != xi::MobType::Normal || (PMob->m_SpawnType & xi::SpawnType::Scripted) != xi::SpawnType::Normal)
                        {
                            return;
                        }
                        if (PMob->m_maxLevel > 0)
                        {
                            levels.push_back(PMob->m_maxLevel);
                        }
                    });
            }
            std::optional<uint8> level;
            if (!levels.empty())
            {
                std::ranges::sort(levels);
                level = levels[levels.size() / 2];
            }
            return known[zone] = level;
        }

        // Where she stands and her level: her character in memory, else her saved row
        auto standing(const uint32 charid) -> std::optional<std::pair<uint16, uint8>>
        {
            const CCharEntity* PChar = pawn::findPawn(charid);
            PChar                    = PChar != nullptr ? PChar : zoneutils::GetChar(charid);
            if (PChar != nullptr)
            {
                return std::pair(static_cast<uint16>(PChar->getZone()), PChar->GetMLevel());
            }
            const auto rset = db::preparedStmt("SELECT c.pos_zone, COALESCE(s.mlvl, 1) AS mlvl FROM chars c LEFT JOIN char_stats s ON s.charid = c.charid "
                                               "WHERE c.charid = ?",
                                               charid);
            if (!rset || !rset->next())
            {
                return std::nullopt;
            }
            return std::pair(rset->get<uint16>("pos_zone"), rset->get<uint8>("mlvl"));
        }

        // The zones she can walk to from `start` at her level, each with the
        // zone lines it lies away
        auto walkable(const uint16 start, const uint8 level) -> std::unordered_map<uint16, uint8>
        {
            std::unordered_map<uint16, uint8> hops{ { start, 0 } };
            std::vector<uint16>               frontier{ start };
            while (!frontier.empty())
            {
                std::vector<uint16> next;
                for (const auto zone : frontier)
                {
                    const auto it = links().find(zone);
                    if (it == links().end())
                    {
                        continue;
                    }
                    for (const auto to : it->second)
                    {
                        if (hops.contains(to))
                        {
                            continue;
                        }
                        if (const auto usual = usualLevel(to); usual.has_value() && *usual > level)
                        {
                            continue;
                        }
                        hops[to] = static_cast<uint8>(hops[zone] + 1);
                        next.push_back(to);
                    }
                }
                frontier = std::move(next);
            }
            return hops;
        }

        // A water's name as the game's data writes it, for people: Bibiki
        // Bay's abbreviations spelled out ("PI - South Beach", "Purgonorgo
        // Isle - South Beach"; "BB - South Seaside", "South Seaside")
        auto spelledOut(std::string name) -> std::string
        {
            if (name.starts_with("PI - "))
            {
                return "Purgonorgo Isle - " + name.substr(5);
            }
            if (name.starts_with("BB - "))
            {
                return name.substr(5);
            }
            return name;
        }

        // Every fishing area of the game, by zone and area: named only where
        // its zone has several, a name twice in one zone told apart by a
        // number ("Whole Zone", "Whole Zone 2"). Read once
        auto waters() -> const std::vector<Spot>&
        {
            static const auto all = []
            {
                std::vector<Spot> out;
                const auto rset = db::preparedStmt("SELECT zoneid, areaid, name, center_x, center_y, center_z FROM fishing_area ORDER BY zoneid, areaid");
                while (rset && rset->next())
                {
                    out.push_back({ .zone   = rset->get<uint16>("zoneid"),
                                    .area   = rset->get<uint16>("areaid"),
                                    .name   = spelledOut(rset->get<std::string>("name")),
                                    .centre = position_t(rset->get<float>("center_x"), rset->get<float>("center_y"), rset->get<float>("center_z"), 0, 0) });
                }
                std::unordered_map<uint16, uint32> perZone;
                for (const auto& w : out)
                {
                    ++perZone[w.zone];
                }
                std::unordered_map<std::string, uint32> seen; // "zone/name" -> how many so far
                for (auto& w : out)
                {
                    if (perZone[w.zone] == 1)
                    {
                        w.name.clear();
                        continue;
                    }
                    const auto n = ++seen[fmt::format("{}/{}", w.zone, w.name)];
                    if (n > 1)
                    {
                        w.name += fmt::format(" {}", n);
                    }
                }
                return out;
            }();
            return all;
        }
    } // namespace

    auto spotsOf(const uint32 charid) -> std::vector<Spot>
    {
        std::vector<Spot> out;
        const auto        here = standing(charid);
        if (!here.has_value())
        {
            return out;
        }
        const auto hops = walkable(here->first, here->second);
        for (const auto& w : waters())
        {
            if (const auto it = hops.find(w.zone); it != hops.end())
            {
                auto spot = w;
                spot.hops = it->second;
                out.push_back(std::move(spot));
            }
        }
        std::ranges::stable_sort(out, {}, [](const Spot& s) { return std::tuple(s.hops, s.zone, s.area); });
        return out;
    }

    auto reaches(const uint32 charid, const uint16 zone, const uint16 area) -> bool
    {
        const auto spots = spotsOf(charid);
        return std::ranges::any_of(spots, [&](const Spot& s) { return s.zone == zone && s.area == area; });
    }

    auto waterOf(const uint16 zone, const uint16 area) -> std::optional<Spot>
    {
        for (const auto& w : waters())
        {
            if (w.zone == zone && w.area == area)
            {
                return w;
            }
        }
        return std::nullopt;
    }
} // namespace pawn::venturespots
