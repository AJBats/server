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

// A cardian and a door the game walks a player through (pawn_doors.h). A
// door with a scene of its own -- the President's Office in Metalworks --
// opens for nobody on the server: the player's client asks him at the door,
// places him at it and walks him through, and the server only hears where
// he ends. Her walk ends a yalm short of such a door (the walker's cut,
// pawn_doors.h firstOnRoute); where she may cross it she walks to the spot
// its event places a player at, stands a moment as the door would open, and
// is shown afresh where the walk leaves him: a fade, as the client fades
// him. A door she may not cross holds her where her walk ends.
//
// Who may cross (the user, 2026-10-23): a door on her way to the player is
// always open to her -- anywhere a cardian is, he brought her -- by any
// event that walks a player through it, and with none by a step out on each
// side. Any other door is open only by its own event, the one the game
// offers anyone at it; the door's own check of her rights, and her memory
// of the way she came in, are OPEN_ISSUES #574.
#include "pawn_controller.h"
#include "pawn.h"
#include "pawn_doors.h"
#include "pawn_travel.h"
#include "route_search.h"

#include "ai/ai_container.h"
#include "ai/helpers/pathfind.h"
#include "common/logging.h"
#include "common/settings.h"
#include "common/utils.h"
#include "entities/char_entity.h"
#include "zone.h"

#include <chrono>

using namespace std::chrono_literals;

namespace
{
    constexpr auto  kLookEvery = 1s;     // a walk that ended at a door is looked at this often
    constexpr auto  kHeldLook  = 5s;     // ... and this often once the door held her
    constexpr auto  kAsked     = 1s;     // ... while a mover asked for the walk this recently
    constexpr float kTakeOver  = 6.0f;   // within this of where her walk ends at the door, the crossing takes her
    constexpr float kAtStand   = 0.6f;   // at the spot the crossing starts from
    constexpr float kOffStand  = 10.0f;  // farther than this from it, something else has moved her: the crossing is let go
    constexpr auto  kOpening   = 600ms;  // she stands at the door this long before she goes through: the door opening
    constexpr auto  kGiveUp    = 20s;    // a crossing not made in this long is given up
    constexpr auto  kNotAgain  = 5s;     // the door she last crossed or gave up on waits this long before she tries it again
} // namespace

void CPawnController::LetGoOfDoor(const std::string_view why)
{
    if (!m_Door.has_value())
    {
        return;
    }
    ShowInfoFmt("door: {} lets go of {} ({})", POwner->getName(), pawn::doors::nameOf(m_Door->npc), why);
    m_DoorCrossed   = m_Door->npc;
    m_DoorCrossedAt = m_Tick;
    m_Door.reset();
}

auto CPawnController::DoorTowardHim(const uint32 npc) const -> bool
{
    const auto* PPlayer = GetLivePlayer();
    if (PPlayer == nullptr || PPlayer == POwner)
    {
        return false;
    }
    const auto route = pawn::travel::routeAvoiding(POwner->getZone(), POwner->loc.p, PPlayer->loc.p, {}, cardian::route::HazardSight{});
    if (!route.has_value())
    {
        return false;
    }
    std::vector<position_t> way(1, POwner->loc.p);
    way.insert(way.end(), route->points.begin(), route->points.end());
    const auto first = pawn::doors::firstOnRoute(POwner, way);
    return first.has_value() && first->npc == npc;
}

auto CPawnController::DoorTick() -> bool
{
    auto* PZone = POwner->loc.zone;
    if (!settings::get<bool>("pawn.DOOR_CROSSING") || PZone == nullptr || POwner->isDead() || !POwner->PAI->CanFollowPath())
    {
        m_Door.reset();
        return false;
    }
    // A fight, his hold and his steering are hers to obey, not a door's
    const bool free = !POwner->PAI->IsEngaged() && !HoldsOnOrder() && m_Mode != Mode::Maneuver;

    if (!m_Door.has_value())
    {
        // A walk still asked for ended short of a door, and she is all but
        // there, on the side it ended on: one she was held at before the
        // walk ended stays held
        const auto& plan = m_Walker.plan;
        if (!free || !plan.known || plan.door == 0 || plan.zone != POwner->getZone() || plan.route.empty() || m_Tick - plan.askedAt > kAsked ||
            distance(POwner->loc.p, plan.route.back()) > kTakeOver || m_Tick < m_DoorLookAt ||
            pawn::doors::sideOf(plan.door, POwner->loc.p) != plan.doorSide)
        {
            return false;
        }
        m_DoorLookAt = m_Tick + kLookEvery;
        if (plan.door == m_DoorCrossed && m_Tick - m_DoorCrossedAt < kNotAgain)
        {
            return false;
        }
        // The door's own crossing first: only without one is her way to him
        // searched, a route across the zone
        auto       crossing = pawn::doors::crossingFor(PZone, plan.door, plan.doorSide, false);
        const bool toHim    = !crossing.has_value() && DoorTowardHim(plan.door);
        if (toHim)
        {
            crossing = pawn::doors::crossingFor(PZone, plan.door, plan.doorSide, true);
        }
        if (!crossing.has_value())
        {
            m_DoorLookAt = m_Tick + kHeldLook;
            if (m_DoorWallSaid != plan.door)
            {
                m_DoorWallSaid = plan.door;
                if (m_Mode == Mode::Travel)
                {
                    TrekSay("This door won't open for me. I'll wait here.");
                }
                ShowInfoFmt("door: {} holds at {} in {}: {}", POwner->getName(), pawn::doors::nameOf(plan.door), PZone->getName(),
                            toHim ? "no ground on its far side" : "no event of its own walks a player through it, and the player is not past it");
            }
            return false;
        }
        m_Door = DoorCross{ .npc = plan.door, .crossing = *crossing, .since = m_Tick };
        ShowInfoFmt("door: {} crosses {} in {} ({}): from ({:.1f}, {:.1f}, {:.1f}) to ({:.1f}, {:.1f}, {:.1f})", POwner->getName(),
                    pawn::doors::nameOf(plan.door), PZone->getName(),
                    crossing->event == 0 ? std::string("on her way to the player, a step through")
                    : toHim              ? fmt::format("on her way to the player, as its event {} walks a player", crossing->event)
                                         : fmt::format("as its own event {} walks anyone", crossing->event),
                    crossing->stand.x, crossing->stand.y, crossing->stand.z, crossing->land.x, crossing->land.y, crossing->land.z);
    }

    auto*       PPathFind = POwner->PAI->PathFind.get();
    const auto& crossing  = m_Door->crossing;

    // Opened meanwhile -- the player's own scene, the door's script -- she
    // walks through it as through any open door
    if (!pawn::doors::blocks(PZone, m_Door->npc))
    {
        m_Door.reset();
        return false;
    }
    if (!free)
    {
        LetGoOfDoor(POwner->PAI->IsEngaged() ? "a fight" : HoldsOnOrder() ? "his hold" : "his steering");
        return false;
    }
    if (pawn::doors::sideOf(m_Door->npc, POwner->loc.p) != crossing.from || distance(POwner->loc.p, crossing.stand) > kOffStand)
    {
        LetGoOfDoor("she was moved from the door");
        return false;
    }
    if (m_Tick - m_Door->since > kGiveUp)
    {
        PPathFind->Clear();
        LetGoOfDoor(m_Door->atDoor ? "never went through" : "no way to the door");
        return false;
    }

    if (!m_Door->atDoor)
    {
        // At the spot, or as near it as a walk comes: a spot in the doorway
        // itself lies past where the walker stops her short of the door
        const bool stopped = m_Walker.plan.door == m_Door->npc && !PPathFind->IsFollowingPath() && distance(POwner->loc.p, crossing.stand) <= 2.0f;
        if (distance(POwner->loc.p, crossing.stand) <= kAtStand || stopped)
        {
            PPathFind->Clear();
            m_Door->atDoor = true;
            m_Door->since  = m_Tick;
            PPathFind->LookAt(crossing.land);
            return true;
        }
        Intent intent{};
        intent.kind      = Intent::Kind::Path;
        intent.point     = crossing.stand;
        intent.arrive    = 0.3f;
        intent.tolerance = 0.4f;
        intent.seat      = true; // the door's own spot: the claim board leaves it
        Walk(std::move(intent)); // steps her path too
        return true;
    }
    if (m_Tick - m_Door->since < kOpening)
    {
        PPathFind->Clear();
        PPathFind->LookAt(crossing.land);
        return true;
    }

    // Through, faced the way the walk took her; her walk is planned afresh
    // from the far side
    position_t land = crossing.land;
    land.rotation   = worldAngle(crossing.stand, crossing.land);
    auto* PPawn     = static_cast<CCharEntity*>(POwner);
    pawn::jumpTo(PPawn, land);
    pawn::showAfresh({ PPawn });
    ShowInfoFmt("door: {} is through {}, at ({:.1f}, {:.1f}, {:.1f})", POwner->getName(), pawn::doors::nameOf(m_Door->npc), land.x, land.y, land.z);
    m_DoorCrossed       = m_Door->npc;
    m_DoorCrossedAt     = m_Tick;
    m_Door.reset();
    m_Walker.plan.known = false;
    m_Walker.plan.door  = 0;
    m_CutOffCheckedAt   = {};    // the ground she stands on now is looked at afresh,
    m_TravelHopKnown    = false; // and her trek's way on from it,
    m_LineApproach.forget();     // its line's approach watched from here
    return false;                // her walk goes on from the far side this tick
}
