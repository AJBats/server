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

#pragma once

#include "ai/controllers/player_controller.h"

#include <chrono>
#include <vector>

class CBattleEntity;
class CCharEntity;
class CMobEntity;
struct position_t;

// The autonomous controller for pawn characters: CTrustController's physical
// layer (formation follow, engage-on-the-player's-swing, combat positioning,
// declumping, rest regen) rebuilt around party membership instead of the
// trust master/minion model, mounted on CPlayerController so the pawn keeps
// a real character's action surface. Decision-making (gambits) arrives in a
// later phase; an engaged pawn auto-attacks via the stock battle engine.
class CPawnController : public CPlayerController
{
public:
    CPawnController(CCharEntity* PPawn);

    auto Tick(timer::time_point tick) -> Task<void> override;

    static constexpr float RoamDistance     = 3.0f;
    static constexpr float CastingDistance  = 15.0f;
    static constexpr float WarpDistance     = 30.0f;
    static constexpr float TransferDistance = 3.0f;
    static constexpr float CrossingSlack    = 40.0f;

private:
    auto DoCombatTick(timer::time_point tick) -> Task<void>;
    auto DoRoamTick(timer::time_point tick) -> Task<void>;

    // The summoner is in another zone: walk the zone graph toward them,
    // requesting a transfer at each zone line.
    void TravelTick();

    // The human party member the pawn formation anchors on
    auto GetLivePlayer() const -> CCharEntity*;

    // This pawn's index among the pawns in its party (formation order)
    auto GetPawnPartyPosition() const -> uint8;

    // Pawn 0 follows the player; pawn N follows pawn N-1
    auto GetFollowTarget() const -> CBattleEntity*;

    void Declump(const CBattleEntity* PTarget) const;

    // Navmesh-path toward a point, healing off-mesh endpoints: an off-mesh
    // destination is snapped to the nearest valid point, and an off-mesh
    // owner is snapped back onto the mesh. Never falls back to raw stepping.
    auto PathToward(const position_t& point, float closeTo) -> bool;

    timer::time_point                 m_CombatEndTime;
    timer::time_point                 m_LastHealTickTime;
    timer::time_point                 m_LastTravelDebugTime;
    timer::time_point                 m_TravelProgressTime;
    float                             m_TravelBestDist = 0.0f;
    xi::ZoneId                        m_TravelHopZone{};
    std::vector<std::chrono::seconds> m_tickDelays      = { std::chrono::seconds(15), std::chrono::seconds(10), std::chrono::seconds(10), std::chrono::seconds(3) };
    std::size_t                       m_NumHealingTicks = 0;
};
