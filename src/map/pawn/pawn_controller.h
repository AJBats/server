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
#include <optional>
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

    // Hunt mode (!pawnhunt): this pawn picks and pulls exp mobs on its own
    // while the party is idle, healthy and past the post-fight breather
    void SetHunting(bool on);
    auto IsHunting() const -> bool;

    // Aggro avoidance (M3.87): keep this pawn, its slot and its way there
    // outside every nearby mob's detection circle, and step away when a mob
    // roams in. Off lets it walk anywhere. Two layers: the BASE
    // (pawn.AVOID_AGGRO, changed by !pawnavoid) and the GAMBIT layer -- a
    // behaviour row asserts a value only while its conditions hold, and
    // wins over the base while it does.
    void SetAvoidAggro(bool on);           // the base
    auto IsAvoidingAggro() const -> bool;  // the effective value

    // The gambit interpreter's behaviour pass: cleared at the start of every
    // think, then each matching behaviour row asserts its value
    void ClearGambitBehaviors();
    void SetGambitBehavior(uint16 behavior, bool on);

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

    // Pawn 0 follows the player; pawn N follows pawn N-1. The lead (a
    // hunting pawn) is outside the chain: it holds a point ahead of the
    // player instead.
    auto GetFollowTarget() const -> CBattleEntity*;
    auto LeadPoint(const CCharEntity* PPlayer) -> position_t;

    // The player as the formation sees them: the Cardian Link's fresh
    // position when it streams (loc.p otherwise), and where they will be
    // predictScale * FORMATION_PREDICT_MS from now along the stream's
    // velocity. The lead predicts at full scale, the first follower gently.
    struct Anchor
    {
        position_t                observed{};  // where the player is
        position_t                anchor{};    // where the formation aims (observed + prediction)
        bool                      moving   = false;
        bool                      streamed = false;
        float                     ahead    = 0.0f; // yalms of prediction applied
        std::chrono::milliseconds streamAge{};
    };
    auto PlayerAnchor(const CCharEntity* PPlayer, float predictScale) -> Anchor;

    // A formation slot: `distance` yalms from the anchor at `angle` radians
    // off the player's facing (0 = ahead, pi = behind), held across the
    // client's coarse updates: re-aimed every tick while the player moves,
    // only past FORMATION_DEADBAND while they stand. `held`/`hasHeld` are
    // the caller's memory of the slot.
    auto FormationPoint(const Anchor& anchor, float offset, float angle, position_t& held, bool& hasHeld) -> position_t;

    // Run faster only to close a gap to a point the player defines, ramped
    // with the gap and only while the player moves
    void RampCatchUp(bool playerMoving, const position_t& point);

    // Back to PAWN_SPEED. Once a fight starts the speed limit is never
    // broken: the catch-up ramp belongs to walking with the player, not to
    // combat repositioning.
    void RestoreNormalSpeed();

    // pawn.FORMATION_DEBUG: score the last prediction and log the formation
    // evidence once a second
    void FormationDebug(const char* role, const CCharEntity* PPlayer, const Anchor& anchor, const position_t& point);

    // The avoidance pass over a roam decision: the pawn itself inside a
    // danger circle is pushed out (that wins over everything); a slot inside
    // one moves to the nearest clear angle on its ring, or is pushed out; a
    // straight way there through a circle goes around it first. Adjusts the
    // point and the follow tolerances in place; logs once a second.
    enum class AvoidAction : uint8
    {
        None,
        Escape,     // the pawn itself was inside a circle
        Slot,       // its slot was; re-seated on the ring
        PushedSlot, // its slot was; no clear angle, pushed straight out
        Detour,     // the way there crossed a circle
    };
    auto Avoid(position_t& point, float& followMax, float& followTarget, float& declumpDistance) -> AvoidAction;

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

    // The mob this pawn should join on: the player's engaged target first
    // (gated by the swing/TrustEngageType convention), else any pawn party
    // member's living target -- how a hunter's pull propagates
    auto PartyEngageTarget(CCharEntity* PPlayer) const -> CBattleEntity*;

    // Everyone in this zone's party above the hunt thresholds and the
    // post-fight breather elapsed
    auto HuntReady(const CCharEntity* PPlayer) const -> bool;

    // The nearest idle, non-special mob in the difficulty band within
    // HUNT_RADIUS of the player
    auto PickHuntTarget(const CCharEntity* PPlayer) const -> CMobEntity*;

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

    bool              m_Hunting = false;
    bool              m_WasEngaged = false;
    timer::time_point m_LastHuntCheckTime;
    bool              m_HasLeadPoint = false;
    position_t        m_LeadPoint{};
    bool              m_HasFollowPoint = false;
    position_t        m_FollowPoint{};
    timer::time_point m_LastLeadDebugTime;

    // Prediction scorecard: the player position the lead aimed for, checked
    // against where the stream later put them once the horizon has elapsed
    struct Prediction
    {
        position_t                point{};
        timer::time_point         at{};
        std::chrono::milliseconds horizon{};
        bool                      valid = false;
    };
    Prediction m_Prediction;
    float      m_LastPredictionError = -1.0f; // yalms; <0 = nothing scored yet
    float      m_LastPredictionAhead = 0.0f;  // yalms of prediction applied on the last tick
    bool       m_Sprinting           = false;
    bool       m_PlayerMoving        = false;  // as of the last LeadPoint

    // Aggro avoidance state
    bool                m_AvoidAggroBase;   // seeded from pawn.AVOID_AGGRO in the constructor
    std::optional<bool> m_AvoidAggroGambit; // asserted by a behaviour row this think, if any
    bool                m_HasSlot = false;  // this tick's FormationPoint ring, for re-seating a slot in danger
    position_t        m_SlotAnchor{};
    float             m_SlotOffset      = 0.0f;
    float             m_SlotAngle       = 0.0f;
    AvoidAction       m_LastAvoidAction = AvoidAction::None;
    timer::time_point m_LastAvoidDebugTime;
};
