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

#include "pawn_doors.h"
#include "door_math.h"
#include "formation_math.h"
#include "pawn_danger.h"
#include "pawn_travel.h"
#include "porter_mesh.h"

#include "common/database.h"
#include "common/logging.h"
#include "common/lua.h"
#include "common/utils.h"

#include "ai/ai_container.h"
#include "ai/helpers/action_queue.h"
#include "entities/char_entity.h"
#include "data/datasets/zones/npcs/dataset.h"
#include "data/datasets/zones/settings/dataset.h"
#include "data/loader.h"
#include "entities/npc_entity.h"
#include "navmesh/navmesh.h"
#include "utils/zoneutils.h"
#include "zone.h"
#include "zone_entities.h"

#include <glaze/json.hpp>

#include <array>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <limits>
#include <mutex>
#include <sstream>
#include <unordered_map>
#include <unordered_set>

namespace pawn::doors::files
{
    // The door table's shapes, as tools/walls/doors.py writes them
    struct Crossing
    {
        int                  event = 0;
        int                  from  = 0;
        std::array<float, 3> stand{};
        std::array<float, 3> land{};
        bool                 own    = false;
        bool                 menu   = false;
        bool                 placed = false;
    };

    struct Door
    {
        int                   zone = 0;
        uint32_t              npc  = 0;
        std::string           name;
        std::array<float, 3>  at{};
        std::array<float, 2>  normal{};
        float                 width  = 0.0f;
        float                 floor  = 0.0f;
        float                 height = 0.0f;
        std::vector<Crossing> crossings;
    };

    struct Table
    {
        int               format = 0;
        std::string       written;
        std::vector<Door> doors;
    };
} // namespace pawn::doors::files

using namespace std::chrono_literals;

namespace
{
    // The door look type: the client names these from the packet and
    // blocks the player at them while closed (0x01a_action.cpp's door test)
    constexpr uint16 kDoorLook = 0x02;

    // A door NPC sits about a yalm above its floor; a door on another
    // storey is further off than this
    constexpr float kSameFloor = 2.5f;

    // How long a door stays open: CTriggerState's and npc:openDoor()'s figure
    constexpr auto kDoorOpenTime = 7s;

    // A walk that meets a door it may not pass stops this short of it
    constexpr float kKeepShort = 1.0f;

    // The door table (tools/walls/doors.py), and the format this build reads
    constexpr auto kTablePath   = ".cache/cardian_doors.json";
    constexpr int  kTableFormat = 2;

    // A door lower than this is no wall: a trap door, a plate in the floor
    constexpr float kStanding = 1.5f;

    // A crossing made from a door's plane: this far out on each side
    constexpr float kStepOut = 1.5f;

    struct KnownDoor
    {
        uint16                             zone = 0;
        std::string                        name;
        cardian::door::Plane               plane; // its y the floor the door stands on
        float                              height = 0.0f;
        std::vector<pawn::doors::Crossing> crossings;
    };

    std::once_flag                                  tableRead;
    std::unordered_map<uint32, KnownDoor>           doorsById;
    std::unordered_map<uint16, std::vector<uint32>> doorsByZone;

    auto toPosition(const std::array<float, 3>& p) -> position_t
    {
        return position_t(p[0], p[1], p[2], 0, 0);
    }

    void readTable()
    {
        std::ifstream in(kTablePath, std::ios::binary);
        if (!in)
        {
            ShowWarningFmt("doors: no {}: a cardian walks through every closed door and crosses none as the game would walk her "
                           "(python tools/walls/doors.py write, on a PC with the game)",
                           kTablePath);
            return;
        }
        std::stringstream text;
        text << in.rdbuf();
        const std::string buffer = text.str();

        pawn::doors::files::Table table;
        constexpr glz::opts       kTableJson{ .error_on_unknown_keys = false };
        if (const auto failed = glz::read<kTableJson>(table, buffer); failed)
        {
            ShowErrorFmt("doors: {} unreadable: {}", kTablePath, glz::format_error(failed, buffer));
            return;
        }
        if (table.format != kTableFormat)
        {
            ShowWarningFmt("doors: {} is format {}, this build reads {}: write it again (python tools/walls/doors.py write)", kTablePath, table.format,
                           kTableFormat);
            return;
        }

        size_t crossings = 0;
        for (const auto& door : table.doors)
        {
            KnownDoor known;
            known.zone  = static_cast<uint16>(door.zone);
            known.name  = door.name;
            known.plane  = cardian::door::Plane{ door.at[0], door.floor, door.at[2], door.normal[0], door.normal[1], door.width };
            known.height = door.height;
            for (const auto& c : door.crossings)
            {
                known.crossings.push_back(pawn::doors::Crossing{ c.event, c.from, toPosition(c.stand), toPosition(c.land), c.own, c.menu, c.placed });
            }
            crossings += known.crossings.size();
            doorsByZone[known.zone].push_back(door.npc);
            doorsById.insert_or_assign(door.npc, std::move(known));
        }
        ShowInfoFmt("doors: {} doors in {} zones, {} crossings, read from {} (written {})", doorsById.size(), doorsByZone.size(), crossings, kTablePath,
                    table.written);
    }

    void ensureTable()
    {
        std::call_once(tableRead, readTable);
    }

    // The mesh point nearest `at`: on the navmesh by the client's rule where
    // her walks keep to it (a town's), the zone's elsewhere; nullopt when the
    // mesh has no ground near
    auto snap(CZone* PZone, const position_t& at) -> std::optional<position_t>
    {
        constexpr float           kOffGround = 2.0f;
        std::optional<position_t> onMesh;
        if (pawn::porter::mesh::walkedOnClient(PZone))
        {
            onMesh = pawn::travel::snapToTown(PZone->GetID(), at);
        }
        if (!onMesh.has_value())
        {
            if (auto* navMesh = PZone->navMesh(); navMesh != nullptr)
            {
                onMesh = navMesh->findClosestValidPoint(at); // a town whose own navmesh is not built
            }
        }
        if (!onMesh.has_value() || std::hypot(onMesh->x - at.x, onMesh->z - at.z) > kOffGround || std::abs(onMesh->y - at.y) > 4.0f)
        {
            return std::nullopt;
        }
        return onMesh;
    }

    // A ferry dock gate or an elevator door follows its timetable, not a
    // passer-by. A 7-second close on one of those would shut a gate the
    // ship had just opened. The zone's own data names them, by the client
    // name the door carries, as the ship and elevator handlers resolve
    // them: each transport's door in the zone settings, each elevator's
    // two doors on its platform NPC. Read once per zone
    auto isScheduledDoor(const CNpcEntity* PNpc) -> bool
    {
        static std::unordered_map<xi::ZoneId, std::unordered_set<std::string>> doorsByZone;

        const auto zoneId        = PNpc->getZone();
        auto [doors, firstVisit] = doorsByZone.try_emplace(zoneId);
        if (firstVisit)
        {
            if (const auto settings = xi::data::loadZoneFile<xi::data::datasets::zones::settings::Dataset>(zoneId))
            {
                for (const auto& transport : settings->Transports)
                {
                    if (!transport.Door.empty())
                    {
                        doors->second.insert(transport.Door);
                    }
                }
            }
            if (const auto npcs = xi::data::loadZoneFile<xi::data::datasets::zones::npcs::Dataset>(zoneId))
            {
                for (const auto& npc : *npcs)
                {
                    if (npc.Elevator.has_value())
                    {
                        doors->second.insert(npc.Elevator->LowerDoor);
                        doors->second.insert(npc.Elevator->UpperDoor);
                    }
                }
            }
        }
        return doors->second.contains(PNpc->name);
    }

    // Whether the interaction framework holds a handler for the door: its
    // zone's DefaultActions (the President's Office's cutscene, Metalworks),
    // a mission's or a quest's. The framework's lookup keys them by zone and
    // by the NPC's script name (scripts/globals/interaction/interaction_lookup.lua)
    auto hasInteraction(const CNpcEntity* PNpc) -> bool
    {
        const sol::object byZone = ::lua["InteractionGlobal"]["lookup"]["data"][static_cast<uint16>(PNpc->getZone())];
        if (!byZone.is<sol::table>())
        {
            return false;
        }
        const sol::object entry = byZone.as<sol::table>()[PNpc->getName()];
        return entry.is<sol::table>();
    }

    // A door with rules of its own -- a key, a quest, a cutscene -- has a
    // script file, or a handler of the interaction framework's; those stay
    // the player's to open: a cardian opening one would let him walk through
    // without the scene it plays him. Without either the framework opens the
    // door for anyone (its onTrigger default is -1), so the cardian may too.
    // Looked up once per door.
    auto isGenericDoor(const CNpcEntity* PNpc) -> bool
    {
        static std::unordered_map<uint32, bool> generic;

        if (const auto it = generic.find(PNpc->id); it != generic.end())
        {
            return it->second;
        }

        const bool scripted = std::filesystem::exists(fmt::format("./scripts/zones/{}/npcs/{}.lua", PNpc->loc.zone->getName(), PNpc->getName())) ||
                              hasInteraction(PNpc);
        const bool ours     = !scripted && !isScheduledDoor(PNpc);
        generic.emplace(PNpc->id, ours);
        return ours;
    }

    // A closed door she may not open, standing in her way: a wall to her.
    // One the game makes untargetable -- a lever's, a drawbridge -- opens by
    // its own mechanism alone, so it is one too; a door lower than a body
    // (a trap door) is never one
    auto walls(const CNpcEntity* PNpc, const KnownDoor& known) -> bool
    {
        return PNpc != nullptr && PNpc->look.size == kDoorLook && PNpc->animation == xi::Animation::CloseDoor && PNpc->status == xi::Status::Normal &&
               PNpc->loc.zone != nullptr && known.height >= kStanding && (!isGenericDoor(PNpc) || PNpc->GetUntargetable());
    }

    auto doorEntity(const uint32 npc) -> CNpcEntity*
    {
        return dynamic_cast<CNpcEntity*>(zoneutils::GetEntity(npc, TYPE_NPC));
    }

    // The game's own door opening, from the server: mirrors CLuaBaseEntity::openDoor
    void open(CNpcEntity* PNpc)
    {
        PNpc->animation = xi::Animation::OpenDoor;
        PNpc->loc.zone->UpdateEntityPacket(PNpc, ENTITY_UPDATE, UPDATE_COMBAT);

        PNpc->PAI->QueueAction(queueAction_t(kDoorOpenTime, false, [](CBaseEntity* PDoor)
                                             {
                                                 PDoor->animation = xi::Animation::CloseDoor;
                                                 if (PDoor->loc.zone != nullptr)
                                                 {
                                                     PDoor->loc.zone->UpdateEntityPacket(PDoor, ENTITY_UPDATE, UPDATE_COMBAT);
                                                 }
                                             }));
    }
} // namespace

namespace pawn::doors
{
    void loadTable()
    {
        ensureTable();
    }

    auto openAhead(CCharEntity* PPawn, const float reach, const float lane) -> int
    {
        if (reach <= 0.0f || PPawn->loc.zone == nullptr)
        {
            return 0;
        }

        auto* entities = pawn::entitiesAround(PPawn);
        if (entities == nullptr)
        {
            return 0;
        }

        const position_t& me    = PPawn->loc.p;
        const position_t  ahead = nearPosition(me, reach, 0.0f);

        int opened = 0;
        entities->spatialGrid().forEachInRange(me, reach + lane, [&](CBaseEntity* PEntity)
                                               {
                                                   if (PEntity->objtype != TYPE_NPC)
                                                   {
                                                       return;
                                                   }

                                                   auto* PNpc = static_cast<CNpcEntity*>(PEntity);
                                                   if (PNpc->look.size != kDoorLook || PNpc->animation != xi::Animation::CloseDoor ||
                                                       PNpc->status != xi::Status::Normal || PNpc->loc.zone == nullptr || PNpc->GetUntargetable())
                                                   {
                                                       return;
                                                   }

                                                   if (std::abs(PNpc->loc.p.y - me.y) > kSameFloor)
                                                   {
                                                       return;
                                                   }

                                                   const cardian::formation::Circle door{ PNpc->loc.p.x, PNpc->loc.p.z, 0.0f };
                                                   if (cardian::formation::segmentClosest(door, me.x, me.z, ahead.x, ahead.z) > lane || !isGenericDoor(PNpc))
                                                   {
                                                       return;
                                                   }

                                                   open(PNpc);
                                                   ++opened;
                                                   ShowInfoFmt("pawn: {} opens {}", PPawn->getName(), PNpc->packetName.empty() ? PNpc->getName() : PNpc->packetName);
                                               });

        return opened;
    }

    auto keepToNearSide(CCharEntity* PPawn, const position_t& from, const position_t& to) -> std::optional<position_t>
    {
        // The doors' planes, where the table has the zone's
        ensureTable();
        if (PPawn->loc.zone != nullptr && doorsByZone.contains(static_cast<uint16>(PPawn->getZone())))
        {
            const auto ahead = firstOnRoute(PPawn, { from, to });
            if (!ahead.has_value())
            {
                return std::nullopt;
            }
            return ahead->stop;
        }

        // Without them, a door blocks the way within this of its middle
        constexpr float kDoorHalfWidth = 1.5f;

        const float dx  = to.x - from.x;
        const float dz  = to.z - from.z;
        const float len = std::hypot(dx, dz);
        auto*       entities = PPawn->loc.zone != nullptr ? pawn::entitiesAround(PPawn) : nullptr;
        if (entities == nullptr || len < 0.5f)
        {
            return std::nullopt;
        }

        const position_t middle((from.x + to.x) / 2.0f, from.y, (from.z + to.z) / 2.0f, 0, 0);
        float            keep = 1.0f;
        bool             hit  = false;
        entities->spatialGrid().forEachInRange(middle, len / 2.0f + kDoorHalfWidth, [&](CBaseEntity* PEntity)
                                               {
                                                   if (PEntity->objtype != TYPE_NPC)
                                                   {
                                                       return;
                                                   }
                                                   auto* PNpc = static_cast<CNpcEntity*>(PEntity);
                                                   if (PNpc->look.size != kDoorLook || PNpc->animation != xi::Animation::CloseDoor ||
                                                       PNpc->status != xi::Status::Normal || PNpc->loc.zone == nullptr ||
                                                       std::abs(PNpc->loc.p.y - from.y) > kSameFloor || isGenericDoor(PNpc))
                                                   {
                                                       return;
                                                   }
                                                   // Where along the way the door stands, and how far off it
                                                   const float ox = PNpc->loc.p.x - from.x;
                                                   const float oz = PNpc->loc.p.z - from.z;
                                                   const float t  = (ox * dx + oz * dz) / (len * len);
                                                   const float off = std::abs(ox * dz - oz * dx) / len;
                                                   if (t <= 0.0f || t * len > len + kDoorHalfWidth || off > kDoorHalfWidth)
                                                   {
                                                       return;
                                                   }
                                                   keep = std::min(keep, std::max(0.0f, t - kKeepShort / len));
                                                   hit  = true;
                                               });
        if (!hit)
        {
            return std::nullopt;
        }
        return position_t(from.x + dx * keep, from.y + (to.y - from.y) * keep, from.z + dz * keep, 0, 0);
    }

    auto firstOnRoute(const CBaseEntity* PWalker, const std::vector<position_t>& route) -> std::optional<Ahead>
    {
        return PWalker != nullptr ? firstOnRoute(PWalker->loc.zone, route) : std::nullopt;
    }

    auto firstOnRoute(const CZone* PZone, const std::vector<position_t>& route) -> std::optional<Ahead>
    {
        ensureTable();
        if (PZone == nullptr || route.size() < 2)
        {
            return std::nullopt;
        }
        const auto byZone = doorsByZone.find(static_cast<uint16>(PZone->GetID()));
        if (byZone == doorsByZone.end())
        {
            return std::nullopt;
        }

        // The ground the route covers: a door off it is passed over unasked
        float minX = std::numeric_limits<float>::max();
        float maxX = std::numeric_limits<float>::lowest();
        float minZ = std::numeric_limits<float>::max();
        float maxZ = std::numeric_limits<float>::lowest();
        for (const auto& p : route)
        {
            minX = std::min(minX, p.x);
            maxX = std::max(maxX, p.x);
            minZ = std::min(minZ, p.z);
            maxZ = std::max(maxZ, p.z);
        }

        std::optional<Ahead> first;
        float                firstT = 0.0f;
        for (const uint32 npc : byZone->second)
        {
            const auto& known = doorsById.at(npc);
            const float reach = known.plane.width / 2.0f + cardian::door::kBodyMargin;
            if (known.plane.x < minX - reach || known.plane.x > maxX + reach || known.plane.z < minZ - reach || known.plane.z > maxZ + reach)
            {
                continue;
            }
            const auto meeting = cardian::door::firstMeeting(known.plane, route);
            if (!meeting.has_value() || (first.has_value() && (meeting->leg > first->leg || (meeting->leg == first->leg && meeting->t >= firstT))))
            {
                continue;
            }
            if (!walls(doorEntity(npc), known))
            {
                continue;
            }
            const auto& a     = route[meeting->leg - 1];
            const auto& b     = route[meeting->leg];
            const float share = cardian::door::stopShort(distance(a, b), meeting->t, kKeepShort);
            first             = Ahead{ .npc  = npc,
                                       .leg  = meeting->leg,
                                       .stop = position_t(a.x + (b.x - a.x) * share, a.y + (b.y - a.y) * share, a.z + (b.z - a.z) * share, 0, 0),
                                       .side = cardian::door::sideOf(known.plane, a.x, a.z) };
            firstT            = meeting->t;
        }
        return first;
    }

    auto blocks(CZone* PZone, const uint32 npc) -> bool
    {
        const auto* PNpc  = doorEntity(npc);
        const auto  known = doorsById.find(npc);
        return PNpc != nullptr && known != doorsById.end() && PNpc->loc.zone == PZone && walls(PNpc, known->second);
    }

    auto crossingFor(CZone* PZone, const uint32 npc, const int side, const bool towardHim) -> std::optional<Crossing>
    {
        ensureTable();
        const auto known = doorsById.find(npc);
        if (PZone == nullptr || known == doorsById.end() || (side != 1 && side != -1))
        {
            return std::nullopt;
        }

        // A crossing from her side, and only with none the reverse of one
        // from the other side: the door's own before a scene's, one that asks
        // him first before one that does not, one that places him at the door
        // before one that does not, then the one that leaves her nearest it
        const auto&     door     = known->second;
        const Crossing* best     = nullptr;
        bool            reversed = false;
        float           bestOff  = std::numeric_limits<float>::max();
        for (const auto& c : door.crossings)
        {
            if (!towardHim && !c.own)
            {
                continue;
            }
            const bool  mine = c.from == side;
            const auto& out  = mine ? c.land : c.stand;
            const float off  = std::hypot(out.x - door.plane.x, out.z - door.plane.z) + (mine ? 0.0f : 4000.0f) + (c.own ? 0.0f : 2000.0f) +
                              (c.menu ? 0.0f : 200.0f) + (c.placed ? 0.0f : 100.0f);
            if (off < bestOff)
            {
                best     = &c;
                reversed = !mine;
                bestOff  = off;
            }
        }

        Crossing crossing;
        if (best != nullptr)
        {
            crossing = *best;
            if (reversed)
            {
                crossing.from = side;
                std::swap(crossing.stand, crossing.land);
            }
        }
        else if (towardHim)
        {
            // On her way to him, a door the client walks nobody through is
            // open all the same: a step out from it on each side
            const auto& plane = door.plane;
            crossing.from     = side;
            crossing.stand    = position_t(plane.x + plane.nx * kStepOut * static_cast<float>(side), plane.y, plane.z + plane.nz * kStepOut * static_cast<float>(side), 0, 0);
            crossing.land     = position_t(plane.x - plane.nx * kStepOut * static_cast<float>(side), plane.y, plane.z - plane.nz * kStepOut * static_cast<float>(side), 0, 0);
        }
        else
        {
            return std::nullopt;
        }
        const auto stand = snap(PZone, crossing.stand);
        const auto land  = snap(PZone, crossing.land);
        if (!stand.has_value() || !land.has_value())
        {
            return std::nullopt;
        }
        crossing.stand = *stand;
        crossing.land  = *land;
        return crossing;
    }

    auto sideOf(const uint32 npc, const position_t& at) -> int
    {
        ensureTable();
        const auto known = doorsById.find(npc);
        return known != doorsById.end() ? cardian::door::sideOf(known->second.plane, at.x, at.z) : 0;
    }

    auto nameOf(const uint32 npc) -> std::string
    {
        if (const auto* PNpc = doorEntity(npc); PNpc != nullptr)
        {
            return PNpc->packetName.empty() ? std::string(PNpc->getName()) : PNpc->packetName;
        }
        const auto known = doorsById.find(npc);
        return known != doorsById.end() ? known->second.name : fmt::format("door {}", npc);
    }
} // namespace pawn::doors
