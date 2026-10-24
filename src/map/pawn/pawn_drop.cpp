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

// A cardian drops down a ledge (OPEN_ISSUES #581), two ways.
//
// Her own: a navmesh links a ledge down to the ground below it -- the
// monsters' some 5 yalms, a dungeon's built by the client's rule as far as
// pawn.CLIENT_NAVMESH_DROP -- and her route search takes such a link as a
// step of her way, the lip one waypoint and the landing the next, wherever
// she walks and whether he went that way or not. Her path finder, which walks
// every leg as ground, is handed her walk only as far as the lip
// (cardian::walk::toNextLip); at the lip she falls to the landing
// (DropOnRoute), her movement counted so the client draws the fall.
//
// His, followed: where her navmesh has no way down where he stepped off a
// ledge, her walk to him finds no route and she stands at the top. His drops
// are noted from his own position packets (pawn::notePlayerFall): where he
// stepped off and where he landed. Where her walk does not reach him, and it
// reaches where he stepped off of a drop whose landing walks to him, she
// walks there and drops after him (DropTrek, DropTick).
#include "pawn_controller.h"
#include "pawn.h"
#include "pawn_travel.h"

#include "ai/ai_container.h"
#include "ai/helpers/pathfind.h"
#include "common/logging.h"
#include "common/settings.h"
#include "entities/char_entity.h"
#include "navmesh/navmesh.h"
#include "zone.h"

#include <algorithm>
#include <chrono>
#include <cmath>

using namespace std::chrono_literals;

namespace
{
    constexpr auto  kLookEvery = 3s;    // how often she looks whether a drop of his leads to him
    constexpr auto  kForgotten = 10min; // a drop older than this is not followed
    constexpr auto  kGiveUp    = 30s;   // a drop not made in this long is given up
    constexpr float kAtTop     = 1.2f;  // at where he stepped off
    constexpr float kOffTop    = 25.0f; // this much farther from it than she has come, something else has moved her
    constexpr float kWithHim   = 3.0f;  // a walk that ends this near him reaches him
    constexpr float kBeside    = 1.5f;  // she lands this far from where he did
    constexpr auto  kFallTime  = 600ms; // the time her drop is drawn over

    constexpr float kAtLip = 1.0f; // at the lip of a drop on her route (safe_walk.h isDrop): her drop begins

    // Whether an everyday walk -- on the walker's widest everyday ground,
    // never the long way round -- ends at `to`, within `within` of it across
    // and on its floor: the route search walks to the ground nearest its
    // goal, which for a ledge's edge can lie yalms back from it, and for a
    // ledge she cannot reach is the floor under it
    auto walksTo(const xi::ZoneId zone, const position_t& from, const position_t& to, const float within) -> bool
    {
        const float reach = distance(from, to) * 3.0f + pawn::walker::kWiderSpare;
        const auto  route = pawn::travel::routeAvoiding(zone, from, to, {}, cardian::route::HazardSight{}, reach);
        if (!route.has_value())
        {
            return false;
        }
        const position_t& last = route->points.empty() ? from : route->points.back();
        return std::hypot(last.x - to.x, last.z - to.z) <= within && std::abs(last.y - to.y) <= pawn::travel::kFloorReach;
    }
} // namespace

void CPawnController::LetGoOfDrop(const std::string_view why)
{
    if (!m_Drop.has_value())
    {
        return;
    }
    ShowInfoFmt("drop: {} lets go of the drop ({})", POwner->getName(), why);
    m_Drop.reset();
}

auto CPawnController::DropTrek(const CCharEntity* PPlayer) -> bool
{
    if (m_Drop.has_value())
    {
        return true;
    }
    if (!settings::get<bool>("pawn.DROP_FOLLOW") || PPlayer == nullptr || POwner->loc.zone == nullptr || m_Tick - m_DropLookedAt < kLookEvery)
    {
        return false;
    }
    m_DropLookedAt = m_Tick;

    // His drops in her zone, fresh, and not one she gave up on
    const auto zone  = POwner->getZone();
    auto       drops = pawn::dropsOf(PPlayer->id);
    std::erase_if(drops, [&](const pawn::PlayerDrop& d)
    {
        return d.zone != static_cast<uint16>(zone) || timer::now() - d.when > kForgotten || d.when <= m_DropGivenUp;
    });
    if (drops.empty() || ReachToPlayer(PPlayer) == Reach::Walk)
    {
        return false; // nothing to follow, or her walk reaches him -- the long way round is no walk to him: his drop is
    }

    // His newest drop whose top an everyday walk takes her to and whose
    // landing an everyday walk joins to him. A drop either end of which only
    // the long way round reaches is no way to him: one he took before he
    // moved on leads away from him (the long way's own hold says so)
    for (auto it = drops.rbegin(); it != drops.rend(); ++it)
    {
        if (!walksTo(zone, POwner->loc.p, it->top, kAtTop) || !walksTo(zone, it->land, PPlayer->loc.p, kWithHim))
        {
            continue;
        }
        m_Drop = DropFollow{ .drop = *it, .since = m_Tick };
        ShowInfoFmt("drop: {} follows {} down from ({:.1f}, {:.1f}, {:.1f}) to ({:.1f}, {:.1f}, {:.1f}): her walk does not reach him", POwner->getName(),
                    PPlayer->getName(), it->top.x, it->top.y, it->top.z, it->land.x, it->land.y, it->land.z);
        return true;
    }
    return false;
}

auto CPawnController::DropOnRoute() -> bool
{
    // On her walk itself: not a sneak-by's hop, a back-off to safe ground,
    // nor held short of a choke, whose paths pass the lip without its drop
    auto* PZone = POwner->loc.zone;
    if (PZone == nullptr || !m_Plan.walking || m_Plan.next >= m_Plan.route.size() || m_Plan.zone != POwner->getZone() || !m_Plan.hop.empty() ||
        !m_Plan.backOff.empty() || (m_Plan.outcome != cardian::walk::Outcome::Walking && m_Plan.outcome != cardian::walk::Outcome::Escaping))
    {
        return false;
    }
    // The lip she stands at: the waypoint she has reached or the one she
    // walks to, her path finder handed her walk only as far as it
    // (cardian::walk::toNextLip) -- on its floor: a landing can lie within
    // a yalm of its lip across, and she has landed
    const auto& route = m_Plan.route;
    const auto  atLip = [&](const size_t lip)
    {
        return lip + 1 < route.size() && cardian::walk::isDrop(route[lip], route[lip + 1]) &&
               std::hypot(POwner->loc.p.x - route[lip].x, POwner->loc.p.z - route[lip].z) <= kAtLip &&
               std::abs(POwner->loc.p.y - route[lip].y) <= pawn::travel::kFloorReach;
    };
    const size_t lip = m_Plan.next > 0 && atLip(m_Plan.next - 1) ? m_Plan.next - 1 : m_Plan.next;
    if (!atLip(lip))
    {
        return false;
    }
    const auto& land   = route[lip + 1];
    const float down   = land.y - route[lip].y; // FFXI's height grows downward
    position_t  spot   = land;
    spot.rotation      = POwner->loc.p.rotation;
    const auto  before = POwner->loc.p;
    POwner->loc.p      = spot;
    PZone->onEntityMoved(POwner);
    CountMove(before, std::chrono::duration_cast<std::chrono::microseconds>(kFallTime));
    POwner->updatemask |= UPDATE_POS;
    // Her path finder's walk ended at the lip: let go of, or it walks her back up to it
    POwner->PAI->PathFind->Clear();
    ShowInfoFmt("drop: {} drops {:.1f}y off a ledge on her way, to ({:.1f}, {:.1f}, {:.1f})", POwner->getName(), down, spot.x, spot.y, spot.z);
    m_CutOffCheckedAt = {}; // the ground she stands on now is looked at afresh
    return true;            // her walk goes on from the landing next tick, its path handed afresh
}

auto CPawnController::DropTick() -> bool
{
    if (!m_Drop.has_value())
    {
        return false;
    }
    auto* PZone = POwner->loc.zone;
    if (!settings::get<bool>("pawn.DROP_FOLLOW") || PZone == nullptr || POwner->isDead() || m_Drop->drop.zone != static_cast<uint16>(POwner->getZone()))
    {
        m_Drop.reset();
        return false;
    }
    if (POwner->PAI->IsEngaged() || HoldsOnOrder())
    {
        LetGoOfDrop(POwner->PAI->IsEngaged() ? "a fight" : "his hold");
        return false;
    }
    const auto& drop  = m_Drop->drop;
    const float toTop = std::hypot(POwner->loc.p.x - drop.top.x, POwner->loc.p.z - drop.top.z);
    m_Drop->nearest   = std::min(m_Drop->nearest, toTop);
    if (toTop > m_Drop->nearest + kOffTop)
    {
        LetGoOfDrop("she was moved from it");
        return false;
    }
    if (m_Tick - m_Drop->since > kGiveUp)
    {
        POwner->PAI->PathFind->Clear();
        m_DropGivenUp = drop.when;
        LetGoOfDrop("no way to where he stepped off");
        return false;
    }

    if (toTop > kAtTop)
    {
        Intent intent{};
        intent.kind      = Intent::Kind::Path;
        intent.point     = drop.top;
        intent.arrive    = 0.5f;
        intent.tolerance = 0.8f;
        intent.seat      = true; // his own spot at the edge: the claim board leaves it
        Walk(std::move(intent)); // steps her path too
        return true;
    }

    // Over the edge: down beside where he landed, on the floor there, the
    // client drawing her fall over the time it took
    const float away  = std::hypot(drop.land.x - drop.top.x, drop.land.z - drop.top.z);
    position_t  spot  = drop.land;
    if (away > 0.1f)
    {
        spot.x += (drop.land.x - drop.top.x) / away * kBeside;
        spot.z += (drop.land.z - drop.top.z) / away * kBeside;
    }
    // on the ground nearest that, where her walks keep to it, if that ground
    // is his: walks to where he landed; else where he landed
    auto onMesh = pawn::travel::snapToTown(POwner->getZone(), spot);
    if (!onMesh.has_value())
    {
        if (auto* navMesh = PZone->navMesh(); navMesh != nullptr)
        {
            onMesh = navMesh->findClosestValidPoint(spot);
        }
    }
    spot = onMesh.has_value() && distance(*onMesh, drop.land) < 4.0f && pawn::travel::walks(POwner->getZone(), *onMesh, drop.land, kBeside) ? *onMesh : drop.land;
    spot.rotation     = POwner->loc.p.rotation;
    const auto before = POwner->loc.p;
    POwner->PAI->PathFind->Clear();
    POwner->loc.p = spot;
    PZone->onEntityMoved(POwner);
    CountMove(before, std::chrono::duration_cast<std::chrono::microseconds>(kFallTime));
    POwner->updatemask |= UPDATE_POS;
    ShowInfoFmt("drop: {} drops {:.1f}y after him, to ({:.1f}, {:.1f}, {:.1f})", POwner->getName(), spot.y - before.y, spot.x, spot.y, spot.z);
    m_Drop.reset();
    m_Walker.plan.known = false;
    m_CutOffCheckedAt   = {};    // the ground she stands on now is looked at afresh,
    m_TravelHopKnown    = false; // and her trek's way on from it,
    m_LineApproach.forget();     // its line's approach watched from here
    return false;                // her follow goes on from the landing this tick
}
