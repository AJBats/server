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
#include "formation_math.h"
#include "pawn_danger.h"

#include "common/database.h"
#include "common/logging.h"
#include "common/utils.h"

#include "ai/ai_container.h"
#include "ai/helpers/action_queue.h"
#include "entities/char_entity.h"
#include "entities/npc_entity.h"
#include "transport.h"
#include "zone.h"
#include "zone_entities.h"

#include <chrono>
#include <cmath>
#include <filesystem>
#include <unordered_map>
#include <unordered_set>

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

    // A ferry dock gate or an elevator door follows its timetable, not a
    // passer-by: the transport table names the gates, the elevators hold
    // their doors. A 7-second close on one of those would shut a gate the
    // ship had just opened.
    auto isScheduledDoor(const CNpcEntity* PNpc) -> bool
    {
        static std::unordered_set<uint32> gates;
        static bool                       gatesLoaded = false;

        if (!gatesLoaded)
        {
            gatesLoaded = true;
            if (const auto rset = db::preparedStmt("SELECT door FROM transport WHERE door > 0"))
            {
                while (rset->next())
                {
                    gates.insert(rset->get<uint32>("door"));
                }
            }
        }

        if (gates.contains(PNpc->id))
        {
            return true;
        }

        // Elevators are registered by their zone scripts; asked each time so
        // a late registration is never missed (this runs only at a closed
        // door on her way)
        auto* transport = CTransportHandler::getInstance();
        for (int id = 0; id < 256; ++id)
        {
            const auto* elevator = transport->getElevator(static_cast<uint8>(id));
            if (elevator == nullptr)
            {
                continue;
            }
            if ((elevator->LowerDoor != nullptr && elevator->LowerDoor->id == PNpc->id) ||
                (elevator->UpperDoor != nullptr && elevator->UpperDoor->id == PNpc->id))
            {
                return true;
            }
        }
        return false;
    }

    // A door with rules of its own -- a key, a quest, a cutscene -- has a
    // script file; those stay the player's to open. Without one the
    // interaction framework opens the door for anyone (its onTrigger
    // default is -1), so the cardian may too. Looked up once per door.
    auto isGenericDoor(const CNpcEntity* PNpc) -> bool
    {
        static std::unordered_map<uint32, bool> generic;

        if (const auto it = generic.find(PNpc->id); it != generic.end())
        {
            return it->second;
        }

        const bool scripted = std::filesystem::exists(fmt::format("./scripts/zones/{}/npcs/{}.lua", PNpc->loc.zone->getName(), PNpc->getName()));
        const bool ours     = !scripted && !isScheduledDoor(PNpc);
        generic.emplace(PNpc->id, ours);
        return ours;
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
                                                       PNpc->status != xi::Status::Normal || PNpc->loc.zone == nullptr)
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
} // namespace pawn::doors
