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
#include <memory>
#include <vector>

class CBattleEntity;
class CCharEntity;
class CMobEntity;
class CSpell;
struct position_t;

namespace pawn
{
    class CGambits;
}

// The autonomous controller for pawn characters: CTrustController's physical
// layer (formation follow, engage-on-the-player's-swing, combat positioning,
// declumping, rest regen) rebuilt around party membership instead of the
// trust master/minion model, mounted on CPlayerController so the pawn keeps
// a real character's action surface. Decisions come from the pawn gambit
// interpreter, fed by a Lua brain that is reloaded whenever the pawn's job
// changes; an engaged pawn also auto-attacks via the stock battle engine.
class CPawnController : public CPlayerController
{
public:
    CPawnController(CCharEntity* PPawn);
    ~CPawnController() override;

    auto Tick(timer::time_point tick) -> Task<void> override;

    // Action surface used by the gambit interpreter. Each faces the target
    // first (the player weapon-skill path refuses a target the character is
    // not facing), then runs the stock player validation: known spell or
    // ability, recasts, TP, ammo, facing.
    auto Cast(EntityId target, SpellID spellid) -> bool override;
    auto WeaponSkill(EntityId target, uint16 wsid) -> bool override;
    auto Ability(EntityId target, uint16 abilityid) -> bool override;
    auto RangedAttack(EntityId target) -> bool override;

    // The human party member the pawn formation anchors on
    auto GetLivePlayer() const -> CCharEntity*;

    // This pawn's index among the pawns in its party (formation order)
    auto GetPawnPartyPosition() const -> uint8;

    // Whoever the pawn's current battle target hates most
    auto GetTopEnmity() const -> CBattleEntity*;

    auto Gambits() -> pawn::CGambits&;

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

    // Pawn 0 follows the player; pawn N follows pawn N-1
    auto GetFollowTarget() const -> CBattleEntity*;

    void Declump(const CBattleEntity* PTarget) const;

    // Navmesh-path toward a point, healing off-mesh endpoints: an off-mesh
    // destination is snapped to the nearest valid point, and an off-mesh
    // owner is snapped back onto the mesh. Never falls back to raw stepping.
    auto PathToward(const position_t& point, float closeTo) -> bool;

    void FaceTarget(EntityId target) const;

    // Reload the Lua brain when the pawn's main or support job changed
    void CheckBrain();

    // Another party member is already casting something that makes this
    // cast redundant (same buff family, a cure on the same healthy target...)
    auto PartyAlreadyCasting(CSpell* PSpell, const CBattleEntity* PTarget) const -> bool;

    std::unique_ptr<pawn::CGambits> m_Gambits;
    uint8                           m_BrainMainJob = 0xFF;
    uint8                           m_BrainSubJob  = 0xFF;

    timer::time_point                 m_CombatEndTime;
    timer::time_point                 m_LastHealTickTime;
    timer::time_point                 m_LastRangedAttackTime;
    timer::time_point                 m_LastTravelDebugTime;
    timer::time_point                 m_TravelProgressTime;
    float                             m_TravelBestDist = 0.0f;
    xi::ZoneId                        m_TravelHopZone{};
    std::vector<std::chrono::seconds> m_tickDelays      = { std::chrono::seconds(15), std::chrono::seconds(10), std::chrono::seconds(10), std::chrono::seconds(3) };
    std::size_t                       m_NumHealingTicks = 0;
};
