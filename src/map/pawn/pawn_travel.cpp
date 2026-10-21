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

#include "collision_walls.h"

#include "subzone_graph.h"

#include "common/logging.h"
#include "common/lua.h"
#include "common/settings.h"
#include "common/utils.h"

#include "data/datasets/zones/settings/dataset.h"
#include "data/loader.h"
#include "entities/base_entity.h"
#include "navmesh/detour_navmesh.h"
#include "utils/zoneutils.h"
#include "ximesh/ximesh.h"
#include "zone.h"

#include <algorithm>
#include <chrono>
#include <deque>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
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

    namespace sz = cardian::subzones;

    // A zone's mesh as the game loaded it, its pieces, and the faces of its
    // ground a player cannot climb, worked out once as it is loaded
    struct ZoneMesh
    {
        const dtNavMesh*      mesh{};
        sz::Pieces            pieces;
        cardian::route::Faces faces;
        cardian::route::Seams seams;
    };

    // Leaked on purpose, as the zones they read are
    auto zoneMeshes() -> std::unordered_map<uint16, ZoneMesh>&
    {
        static auto* meshes = new std::unordered_map<uint16, ZoneMesh>();
        return *meshes;
    }

    auto world() -> sz::World&
    {
        static auto* built = new sz::World();
        return *built;
    }

    bool worldBuilt = false;

    // A walled zone's seams, saved beside its navmesh for the development
    // tools to draw (tools/walls/navlines.py): one a line, from x y z to x y z
    // in FFXI's axes
    void saveSeams(const uint16 zone, const cardian::route::Seams& seams)
    {
        std::ofstream out(cardian::walls::builtPath(zone, "seams"), std::ios::trunc);
        out << "# The walker's seams over zone " << zone << "'s walled navmesh (pawn/route_search.h seamsOf)\n";
        for (const auto& s : seams.list)
        {
            out << s.from[0] << ' ' << -s.from[1] << ' ' << -s.from[2] << ' ' << s.to[0] << ' ' << -s.to[1] << ' ' << -s.to[2] << '\n';
        }
    }

    void rejoin(uint16 zone);

    // The zone's mesh and pieces, none for a zone without a Detour mesh. A
    // mesh the zone has rebuilt since (a GM's zone:rebuildNavmesh) is cut
    // into pieces again, and its lines and drops placed on them again
    auto meshOf(const uint16 zone) -> const ZoneMesh*
    {
        const dtNavMesh* current = nullptr;
        const auto*      PZone   = zoneutils::GetZone(static_cast<xi::ZoneId>(zone));
        if (PZone != nullptr)
        {
            if (const auto* detour = dynamic_cast<const DetourNavMesh*>(PZone->navMesh()); detour != nullptr)
            {
                current = detour->detourMesh();
            }
        }
        auto&      meshes = zoneMeshes();
        const auto it     = meshes.find(zone);
        if (it != meshes.end() && it->second.mesh == current)
        {
            return current != nullptr ? &it->second : nullptr;
        }
        const bool changed = it != meshes.end();
        ZoneMesh   entry{ .mesh = current };
        if (current != nullptr)
        {
            // the mesh's seams, kept where the zone's collision lets a body
            // across, its invisible barriers included, and no collision wall
            // stands (pawn/collision_walls.h)
            const XiMesh* collision = PZone != nullptr ? PZone->xiMesh() : nullptr;
            const auto*   walls     = cardian::walls::indexFor(zone);
            entry.seams             = cardian::route::seamsOf(*current, collision == nullptr ? cardian::route::SeamOpen{} : cardian::route::SeamOpen([collision, walls](const position_t& a, const position_t& b)
            {
                return !collision->rayIntersect(Vector3{ a.x, a.y, a.z }, Vector3{ b.x, b.y, b.z }, IgnoreTransparentBarriers::No) &&
                       (walls == nullptr || !walls->crosses(a.x, a.y, a.z, b.x, b.y, b.z));
            }));
            if (walls != nullptr)
            {
                saveSeams(zone, entry.seams);
            }
            entry.pieces = sz::piecesOf(*current, &entry.seams);
            entry.faces  = cardian::route::facesOf(*current);
        }
        auto& kept = meshes[zone] = std::move(entry);
        if (changed && worldBuilt)
        {
            rejoin(zone);
        }
        return current != nullptr ? &kept : nullptr;
    }

    auto pieceAt(const uint16 zone, const position_t& point, const float* extents) -> sz::Piece
    {
        const auto* zm = meshOf(zone);
        return zm != nullptr ? sz::pieceAt(*zm->mesh, zm->pieces, point, extents) : 0;
    }

    // Where a zone line's end stands on its zone's mesh (subzones.h lineEnd):
    // piece 0, at its point, in a zone without a mesh
    auto lineEnd(const uint16 zone, const position_t& point) -> sz::LineEnd
    {
        const auto* zm = meshOf(zone);
        return zm != nullptr ? sz::lineEnd(*zm->mesh, zm->pieces, point) : sz::LineEnd{ 0, point };
    }

    // A body's piece: the one she stands on, else the nearest the mesh reaches
    auto bodyPiece(const uint16 zone, const position_t& at) -> sz::Piece
    {
        const auto piece = pieceAt(zone, at, sz::kBodyExtents);
        return piece != 0 ? piece : pieceAt(zone, at, sz::kLineExtents);
    }

    // A stable placed on the ground: where she stands to rent, a step before
    // the NPC, and its set-down, each as a line's end is placed
    void placeStable(sz::Rental& stable)
    {
        const auto spot = lineEnd(stable.zone, stable.stand);
        const auto land = lineEnd(stable.to, stable.setDown);
        stable.piece    = spot.piece;
        stable.at       = spot.at;
        stable.lands    = land.piece;
        stable.landsAt  = land.at;
    }

    // The cities' stables (ROADMAP T, "Chocobos on the trek"): each NPC whose
    // own script rents a chocobo (xi.chocobo.renterOnTrigger), in a zone whose
    // row of xi.chocobo.chocoboInfo sets a rider down in another zone (`pos`)
    // and is not of the past; a stable in the field, which mounts her where
    // she stands, comes later (#529). Read from the game's own Lua, which the
    // map has loaded by now
    void addStables(sz::World& built, const std::vector<CZone*>& zones)
    {
        constexpr float   kStableFront = 2.5f; // yalms before the NPC, the way it faces
        const sol::object chocobo      = ::lua["xi"]["chocobo"];
        const sol::object infos        = chocobo.get_type() == sol::type::table ? chocobo.as<sol::table>()["chocoboInfo"] : sol::object{};
        if (infos.get_type() != sol::type::table)
        {
            ShowWarning("pawn: travel: xi.chocobo.chocoboInfo is not loaded, so no stable is on the trek's map");
            return;
        }
        for (CZone* PZone : zones)
        {
            const auto        zone = static_cast<uint16>(PZone->GetID());
            const sol::object row  = infos.as<sol::table>()[zone];
            if (row.get_type() != sol::type::table)
            {
                continue;
            }
            const auto        info = row.as<sol::table>();
            const sol::object pos  = info["pos"];
            if (info.get_or("past", false) || pos.get_type() != sol::type::table)
            {
                continue;
            }
            const auto p = pos.as<sol::table>();
            position_t setDown{};
            setDown.x       = p.get<sol::optional<float>>(1).value_or(0.0f);
            setDown.y       = p.get<sol::optional<float>>(2).value_or(0.0f);
            setDown.z       = p.get<sol::optional<float>>(3).value_or(0.0f);
            const auto to   = p.get<sol::optional<uint16>>(5).value_or(uint16{ 0 });
            std::error_code error;
            for (const auto& file : std::filesystem::directory_iterator(fmt::format("./scripts/zones/{}/npcs", PZone->getName()), error))
            {
                if (file.path().extension() != ".lua")
                {
                    continue;
                }
                std::ifstream     in(file.path(), std::ios::binary);
                const std::string text{ std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>() };
                if (text.find("renterOnTrigger") == std::string::npos)
                {
                    continue;
                }
                for (const auto* PNpc : PZone->queryEntitiesByName(file.path().stem().string()))
                {
                    if (PNpc == nullptr || PNpc->status == xi::Status::Disappear)
                    {
                        continue;
                    }
                    sz::Rental stable{};
                    stable.zone    = zone;
                    stable.to      = to;
                    stable.stand   = nearPosition(PNpc->loc.p, kStableFront, 0.0f);
                    stable.setDown = setDown;
                    placeStable(stable);
                    built.rentals.push_back(stable);
                    ShowInfoFmt("pawn: travel: the stable at {} in {} sets a rider down in zone {} at {:.1f} {:.1f} {:.1f}", PNpc->getName(), PZone->getName(),
                                to, setDown.x, setDown.y, setDown.z);
                }
            }
        }
    }

    void buildWorld()
    {
        worldBuilt         = true;
        const auto started = std::chrono::steady_clock::now();

        std::vector<xi::ZoneId> zoneIds;
        std::vector<CZone*>     zones;
        zoneutils::ForEachZone([&](CZone* PZone)
        {
            zoneIds.push_back(PZone->GetID());
            zones.push_back(PZone);
        });

        auto& built = world();
        built       = {};
        for (const auto* PZone : zones)
        {
            if (PZone->CanUseMisc(xi::ZoneMisc::Mount))
            {
                built.mountZones.insert(static_cast<uint16>(PZone->GetID()));
            }
        }
        std::size_t pieces    = 0;
        std::size_t faces     = 0;
        std::size_t faceBytes = 0; // what the steep faces hold in memory, kept for good
        std::size_t seams     = 0;
        std::size_t seamBytes = 0; // what the seams hold in memory, kept for good
        for (const auto zoneId : zoneIds)
        {
            const auto zone = static_cast<uint16>(zoneId);
            if (const auto* zm = meshOf(zone); zm != nullptr)
            {
                pieces += zm->pieces.polys.size() - 1;
                faces += zm->faces.faces.size();
                faceBytes += zm->faces.faces.capacity() * sizeof(cardian::route::Face) +
                             (zm->faces.first.capacity() + zm->faces.base.capacity()) * sizeof(uint32_t);
                seams += zm->seams.list.size();
                seamBytes += zm->seams.list.capacity() * sizeof(cardian::route::Seam) +
                             (zm->seams.first.capacity() + zm->seams.base.capacity() + zm->seams.index.capacity()) * sizeof(uint32_t);
                if (!zm->pieces.drops.empty())
                {
                    built.drops[zone] = zm->pieces.drops;
                }
            }
            const auto settings = xi::data::loadZoneFile<xi::data::datasets::zones::settings::Dataset>(zoneId);
            if (!settings)
            {
                continue;
            }
            for (const auto& line : settings->ZoneLines)
            {
                if (line.DestinationZone == zoneId)
                {
                    continue;
                }
                const auto to = static_cast<uint16>(line.DestinationZone);
                built.lines.push_back({ .zone    = zone,
                                        .to      = to,
                                        .origin  = line.Origin,
                                        .arrival = line.Destination,
                                        .leaves  = lineEnd(zone, line.Origin),
                                        .lands   = lineEnd(to, line.Destination) });
            }
        }
        addStables(built, zones);
        if (built.rentals.empty())
        {
            ShowWarning("pawn: travel: no stable found -- xi.chocobo.chocoboInfo's rows or the renters' scripts (renterOnTrigger) have changed shape; "
                        "the treks ride nowhere");
        }
        sz::join(built);
        std::size_t drops = 0;
        for (const auto& [_, zoneDrops] : built.drops)
        {
            drops += zoneDrops.size();
        }
        constexpr double kMB = 1024.0 * 1024.0;
        ShowInfoFmt("pawn: travel sub-zones built: {} zones ({} allow a mount), {} pieces, {} lines, {} stables, {} drops, {} steep faces ({:.1f} MB), {} seams ({:.1f} MB), the walk's tables ({:.1f} MB), in {} ms",
                    zoneIds.size(), built.mountZones.size(), pieces, built.lines.size(), built.rentals.size(), drops, faces, static_cast<double>(faceBytes) / kMB, seams, static_cast<double>(seamBytes) / kMB,
                    static_cast<double>(built.graph.bytes()) / kMB,
                    std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started).count());
    }

    // A zone's mesh changed: its lines out and in, and its drops, placed on
    // its new pieces, and the walk's tables joined again
    void rejoin(const uint16 zone)
    {
        auto&       built = world();
        const auto* zm    = zoneMeshes().contains(zone) ? &zoneMeshes().at(zone) : nullptr;
        const auto  place = [&](const position_t& at) -> sz::LineEnd
        {
            return zm != nullptr && zm->mesh != nullptr ? sz::lineEnd(*zm->mesh, zm->pieces, at) : sz::LineEnd{ 0, at };
        };
        for (auto& line : built.lines)
        {
            if (line.zone == zone)
            {
                line.leaves = place(line.origin);
            }
            if (line.to == zone)
            {
                line.lands = place(line.arrival);
            }
        }
        built.drops.erase(zone);
        if (zm != nullptr && !zm->pieces.drops.empty())
        {
            built.drops[zone] = zm->pieces.drops;
        }
        for (auto& stable : built.rentals)
        {
            if (stable.zone == zone || stable.to == zone)
            {
                placeStable(stable);
            }
        }
        ++built.version;
        sz::join(built);
        ShowInfoFmt("pawn: travel sub-zones: zone {} has a new mesh, {} pieces", zone, zm != nullptr ? zm->pieces.polys.size() - 1 : 0);
    }
} // namespace

namespace pawn::travel
{
    void prepare()
    {
        if (!worldBuilt)
        {
            buildWorld();
        }
    }

    auto nextHop(const xi::ZoneId from, const position_t& at, const xi::ZoneId to, const std::optional<position_t> toPoint, const std::optional<position_t> prefer,
                 const TravelRide& ride) -> std::optional<TravelHop>
    {
        const auto      fromKey = static_cast<uint16>(from);
        const auto      toKey   = static_cast<uint16>(to);
        const sz::Piece here    = bodyPiece(fromKey, at);
        const auto      there   = toPoint.has_value() ? std::optional<sz::Piece>(bodyPiece(toKey, *toPoint)) : std::nullopt;
        if (from == to && (!there.has_value() || sz::detail::joins(here, *there)))
        {
            return std::nullopt;
        }
        if (!worldBuilt)
        {
            buildWorld();
        }

        const auto&    built = world();
        const sz::Ride rider{ ride.mayRent, ride.mounted, ride.rideFactor, ride.stayMounted };
        auto           plan  = sz::plan(built, fromKey, here, at, toKey, there, toPoint, rider);
        const auto     moves = [](const sz::Plan& p)
        {
            return p.reachable && (p.firstLine.has_value() || p.firstRental.has_value());
        };
        if (!moves(plan) && there.has_value() && from != to)
        {
            // His piece is one no line lands on or leads to (a scrap of mesh
            // he stands on): the walk goes to his zone, and on from there
            ShowInfoFmt("pawn: travel: no line reaches piece {} of zone {}, so the walk goes to the zone", *there, toKey);
            plan = sz::plan(built, fromKey, here, at, toKey, std::nullopt, std::nullopt, rider);
        }
        if (plan.firstRental.has_value())
        {
            const sz::Rental& stable = built.rentals[*plan.firstRental];
            ShowInfoFmt("pawn: travel: zone {} (piece {}) to zone {} (piece {}): some {:.0f} yalms on foot's time; first the stable at {:.1f} {:.1f} {:.1f}, "
                        "which sets her down in zone {}",
                        fromKey, here, toKey, there.value_or(0), plan.length, stable.at.x, stable.at.y, stable.at.z, stable.to);
            return TravelHop{ static_cast<xi::ZoneId>(stable.to), stable.at, stable.landsAt, true };
        }
        if (!plan.reachable || !plan.firstLine.has_value())
        {
            ShowInfoFmt("pawn: travel: no walk from zone {} (piece {}, at {:.1f} {:.1f} {:.1f}) to zone {} (piece {})", fromKey, here, at.x, at.y, at.z, toKey,
                        there.value_or(0));
            return std::nullopt;
        }

        // of the lines that land on the same piece of the same next zone, the
        // one nearest where she would rather go through
        const sz::Line* first = &built.lines[*plan.firstLine];
        if (prefer.has_value())
        {
            for (const auto& line : built.lines)
            {
                if (line.zone == fromKey && line.to == first->to && line.lands.piece == first->lands.piece && sz::detail::joins(here, line.leaves.piece) &&
                    sz::detail::crow(line.origin, *prefer) < sz::detail::crow(first->origin, *prefer))
                {
                    first = &line;
                }
            }
        }
        ShowInfoFmt("pawn: travel: zone {} (piece {}) to zone {} (piece {}): {} zone lines, some {:.0f} yalms; first the line to zone {} at {:.1f} {:.1f} {:.1f}",
                    fromKey, here, toKey, there.value_or(0), plan.lines, plan.length, first->to, first->origin.x, first->origin.y, first->origin.z);
        return TravelHop{ static_cast<xi::ZoneId>(first->to), first->leaves.at, first->lands.at };
    }

    auto cutOff(const xi::ZoneId zone, const position_t& a, const position_t& b) -> bool
    {
        const auto key = static_cast<uint16>(zone);
        const auto pa  = pieceAt(key, a, sz::kBodyExtents);
        const auto pb  = pieceAt(key, b, sz::kBodyExtents);
        if (pa == 0 || pb == 0 || sz::detail::joins(pa, pb))
        {
            return false;
        }
        // Only when a walk round reaches his piece: a scrap of mesh no line
        // leads to (a crate top) is no place for a trek, and she follows him
        // as near as her own piece goes
        if (!worldBuilt)
        {
            buildWorld();
        }
        const auto plan = sz::plan(world(), key, pa, a, key, pb, b);
        return plan.reachable && plan.firstLine.has_value();
    }

    auto routeToSafeGround(const xi::ZoneId zone, const position_t& from, const std::vector<cardian::route::Hazard>& hazards,
                           const cardian::route::HazardSight& sight, const float within) -> std::optional<cardian::route::Route>
    {
        const auto* zm = meshOf(static_cast<uint16>(zone));
        if (zm == nullptr)
        {
            return std::nullopt;
        }
        auto route = cardian::route::searchSafeGround(*zm->mesh, from, sz::kBodyExtents, hazards, sight, within, &zm->faces, &zm->seams);
        if (!route.has_value())
        {
            route = cardian::route::searchSafeGround(*zm->mesh, from, sz::kLineExtents, hazards, sight, within, &zm->faces, &zm->seams);
        }
        return route;
    }

    auto routeAvoiding(const xi::ZoneId zone, const position_t& from, const position_t& to, const std::vector<cardian::route::Hazard>& hazards,
                       const cardian::route::HazardSight& sight, const float reach) -> std::optional<cardian::route::Route>
    {
        const auto* zm = meshOf(static_cast<uint16>(zone));
        if (zm == nullptr)
        {
            return std::nullopt;
        }
        auto route = cardian::route::searchAvoiding(*zm->mesh, from, to, sz::kBodyExtents, sz::kLineExtents, hazards, sight, reach, &zm->faces, &zm->seams);
        if (!route.has_value())
        {
            route = cardian::route::searchAvoiding(*zm->mesh, from, to, sz::kLineExtents, sz::kLineExtents, hazards, sight, reach, &zm->faces, &zm->seams);
        }
        return route;
    }

    auto routeOn(const dtNavMesh& mesh, const position_t& from, const position_t& to, const std::vector<cardian::route::Hazard>& hazards)
        -> std::optional<cardian::route::Route>
    {
        auto route = cardian::route::searchAvoiding(mesh, from, to, sz::kBodyExtents, sz::kLineExtents, hazards, {});
        if (!route.has_value())
        {
            route = cardian::route::searchAvoiding(mesh, from, to, sz::kLineExtents, sz::kLineExtents, hazards, {});
        }
        return route;
    }

    auto allowsMount(const xi::ZoneId zone) -> bool
    {
        if (!worldBuilt)
        {
            buildWorld();
        }
        return world().mountZones.contains(static_cast<uint16>(zone));
    }

    auto rideableCities() -> std::vector<xi::ZoneId>
    {
        if (!worldBuilt)
        {
            buildWorld();
        }
        const auto&                built = world();
        std::unordered_set<uint16> seen;
        std::vector<xi::ZoneId>    cities;
        for (const auto& line : built.lines)
        {
            if (!built.mountZones.contains(line.zone) || seen.contains(line.to))
            {
                continue;
            }
            auto* PZone = zoneutils::GetZone(static_cast<xi::ZoneId>(line.to));
            if (PZone == nullptr || (PZone->GetTypeMask() & xi::ZoneType::City) == xi::ZoneType::Unknown)
            {
                continue;
            }
            seen.insert(line.to);
            cities.push_back(static_cast<xi::ZoneId>(line.to));
        }
        return cities;
    }

    auto rideCosts(const xi::ZoneId from, const position_t& at, const std::vector<RideGoal>& goals) -> std::vector<std::optional<float>>
    {
        if (!worldBuilt)
        {
            buildWorld();
        }
        const auto                    fromKey = static_cast<uint16>(from);
        std::vector<sz::RideTarget>   targets;
        targets.reserve(goals.size());
        for (const auto& goal : goals)
        {
            const auto key = static_cast<uint16>(goal.zone);
            targets.push_back({ .zone  = key,
                                .piece = goal.point.has_value() ? std::optional<sz::Piece>(bodyPiece(key, *goal.point)) : std::nullopt,
                                .point = goal.point });
        }
        // one search of the whole world graph on the map's thread: a slow one is said
        const auto began = std::chrono::steady_clock::now();
        auto       costs = sz::rideCosts(world(), fromKey, bodyPiece(fromKey, at), at, targets);
        const auto took  = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - began).count();
        if (took >= 50)
        {
            ShowInfoFmt("pawn: travel: {} ride times from zone {} took {} ms", goals.size(), fromKey, took);
        }
        return costs;
    }

    auto hops(const std::vector<xi::ZoneId>& from) -> std::unordered_map<uint16, uint32>
    {
        if (!graphBuilt)
        {
            buildGraph();
        }
        std::unordered_map<uint16, uint32> out;
        std::deque<uint16>                 frontier;
        for (const auto zone : from)
        {
            if (out.emplace(static_cast<uint16>(zone), 0).second)
            {
                frontier.push_back(static_cast<uint16>(zone));
            }
        }
        while (!frontier.empty())
        {
            const uint16 current = frontier.front();
            frontier.pop_front();
            const auto it = graph.find(current);
            if (it == graph.end())
            {
                continue;
            }
            for (const auto& edge : it->second)
            {
                if (out.emplace(static_cast<uint16>(edge.to), out[current] + 1).second)
                {
                    frontier.push_back(static_cast<uint16>(edge.to));
                }
            }
        }
        return out;
    }

    auto rideRegion(const std::vector<xi::ZoneId>& from) -> std::unordered_set<uint16>
    {
        if (!graphBuilt)
        {
            buildGraph();
        }
        std::unordered_set<uint16> seen;
        std::unordered_set<uint16> region;
        std::deque<uint16>         frontier;
        for (const auto zone : from)
        {
            if (seen.insert(static_cast<uint16>(zone)).second)
            {
                frontier.push_back(static_cast<uint16>(zone));
            }
        }
        while (!frontier.empty())
        {
            const uint16 current = frontier.front();
            frontier.pop_front();
            if (allowsMount(static_cast<xi::ZoneId>(current)))
            {
                region.insert(current);
            }
            const auto it = graph.find(current);
            if (it == graph.end())
            {
                continue;
            }
            for (const auto& edge : it->second)
            {
                const auto to = static_cast<uint16>(edge.to);
                if (allowsMount(edge.to) && seen.insert(to).second)
                {
                    frontier.push_back(to);
                }
            }
        }
        return region;
    }

    void noteTrekTime(const std::string_view who, const std::chrono::steady_clock::duration took)
    {
        // Every zone ticks once each 400 ms, each zone at its own moment: a
        // window opened by a trek and shut 400 ms later holds one tick of
        // every zone, the load the map carries for treks in that time
        constexpr auto kTickWindow = std::chrono::milliseconds(400);
        struct Spent
        {
            std::string                          who;
            int                                  treks = 0;
            std::chrono::steady_clock::duration took{};
        };
        static std::mutex                            mutex;
        static std::chrono::steady_clock::time_point opened{};
        static std::chrono::steady_clock::duration   total{};
        static std::vector<Spent>                    spent;
        static bool                                  shouted = false;

        const std::lock_guard lock(mutex);
        const auto            now = std::chrono::steady_clock::now();
        if (now - opened >= kTickWindow)
        {
            opened  = now;
            total   = {};
            shouted = false;
            spent.clear();
        }
        total += took;
        auto it = std::find_if(spent.begin(), spent.end(), [&](const Spent& s) { return s.who == who; });
        if (it == spent.end())
        {
            it = spent.insert(spent.end(), Spent{ .who = std::string(who) });
        }
        ++it->treks;
        it->took += took;

        using Ms          = std::chrono::duration<float, std::milli>;
        const float budget = settings::get<float>("pawn.TREK_PLAN_BUDGET_MS");
        if (shouted || std::chrono::duration_cast<Ms>(total).count() <= budget)
        {
            return;
        }
        shouted = true;
        std::string by;
        for (const auto& s : spent)
        {
            by += fmt::format("{}{} {:.1f} ms", by.empty() ? "" : ", ", s.who, std::chrono::duration_cast<Ms>(s.took).count());
        }
        ShowCriticalFmt("!!! TREK PLANNING OVER BUDGET !!! {:.1f} ms of treks in one 400 ms logic tick, over the budget of {:.0f} ms (pawn.TREK_PLAN_BUDGET_MS): {}",
                        std::chrono::duration_cast<Ms>(total).count(), budget, by);
    }
} // namespace pawn::travel
