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

#include "pawn_danger.h"
#include "pawn_gambits.h"
#include "pawn_rules.h"

#include "ai/controllers/player_controller.h"

#include <array>
#include <chrono>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
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

    // The mode (pawn-modes step 2): what she is doing, one word, with one
    // writer (Transition) and every change said with its reason. Follow is
    // the rest state; Wait, Travel and Retreat are the player's; Approach
    // is a walk in with her weapon away; Hold is drawn on the player's
    // word, waiting for their strike; Fight is a fight; Down is KO'd. The
    // server's attack state is an input, not the mode: every tick the two
    // are reconciled, and a fight the server ended is a transition out of
    // Fight with the server's reason -- never a silent one.
    enum class Mode : uint8
    {
        Follow,
        Wait,
        Travel,
        Approach,
        Hold,
        Fight,
        Retreat,
        Down
    };
    static auto modeName(Mode mode) -> const char*;
    auto        CurrentMode() const -> Mode;

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

    // Wait here / follow me. Waiting, she holds her ground: no following,
    // hunting or travel, but she answers a mob that comes for her, keeps
    // her gambits and fidgets. An ordered wait holds until told otherwise;
    // an automatic one (left behind by a warp or a teleport, or carried
    // off alone) ends when the player is back in her zone.
    void SetWaiting(bool on, bool ordered, std::string_view why = {}); // `why` is the transition's reason; empty takes a plain one
    auto IsWaiting() const -> bool;
    void Carried(bool withPlayer); // carried off by a warp or a teleport: alone, she waits where she lands; with the player, she arrives following
    void EngageOn(CMobEntity* PMob);        // the player's order: fight this, after her beat (FireOrderedEngage)
    void ShareSignet(CCharEntity* PPlayer); // the gate guard's Signet, taken with the player for its remaining time

    // Her bag kept stacked (pawn::items::tidyStacks), a quiet sweep every
    // 15 s between fights: a drop lands unstacked like anything else
    void TidyBag();

    // Her head turns to PAt (nullptr: straight ahead): the face-target
    // index in the character update, which the client turns a player's
    // head with -- players set it with every position packet, she never
    // sends one. An update goes out only when it changes.
    void HeadLook(const CBaseEntity* PAt);

    // Mid-action: casting, readying a weapon skill or ability, or shooting
    auto Acting() const -> bool;

    // A fidget now and then while standing about -- motion only, no text,
    // to everyone in range; a stare goes to the player. Never mid-walk,
    // in a fight, resting or casting.
    void IdleEmote(const CCharEntity* PPlayer);

    // May she draw on this target yet? The cooldown is set when she LEAVES
    // a fight, not by her last swing: a cardian fresh from rest draws at
    // once, one just off a kill waits. The mob she just left costs the
    // longer wait (her weapon delay, the anti-exploit), anything else the
    // shorter one (cardian.REENGAGE_SWITCH_DELAY).
    auto CanDrawOn(CBattleEntity* PTarget) -> bool;

    // The command window: one action now, on the target the player picked.
    // `key` is the vocabulary's action key, kind:mode:id -- the concrete
    // ones only: a spell (2:2:id), an ability (3:2:id), a weapon skill
    // (4:2:id), the ranged attack (1:0:0); the "best of" entries are the
    // gambit engine's. "" when it fired, else why not.
    auto DoAction(const std::string& key, CBattleEntity* PTarget) -> std::string;

    // The order given while she acts, or while the spell is on recast,
    // fired the moment both allow -- the client queues one action behind
    // a cast the same way, and a string of the same spell fires each one
    // the moment its timer allows. A newer order replaces it; 30 s
    // without a chance and it is let go.
    void FireQueuedOrder();

    // The attack order, fired once her beat is served: the front row draws
    // first, the back line a touch later
    void FireOrderedEngage();
    auto HatedByAnyMob() const -> bool;     // some mob nearby holds enmity on her

    // The behaviour layer (M3.85): what the gambit rows assert this think,
    // by pawn::Behavior. Cleared at the start of every think; the first row,
    // top down, to speak for a behaviour wins; a switch no row speaks for
    // is off, a parameter takes its default. Rows are the only source.
    void ClearGambitBehaviors();
    void SetGambitBehavior(uint16 behavior, uint16 arg);
    auto Behavior(pawn::Behavior behavior) const -> std::optional<uint16>;

    auto FormationSlot() const -> pawn::Slot;

    // Her seat on a mob's fight ring, and where it is, for the party's
    // other cardians to read when they pick theirs (TakeFightSeat)
    auto FightSeatOn(uint32 mobId) const -> std::optional<pawn::Slot>;
    auto HeldSeatPoint(const CBattleEntity* PTarget) const -> std::optional<position_t>;
    auto IsAvoidingAggro() const -> bool;  // keep out of every nearby mob's detection circle (M3.87)
    auto RestsWithPlayer() const -> bool;
    auto HomePointsWithPlayer() const -> bool;

    static constexpr float RoamDistance     = 3.0f;
    static constexpr float LockOnSlack      = 2.0f; // lock-on holds this far beyond melee reach, so a step out of reach does not drop it
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

    // The lead holds a point ahead of the player; everyone else holds a
    // seat on the ring around them. RingSlot is a Formation row's seat, or
    // the silent one by job over the party's cardians in this zone
    // (formation_math.h assignSlots); SeatOf places it from the
    // FORMATION_FLANK_* / FOLLOW_* / REAR_DISTANCE settings.
    auto LeadPoint(const CCharEntity* PPlayer) -> position_t;
    auto RingSlot() const -> pawn::Slot;
    struct SeatGeometry
    {
        float offset = 0.0f; // yalms from the anchor
        float angle  = 0.0f; // radians off the player's facing
    };
    static auto SeatOf(pawn::Slot slot) -> SeatGeometry;

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
    // padded by a margin, so the boundary is not slippery. PIgnore is the
    // party's own mob, never a danger. Adjusts the point and the follow
    // tolerances in place; logs once a second.
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

    // The locomotion pass (pawn-modes step 3). The movers -- formation,
    // the walk in, the seat, the step back, the declump -- only propose:
    // an Intent says where she wants to be this tick and how. Move is
    // the one walker: it vets the proposal against the tick's danger map
    // the same way for every mover (escape, hold at the rim, detour,
    // re-seat a slot), makes the tick's one path or step, and keeps her
    // face on the mob in reach. Nothing else moves her.
    struct Intent
    {
        enum class Kind : uint8
        {
            Stand,     // stay: drop any path under her feet
            Keep,      // leave the path she is on alone
            Hop,       // a step under the planner's floor, straight at the point
            Path,      // a path to the point, when farther than `tolerance`
            Formation, // the follow rules: path, nudge out of a clump, warp when lost
        };
        Kind                 kind       = Kind::Stand;
        position_t           point{};
        float                arrive     = 1.0f;    // close enough: the path's end
        float                tolerance  = 2.0f;    // this far off before she walks
        float                declump    = 0.0f;    // Formation: nudge out when closer than this
        const CBattleEntity* target     = nullptr; // the mob in the fight: lock-on in reach
        bool                 fighting   = false;   // in danger, hold at the rim (else re-seat the slot)
        bool                 vet        = true;    // false: the party waved the company through
        bool                 warpIfLost = false;   // Formation: far and no path, warp to the player
        bool                 seat       = false;   // a seat's path: failing it drops the seat
    };

    // The tick's danger map, scanned once before the movers run so every
    // one of them can ask IsClear while choosing, and the vet sees the
    // same circles. PIgnore is the party's own mob, never a danger.
    void RefreshDangers(const CBattleEntity* PIgnore);
    auto IsClear(float x, float z) const -> bool;    // outside every padded circle
    auto InsideDanger() const -> bool;               // she stands inside a true circle

    // The walker. Returns the vet's action, or nothing when the tick was
    // spent on a warp.
    auto Move(Intent intent) -> std::optional<AvoidAction>;

    // The formation mover, roaming or holding for the player's strike:
    // where this pawn belongs (the lead's point ahead of the player, or a
    // chain slot). PStandOff is a mob the party is holding on: no point is
    // placed within its reach plus FORMATION_STANDOFF (a point aimed past
    // it comes round to the player's side).
    auto FormationIntent(CCharEntity* PPlayer, const CBattleEntity* PStandOff) -> Intent;

    // The walk in on a mob: to within RoamDistance of it
    auto ApproachIntent(const CBattleEntity* PTarget) const -> Intent;

    // An avoidance move too short for the planner, which refuses a hop under
    // a yalm and plans nothing: such a move steps straight at its point when
    // the point is on the mesh
    auto IsShortHop(const position_t& point, float followMax) const -> bool;

    // An avoidance move the planner could not path: one line a second, so a
    // cardian standing still in Escape, Hold or Detour names its cause
    void NotePathFailure(AvoidAction action, const position_t& point, float away);

    // The declump mover: a party cardian standing on her at the front is
    // given room by a sidestep round the mob, to a clear spot
    auto DeclumpIntent(const CBattleEntity* PTarget) const -> std::optional<Intent>;

    // The step back: a target that has settled on her toes (a mob walks
    // onto its target's exact coordinates) is given room. Once it has
    // stood still for MELEE_BACKOFF_DELAY, a cardian nearer it than
    // MELEE_BACKOFF_TRIGGER steps straight back to RoamDistance, capped at
    // its melee reach less MELEE_BACKOFF_MARGIN -- a target out of reach is
    // one it walks onto again -- and never twice within
    // MELEE_BACKOFF_COOLDOWN. The step is proposed to a clear spot (a wall
    // or a circle behind her: round the mob a little, either side), or
    // not at all.
    auto StepBackIntent(const CBattleEntity* PTarget) -> std::optional<Intent>;

    // The fight ring (formation_math.h RingSeats): every cardian on a mob
    // but the one it is fighting takes a seat around it -- the nearest
    // free one, kept for the fight -- and walks to it; as the mob's target
    // she has none, the front being wherever she stands. The seat sits
    // FightRadius out: RoamDistance, capped inside the mob's reach (the
    // step back's rule). A far seat is reached round the mob's side, never
    // through it.
    auto FightRadius(const CBattleEntity* PTarget) const -> float;
    auto TakeFightSeat(const CBattleEntity* PTarget) -> std::optional<pawn::Slot>;
    auto LiveFrame(const CBattleEntity* PTarget) const -> uint8; // the ring's rotation now: the mob's bearing to its target
    auto SeatPoint(const CBattleEntity* PTarget, pawn::Slot seat, uint8 frame) const -> position_t;
    auto SeatPoint(const CBattleEntity* PTarget, pawn::Slot seat) const -> position_t; // by the live frame
    auto SeatIntent(const CBattleEntity* PTarget, const position_t& seat, bool inReach) -> Intent; // the seat mover: stand on it, hop to it, keep the path, or path round the mob's side

    // The beat: how long she takes to act on a decision -- to set off on
    // a hunt, to draw with the party, to close when the hold ends, to step
    // back -- by her formation row (the Formation gambit): the lead at
    // once, the others REACTION_BEATS_* beats later, plus up to
    // REACTION_JITTER random beats. Safety moves never wait on it.
    auto ReactionBeat() const -> timer::duration;

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

    // The mob this pawn should join on, and why: the player's engaged
    // target first (gated by the swing/TrustEngageType convention), else
    // any pawn party member's living target -- how a hunter's pull
    // propagates -- else a mob that has chosen one of us
    struct PartyFight
    {
        CBattleEntity* target = nullptr;
        std::string    why;
    };
    auto PartyEngageTarget(CCharEntity* PPlayer) const -> PartyFight;

    // Waiting: the tick that holds her ground (WaitTick), the mob that has
    // come for her (SelfDefenceTarget), and the player's magic noted while
    // they are still here (NotePlayerMagic)
    void WaitTick(CCharEntity* PPlayer);
    auto SelfDefenceTarget() -> CMobEntity*;
    void NotePlayerMagic(const CCharEntity* PPlayer);

    // Everyone in this zone's party above the hunt thresholds and the
    // post-fight breather elapsed
    auto HuntBlocker(const CCharEntity* PPlayer) const -> std::string; // "" when the hunt may pull; otherwise what holds it

    // HuntBlocker without the distance rule: the player resting, a member
    // down or fighting. Judged again on the walk in, where the hunter is
    // meant to be away from the player
    auto PacingBlocker(const CCharEntity* PPlayer) const -> std::string;

    // The nearest idle, non-special mob in the difficulty band within
    // HUNT_RADIUS of the player. `skipped`, when given, collects what
    // was in the band but not pulled, and why (a few at most), for the
    // quiet hunt's line
    auto PickHuntTarget(const CCharEntity* PPlayer, std::string* skipped = nullptr) const -> CMobEntity*;

    // Home point with the player: a KO'd cardian whose player has died and
    // come back at their home point goes there too
    void WatchPlayerHomePoint();

    // A walk in on a mob, weapon away: her own pull (the hunt's pacing and
    // radius keep applying), the party's fight when it is farther than she
    // may draw from (dropped when the party moves on), or the player's
    // order (dropped only with the mob)
    enum class ApproachKind : uint8
    {
        Hunt,
        Join,
        Order
    };
    struct Approach
    {
        EntityId     target;
        ApproachKind kind = ApproachKind::Hunt;
    };

    // The one door into a fight (pawn_rules.h): the rules first; the draw
    // when they allow, said as `how`; the walk in, weapon away, when only
    // the distance or the draw's own wait stands in the way (m_Approach);
    // a refusal said once otherwise. True when she drew.
    auto Draw(CBattleEntity* PTarget, ApproachKind kind, std::string_view how, bool hold = false) -> bool;

    // The one writer of the mode: the exits it owns (a fight's draw
    // cooldown, seat and beats; a walk in's target) happen here, and the
    // change is said -- "Follow -> Fight: draws on X (with Jevyak)". A call
    // that changes nothing still says its reason (a new target in a fight).
    void Transition(Mode to, std::string_view why);

    // The mode she rests in when a fight or a walk in ends: Retreat while
    // the switch is up, Wait while she holds her ground, else Follow
    auto IdleMode() const -> Mode;

    // Why the server ended her fight, read from the rules on the target she
    // had: it died, she lost sight of it, another party claimed it
    auto ServerExitReason() -> std::string;
    auto EngageFactsFor(CBattleEntity* PTarget) -> cardian::rules::EngageFacts;
    void SayRefusal(const CBattleEntity* PTarget, const std::string& why);

    // Why she will neither draw on nor walk in on this target: the rules
    // (mayFight, past what a walk in cures), then the pull rule as the
    // fight asks it -- an idle target the party would not pull is refused
    // at the door too, so the door and the fight never disagree. "" when
    // she may.
    auto Refusal(CBattleEntity* PTarget, const cardian::rules::EngageFacts& facts) const -> std::string;

    // The pull rule as the fight and the walk in ask it: the pick's padded
    // circles, scanned around her now. "" when clean, else what makes it
    // unclean
    auto PullBlocker(const CMobEntity* PMob) const -> std::string;

    // The walk in on a mob, through the locomotion pass: the danger map,
    // the approach proposal, the vet, the step
    void WalkToward(CBattleEntity* PTarget);

    // The tick's danger map (RefreshDangers): the true circles, and the
    // planning circles padded by the clearance
    std::vector<pawn::danger::Danger>       m_Dangers;
    std::vector<cardian::formation::Circle> m_Padded;

    std::unique_ptr<pawn::CGambits> m_Gambits;
    bool                            m_BrainLoaded = false;

    timer::time_point                 m_LastRangedAttackTime;
    timer::time_point                 m_LastTravelDebugTime;
    timer::time_point                 m_TravelProgressTime;
    float                             m_TravelBestDist = 0.0f;
    xi::ZoneId                        m_TravelHopZone{};

    bool              m_Hunting    = false;
    bool              m_Retreat    = false;
    bool              m_Waiting     = false;
    bool              m_WaitOrdered = false;
    timer::time_point m_PlayerMagicSeen{ timer::time_point::min() }; // the player seen mid-warp or mid-teleport, so their vanishing reads as magic
    bool              m_HoldForPlayer = false; // drawn on the player's word: walking in with them, no closing until they strike

    // The mob she is walking to, weapon still away (Approach, above): she
    // commits the moment it is chosen and closes; only the draw waits, on
    // the rules and the re-engage timer
    std::optional<Approach> m_Approach;

    // The last refusal said, so a standing reason is not said every beat
    uint32      m_RefusedTarget = 0;
    std::string m_RefusedWhy;

    Mode m_Mode = Mode::Follow;

    // The draw cooldown's memory: when she last left a fight and who it
    // was with. Never fought means ready. m_LastFought is the same mob as
    // a handle, for the server's exit reason.
    timer::time_point       m_LeftFightAt{ timer::time_point::min() };
    uint32                  m_LastFoughtId = 0;
    std::optional<EntityId> m_LastFought;

    // The step back's rest clock: the target's id and spot as of the tick
    // it was last seen moving, and when she last stepped
    uint32            m_TargetRestId = 0;
    position_t        m_TargetRestPos{};
    timer::time_point m_TargetRestSince{ timer::time_point::min() };
    timer::time_point m_LastStepBackAt{ timer::time_point::min() };
    std::optional<timer::duration> m_TargetRestBeat; // the step back's beat, drawn once the rest is seen

    // The fight ring: her seat on the mob she fights, the way round to it,
    // and where the seat was when the path there was planned. A seat is
    // sticky: walking to it she follows the live ring (the mob's bearing
    // to its target), and once she has settled on it the ring's frame is
    // hers for the fight, so a hate swing that turns the mob does not
    // send her round its body to the same seat on the other side
    struct FightSeat
    {
        uint32     mob     = 0;
        pawn::Slot seat    = pawn::Slot::Follow;
        bool       settled = false;
        uint8      frame   = 0; // the ring's rotation she settled by
    };
    FightSeat  m_FightSeat;
    bool       m_SeatVia = false;
    position_t m_SeatDestination{};

    // The beats: a hunt's walk starts here; the party's draw, on this mob,
    // here; the hold's end was seen and she closes here
    timer::time_point m_SetOffAt{ timer::time_point::min() };
    uint32            m_EngageBeatMob = 0;
    timer::time_point m_EngageAt{ timer::time_point::min() };
    timer::time_point m_CloseAt{ timer::time_point::min() };
    timer::duration   m_HuntBeat{};                                // the hunt's beat, drawn at set-off; the draw takes it again
    timer::time_point m_DrawAt{ timer::time_point::min() };        // the hunt's draw, once the re-engage wait is served plus the beat
    std::optional<EntityId> m_Order;                               // the player's attack order, waiting its beat
    timer::time_point       m_OrderAt{ timer::time_point::min() };
    timer::time_point m_LastTidyTime;
    timer::time_point m_NextIdleEmoteTime;
    std::optional<std::pair<std::string, EntityId>> m_QueuedOrder;
    timer::time_point                               m_QueuedOrderDeadline;

    // The action itself, no queueing: "" when it fired, "recast", or why
    // not
    auto TryAction(unsigned kind, unsigned mode, unsigned id, EntityId target) -> std::string;
    timer::time_point m_LastHuntLogTime;
    timer::time_point m_LastSurfaceLogTime;
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
