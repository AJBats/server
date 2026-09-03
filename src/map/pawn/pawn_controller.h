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

#include "pawn_gambits.h"

#include "ai/controllers/player_controller.h"

#include <array>
#include <chrono>
#include <memory>
#include <optional>
#include <vector>

class CBattleEntity;
class CCharEntity;
class CMobEntity;
class CSpell;
struct position_t;

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

    // Hunt mode: pull for the party while it is idle and healthy. The
    // party's strategy, not a gambit -- set by !pawnhunt until the strategy
    // channel exists (RESEARCH §8)
    void SetHunting(bool on);
    auto IsHunting() const -> bool;
    void SetRetreat(bool on); // the "on me" switch: disengage now, engage nobody, avoid nothing, until cleared
    auto IsRetreating() const -> bool;
    void EngageOn(CMobEntity* PMob);        // the player's order: fight this, closing at once
    auto HatedByAnyMob() const -> bool;     // some mob nearby holds enmity on her

    // The behaviour layer (M3.85): what the gambit rows assert this think,
    // by pawn::Behavior. Cleared at the start of every think; the first row,
    // top down, to speak for a behaviour wins; a switch no row speaks for
    // is off, a parameter takes its default. Rows are the only source.
    void ClearGambitBehaviors();
    void SetGambitBehavior(uint16 behavior, uint16 arg);
    auto Behavior(pawn::Behavior behavior) const -> std::optional<uint16>;

    auto FormationSlot() const -> pawn::Slot;
    auto IsAvoidingAggro() const -> bool;  // keep out of every nearby mob's detection circle (M3.87)
    auto RestsWithPlayer() const -> bool;
    auto HomePointsWithPlayer() const -> bool;

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

    // The avoidance pass over a movement decision, roaming or fighting: the
    // pawn itself inside a danger circle is pushed out (that wins over
    // everything); a slot inside one moves to the nearest clear angle on its
    // ring, or is pushed out; a fight's target inside one is not approached
    // -- she walks up to the boundary and stands there until the tank
    // brings it out; a way to the point that cuts into a circle goes round
    // it first. Every point she walks to is planned against the circles
    // padded by a margin, so the boundary is not slippery. Adjusts the
    // point and the follow tolerances in place; logs once a second.
    enum class AvoidAction : uint8
    {
        None,
        Escape,     // the pawn itself was inside a circle
        Slot,       // its slot was; re-seated on the ring
        PushedSlot, // its slot was; no clear angle, pushed straight out
        Perch,      // its slot is in or beside a circle; standing on the perch it took
        Hold,       // its target was; standing at the boundary
        Detour,     // the way there crossed a circle
    };
    auto Avoid(position_t& point, float& followMax, float& followTarget, float& declumpDistance, bool fighting) -> AvoidAction;

    // An avoidance move too short for the planner, which refuses a hop under
    // a yalm and plans nothing: such a move steps straight at its point when
    // the point is on the mesh
    auto IsShortHop(const position_t& point, float followMax) const -> bool;

    // An avoidance move the planner could not path: one line a second, so a
    // cardian standing still in Escape, Hold or Detour names its cause
    void NotePathFailure(AvoidAction action, const position_t& point, float away);

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

    // Home point with the player: a KO'd cardian whose player has died and
    // come back at their home point goes there too
    void WatchPlayerHomePoint();

    std::unique_ptr<pawn::CGambits> m_Gambits;
    bool                            m_BrainLoaded = false;

    timer::time_point                 m_CombatEndTime;
    timer::time_point                 m_LastRangedAttackTime;
    timer::time_point                 m_LastTravelDebugTime;
    timer::time_point                 m_TravelProgressTime;
    float                             m_TravelBestDist = 0.0f;
    xi::ZoneId                        m_TravelHopZone{};

    bool              m_Hunting    = false;
    bool              m_Retreat    = false;
    bool              m_WasEngaged = false;
    bool              m_HoldForPlayer = false; // drawn on the player's word: no closing until they strike
    timer::time_point m_LastHuntCheckTime;
    timer::time_point m_LastHuntLogTime;
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

    std::array<std::optional<uint16>, pawn::BehaviorCount> m_Behaviors{}; // the behaviour layer, by pawn::Behavior
    bool                                                    m_PlayerSeenDead = false; // while KO'd: the player has been seen dead since

    // Aggro avoidance state
    bool                m_HasSlot = false;  // this tick's FormationPoint ring, for re-seating a slot in danger
    position_t        m_SlotAnchor{};
    float             m_SlotOffset      = 0.0f;
    float             m_SlotAngle       = 0.0f;
    AvoidAction       m_LastAvoidAction = AvoidAction::None;
    // The settle rule: the clear spot she committed to while her own spot
    // lies inside a circle, and the itch to leave it (yalm-seconds)
    std::optional<position_t> m_AvoidPerch;
    float                     m_AvoidItch = 0.0f;
    timer::time_point         m_LastItchTick;
    timer::time_point m_LastAvoidDebugTime;
    timer::time_point m_LastPathFailTime;
};
