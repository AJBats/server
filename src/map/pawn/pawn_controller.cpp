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

#include "pawn_controller.h"
#include "pawn.h"

#include "common/utils.h"
#include "common/xirand.h"

#include "ai/ai_container.h"
#include "ai/helpers/pathfind.h"
#include "ai/states/magic_state.h"
#include "ai/states/range_state.h"
#include "enmity_container.h"
#include "entities/char_entity.h"
#include "entities/mob_entity.h"
#include "navmesh/navmesh.h"
#include "party.h"
#include "utils/charutils.h"
#include "utils/zoneutils.h"
#include "zone.h"

CPawnController::CPawnController(CCharEntity* PPawn)
: CPlayerController(PPawn)
{
}

auto CPawnController::Tick(const timer::time_point tick) -> Task<void>
{
    TracyZoneScoped;
    TracyZoneString(POwner->getName());

    m_Tick = tick;

    if (POwner->PAI->IsEngaged())
    {
        co_await DoCombatTick(tick);
    }
    else if (!POwner->isDead())
    {
        co_await DoRoamTick(tick);
    }

    co_return;
}

auto CPawnController::DoCombatTick(const timer::time_point tick) -> Task<void>
{
    TracyZoneScoped;

    std::ignore = tick;

    CCharEntity* PPlayer = GetLivePlayer();

    if (PPlayer == nullptr || !PPlayer->PAI->IsEngaged())
    {
        ShowInfoFmt("pawn: {} disengaging ({})", POwner->getName(), PPlayer == nullptr ? "player gone" : "player disengaged");
        POwner->PAI->Internal_Disengage();
        m_CombatEndTime = m_Tick;
        co_return;
    }

    // Fight what the player fights, once the player has active enmity on it
    if (auto* PMob = dynamic_cast<CMobEntity*>(PPlayer->GetBattleTarget());
        PMob != nullptr && POwner->battleTarget() != PPlayer->battleTarget())
    {
        const auto* enmityList = PMob->PEnmityContainer->GetEnmityList();
        if (const auto it = enmityList->find(PPlayer->id); it != enmityList->end())
        {
            const EnmityObject_t& entry = it->second;
            if (entry.active && (entry.CE + entry.VE) > 0)
            {
                POwner->PAI->Internal_ChangeTarget(PPlayer->battleTarget());
            }
        }
    }

    if (POwner->PAI->IsCurrentState<CMagicState>() || POwner->PAI->IsCurrentState<CRangeState>())
    {
        co_return;
    }

    CBattleEntity* PTarget = POwner->GetBattleTarget();
    if (PTarget == nullptr)
    {
        co_return;
    }

    if (POwner->PAI->CanFollowPath() && POwner->GetSpeed() > 0)
    {
        const float currentDistanceToTarget = distance(POwner->loc.p, PTarget->loc.p);

        POwner->PAI->PathFind->LookAt(PTarget->loc.p);

        // Melee archetype: continually reposition into attack range
        std::unique_ptr<CBasicPacket> err;
        if (!POwner->CanAttack(PTarget, err) && currentDistanceToTarget > RoamDistance)
        {
            PathToward(PTarget->loc.p, RoamDistance);
        }

        if (!POwner->PAI->PathFind->IsFollowingPath())
        {
            Declump(PTarget);
        }

        POwner->PAI->PathFind->FollowPath(m_Tick);
    }

    co_return;
}

auto CPawnController::DoRoamTick(const timer::time_point tick) -> Task<void>
{
    TracyZoneScoped;

    std::ignore = tick;

    if (!POwner->PAI->CanFollowPath())
    {
        co_return;
    }

    if (pawn::travelOrderOf(POwner->id).has_value())
    {
        TravelTick();
        co_return;
    }

    CCharEntity* PPlayer = GetLivePlayer();
    if (PPlayer == nullptr)
    {
        TravelTick();
        co_return;
    }

    auto*      playerController = dynamic_cast<CPlayerController*>(PPlayer->PAI->GetController());
    const bool playerMeleeSwing = playerController != nullptr && playerController->getLastAttackTime() > timer::now() - 1s;

    bool engageCondition = false;
    switch (charutils::GetCharVar(PPlayer, "TrustEngageType"))
    {
        case 1: // Player engages a monster, no melee swing required
        {
            engageCondition = PPlayer->GetBattleTarget() != nullptr;
            break;
        }
        default: // Retail behavior: player engages a monster and executes a melee swing
        {
            engageCondition = PPlayer->GetBattleTarget() != nullptr && playerMeleeSwing;
            break;
        }
    }

    if (PPlayer->PAI->IsEngaged() && engageCondition)
    {
        POwner->PAI->Internal_Engage(PPlayer->battleTarget());
        co_return;
    }

    const CBattleEntity* PFollowTarget = GetFollowTarget();
    if (PFollowTarget == nullptr)
    {
        co_return;
    }

    const uint8 currentPartyPos = GetPawnPartyPosition();
    const bool  isFirstPawn     = currentPartyPos == 0;

    const float declumpDistance = isFirstPawn ? 1.0f : 1.5f;
    const float followMax       = isFirstPawn ? 2.0f : 3.5f;
    const float followTarget    = isFirstPawn ? 1.5f : 3.0f;

    const float currentDistance = distance(POwner->loc.p, PFollowTarget->loc.p);

    if (currentDistance > followMax)
    {
        // Warp only when pathing genuinely fails; a pawn arriving at a zone
        // gate runs to its player like anyone else would
        if (!PathToward(PFollowTarget->loc.p, followTarget) && currentDistance > WarpDistance)
        {
            POwner->PAI->PathFind->WarpTo(PFollowTarget->loc.p);
            co_return;
        }
    }
    else if (currentDistance < declumpDistance)
    {
        if (!POwner->PAI->PathFind->IsFollowingPath())
        {
            PathToward(PFollowTarget->loc.p, followTarget + 0.5f);
        }
    }
    else if (POwner->PAI->PathFind->IsFollowingPath())
    {
        POwner->PAI->PathFind->Clear();
    }

    if (POwner->PAI->PathFind->IsFollowingPath())
    {
        POwner->PAI->PathFind->FollowPath(m_Tick);
    }

    if (POwner->CanRest() &&
        m_Tick - POwner->LastAttacked > m_tickDelays.at(0) &&
        m_Tick - m_CombatEndTime > m_tickDelays.at(0) &&
        m_Tick - m_LastHealTickTime > m_tickDelays.at(m_NumHealingTicks))
    {
        if (POwner->health.hp != POwner->health.maxhp || POwner->health.mp != POwner->health.maxmp)
        {
            const auto recoverHP = static_cast<uint32>(POwner->health.maxhp * 0.05);
            const auto recoverMP = static_cast<uint32>(POwner->health.maxmp * 0.05);
            POwner->addHP(recoverHP);
            POwner->addMP(recoverMP);
            m_LastHealTickTime = m_Tick;
            POwner->updatemask |= UPDATE_HP;
            m_NumHealingTicks = std::clamp(m_NumHealingTicks + 1, static_cast<std::size_t>(0U), m_tickDelays.size() - 1U);
        }
    }

    co_return;
}

void CPawnController::TravelTick()
{
    const bool narrate = m_Tick - m_LastTravelDebugTime > 5s;
    if (narrate)
    {
        m_LastTravelDebugTime = m_Tick;
    }

    const auto* PPawn = static_cast<CCharEntity*>(POwner);
    const auto  order = pawn::travelOrderOf(POwner->id);

    xi::ZoneId targetZone{};
    if (order.has_value())
    {
        if (*order == POwner->getZone())
        {
            ShowInfoFmt("pawn: travel {}: arrived at ordered zone {}", POwner->getName(), static_cast<uint16>(*order));
            pawn::clearTravelOrder(POwner->id);
            return;
        }
        targetZone = *order;
    }
    else
    {
        if (PPawn->PParty == nullptr)
        {
            if (narrate)
            {
                ShowInfoFmt("pawn: travel {}: no party, idling", POwner->getName());
            }
            return;
        }

        CCharEntity* PSummoner = zoneutils::GetChar(pawn::summonerOf(POwner->id));
        if (PSummoner == nullptr || PSummoner->loc.zone == nullptr)
        {
            if (narrate)
            {
                ShowInfoFmt("pawn: travel {}: summoner not in world (loading?), idling", POwner->getName());
            }
            return;
        }

        if (PSummoner->loc.zone == POwner->loc.zone)
        {
            if (narrate)
            {
                ShowInfoFmt("pawn: travel {}: summoner shares zone {} but no live player found by party scan", POwner->getName(), static_cast<uint16>(POwner->getZone()));
            }
            return;
        }

        targetZone = PSummoner->getZone();
    }

    const auto hop = pawn::travel::nextHop(POwner->getZone(), targetZone);
    if (!hop.has_value())
    {
        if (order.has_value())
        {
            ShowInfoFmt("pawn: travel {}: no route {} -> {}, order cancelled", POwner->getName(), static_cast<uint16>(POwner->getZone()), static_cast<uint16>(targetZone));
            pawn::clearTravelOrder(POwner->id);
        }
        else
        {
            ShowInfoFmt("pawn: travel {}: no route {} -> {}, teleporting to summoner", POwner->getName(), static_cast<uint16>(POwner->getZone()), static_cast<uint16>(targetZone));
            pawn::requestTransfer(POwner->id, std::nullopt);
        }
        return;
    }

    const float distToLine = distance(POwner->loc.p, hop->walkTo);

    if (narrate)
    {
        ShowInfoFmt("pawn: travel {}: zone {} -> {} via line at {:.1f}y", POwner->getName(), static_cast<uint16>(POwner->getZone()), static_cast<uint16>(hop->destinationZone), distToLine);
    }

    // Crossing requires physically reaching the line, like a player does
    if (distToLine < TransferDistance)
    {
        pawn::requestTransfer(POwner->id, hop);
        return;
    }

    // Fresh hop: start progress tracking anew
    if (hop->destinationZone != m_TravelHopZone)
    {
        m_TravelHopZone      = hop->destinationZone;
        m_TravelBestDist     = distToLine;
        m_TravelProgressTime = m_Tick;
    }

    if (distToLine + 0.5f < m_TravelBestDist)
    {
        m_TravelBestDist     = distToLine;
        m_TravelProgressTime = m_Tick;
    }
    else if (m_Tick - m_TravelProgressTime > 3s && distToLine < CrossingSlack)
    {
        // As close as the mesh allows counts as arrival
        ShowInfoFmt("pawn: travel {}: no progress at {:.1f}y from the line, crossing", POwner->getName(), distToLine);
        m_TravelHopZone = {};
        pawn::requestTransfer(POwner->id, hop);
        return;
    }

    if (!PathToward(hop->walkTo, 2.0f))
    {
        // Walked as far as the mesh reaches; the mesh often ends short of
        // the zone mouth, and the server itself accepts crossings from up
        // to ~40 yalms out
        if (distToLine < CrossingSlack)
        {
            pawn::requestTransfer(POwner->id, hop);
        }
        else if (order.has_value())
        {
            ShowInfoFmt("pawn: travel {}: cannot path to line ({:.1f}y away), order cancelled", POwner->getName(), distToLine);
            pawn::clearTravelOrder(POwner->id);
        }
        else
        {
            ShowInfoFmt("pawn: travel {}: cannot path to line ({:.1f}y away), teleporting to summoner", POwner->getName(), distToLine);
            pawn::requestTransfer(POwner->id, std::nullopt);
        }
        return;
    }

    if (POwner->PAI->PathFind->IsFollowingPath())
    {
        POwner->PAI->PathFind->FollowPath(m_Tick);
    }
}

auto CPawnController::GetLivePlayer() const -> CCharEntity*
{
    const auto* PPawn = static_cast<CCharEntity*>(POwner);
    if (PPawn->PParty == nullptr)
    {
        return nullptr;
    }

    for (auto* PMember : PPawn->PParty->members)
    {
        if (auto* PChar = dynamic_cast<CCharEntity*>(PMember);
            PChar != nullptr && PChar->PSession != nullptr && PChar->loc.zone == POwner->loc.zone)
        {
            return PChar;
        }
    }
    return nullptr;
}

auto CPawnController::GetPawnPartyPosition() const -> uint8
{
    const auto* PPawn = static_cast<CCharEntity*>(POwner);
    if (PPawn->PParty == nullptr)
    {
        return 0;
    }

    uint8 position = 0;
    for (const auto* PMember : PPawn->PParty->members)
    {
        if (const auto* PChar = dynamic_cast<const CCharEntity*>(PMember);
            PChar != nullptr && pawn::isPawn(PChar))
        {
            if (PChar == POwner)
            {
                return position;
            }
            ++position;
        }
    }
    return 0;
}

auto CPawnController::GetFollowTarget() const -> CBattleEntity*
{
    const auto* PPawn = static_cast<CCharEntity*>(POwner);
    CCharEntity* PPlayer = GetLivePlayer();

    const uint8 currentPartyPos = GetPawnPartyPosition();
    if (currentPartyPos == 0 || PPawn->PParty == nullptr)
    {
        return PPlayer;
    }

    uint8 position = 0;
    for (auto* PMember : PPawn->PParty->members)
    {
        if (auto* PChar = dynamic_cast<CCharEntity*>(PMember);
            PChar != nullptr && pawn::isPawn(PChar))
        {
            if (position == currentPartyPos - 1 && PChar->loc.zone == POwner->loc.zone)
            {
                return PChar;
            }
            ++position;
        }
    }
    return PPlayer;
}

void CPawnController::Declump(const CBattleEntity* PTarget) const
{
    TracyZoneScoped;

    const auto* PPawn = static_cast<CCharEntity*>(POwner);
    if (PPawn->PParty == nullptr)
    {
        return;
    }

    const uint8 currentPartyPos = GetPawnPartyPosition();
    for (const auto* PMember : PPawn->PParty->members)
    {
        const auto* POther = dynamic_cast<const CCharEntity*>(PMember);
        if (POther == nullptr || POther == POwner || !pawn::isPawn(POther) ||
            POther->loc.zone != POwner->loc.zone ||
            (POther->PAI->PathFind && POther->PAI->PathFind->IsFollowingPath()) ||
            distance(POther->loc.p, POwner->loc.p) >= 1.5f)
        {
            continue;
        }

        // Spread around the shared target rather than away from each other
        const float moveAmount = xirand::GetRandomNumber(0.0f, 1.5f) * ((currentPartyPos % 2) ? 1.0f : -1.0f);
        const auto  newPos     = sidestepPosition(POwner->loc.p, PTarget->loc.p, moveAmount);

        if (POwner->PAI->PathFind->ValidPosition(newPos))
        {
            POwner->PAI->PathFind->PathTo(newPos, PATHFLAG_RUN);
        }
        break;
    }
}

auto CPawnController::PathToward(const position_t& point, const float closeTo) -> bool
{
    auto* PPathFind = POwner->PAI->PathFind.get();

    if (PPathFind->PathAround(point, closeTo, PATHFLAG_RUN))
    {
        return true;
    }

    const auto* navMesh = POwner->loc.zone != nullptr ? POwner->loc.zone->navMesh() : nullptr;
    if (navMesh == nullptr)
    {
        return false;
    }

    // Client-positioned players can stand where the mesh doesn't reach
    if (const auto snapped = navMesh->findClosestValidPoint(point); snapped.has_value())
    {
        if (PPathFind->PathAround(*snapped, closeTo, PATHFLAG_RUN))
        {
            return true;
        }
    }

    // We may be off-mesh ourselves (knockback, legacy stepping)
    navMesh->snapToValidPosition(POwner->loc.p);
    return PPathFind->PathAround(point, closeTo, PATHFLAG_RUN);
}
