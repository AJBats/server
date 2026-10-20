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

#include "porter_mesh.h"

#include "cardian_link.h"
#include "client_navmesh.h"
#include "collision_walls.h"

#include "common/logging.h"
#include "common/scheduler.h"
#include "common/settings.h"
#include "navmesh/detour_navmesh.h"
#include "navmesh/navmesh_builder.h"
#include "utils/zoneutils.h"
#include "ximesh/ximesh.h"
#include "zone.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <unordered_map>

namespace pawn::porter::mesh
{
    namespace
    {
        // The recipe's version: raised when the build's own rule changes
        // (client_navmesh.h, the builder's links), so every older file is
        // stale at once
        constexpr int kRecipeVersion = 3;

        struct Entry
        {
            bool                           tried = false; // its file looked for
            std::unique_ptr<DetourNavMesh> mesh;          // read, and built for today's settings
        };

        // By zone, read and written on the map's thread alone; never torn
        // down, as the pawn container is not: a mesh outlives its readers
        auto entries() -> std::unordered_map<uint16, Entry>&
        {
            static auto* all = new std::unordered_map<uint16, Entry>();
            return *all;
        }

        auto meshPath(const uint16 zoneId) -> std::string
        {
            return cardian::walls::builtPath(zoneId, "porter.nav");
        }

        auto recipePath(const uint16 zoneId) -> std::string
        {
            return cardian::walls::builtPath(zoneId, "porter.recipe");
        }

        // The zone's own navmesh config -- its skip planes and its seeds --
        // with the client's rule and the rider's width in place of the
        // monsters', and no walls
        auto configFor(const CZone* zone) -> NavMeshConfig
        {
            auto        config = zone->cardianNavMeshConfig();
            const float width  = std::max(0.0f, settings::get<float>("pawn.PORTER_WIDTH"));
            const float added  = std::max(0.0f, width - config.agentRadius);
            config.cardianWalls.clear();
            config.cardianClient      = true;
            config.agentMaxClimb      = cardian::client::kStep;
            config.walkableSlopeAngle = cardian::client::kSlope;
            config.cellHeight         = cardian::client::kCellHeight;
            config.filterLedgeSpans   = false;
            // finer cells, and as many more of them a tile, so a tile covers the same ground
            config.tileSize = static_cast<int>(std::lround(static_cast<float>(config.tileSize) * config.cellSize / cardian::client::kCellSize));
            config.cellSize = cardian::client::kCellSize;
            config.agentRadius += added;
            config.offMeshHorizReach += 2.0f * added; // the margin the width keeps on both sides of a ledge, back from its lip and out from its foot
            return config;
        }

        // FNV-1a, over the bytes of a value or of a vector's elements
        struct Hash
        {
            uint64 h = 1469598103934665603ull;

            void bytes(const void* p, const std::size_t n)
            {
                const auto* b = static_cast<const unsigned char*>(p);
                for (std::size_t i = 0; i < n; ++i)
                {
                    h = (h ^ b[i]) * 1099511628211ull;
                }
            }

            template <typename T>
            void add(const T& v)
            {
                bytes(&v, sizeof(v));
            }

            template <typename T>
            void all(const std::vector<T>& v)
            {
                add(v.size());
                if (!v.empty())
                {
                    bytes(v.data(), v.size() * sizeof(T));
                }
            }
        };

        // Every field of the build's config, the zone's own overrides and
        // seeds among them
        auto configHash(const NavMeshConfig& c) -> uint64
        {
            Hash h;
            for (const float v : { c.cellSize, c.cellHeight, c.walkableSlopeAngle, c.agentHeight, c.agentRadius, c.agentMaxClimb, c.maxEdgeLen,
                                   c.maxSimplificationError, c.detailSampleDist, c.detailSampleMaxError, c.offMeshMaxDrop, c.offMeshHorizReach })
            {
                h.add(v);
            }
            for (const int v : { c.minRegionArea, c.mergeRegionArea, c.maxVertsPerPoly, c.tileSize })
            {
                h.add(v);
            }
            for (const bool v : { c.filterLowHangingObstacles, c.filterLedgeSpans, c.filterWalkableLowHeightSpans, c.generateOffMeshLinks, c.cardianClient })
            {
                h.add(v);
            }
            h.all(c.ySkipPlanes);
            h.add(c.skipSpheres.size());
            for (const auto& s : c.skipSpheres)
            {
                h.add(s.center);
                h.add(s.radius);
            }
            h.all(c.seeds);
            h.all(c.cardianWalls);
            return h.h;
        }

        // What a zone's porter navmesh is built from, as one line: the
        // build's main numbers to read, a hash of its whole config, and the
        // size of the zone's collision, so a change to any -- a new width, a
        // zone's new overrides or seeds, an upstream merge's new ximesh --
        // leaves the file stale. A change to the builder's own code is not
        // seen: kRecipeVersion, and the base merge's checklist (CLAUDE.md)
        auto recipeOf(const CZone* zone, const NavMeshConfig& c) -> std::string
        {
            const auto* x = zone->xiMesh();
            return fmt::format("porter {} width {:.3f} climb {:.3f} slope {:.1f} cell {:.3f} {:.3f} reach {:.2f} config {:016x} collision {} {} {} {}x{}",
                               kRecipeVersion, c.agentRadius, c.agentMaxClimb, c.walkableSlopeAngle, c.cellSize, c.cellHeight, c.offMeshHorizReach,
                               configHash(c), x->blocks().size(), x->placements().size(), x->entries().size(), x->gridWidth(), x->gridHeight());
        }

        // The recipe a build under way leaves until its file is written:
        // never today's, so a build that died part-way is stale
        constexpr auto kBuilding = "building";

        // Why a zone has no porter navmesh for today's settings, or empty when it has one
        auto staleness(const CZone* zone) -> std::string
        {
            const auto zoneId = static_cast<uint16>(zone->GetID());
            if (zone->xiMesh() == nullptr)
            {
                return "it has no collision";
            }
            std::ifstream in(recipePath(zoneId));
            std::string   saved;
            if (!in || !std::getline(in, saved))
            {
                return "never built";
            }
            if (saved == kBuilding)
            {
                return "its build never finished";
            }
            if (saved != recipeOf(zone, configFor(zone)))
            {
                return "built for other settings";
            }
            std::error_code ec;
            return std::filesystem::exists(meshPath(zoneId), ec) ? std::string() : std::string("its file is gone");
        }

        auto nameOf(const uint16 zoneId) -> std::string
        {
            auto* zone = zoneutils::GetZone(static_cast<xi::ZoneId>(zoneId));
            return zone != nullptr ? std::string(zone->getName()) : fmt::format("zone {}", zoneId);
        }
    } // namespace

    auto of(CZone* zone) -> const dtNavMesh*
    {
        if (zone == nullptr)
        {
            return nullptr;
        }
        const auto zoneId = static_cast<uint16>(zone->GetID());
        auto&      entry  = entries()[zoneId];
        if (!entry.tried)
        {
            entry.tried      = true;
            const auto stale = staleness(zone);
            auto       mesh  = std::make_unique<DetourNavMesh>(zoneId);
            if (!stale.empty())
            {
                ShowWarningFmt("porter: {} has no navmesh for the rider ({}): run ./dev-servers.ps1 porter-navmeshes; its rides keep to the zone's navmesh",
                               zone->getName(), stale);
            }
            else if (!mesh->load(meshPath(zoneId)))
            {
                ShowWarningFmt("porter: {}'s navmesh for the rider would not load ({}): its rides keep to the zone's navmesh", zone->getName(), meshPath(zoneId));
            }
            else
            {
                entry.mesh = std::move(mesh);
            }
        }
        return entry.mesh != nullptr ? entry.mesh->detourMesh() : nullptr;
    }

    void check(const std::vector<uint16>& zones)
    {
        std::vector<std::string> missing;
        for (const auto zoneId : zones)
        {
            auto* zone = zoneutils::GetZone(static_cast<xi::ZoneId>(zoneId));
            if (zone == nullptr)
            {
                continue;
            }
            if (const auto stale = staleness(zone); !stale.empty())
            {
                missing.push_back(fmt::format("{} ({})", zone->getName(), stale));
            }
        }
        if (missing.empty())
        {
            ShowInfoFmt("porter: navmeshes for the rider in all {} zones a ride can plan in", zones.size());
            return;
        }
        ShowWarningFmt("porter: {} of {} zones a ride can plan in have no navmesh for the rider, and their rides keep to the zones' navmeshes: {}. "
                       "Run ./dev-servers.ps1 porter-navmeshes",
                       missing.size(), zones.size(), fmt::join(missing, ", "));
    }

    void precalc(const std::vector<uint16>& zones)
    {
        auto* scheduler = cardian::link::scheduler();
        if (scheduler == nullptr)
        {
            ShowErrorFmt("porter: the navmeshes for the rider cannot be built: the map handed no scheduler");
            std::exit(1);
        }
        ShowInfoFmt("porter: building the navmeshes for the rider of {} zones", zones.size());
        scheduler->postToMainThread(
            [zones, scheduler]() -> Task<void>
            {
                std::size_t written = 0;
                for (const auto zoneId : zones)
                {
                    auto* zone = zoneutils::GetZone(static_cast<xi::ZoneId>(zoneId));
                    if (zone == nullptr || zone->xiMesh() == nullptr)
                    {
                        ShowWarningFmt("porter: {} has no collision: no navmesh for the rider", nameOf(zoneId));
                        continue;
                    }
                    const auto config = configFor(zone);
                    {
                        std::ofstream building(recipePath(zoneId), std::ios::trunc);
                        building << kBuilding << '\n';
                    }
                    NavMeshBuilder builder(*zone->xiMesh());
                    auto*          built = co_await builder.buildAsync(*scheduler, fmt::format("{} (the porter's)", zone->getName()), zoneId, config);
                    DetourNavMesh  mesh(zoneId);
                    if (built == nullptr || !mesh.installNavMesh(built) || !mesh.save(meshPath(zoneId)))
                    {
                        ShowWarningFmt("porter: {}'s navmesh for the rider could not be built or written", zone->getName());
                        continue;
                    }
                    std::ofstream out(recipePath(zoneId), std::ios::trunc);
                    out << recipeOf(zone, config) << '\n';
                    if (!out)
                    {
                        ShowWarningFmt("porter: {}'s navmesh recipe could not be written ({})", zone->getName(), recipePath(zoneId));
                        continue;
                    }
                    ++written;
                }
                ShowInfoFmt("porter: navmeshes for the rider written for {} of {} zones, in {}; exiting", written, zones.size(), ".cache/cardian_navmeshes");
                std::exit(written == zones.size() ? 0 : 1);
            });
    }
} // namespace pawn::porter::mesh
