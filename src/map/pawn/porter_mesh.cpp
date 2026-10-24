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
#include "navmesh_dump.h"
#include "route_search.h"

#include "common/logging.h"
#include "common/scheduler.h"
#include "common/settings.h"
#include "navmesh/detour_navmesh.h"
#include "navmesh/navmesh_builder.h"
#include "utils/zoneutils.h"
#include "ximesh/ximesh.h"
#include "ximesh/ximesh_impl.h"
#include "zone.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <unordered_map>

// The builder flags the tight strip with the bit the route search reads
static_assert(cardian::client::kTightFlag == cardian::route::kPolyTight);

namespace pawn::porter::mesh
{
    namespace
    {
        // The recipe's version: raised when the build's own rule changes
        // (client_navmesh.h, the builder's links), so every older file is
        // stale at once
        constexpr int kRecipeVersion = 13;

        struct Entry
        {
            std::once_flag                 read; // its file looked for, once, by whoever asks first
            std::unique_ptr<DetourNavMesh> mesh; // read, and built for today's settings
        };

        // By zone, found and added under of()'s lock; never torn down, as
        // the pawn container is not: a mesh outlives its readers
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

        auto zoneName(const CZone* zone) -> std::string
        {
            auto* named = zoneutils::GetZone(zone->GetID()); // its name is read through a zone that is not const
            return named != nullptr ? std::string(named->getName()) : std::string();
        }

        // The zone's collision with its props in it -- the sub-models a city's
        // counters, furniture and rooms stand in, which upstream's ximesh leaves
        // out -- merged offline from the client's own files by
        // tools/walls/props.py; the zone's own collision where there is none
        auto propsPath(const CZone* zone) -> std::string
        {
            return fmt::format(".cache/cardian_ximeshes/{}.ximesh", zoneName(zone));
        }

        // A zone's merged props: the merged file's size, the recipe's mark of
        // which props went in (0 for none), and whether it was merged from
        // a collision upstream has since changed -- props.py writes the size
        // of the ximesh it merged from beside it (<zone>.source), and a
        // navmesh built from an outdated merge would be built from the old
        // collision
        struct Props
        {
            uintmax_t size     = 0;
            bool      outdated = false;
        };

        auto propsOf(const CZone* zone) -> Props
        {
            std::error_code ec;
            const auto      path = propsPath(zone);
            const auto      size = std::filesystem::file_size(path, ec);
            if (ec)
            {
                return {};
            }
            std::ifstream in(fmt::format(".cache/cardian_ximeshes/{}.source", zoneName(zone)));
            uintmax_t     mergedFrom = 0;
            in >> mergedFrom;
            const auto now = std::filesystem::file_size(fmt::format("ximeshes/{}.ximesh", zoneName(zone)), ec);
            return Props{ size, !in || ec || mergedFrom != now };
        }

        // The zone's own navmesh config -- its skip planes and its seeds --
        // with the client's rule in place of the monsters': the client's body
        // kept off every edge, and the rider's width past it kept where there
        // is room (the tight strip between, client_navmesh.h); and no walls
        auto configFor(const CZone* zone) -> NavMeshConfig
        {
            auto        config = zone->cardianNavMeshConfig();
            const float added  = std::max(0.0f, cardian::client::kBody - config.agentRadius);
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
            config.offMeshHorizReach += 2.0f * added; // the margin the body keeps on both sides of a ledge, back from its lip and out from its foot
            config.cardianBerth = std::max(config.agentRadius, settings::get<float>("pawn.PORTER_WIDTH"));
            // In a dungeon a ledge links down to ground as far below as
            // pawn.CLIENT_NAVMESH_DROP: a client drops from any height, and a
            // dungeon's ways down are often taller than the monsters' few
            // yalms (Palborough Mines' refiner ledge, some 16). Read through a
            // zone that is not const, as its type is
            if (auto* PZone = zoneutils::GetZone(zone->GetID()); PZone != nullptr && (PZone->GetTypeMask() & xi::ZoneType::Dungeon) != xi::ZoneType::Unknown)
            {
                config.offMeshMaxDrop = std::max(config.offMeshMaxDrop, settings::get<float>("pawn.CLIENT_NAVMESH_DROP"));
            }
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
                                   c.maxSimplificationError, c.detailSampleDist, c.detailSampleMaxError, c.offMeshMaxDrop, c.offMeshHorizReach, c.cardianBerth })
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
        // build's main numbers to read, a hash of its whole config, the size
        // of the zone's collision and of its merged props where it has them
        // (a zone without keeps the recipe it had before props), so a change to any
        // -- a new width, a zone's new overrides or seeds, an upstream merge's
        // new ximesh, props merged again -- leaves the file stale. A change to
        // the builder's own code is not seen: kRecipeVersion, and the base
        // merge's checklist (CLAUDE.md)
        auto recipeOf(const CZone* zone, const NavMeshConfig& c) -> std::string
        {
            const auto* x     = zone->xiMesh();
            const auto  props = propsOf(zone).size;
            return fmt::format("porter {} body {:.3f} width {:.3f} climb {:.3f} slope {:.1f} cell {:.3f} {:.3f} reach {:.2f} config {:016x} collision {} {} {} {}x{}{}",
                               kRecipeVersion, c.agentRadius, c.cardianBerth, c.agentMaxClimb, c.walkableSlopeAngle, c.cellSize, c.cellHeight, c.offMeshHorizReach,
                               configHash(c), x->blocks().size(), x->placements().size(), x->entries().size(), x->gridWidth(), x->gridHeight(),
                               props > 0 ? fmt::format(" props {}", props) : std::string());
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
            if (propsOf(zone).outdated)
            {
                return "its props were merged from an older collision: run tools/walls/props.py merge first";
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

    auto isTown(CZone* zone) -> bool
    {
        // The ferries' decks, the pirates' crossings too
        static constexpr std::array kFerries{ xi::ZoneId::ShipBoundForSelbina, xi::ZoneId::ShipBoundForMhaura,
                                              xi::ZoneId::ShipBoundForSelbinaPirates, xi::ZoneId::ShipBoundForMhauraPirates };
        const auto* own = zone != nullptr ? dynamic_cast<const DetourNavMesh*>(zone->navMesh()) : nullptr;
        return own != nullptr && own->detourMesh() != nullptr && zone->xiMesh() != nullptr &&
               ((zone->GetTypeMask() & xi::ZoneType::City) != xi::ZoneType::Unknown || std::ranges::contains(kFerries, zone->GetID()));
    }

    auto walkedOnClient(CZone* zone) -> bool
    {
        if (zone == nullptr)
        {
            return false;
        }
        if (isTown(zone))
        {
            return settings::get<bool>("pawn.TOWN_CLIENT_NAVMESH");
        }
        if (!settings::get<bool>("pawn.CLIENT_NAVMESH_EVERYWHERE"))
        {
            return false;
        }
        // One built for it: its file written, looked for once a zone. A
        // stale one is said by of(), which then answers nullptr
        static auto*    lock   = new std::mutex();
        static auto*    built  = new std::unordered_map<uint16, bool>();
        const auto      zoneId = static_cast<uint16>(zone->GetID());
        std::lock_guard held(*lock);
        const auto [it, added] = built->try_emplace(zoneId, false);
        if (added)
        {
            std::error_code ec;
            it->second = zone->xiMesh() != nullptr && std::filesystem::exists(meshPath(zoneId), ec);
        }
        return it->second;
    }

    auto of(CZone* zone) -> const dtNavMesh*
    {
        if (zone == nullptr)
        {
            return nullptr;
        }
        // asked from the map's thread (a ride's plan) and from the trip
        // checks' worker (porter.cpp). The lock only finds the zone's entry;
        // each zone is read once, by whichever asks first, and one who asks
        // for that zone meanwhile waits for the read -- never for another
        // zone's (a read takes up to 70 ms, its zone's data files parsed for
        // the recipe)
        static auto* lock   = new std::mutex();
        const auto   zoneId = static_cast<uint16>(zone->GetID());
        Entry*       entry  = nullptr;
        {
            std::lock_guard held(*lock);
            entry = &entries()[zoneId]; // a node of its own: no rehash moves it
        }
        std::call_once(entry->read, [&]()
        {
            const auto stale = staleness(zone);
            auto       mesh  = std::make_unique<DetourNavMesh>(zoneId);
            if (!stale.empty())
            {
                ShowWarningFmt("porter: {} has no navmesh by the client's rule ({}): run ./dev-servers.ps1 porter-navmeshes; its rides and its town walks keep to the zone's navmesh",
                               zone->getName(), stale);
            }
            else if (!mesh->load(meshPath(zoneId)))
            {
                ShowWarningFmt("porter: {}'s navmesh by the client's rule would not load ({}): its rides and its town walks keep to the zone's navmesh", zone->getName(), meshPath(zoneId));
            }
            else
            {
                entry->mesh = std::move(mesh);
            }
        });
        return entry->mesh != nullptr ? entry->mesh->detourMesh() : nullptr;
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
            ShowInfoFmt("porter: navmeshes by the client's rule in all {} zones a ride can plan in and the towns", zones.size());
            return;
        }
        ShowWarningFmt("porter: {} of {} zones a ride can plan in or towns have no navmesh by the client's rule, and keep to the zones' navmeshes: {}. "
                       "Run ./dev-servers.ps1 porter-navmeshes",
                       missing.size(), zones.size(), fmt::join(missing, ", "));
    }

    void precalc(const std::vector<uint16>& zones)
    {
        auto* scheduler = cardian::link::scheduler();
        if (scheduler == nullptr)
        {
            ShowErrorFmt("porter: the navmeshes by the client's rule cannot be built: the map handed no scheduler");
            std::exit(1);
        }
        ShowInfoFmt("porter: building the navmeshes by the client's rule of {} zones", zones.size());
        // a dump run (navmesh_dump.h) builds each zone again, current or not
        const bool dumping = cardian::client::dump::point().has_value();
        if (dumping)
        {
            ShowInfoFmt("porter: dumping the build's steps round ({:.1f}, {:.1f}) into log/navmesh_dump", cardian::client::dump::point()->first,
                        cardian::client::dump::point()->second);
        }
        scheduler->postToMainThread(
            [zones, scheduler, dumping]() -> Task<void>
            {
                std::size_t written = 0;
                for (const auto zoneId : zones)
                {
                    auto* zone = zoneutils::GetZone(static_cast<xi::ZoneId>(zoneId));
                    if (zone == nullptr || zone->xiMesh() == nullptr)
                    {
                        ShowWarningFmt("porter: {} has no collision: no navmesh by the client's rule", nameOf(zoneId));
                        continue;
                    }
                    if (!dumping && staleness(zone).empty())
                    {
                        ++written; // already built for today's settings: a rerun builds only what is missing or stale
                        continue;
                    }
                    // A zone with props is built with them or not at all: a
                    // navmesh built without them would pass for one with them
                    const auto                  merged = propsOf(zone);
                    std::unique_ptr<XiMeshImpl> props;
                    if (merged.outdated)
                    {
                        ShowWarningFmt("porter: {}'s props were merged from an older collision: run tools/walls/props.py merge {}, then this again; not built",
                                       zone->getName(), zoneId);
                        continue;
                    }
                    if (merged.size > 0)
                    {
                        try
                        {
                            props = std::make_unique<XiMeshImpl>(propsPath(zone));
                        }
                        catch (const std::exception& e)
                        {
                            ShowWarningFmt("porter: {}'s collision with its props would not load ({}): not built", zone->getName(), e.what());
                            continue;
                        }
                    }
                    const auto config = configFor(zone);
                    {
                        std::ofstream building(recipePath(zoneId), std::ios::trunc);
                        building << kBuilding << '\n';
                    }
                    NavMeshBuilder builder(props != nullptr ? static_cast<const XiMesh&>(*props) : *zone->xiMesh());
                    auto*          built = co_await builder.buildAsync(*scheduler, fmt::format("{} (the porter's)", zone->getName()), zoneId, config);
                    DetourNavMesh  mesh(zoneId);
                    if (built == nullptr || !mesh.installNavMesh(built) || !mesh.save(meshPath(zoneId)))
                    {
                        ShowWarningFmt("porter: {}'s navmesh by the client's rule could not be built or written", zone->getName());
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
                ShowInfoFmt("porter: navmeshes by the client's rule written for {} of {} zones, in {}; exiting", written, zones.size(), ".cache/cardian_navmeshes");
                std::exit(written == zones.size() ? 0 : 1);
            });
    }
} // namespace pawn::porter::mesh
