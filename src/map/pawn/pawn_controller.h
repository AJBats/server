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

#include "cardian_link_messages.h"
#include "claim_board.h"
#include "engage_math.h"
#include "glance_math.h"
#include "herd_math.h"
#include "pawn.h"
#include "pawn_danger.h"
#include "pawn_gambits.h"
#include "pawn_rules.h"
#include "perimeter_math.h"
#include "stake_math.h"
#include "rest_math.h"
#include "warp_hold.h"

#include "ai/controllers/player_controller.h"
#include "data/enums/status_effect.h"

#include <array>
#include <chrono>
#include <deque>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace pawn::world
{
    struct TownOrder;
}

namespace pawn
{
    struct HuntRules;
}
class CBattleEntity;
class CCharEntity;
class CMobEntity;
class CSpell;
#include "common/types/position.h" // m_WalkPoint holds one

// The autonomous controller for pawn characters: CTrustController's physical
// layer (formation follow, engage-on-the-player's-swing, combat positioning,
// declumping, rest regen) rebuilt around party membership instead of the
// trust master/minion model, mounted on CPlayerController so the pawn keeps
// a real character's action surface. Decisions come from the pawn gambit
// interpreter over her rows (seeded once from her job's defaults), and the
// fights she takes from her Attack rows (the engage door, engage_math.h);
// an engaged pawn also auto-attacks via the stock battle engine.
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
    // word, waiting for their strike; Fight is a fight; Attend is a support
    // mage at the party's fight from the perimeter, weapon away (RESEARCH
    // §12.15); Maneuver is the player driving her himself, her gambits
    // off, until the finisher he chose fires (docs/maneuvers.md); Down is
    // KO'd. The
    // server's attack state is an input, not the mode: every tick the two
    // are reconciled, and a fight the server ended is a transition out of
    // Fight with the server's reason -- never a silent one.
    enum class Mode : uint8
    {
        Follow,
        Wait,
        Travel,
        Walk,
        Roam,
        Approach,
        Hold,
        Fight,
        Attend,
        Retreat,
        Maneuver,
        Down
    };
    static auto modeName(Mode mode) -> const char*;
    auto        CurrentMode() const -> Mode;

    // A world body (pawn::world): no party and nobody to follow. Her idle
    // mode is Roam, and the roam tick is hers alone (RoamTick)
    void SetWorld(bool on);
    auto IsWorld() const -> bool;
    // The party's anchor: the live player, else (a world body in a camp) her
    // camp's leader when she is not it; nobody for a solo body or the leader
    auto GetAnchor() const -> CCharEntity*;

    // The perimeter (RESEARCH §12.15). She attends the fight on this mob
    // instead of drawing on it when her rows offer the party spells and no
    // Attack row of hers claims the mob (engage_math.h attendsFight): from
    // the nearest safe spot outside the mob's TP reach and inside cure range
    // of the tank (AttendIntent). She may act offensively once the mob is
    // engaged. An Attack row that claims the mob makes her fight it, and so
    // "Foe: targeting ally", which claims nearly every fight, makes her a
    // melee mage. The player's own Attack on the mob (PlayersOrderOn) is his,
    // not her tactician's: she fights it
    auto AttendsFight(CBattleEntity* PTarget) const -> bool;
    // The player's own Attack, from her command window: the mob he named.
    // She draws on it and closes, whatever her role and her rows say, and
    // nothing of her tactician's (the attend, her own rest) takes it back
    // while it lives. It ends with that fight, and on his Disengage, a new
    // order, retreat or zoning. The party's engage chord is not his own
    // order to her and sets nothing here (RESEARCH §14.12 decision 16)
    auto PlayersOrderOn(const CBattleEntity* PTarget) const -> bool;
    auto HasPlayersOrder() const -> bool;
    // A target refused at the door is left alone for a while (HoldOff), so
    // a standing refusal (claimed, unclean) is not tried every beat
    auto HoldingOff(const CBattleEntity* PTarget) const -> bool;
    // The beat before she takes a fight: the reaction beat for a draw or a
    // walk in, none for a fight she attends, since she is not walking in
    auto JoinBeat(CBattleEntity* PTarget) const -> timer::duration;
    auto AttendedTarget() const -> CBattleEntity*;
    auto Attending(const CBattleEntity* PTarget) const -> bool;
    auto AttendedEngaged() const -> bool;
    // What her rows call "the mob" while she has no battle target: the mob
    // she attends, or the one she walks in on for the party (a hunt's walk
    // in is her own pull, not the party's fight yet)
    auto PartyFightTarget() const -> CBattleEntity*;

    // Action surface used by the gambit interpreter. Each faces the target
    // first (the player weapon-skill path refuses a target the character is
    // not facing), then runs the stock player validation: known spell or
    // ability, recasts, TP, ammo, facing.
    auto Cast(EntityId target, SpellID spellid) -> bool override;

    // A cast the tactician's conveyor assigned her (RESEARCH §12.12 item
    // 2): the conveyor's lock stands in for the party-already-casting rule,
    // so this goes straight to the player controller's cast
    auto CastAssigned(EntityId target, SpellID spellid) -> bool;
    // The cast itself, for both: a cast that begins drops the walk she was on
    auto CastAndStop(EntityId target, SpellID spellid) -> bool;
    auto RestAllowsAction() const -> bool;
    auto RestReadyIn(double now) const -> double;
    auto RestInterruptionCost() const -> double;
    auto PrepareRestAction(bool ordered = false) -> bool;
    void StandFromRest(std::string_view why);
    // True while the policy keeps her kneeling; defer routine positioning then.
    auto RestTick(bool stationary, bool townKneel = false, bool routinePosition = false) -> bool;
    // The player's rest order (ComposeRest): down until her HP and MP both
    // reach N%. Only the emergency cure stands her meanwhile; any order of
    // his, or her leaving his party, ends it first. byRow: her own plain
    // Rest row's, the same order at 100% (RESEARCH §17.13) -- danger and
    // the party's fight stand her up meanwhile, and she kneels again after
    void SetRestOrder(int percent, std::string_view why, bool byRow = false);
    void DropQueuedRest(std::string_view why); // a rest still queued for the release gives way to his later order
    void EndRestOrder(std::string_view why);
    auto WeaponSkill(EntityId target, uint16 wsid) -> bool override; // her rows' weapon skill; never tried beyond its reach (BoostOrWeaponSkill)
    auto Ability(EntityId target, uint16 abilityid) -> bool override;
    auto RangedAttack(EntityId target) -> bool override;

    // The human party member the pawn formation anchors on
    auto GetLivePlayer() const -> CCharEntity*;
    // One accepted zone change ends the player's current fight commitment.
    // It does not prevent the next idle tick answering a new threat.
    void PlayerZoning();
    // Out of any fight she was in or on her way to: the draw on its beat,
    // the walk in, the weapon-skill wait, the path, the weapon. One sequence
    // for the player zoning, a rest order and a landing beside him
    void StandDown(std::string_view why);

    // This pawn's index among the pawns in its party (formation order)
    auto GetPawnPartyPosition() const -> uint8;

    // Whoever the pawn's current battle target hates most
    auto GetTopEnmity() const -> CBattleEntity*;

    // Whether a foe is of a finder's kind, as the door reads it (FoeFacts
    // against the one she is with): how a Foe row with another action than
    // Attack names her fight
    auto FoeOfKind(cardian::engage::Finder finder, CBattleEntity* PFoe) const -> bool;

    auto Gambits() -> pawn::CGambits&;

    // Hunt mode: pull for the party while it is idle and healthy. The
    // party's strategy, not a gambit -- set by !pawnhunt until the strategy
    // channel exists (RESEARCH §8)
    void SetHunting(bool on);
    // What the game never writes for a body with no client, between her
    // despawns: her position, flagged for the persist sweep when she moved;
    // her HP and MP, every kHealthSaveEvery while they changed
    void NoteForSaving();
    auto IsHunting() const -> bool;
    void SetRetreat(bool on); // the "on me" switch: disengage now, engage nobody, avoid nothing, until cleared
    auto IsRetreating() const -> bool;
    // Her tactician (tactician_line.h) runs: her rows offer it a tool (a
    // marked row that is on), her gambits are on, and a tactician watches
    // her scope
    auto TacticianRuns() const -> bool;
    // Her tactician's recovery is due: a casting mage's MP, as her rest
    // policy says. A tank's never is: she leaves no fight to rest
    auto RecoveryDue() const -> bool;
    // The stake (RESEARCH §12.16): the party's place whenever it stands,
    // pushed by the orders (pawn::applyOrdersTo); hers while she stands in
    // its zone and no retreat is called (Staked). Staked, she keeps to it:
    // no trek after the player. Zoning ends the current fight commitment;
    // new fights are what comes within the leash of the stake.
    void SetStake(std::optional<pawn::Stake> stake);

    auto Staked() const -> bool;
    // She follows the player through zone lines: not waiting, not staked
    auto Treks() const -> bool;

    // The steer tick's step (pawn/view.h, every kSteerPeriodMs; the pawn
    // module calls it): a fraction of a logic tick's step at her own speed,
    // so a steered walk moves at frame rate, and re-paths as the ring moves
    void WalkStep();

    // A maneuver (docs/maneuvers.md): a bounded episode of live control
    // over this one cardian by the player looking through her. Her gambits
    // go off, nothing of the controller's moves her but his walk order, and
    // it ends the moment an order of his leaves her (DoAction, or the
    // queued order firing): he is handed back and she carries it out. His
    // camera leaving her, her leaving his party, her death and his cancel
    // end it too; every end restores her gambit switch to what it was. One
    // maneuver per player at a time. Begin answers CL_S_OK or why not (the
    // Link's outcomes, cardian_link_protocol.h); other receives the cardian he
    // drives already, on CL_S_ONE_MANEUVER.
    auto BeginManeuver(CCharEntity* PBy, uint32* other = nullptr) -> uint16;
    void EndManeuver(std::string_view why);
    auto InManeuver() const -> bool;
    // A paused maneuver (held, docs/maneuvers.md): the ring lays a route
    // while she stands, and the command given from it is her queued order.
    // Composed, the maneuver no longer needs his eye on her: at the release
    // she walks the route, the order fires at its end, and that is the
    // maneuver's end. ComposeMove is the route with no order: end at its
    // end, holding position there if `wait`. Answers CL_S_OK or why not.
    auto ComposeMove(bool wait) -> uint16;
    // The maneuver's "Rest until N%", her queued order either way: live,
    // composed at once where she stands, his camera handed back; paused,
    // after the route if one is laid. The maneuver lasts through the rest
    // until HP and MP both reach N% -- her queued order, and cancelled as
    // one -- gambits off throughout (the user, 2026-09-23). Answers CL_S_OK or why not
    auto ComposeRest(int percent) -> uint16;
    void MarkComposed(std::string_view what); // the maneuver's order is given, to play out without him: his live slot frees for the next cardian
    void TellManeuver(uint8 state) const;     // his addon hears the change (MANEUVER_STATE, CL_MS_*)
    auto ManeuverComposed() const -> bool;
    auto ManeuverBy() const -> uint32; // whose maneuver she is on, 0 for none
    // Her gambit master switch as her own setting: while a maneuver holds
    // her gambits off, the value its end restores. Seeding or loading her
    // rows sets it through SetOwnMaster, so a reload during a maneuver
    // never turns her gambits on under it; a saved set records OwnMaster
    auto OwnMaster() const -> bool;
    void SetOwnMaster(bool on);

    // Hold position / follow me. Holding, she has nowhere to go by order: no
    // following, hunting or travel, and no step of her own at all (Move),
    // in a fight or out of one -- she fights and casts from where she
    // stands, and a weapon skill out of her reach is refused rather than
    // walked to. An ordered hold lasts until told otherwise. The automatic
    // hold (warp_hold.h) is a warp's: parted from the player by one, either
    // way, she holds where she is (HoldForWarp, unless she holds on his order
    // already), and it lifts by itself once he is in her zone again. Any
    // other hold or follow given here is not the automatic one.
    void SetWaiting(bool on, bool ordered, std::string_view why = {}); // `why` is the transition's reason; empty takes a plain one
    auto IsWaiting() const -> bool;
    void HoldForWarp(std::string_view why);
    void Carried(); // carried off by a warp or a teleport: she holds where she lands until the player is in her zone with her
    void ArriveWith(const position_t& landing); // set down beside the player by an event (pawn::landWithPlayer): she stands until he is seen there
    void EngageOn(CMobEntity* PMob);        // the player's order: fight this, after her beat (FireOrderedEngage)
    void ShareSignet(CCharEntity* PPlayer); // the gate guard's Signet, taken with the player for its remaining time

    // Her bag kept stacked (pawn::items::tidyStacks), a quiet sweep every
    // 15 s between fights: a drop lands unstacked like anything else
    void TidyBag();

    // Her head turns to PAt (nullptr: straight ahead): the face-target
    // index in the character update, which the client turns a player's
    // head with -- players set it with every position packet, she never
    // sends one. Her body's heading is the update's other field, and this
    // leaves it alone. An update goes out only when it changes.
    void HeadLook(const CBaseEntity* PAt);
    // Her eyes while she is idle, held or left behind (ROADMAP G item 1,
    // #249; glance_math.h): in a fight anywhere in her party, on its mob --
    // the one she attends, else the player's -- or ahead; out of one, on
    // whoever she emotes at (or who emoted at her) for its seconds, on the
    // player now and then for a few seconds, and otherwise ahead. Never
    // through an action, whose own target holds her head. After a fight, or
    // any stretch away from the idle tick (a fight of her own, a walk he
    // ordered, a maneuver), the glances start afresh and an emote already
    // due waits a short stagger, so nothing due meanwhile goes the moment
    // she is back, the whole party at once
    void IdleLook(const CCharEntity* PPlayer);
    // Out of the idle tick -- a walk he ordered, a maneuver, a trek -- her
    // head is ahead, not left on whatever she last looked at
    void LookAhead();
    // A body of the world on her own (RoamTick, a town seat): her head on
    // whoever she emoted at while it lasts, else ahead
    void RoamLook();
    // A fight anywhere in her party (glance_math.h inFight): her own --
    // engaged, holding, attending, walking in on a mob, a retreat -- a party
    // member engaged, or a mob on the party, engaged on a member (or a
    // member's pet) or claimed by one, within the hunt leash of her. Scanned
    // once a tick
    auto PartyInFight() const -> bool;

    // Mid-action: casting, readying a weapon skill or ability, or shooting
    auto Acting() const -> bool;
    // The pacer: the server's own test for a new action -- she can act (the
    // player controller's canAct: 2.5 s after her last spell finished) and
    // her state lets go (an ability once it has landed, not its animation;
    // a spell, a weapon skill, a shot or an item not before it ends). Every
    // action a cardian sends waits on it -- her think, his queued orders,
    // the weapon skill held behind a Boost, her tactician's calls -- so it
    // fires on the first tick the server would take it, never refused for
    // coming too soon and never cutting into one under way
    auto ReadyToAct() -> bool;
    // Something on her the ability state refuses every job ability for:
    // Amnesia, or Impairment of abilities
    auto AbilitiesShutOut() const -> bool;
    // Sneak Attack is spent by the next blow and lands only from behind, so
    // it goes right before her weapon skill: her marked Sneak Attack row
    // offers it (the tactician's tool, RESEARCH §17.13), and it is hers to
    // use now
    auto SneakAttackReady() const -> bool;
    // Sneak Attack can go now, before her weapon skill or naked: ready, she
    // is engaged on this mob, the mob faces someone else (its back away
    // from her side), and no give-up of hers rests on it (m_SneakRest)
    auto SneakAttackNow(const CBattleEntity* PTarget) -> bool;
    // A weapon skill Sneak Attack works with: one whose script goes through
    // the server's physical path (tactician_line.h scriptTakesSneakAttack),
    // read once per weapon skill. Any other spends it all the same
    static auto TakesSneakAttack(uint16 wsid) -> bool;
    // The player's own Berserk or Defender has just fired, by its ability
    // id: his order (TryAction), or a plain row of his (CGambits::Execute)
    void NoteOrderedStance(uint16 ability);
    // Whether the Berserk or Defender on her now is the one the player's
    // own fired (NoteOrderedStance; tactician_line.h isOrderedUse): her
    // tactician's stance never takes it off. That one use only: once it is
    // over, a later one of the tactician's is the tactician's again
    auto PlayersBuff(uint16 ability, xi::StatusEffect effect) -> bool;

    // An emote now and then while standing about (glance_math.h) --
    // motion only, no text, to everyone in range: a fidget of her own, or
    // one aimed at the player or at another party member near her, whom she
    // looks at while she does it; a cardian she emotes at looks back. Its
    // clock runs from her last emote. One that comes due waits out a fight
    // anywhere in her party, an action and a kneel, and is let go while she
    // walks (`stands` false) or the player is far off.
    void IdleEmote(const CCharEntity* PPlayer, bool stands);

    // May she draw on this target yet? The cooldown is set when she LEAVES
    // a fight, not by her last swing: a cardian fresh from rest draws at
    // once, one just off a kill waits. The mob she just left costs the
    // longer wait (her weapon delay, the anti-exploit), anything else the
    // shorter one (cardian.REENGAGE_SWITCH_DELAY).
    auto CanDrawOn(CBattleEntity* PTarget) -> bool;

    // The command window: one action now, on the target the player picked.
    // `key` is the vocabulary's action key, kind:mode:id -- the concrete
    // ones only: a spell (2:2:id), an ability (3:2:id), a weapon skill
    // (4:2:id), the ranged attack (1:0:0), an item she carries (item:<id>);
    // the "best of" entries are the gambit engine's -- or "attack", her order
    // to fight the mob picked (AttackOrder), or "disengage" (DisengageOrder).
    // CL_S_OK when it fired or joined her line (the Link's outcomes), else
    // why not; never refused for its timing, only CL_S_QUEUE_FULL when four wait.
    auto DoAction(const std::string& key, CBattleEntity* PTarget) -> uint16;
    // His Rescue (pawn::rescue) as an order in her line, "rescue" on him: it
    // waits its turn, a pause and the rescue's cooldown as an order waits out
    // its recast, then brings her to his side. CL_S_OK when it joined her line
    auto QueueRescue(CCharEntity* PPlayer) -> uint16;

    // An order she cannot start now -- while she acts, while the spell is on
    // recast, out of reach, the game paused -- waits in her line and fires the
    // moment it can, however long that is: nothing is refused or let go for
    // its timing (the user, 2026-10-05). Orders behind it wait their turn,
    // kQueueDepth deep in all.
    void FireQueuedOrder();
    auto HasQueuedOrder() const -> bool
    {
        return m_QueuedOrder.has_value();
    }
    // Her line holds her own AI back -- her gambits stand aside, and her Cure
    // readiness with them -- while its first waits on anything but its own
    // recast: an order waiting out a long recast (Raise pressed twice) leaves
    // her free to cure meanwhile, and goes the moment the recast is over
    auto QueuedOrderHoldsHer() const -> bool;

    // The queued order as the command window's queue line shows it: the Link's
    // QUEUE, her action and its target's index (CL_AK_NONE with none). The addon
    // words it from the list it holds. It is told whenever this changes, and the
    // player can take the order back.
    auto QueueLine() const -> cl_queue;
    auto CancelQueuedOrder() -> bool;
    // Her whole line dropped: the orders behind the first, then the first
    auto ClearQueuedOrders(std::string_view why, uint32 formerOwner = 0) -> bool;
    // The player's cancel on her queue line with nothing queued: a rest she
    // is on -- her own, or his order's -- called off. She stands, and her own
    // kneels are held off for pawn.REST_CALL_OFF_SECONDS, so she stays with
    // the party. False when she is not resting
    auto CallOffRest() -> bool;
    // The rest her queue line shows (m_RestLineKind) brought up to date, and
    // told to her player when it changed with nothing queued
    void UpdateRestLine();

    // The enchanted-item lane (OPEN_ISSUES #297, pawn_enchant.cpp): an
    // enchanted piece -- an experience ring -- used from her inventory is put
    // on at once, waits out its delay while her gambits and his orders go on,
    // is used the moment she is free, ahead of her line and without a place
    // in it, and the piece it replaced goes back on. One at a time; shown on
    // her queue line above the rest (QUEUE's lane)
    auto StartEnchant(uint8 location, uint8 slot) -> uint16;
    // Ended early: the replaced piece back on at once. A use already under way
    // is the game's to finish: its charge is spent and its effect lands
    void EndEnchant(std::string_view why);
    // His cancel, once nothing waits in her line and no rest is on it: the
    // lane taken back, unless its use is under way
    auto CancelEnchant() -> bool;

    // An order behind whatever waits in her line, or first when nothing does:
    // CL_S_QUEUE_FULL when kQueueDepth wait already
    auto JoinLine(const std::string& key, EntityId target) -> uint16;

    // An order waits on the player's behalf: it ends with the tie to him, as a trek
    // does (pawn::leftParty). Says whether one was queued. `formerOwner` is told her
    // queue line is empty when she no longer has an orders owner to tell.
    auto DropQueuedOrder(std::string_view why, uint32 formerOwner = 0) -> bool;
    // Out of his party, his addon's copy of her line emptied (DropQueuedOrder)
    void TellFormerOwner(uint32 formerOwner) const;

    // The game told her something (pawn::noteBattleMessage). An order that has just
    // started and this on its heels is the game refusing it -- out of range, no line
    // of sight, its own script's word -- which the player hears as a note. Her
    // auto-attack's own "target out of range" is not, after any order but a pet's.
    void ToldAfterOrder(uint16 message, const std::string& said);

    // The attack order, fired once her beat is served: the front row draws
    // first, the back line a touch later
    void FireOrderedEngage();
    auto HatedByAnyMob() const -> bool;     // some mob nearby holds enmity on her

    // The behaviour layer (M3.85): what the gambit rows assert this think,
    // by pawn::Behavior. Cleared at the start of every think; the first row,
    // top down in the running order (the world's layer first in the wild,
    // gambit_layers.h), to speak for a behaviour wins; a switch no row
    // speaks for is off, a parameter takes its default. Rows are the only
    // source.
    void ClearGambitBehaviors();
    void SetGambitBehavior(uint16 behavior, uint16 arg);
    auto Behavior(pawn::Behavior behavior) const -> std::optional<uint16>;

    auto FormationSlot() const -> pawn::Slot;

    // Her body in the herd round a mob (herd_math.h, ROADMAP A item 9),
    // for the herd pass to read: the bearing she stands on, or the one she
    // is walking to, and whether the pass may move her. nullopt when she
    // is not in its melee ring
    auto HerdBody(const CBattleEntity* PMob) const -> std::optional<cardian::herd::Body>;
    auto IsAvoidingAggro() const -> bool;  // keep out of every nearby mob's detection circle (M3.87)
    auto IsAvoidingLinks() const -> bool;  // keep clear of the idle kin of every mob fighting her (ROADMAP K6)
    auto IsAvoiding() const -> bool;       // either: the danger map is hers to keep to
    auto RestsWithPlayer() const -> bool;
    auto RestsByRow() const -> bool;
    auto RestRowDue() const -> bool; // her plain Rest row speaks, she is short, and no rest order is on
    auto HomePointsWithPlayer() const -> bool;
    auto EatsWithPlayer() const -> bool; // her "Self -> Eat with the player" row speaks (RESEARCH §19)

    static constexpr float RoamDistance     = 3.0f;
    static constexpr float LockOnSlack      = 2.0f; // lock-on holds this far beyond melee reach, so a step out of reach does not drop it
    static constexpr float WarpDistance     = 30.0f;
    static constexpr float TransferDistance = 3.0f;
    static constexpr float CrossingSlack    = 40.0f;

private:
    auto DoCombatTick(timer::time_point tick) -> Task<void>;
    auto DoRoamTick(timer::time_point tick) -> Task<void>;

    // A travel order's zone, or the player in another zone and her party:
    // walk the zone graph toward it, requesting a transfer at each zone line.
    void TravelTick();

    // A walk order (pawn::walkOrderOf): the logic tick's half -- the order's
    // bookkeeping (WalkOrderTick) and, with no steer timer, her step
    void WalkTick();
    void WalkOrderTick(timer::time_point now);
    std::optional<position_t> m_WalkPoint; // the point the current path was made for
    timer::time_point         m_LastWalkStep{}; // the steer tick's last step, for its elapsed-time scale

    // The maneuver's books (BeginManeuver): who drives her, and her gambit
    // switch before it
    uint32 m_ManeuverBy          = 0;
    bool   m_ManeuverPriorMaster = true;
    bool   m_ManeuverComposed    = false; // its order given (held: after the route, at the release; a rest, live too): it plays out without his eye
    bool   m_ManeuverResting     = false; // its rest is under way: kept in step with the queue by SetQueuedOrder
    void   ManeuverTick();
    void   NoteOrderFired(); // an order of his left her: a maneuver ends here
    auto   RouteWalked() const -> bool; // no walk order, or its route walked and its point reached
    auto   OrderReach(unsigned kind, unsigned id, const CBattleEntity* PTarget) const -> float; // how close an order needs her, in yalms
    // The order's walk in: an order whose target is out of its reach is a
    // walk in first and the action second -- "run to and use", as the gambit
    // engine's own approach is (the user, 2026-09-22) -- and the walk is an
    // intent the tick's mover takes like any other (Move), so one rule serves
    // a plain order, a maneuver's and the engine's. OrderOutOfReach says
    // whether the queued order needs one, and to whom; OrderApproach is the
    // intent while it does. The grace waits through it, up to kOrderApproachMax
    auto   OrderOutOfReach() const -> std::optional<std::pair<CBattleEntity*, float>>;
    bool              m_OrderApproaching = false;
    timer::time_point m_OrderApproachSince{};
    timer::time_point m_DoorSaidAt{}; // the fight door's debug line, once a second
    struct WalkStats
    {
        timer::time_point         since{};
        uint32                    steps   = 0;
        float                     moved   = 0.0f;
        std::chrono::microseconds elapsed = std::chrono::microseconds(0);
        std::chrono::microseconds lost    = std::chrono::microseconds(0);
    } m_WalkStats; // the walk's five-second accounting, in the map log

    // The lead holds a point ahead of the player; everyone else holds a
    // seat on the ring around them. RingSlot is a Formation row's seat, or
    // the silent one by job over the party's cardians in this zone
    // (formation_math.h assignSlots); SeatOf places it from the
    // FORMATION_FLANK_* / FOLLOW_* / REAR_DISTANCE settings.
    struct Place;
    auto LeadPoint(const Place& place, const CCharEntity* PPlayer) -> position_t;
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
    static auto PlayerAnchor(const CCharEntity* PPlayer, float predictScale) -> Anchor;

    // The place (RESEARCH §12.16): where the party is -- the origin of the
    // formation, the leash, the stand-down and the warp. Two things stand
    // behind it: the player, streamed and predicted as above, and a stake,
    // fixed with its heading. The readers take the interface and never
    // learn which; the person she is with (GetAnchor) stays the player.
    struct Place
    {
        virtual ~Place()                                        = default;
        virtual auto anchor(float predictScale) const -> Anchor = 0; // where the formation aims
        virtual auto position() const -> position_t             = 0; // where it is now
        virtual auto name() const -> std::string                = 0;
        virtual auto fixed() const -> bool                      = 0; // never moves: a seat left after a fight is kept
        // The backline: the point `radius` behind the place's heading (its
        // 6 o'clock), where the mages belong at a fixed place; nothing at
        // a place that moves, where the nearest spot serves
        virtual auto behind(float radius) const -> std::optional<position_t> = 0;
    };
    struct PlayerPlace final : Place
    {
        const CCharEntity* PPlayer = nullptr;
        auto               anchor(float predictScale) const -> Anchor override;
        auto               position() const -> position_t override;
        auto               name() const -> std::string override;
        auto               fixed() const -> bool override;
        auto               behind(float radius) const -> std::optional<position_t> override;
    };
    struct StakePlace final : Place
    {
        pawn::Stake stake;
        auto        anchor(float predictScale) const -> Anchor override;
        auto        position() const -> position_t override;
        auto        name() const -> std::string override;
        auto        fixed() const -> bool override;
        auto        behind(float radius) const -> std::optional<position_t> override;
    };
    PlayerPlace m_PlayerPlace;
    StakePlace  m_StakePlace;
    // The tick's place: the stake while she is staked (which retreat
    // lifts), else the player; nobody's when there is neither
    auto CurrentPlace(const CCharEntity* PPlayer) -> const Place*;

    // A formation slot: `distance` yalms from the anchor at `angle` radians
    // off the player's facing (0 = ahead, pi = behind), held across the
    // client's coarse updates: re-aimed every tick while the player moves,
    // only past FORMATION_DEADBAND while they stand. `held`/`hasHeld` are
    // the caller's memory of the slot.
    // A held formation point: the point she holds, as the world allows,
    // and the raw projection it came from, which the deadband is judged
    // on -- so a point brought in by a wall stays put until the player
    // turns away from the wall
    struct HeldPoint
    {
        position_t point{};
        position_t raw{};
        bool       has = false;
    };
    auto FormationPoint(const Anchor& anchor, float offset, float angle, HeldPoint& held) -> position_t;

    // The formation point the world allows: the ray from the player to the
    // projected point clipped where the mesh ends (a wall, a cliff's edge),
    // then priced by the walk from where she stands; a walk not worth it
    // (worthTheWalk) brings the point in toward the
    // player a third at a time, so a lead point across a wall never sends
    // her round the maze
    auto ReachableFormationPoint(const Anchor& anchor, float offset, float angle) -> position_t;

    // The navmesh's walk from where she stands to a point, in yalms; none
    // when there is no path (or only a partial one). No mesh: the straight
    // line.
    auto WalkLength(const position_t& to) const -> std::optional<float>;

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

    // Movers propose an intent. Move applies spell positioning; Walk checks
    // avoidance, executes movement, and maintains combat facing.
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
        bool                 seat       = false;   // a walk to a seat (her herd spot, the mob's back, the camp tank's spot): the mover keeps its path
        bool                 herdSpot   = false;   // a walk to her herd spot: failing it pins her where she stands (HerdPin)
        bool                 comesIn    = false;   // the mob she attends is coming in on her: a move no rest puts off
        std::optional<position_t> rearBoundary;    // normal positioning stays behind this frontline; avoidance overrides
        std::optional<position_t> fallback;        // Path: retry toward this target with no stop-short, vetted again
        // The claim board (claim_board.h): what a spot that gives way to
        // another member's claim keeps as it slides
        std::optional<position_t> slideRound;      // the centre she keeps her distance from (the attended mob, a spell's target); none: the party's place
        std::optional<position_t> keepTo;          // and a point she keeps within keepWithin of (the tank she cures)
        float                     keepWithin = 0.0f;
        bool                      holdsSpot  = false; // Stand: she was placed where she stands (the crescent, the camp's backline): crowded, she gives way as a walk would
        bool                      claimed    = false; // the board has had its say on this point: the walker does not ask again
    };
    auto OrderApproach() -> std::optional<Intent>; // the queued order's walk in, while one is on (see OrderOutOfReach)

    // The tick's danger map, scanned once before the movers run so every
    // one of them can ask IsClear while choosing, and the vet sees the
    // same circles. PIgnore is the party's own mob, never a danger.
    void RefreshDangers(const CBattleEntity* PIgnore);
    auto IsClear(float x, float z) const -> bool; // the walk to the spot ends outside every padded circle that matters to it
    auto InsideDanger() const -> bool;      // she stands inside a true circle whose mob sees her (or an ambusher's)

    // The perimeter's movers (RESEARCH §12.15): a mob's TP reach off its
    // live skill list, every hostile move, read once per list per mob
    // (ReachOf); her mover while she attends -- the crescent outside the
    // ring the reach makes and inside cure range of the tank: out of it she
    // walks to its nearest point, in it she holds where she stands, and
    // when it is empty she stays in at cure range and says so once per
    // fight (AttendIntent); the cure's range from the spell table
    // (CastRange). Attend is the door's exit for her (Attend), left with
    // what became of the mob (AttendExitReason).
    auto ReachOf(CMobEntity* PMob) -> cardian::perimeter::Reach;
    auto AttendIntent(CMobEntity* PMob, const Place* place) -> Intent;
    auto CampAttendIntent(CMobEntity* PMob, const Place& place, const CBattleEntity* PTank) -> Intent;
    // The camp's backline search (CampAttendIntent's): the best spot behind
    // the flag for a mob at `mob` with its tank at `tank` and AoE out to
    // `ring`. The fellow mages are kept clear of by the claim board, as
    // every mover's spot is (ClaimSpot). `geometry` names the picture for
    // the log; nothing is said when it is empty
    auto CampSpot(const Place& place, const position_t& mob, const position_t& tank, float ring, std::string_view geometry) -> Intent;
    // Between pulls at a camp, an attending mage waits -- and kneels --
    // where she expects to attend the next fight (the user, 2026-10-03):
    // the backline for a pull landing at the flag, planned once a camp
    // with the ring last seen there, and after a fight the spot she
    // attended from; a plan another member has claimed slides off it
    auto CampWaitIntent(const Place& place) -> Intent;
    auto WaitsAtCampSpot() const -> bool; // she attends the camp's fights: a mage whose rows take none
    std::optional<position_t> m_CampWaitPoint; // her planned spot at this camp; reset when the camp is set again
    std::optional<position_t> m_CampWaitSlidTo; // where the claim board last slid the plan: walking to it, she arrives as close as a slide asks
    float                     m_CampWaitBest = 0.0f; // her nearest to the spot so far, and when: a walk that gains nothing ends
    timer::time_point         m_CampWaitBestAt{};
    float                     m_CampRing   = 0.0f; // the ring of the last mob she attended at a camp; 0: none yet
    float                     m_AttendRing = 0.0f; // the ring of the mob she attends, as her mover last measured it

public:
    // Her claims on the party's claim board (claim_board.h): where the
    // walker is taking her while she walks, else where she stands -- and,
    // waiting at a camp, the spot she plans to wait on while her rest puts
    // the walk there off
    auto SpotClaims() const -> std::vector<position_t>;

private:
    // The claim board (claim_board.h, RESEARCH §12.15): no two party
    // members aim for one spot. The claims of the cardians before her in
    // the party order, in that order: those she gives way to
    struct BoardClaim
    {
        const CCharEntity*     PMember = nullptr;
        position_t             at{};
    };
    auto ClaimsBefore() const -> std::vector<BoardClaim>;
    // The rule for one spot she proposes: crowded by a claim before hers,
    // it slides to the nearest clear point that keeps the intent's purpose
    // -- round slideRound (else the party's place) at the spot's distance,
    // within keepWithin of keepTo, behind a rear boundary -- on the mesh and
    // out of the danger map. Nothing in `to` when nothing clear is near:
    // the spot stands
    struct GaveWay
    {
        const CCharEntity*        PBy = nullptr; // the member whose claim crowds the spot; none: the spot is hers
        position_t                at{};          // that claim
        std::optional<position_t> to;
    };
    auto GiveWay(const position_t& spot, const Intent& intent) -> GaveWay;
    // The walker's pass through the board: a walk to a point (Path) that
    // is crowded -- or the spot of a stand she was placed on -- gives way,
    // said in the map log when it changes. Not the fight's own movers (a
    // seat, a walk in on a mob, the player's order), a hop, nor the
    // formation's followers, whose seats are spaced already: they are
    // claims on the board, never slid
    void ClaimSpot(Intent& intent);
    // This tick's verdicts (GiveWay), each with what it was asked on: the
    // spot, where she stood, the purpose and the board
    struct GiveWayMemo
    {
        std::array<float, 12>               asked{};
        std::vector<cardian::claims::Point> board;
        GaveWay                             verdict;
    };
    timer::time_point        m_GiveWayTick{};
    std::vector<GiveWayMemo> m_GiveWayMemo;
    std::optional<position_t> m_WalkTo; // where the walker is taking her: her claim while she walks
    timer::time_point         m_WalkToAt{};
    uint32                    m_GaveWayTo = 0; // the slide last said: to whom, where to, or kept
    position_t                m_GaveWayAt{};
    bool                      m_GaveWayKept = false;
    auto HoldNow() const -> cardian::hold::Hold; // her hold as warp_hold.h's rules read it
    auto RearCampRoute(const position_t& point, const position_t& camp) const -> std::optional<std::vector<pathpoint_t>>;
    auto CastRange() const -> float;
    void Attend(CBattleEntity* PTarget, std::string_view how);
    auto AttendExitReason() -> std::string;

    // The walker. Returns the vet's action, or nothing when the tick was
    // spent on a warp.
    auto Move(Intent intent) -> std::optional<AvoidAction>;
    auto Walk(Intent intent) -> std::optional<AvoidAction>;

    // The formation mover, roaming or holding for the player's strike:
    // where this pawn belongs (the lead's point ahead of the place, or a
    // chain slot). PStandOff is a mob the party is holding on: no point is
    // placed within its reach plus FORMATION_STANDOFF (a point aimed past
    // it comes round to the place's side). PPlayer, when here, is the
    // formation debug's subject only.
    auto FormationIntent(const Place& place, const CCharEntity* PPlayer, const CBattleEntity* PStandOff) -> Intent;
    // Set down beside the player (ArriveWith), she stays put until the place
    // she follows reads him at the landing, or five seconds pass: his
    // own position reaches the server a moment after the move
    auto AwaitsArrival(const Place& place) -> bool;

    // The tank's tow at a stake (RESEARCH §12.16): a cardian with the Tank
    // role, staked, receives the party's mob while her rows Provoke it.
    // Arrival or a stalled pull's grace period releases her to melee;
    // with hate she tows unless the mob has stopped ahead of the flag,
    // within 20 yalms. Waiting, she stands one mob's reach past its
    // landing point, just ahead of the flag (stake_math.h towPoint),
    // so the mob stops in front of camp; once it is there she
    // takes the stake's 3 o'clock on the mob, at her reach, and it turns
    // to face her without moving. Both through the seat mover.
    auto TowsAtStake() const -> bool;
    auto CampReceive(const CBattleEntity* PTarget) -> cardian::stake::ReceiveAction;
    auto ResumeCampReceive() -> bool;
    auto TowIntent(CBattleEntity* PTarget) -> Intent;

    // The courtesy (local_planner.h): this tick's step toward `point`,
    // planned over a one-yalm grid round her against the player's body
    // and wake, where the straight walk would cut through them; the point
    // itself otherwise. Under every path the walker makes (PathToward).
    // pawn.COURTESY_* size and price the field; BODY_COST 0 is off.
    auto CourtesyStep(const position_t& point) -> position_t;

    // The walk in on a mob: to within RoamDistance of it
    auto ApproachIntent(const CBattleEntity* PTarget) const -> Intent;

    // An avoidance move too short for the planner, which refuses a hop under
    // a yalm and plans nothing: such a move steps straight at its point when
    // the point is on the mesh
    auto IsShortHop(const position_t& point, float followMax) const -> bool;

    // An avoidance move the planner could not path: one line a second, so a
    // cardian standing still in Escape, Hold or Detour names its cause
    void NotePathFailure(AvoidAction action, const position_t& point, float away);

    // The step back, off unless MELEE_STEP_BACK (the herd keeps her
    // distance now): a target that has settled on her toes (a mob walks
    // onto its target's exact coordinates) is given room. Once it has
    // stood still for MELEE_BACKOFF_DELAY, a cardian nearer it than
    // FightClearance steps straight back to RoamDistance, capped at
    // its melee reach less MELEE_BACKOFF_MARGIN -- a target out of reach is
    // one it walks onto again -- and never twice within
    // MELEE_BACKOFF_COOLDOWN. The step is proposed to a clear spot (a wall
    // or a circle behind her: round the mob a little, either side), or
    // not at all. Camp positioning passes positioned=false until arrival,
    // resetting the settle wait without changing the movement/cooldown.
    auto StepBackIntent(const CBattleEntity* PTarget, bool positioned = true) -> std::optional<Intent>;

    // The herd (herd_math.h, ROADMAP A item 9): every melee cardian on a
    // mob stands at a bearing from it in world terms, so the mob turning
    // moves nobody. Mobs tow: in reach she stands; out of reach she closes
    // from where she is. One pass per mob per tick (HerdBearing) spaces
    // the party's melee round it -- order kept, the least movement, the
    // ring evened out a step every HERD_BEAT -- round the fixed bodies:
    // the Tank, the player, a Thief on her Sneak Attack walk, a cardian
    // holding her position. Her spot sits FightRadius out: RoamDistance,
    // capped inside the mob's reach.
    auto FightRadius(const CBattleEntity* PTarget) const -> float;
    auto FightClearance(const CBattleEntity* PTarget) const -> float;
    auto HerdFixed(const CBattleEntity* PMob) const -> bool;                   // the pass never moves her
    auto HerdBearing(const CBattleEntity* PMob) -> float;                     // the herd pass's bearing for her this tick
    auto HerdPoint(const CBattleEntity* PMob, float bearing) const -> position_t; // her spot at a bearing
    void NoteHerd(const CBattleEntity* PMob, float bearing, bool inReach, const position_t& point);
    void HerdPin(std::string_view why);                                         // a bad spot: she holds where she stands a while
    auto HerdPinned(const CBattleEntity* PMob) const -> bool;                   // pinned on this mob now
    auto HerdAvoids(const CBattleEntity* PMob, float bearing) -> bool;          // a bearing near the spot that failed her lately: she holds instead
    auto HerdDeadband() const -> float;                                         // how far off her spot she stands: HERD_MOVE_MIN to set off, the arrival once walking
    static constexpr float kSpotArrive = 0.3f;                                  // the spot mover's arrival
    auto SeatIntent(const CBattleEntity* PTarget, const position_t& seat, bool inReach, bool campRoute = false, bool ownSeat = true) -> Intent; // the spot mover: stand on it, hop to it, keep the path, or path round the mob's side; not her own herd spot (the walk for Sneak Attack): her herd bookkeeping left alone

    // The beat: how long she takes to act on a decision -- to set off on
    // a hunt, to draw with the party, to close when the hold ends, to step
    // back -- by her formation row (the Formation gambit): the lead at
    // once, the others REACTION_BEATS_* beats later, plus up to
    // REACTION_JITTER random beats. Safety moves never wait on it.
    auto ReactionBeat() const -> timer::duration;

    // Navmesh-path toward a point, healing off-mesh endpoints: an off-mesh
    // destination is snapped to the nearest valid point, and an off-mesh
    // owner is snapped back onto the mesh. Zero closeTo uses PathTo, otherwise
    // PathAround supplies the stop-short distance. Never uses raw stepping.
    auto PathToward(const position_t& point, float closeTo, const position_t* rearBoundary = nullptr) -> bool;

    // Her body turns to the target of an action as it fires, a foe only:
    // an ally's action turns her head alone (ROADMAP G, #249)
    void FaceTarget(EntityId target) const;

    // Seed her gambit rows once, on her first living tick (pawn::loadBrain);
    // a job change leaves them as they are
    void CheckBrain();

    // Another party member is already casting something that makes this
    // cast redundant (same buff family, a cure on the same healthy target...)
    auto PartyAlreadyCasting(CSpell* PSpell, const CBattleEntity* PTarget) const -> bool;

    // The engage door (ROADMAP K3), her engine's (CGambits PartyFightScan,
    // EngageChoice and the rest), the same for every character whose rows
    // run: these forward to it. PLeader is the one she follows (GetAnchor:
    // the player, or her camp's leader)
    using FightPick = pawn::CGambits::FightPick;
    using RowClaim  = pawn::CGambits::RowClaim;
    auto PartyFightScan(CCharEntity* PLeader, const position_t& from) const -> FightPick;
    auto EngageChoice(CCharEntity* PLeader, const position_t& from) const -> FightPick;
    auto ClaimingRow(CBattleEntity* PTarget) const -> std::optional<RowClaim>;
    auto ClaimingRowAs(CBattleEntity* PTarget, bool melee) const -> std::optional<RowClaim>;
    auto TakesFights() const -> bool;
    auto TacticianMelee() const -> bool;
    auto FoesAround(CCharEntity* PLeader, const position_t& from) const -> std::vector<CBattleEntity*>;
    auto FoeFacts(CBattleEntity* PFoe, const CCharEntity* PLeader) const -> cardian::engage::Foe;
    auto FoeWhy(cardian::engage::Finder finder, CBattleEntity* PFoe, const CCharEntity* PLeader) const -> std::string;

    // A world body's idle tick (ROADMAP D1): rest when low, answer a mob on
    // her, and farming, pick a mob in her band within reach or head toward
    // the nearest farther off and fight what she meets
    void RoamTick();
    // A town seat's walk: in from her exit point, the dwell facing her point,
    // out to an exit (pawn::world::TownOrder)
    void TownTick(const pawn::world::TownOrder& order);
    // The mesh's route to the point walked in her own lane (pawn::world::laneOf)
    auto LanePath(const position_t& goal) -> bool;
    // A town walk's step, its length varied on her own cycle (the walk-step
    // jitter, pawn.WORLD_STEP_JITTER) so no two run cycles lock together
    void TownStep(const Intent& intent);
    // The mob that has come for her (a world body's roam answers it), and
    // the player's magic noted while they are still here
    auto SelfDefenceTarget() -> CMobEntity*;
    void NotePlayerMagic(const CCharEntity* PPlayer);

    // Everyone in this zone's party above the hunt thresholds and the
    // post-fight breather elapsed
    auto HuntBlocker(const CCharEntity* PPlayer) const -> std::string; // "" when the hunt may pull; otherwise what holds it

    // HuntBlocker without the distance rule: the player resting, a member
    // down or fighting. Judged again on the walk in, where the hunter is
    // meant to be away from the player
    auto PacingBlocker(const CCharEntity* PPlayer) const -> std::string;
    auto CampBlocker() const -> std::string;

    // The nearest idle, non-special mob in the difficulty band within
    // HUNT_RADIUS of the player. `skipped`, when given, collects what
    // was in the band but not pulled, and why (a few at most), for the
    // quiet hunt's line
    auto PickHuntTarget(const CCharEntity* PPlayer, std::string* skipped = nullptr) const -> CMobEntity*;
    // The same pick round any point, for a level, under the rules given:
    // the world's farmers (ROADMAP D1) measure from their route point
    auto PickHuntTarget(const position_t& around, uint8 level, const pawn::HuntRules& rules, std::string* skipped = nullptr) const -> CMobEntity*;

    // The walk in on a mob and the draw at the door, shared by party pawns
    // and world bodies (see the definition). True when the tick was hers
    auto ApproachTick(const position_t& anchor, uint8 level, const std::string& pacing, bool hunting, CBattleEntity* PPartyTarget) -> bool;

    // The hunt's eligibility (idle, unclaimed, ordinary, in the band), and
    // the nearest such mob within a radius of a point: the farmer's errand
    static auto huntable(CMobEntity* PMob, uint8 level, const pawn::HuntRules& rules) -> bool;
    auto        NearestPrey(const position_t& around, float radius, uint8 level, const pawn::HuntRules& rules) const -> CMobEntity*;
    auto        NearestPreyInZone(uint8 level, const pawn::HuntRules& rules) const -> CMobEntity*;

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
    // At a camp a damage dealer joining the party's fight waits at her seat,
    // weapon away, until the tank's receive rule (CampReceive) says the pull
    // has come in: at the landing point, on her, or stalled outside (#253)
    auto WaitsForThePull(const CBattleEntity* PTarget, ApproachKind kind) -> bool;

    // At a camp a caster holds her spells on a pull until it has come in,
    // as the melee wait for it: no walk out to meet it, and no enfeeble or
    // nuke from afar that would turn it from the puller. A cure for the
    // puller still goes; the player's own order is never held
    auto HoldsFireOn(const CBattleEntity* PTarget) -> bool;
    uint32 m_HeldFireOn = 0; // the pull her spells wait on, said once

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

    // The tick's danger map (RefreshDangers): every circle in range. The
    // vet works on the ones that matter to the walk in question
    // (FocusDangers, pawn::danger::forWalk) -- the active set, which Avoid
    // plans against -- and its escape test on the ones whose mobs see her
    // where she stands. Sees is the line-of-sight test the whole tick
    // shares, memoised so the mobs' own small caches are not thrashed by
    // one tick's questions.
    std::vector<pawn::danger::Danger>       m_Dangers;
    std::vector<pawn::danger::Danger>       m_ActiveDangers;
    std::vector<cardian::formation::Circle> m_ActivePadded;
    std::vector<pawn::danger::Danger>       m_EscapeDangers;
    std::vector<cardian::formation::Circle> m_EscapePadded;
    void                                    FocusDangers(const position_t& from, const position_t& to);
    auto                                    Sees(const pawn::danger::Danger& danger, const position_t& point) const -> bool;
    struct SightMemo
    {
        uint32 mob = 0;
        int    x   = 0;
        int    z   = 0;
        bool   seen = false;
    };
    mutable std::vector<SightMemo> m_SightMemo;

    // What her gambit engine asks of her (gambit_host.h): this controller,
    // through an adapter, so its names stay the engine's
    std::unique_ptr<pawn::GambitHost> m_Host;
    std::unique_ptr<pawn::CGambits>   m_Gambits;
    bool                              m_BrainLoaded = false;

    timer::time_point                 m_LastRangedAttackTime;
    timer::time_point                 m_LastTravelDebugTime;
    timer::time_point                 m_TravelProgressTime;
    float                             m_TravelBestDist = 0.0f;
    xi::ZoneId                        m_TravelHopZone{};
    // The hop her travel chose as she set out, from this zone toward that
    // one, kept while she walks it (TravelTick): chosen afresh every tick, the
    // nearest line could change under her mid-walk. Let go once TravelTick
    // has not run for a while, so the next journey chooses again
    std::optional<pawn::TravelHop>    m_TravelHop;
    xi::ZoneId                        m_TravelHopFrom{};
    xi::ZoneId                        m_TravelHopTarget{};
    timer::time_point                 m_TravelHopAt;

    bool              m_Hunting    = false;
    bool              m_World      = false;
    timer::time_point m_WorldNextHunt{}; // the farmer's next hunt check
    // The farmer's errand: where she is heading when nothing is in reach,
    // when she last moved toward it, and the beat's pause before the next
    std::optional<position_t> m_WorldHeading;
    timer::time_point         m_WorldPauseUntil{};
    timer::time_point         m_RoamStillSince{};
    position_t                m_RoamLastPos{};
    // A town seat (ROADMAP D4): the leg she is on (to her seat, at it, to
    // the exit) and how long she has stood still on a walk -- a stall gives
    // the leg up where she is and says so, the route being the thing to fix
    uint8             m_TownLeg = 0;
    position_t        m_TownGoal{};
    timer::time_point m_TownStillSince{};
    position_t        m_TownLastPos{};
    uint32            m_TownStepCount = 0; // the walk-step jitter's place in her cycle
    bool              m_Retreat    = false;
    bool              m_Waiting     = false;
    // The stake the orders pushed, and the tank's tow: she is
    // towing toward its frontline unless the player's pull has stopped
    // at an acceptable spot in front, within 20 yalms of the flag
    std::optional<pawn::Stake> m_Stake;
    bool                       m_Towing = false;
    bool                       m_KeepCampFightSpot = false;
    cardian::stake::Settlement  m_CampSettlement;
    cardian::stake::Route       m_TowRoute;
    timer::time_point          m_LastTowRouteDebug{};
    std::optional<EntityId>     m_TowingMob;
    std::optional<EntityId>     m_ReceiveMob;
    cardian::stake::Receive     m_Receive;
    bool                       m_ClosingWithoutHate = false;

    // NoteForSaving's book: what she last had written, and when
    bool              m_SaveSeeded    = false;
    int32             m_SavedHp       = -1;
    int32             m_SavedMp       = -1;
    position_t        m_SavedAt{};
    timer::time_point m_HealthSavedAt{};
    // Nobody real in her party since: a load lasts seconds, a party he left lasts
    timer::time_point m_NoPlayerSince{};
    bool              m_WaitOrdered = false;
    bool              m_WarpHold    = false; // the hold she holds is the automatic one a warp set (warp_hold.h)
    timer::time_point m_PlayerMagicSeen{ timer::time_point::min() }; // the player seen mid-warp or mid-teleport, so their vanishing reads as magic
    bool              m_HoldForPlayer = false; // drawn on the player's word: walking in with them, no closing until they strike
    struct Arrival
    {
        position_t        landing;
        timer::time_point until;
    };
    std::optional<Arrival> m_Arrival; // landed beside the player, waiting to see him there (AwaitsArrival)

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

    // The perimeter: the mob she attends, and the fight she has said "no
    // safe spot" for
    std::optional<EntityId> m_Attended;
    bool                    m_AttendedOrdered = false; // an explicit Engage is held until ended or replaced
    std::optional<EntityId> m_PlayersOrder;            // the mob his own Attack named (PlayersOrderOn)
    uint32                  m_SaidNoSpotFor   = 0;
    bool                    m_AttendedEngaged = false; // the attended mob was engaged last tick: the flip prompts her think
    uint8                   m_AttendVerdict   = 0;     // the crescent's last verdict, so the milestone log speaks only on a change
    bool                    m_Joining         = false; // walking to the first attending mage's side: she walks on until beside her (AttendIntent)
    // The attended mob's reach, read once per mob and skill list
    struct ReachMemo
    {
        uint32                    mob  = 0;
        uint16                    list = 0;
        cardian::perimeter::Reach reach;
    };
    ReachMemo m_Reach;

    // The step back's rest clock: the target's id and spot as of the tick
    // it was last seen moving, and when she last stepped
    uint32            m_TargetRestId = 0;
    position_t        m_TargetRestPos{};
    timer::time_point m_TargetRestSince{ timer::time_point::min() };
    timer::time_point m_LastStepBackAt{ timer::time_point::min() };

    // Her spot in the herd on the mob she fights: the bearing the herd
    // pass gave her, whether she has taken it (in reach and on it), whether
    // she is walking to it in reach and since when, how long a bad spot
    // pins her where she stands, and the bearing that failed her, avoided
    // a while. Then the way round to it, and where it was when the path
    // there was planned
    struct HerdSpot
    {
        uint32            mob     = 0;
        float             bearing = 0.0f;
        bool              taken   = false;
        bool              walking = false;
        timer::time_point walkingSince{};
        timer::time_point pinnedUntil{};
        float             blocked = 0.0f;
        timer::time_point blockedUntil{};
    };
    HerdSpot   m_Herd;
    bool       m_SeatVia = false;
    bool       m_SeatPathActive = false; // only the seat mover owns this path
    position_t m_SeatDestination{};

    // The beat (pawn-modes step 4): one pending act, due at a time. A
    // decision is acted on a beat later, by formation row (ReactionBeat)
    // -- the lead at once, the back line later. When it comes due the
    // rules run again: pass and it fires, fail and it is dropped with its
    // reason (the refusal line) and a hold-off on that target, never
    // re-armed in silence. One slot: a newer act replaces an older one,
    // and a mode change drops all but the player's order.
    struct Pending
    {
        enum class Act : uint8
        {
            SetOff,   // a hunt's walk starts
            Draw,     // a walk in's draw, once the rules allow
            Join,     // the party's fight, at the door
            Close,    // the hold's end was seen; she closes
            Order,    // the player's attack order
            StepBack, // the mob settled on her toes
        };
        Act               act;
        EntityId          target;
        timer::time_point due;
    };
    std::optional<Pending> m_Pending;
    timer::duration        m_HuntBeat{}; // the hunt's beat, drawn at set-off; the draw takes it again

    void Schedule(Pending::Act act, const CBattleEntity* PTarget, timer::duration beat);
    auto PendingIs(Pending::Act act, const CBattleEntity* PTarget) const -> bool; // this act on this mob is pending, due or not
    auto Due(Pending::Act act, const CBattleEntity* PTarget) const -> bool;       // pending, and the beat served

    // A target refused at the door is left alone for a while, so a
    // standing refusal (claimed, unclean) is not tried every beat
    uint32            m_HoldOffTarget = 0;
    timer::time_point m_HoldOffUntil{ timer::time_point::min() };
    void              HoldOff(const CBattleEntity* PTarget);
    timer::time_point m_LastTidyTime;

    // A mage's safety spot faces the battle (ROADMAP G, #249): where the
    // battle is when this tick's proposal is one -- the crescent or the
    // camp's backline while she attends (the mob, or where its pull lands),
    // her camp spot between pulls (where the pull lands); nothing otherwise
    auto SafetySpotBattle(const Place* place) -> std::optional<position_t>;
    // After her move: walking to her safety spot (moved since `before`, or
    // on a path) marks the walk; standing at it after one -- `planned`, the
    // safety proposal, met -- she turns to face `battleAt`, give or take
    // pawn.SPOT_FACING_ARC degrees, once, and holds it until she moves again
    void FaceBattleOnArrival(const std::optional<position_t>& battleAt, const Intent& planned, const position_t& before);
    bool m_SpotWalk = false; // a walk to her safety spot is under way

    // Who holds her eyes out of a fight, and until when: the one she emoted
    // at, or a member who emoted at her (IdleLook)
    struct Look
    {
        EntityId          at;
        timer::time_point until;
    };
    std::optional<Look>       m_Look;
    auto                      HeldLook() -> const CBaseEntity*; // m_Look's entity while it lasts; let go once it is over
    cardian::glance::Glances  m_Glances;
    cardian::glance::Emotes   m_Emotes;
    timer::time_point         m_IdleLookAt{};      // the last idle tick, a cast's included: a gap settles the clocks
    bool                      m_IdleFight = false; // a fight was on in her party at the last idle tick
    mutable timer::time_point m_FightAskedAt{};    // PartyInFight's tick, and its answer then
    mutable bool              m_FightAnswer = false;

    std::optional<std::pair<std::string, EntityId>> m_QueuedOrder;
    // The orders behind m_QueuedOrder, in the order he gave them: the line is
    // kQueueDepth deep in all, the first taking the next's place as it goes
    std::deque<std::pair<std::string, EntityId>> m_QueuedNext;
    static constexpr std::size_t                 kQueueDepth = 4;

    auto OrderName(unsigned kind, unsigned id) const -> std::string;
    // What came of one of the player's orders, to his addon (the Link's NOTE,
    // cardian_link_protocol.h): the note as the caller filled it, and why
    void Note(cl_note note, uint16 reason) const;

    // The command window's Attack: the party's engage order (EngageOn), given
    // to her alone, replacing any order she has queued. Held, it waits as her
    // one queued order -- a paused maneuver's, played out at the end of its
    // route; live, a maneuver ends as it is given, and she walks in and
    // fights with her gambits back
    auto AttackOrder(CBattleEntity* PTarget) -> uint16;
    // The command window's Disengage: she sheathes. An Attack row of hers
    // that claims the mob takes her back into the fight once her draw
    // cooldown (the usual re-engage wait) is served, and a Support Mage
    // whose rows take no fight attends it again; with her gambits off she
    // stays out. Held and in a maneuver, as Attack
    auto DisengageOrder() -> uint16;

    // The one way the first in her line changes, so the addon's queue line is never stale
    void SetQueuedOrder(std::optional<std::pair<std::string, EntityId>> order);

    // The order of his she is carrying out now: one fired from the command
    // window or from her line whose action is under way (a cast, an ability,
    // a weapon skill, a shot, an item's use), shown first on her queue line as
    // executing until that action is over (the user, 2026-10-05). Set as it
    // fires (SetRunning); let go once she has been seen acting and no longer
    // is, or never seen acting within kRunningBeat (UpdateRunning, each tick)
    std::optional<std::pair<std::string, EntityId>> m_Running;
    timer::time_point                               m_RunningSince;
    bool                                            m_RunningSeen = false;
    void                                            SetRunning(const std::string& key, EntityId target);
    void                                            UpdateRunning();

    // The enchanted-item lane (StartEnchant): the piece by the slot it is
    // worn in (sorting her bags moves it; worn it stays), whether the lane put
    // it on there and the item that slot held before (0: bare), its tries,
    // and its use once fired -- when the item was last used then, so a use
    // that lands is told from one cut short
    struct Enchant
    {
        uint16            itemId     = 0;
        uint8             equipSlot  = 0;
        bool              putOn      = false;
        uint16            replacedId = 0;
        bool              fired      = false;
        timer::time_point usedBefore;
        uint8             tries = 0;
    };
    std::optional<Enchant> m_Enchant;
    void                   EnchantTick();
    void                   TellQueueLine() const;

    // Her food (RESEARCH §19, pawn_food.cpp). A body of the world in the
    // player's party is topped up every kTopUpEvery. When her Eat with the
    // player row speaks, the player in her zone has food on and she has none,
    // she waits a delay of her own, 2 to 7 seconds, and her role's food is
    // due -- eaten at the first moment between fights she is free and
    // standing still. A Healer's cookie is not: it waits for her kneel
    // (EatCookieBeforeKneel, from RestTick)
    timer::time_point                m_FoodTopUpAt{};
    std::optional<timer::time_point> m_FoodAt;      // when she looks for her food: her delay after the player, or a look again
    std::optional<uint16>            m_FoodDue;     // the food she eats at her first free moment
    std::string                      m_FoodSaid;    // what her food check last said in the map log
    void                  FoodTick();
    auto                  EatCookieBeforeKneel(bool aboutToKneel, bool shortOfMp) -> bool;
    void                  SayFood(const std::string& line);

    // An order that started, the game's word on it heard for kHeels after:
    // its name for the log, its key for the note, when it was tried, and the
    // start of the action state it began (min when it began none)
    struct StartedOrder
    {
        std::string       name;
        std::string       key;
        timer::time_point at{ timer::time_point::min() };
        timer::time_point stateAt{ timer::time_point::min() };
    };
    // The order last started, stamped before it is tried, so a refusal the
    // game gives as it starts is its own; a try that fails takes it back
    // (OrderNotStarted). The one before it keeps its own word: a refusal its
    // action gives once the next has started is still about it (ToldAfterOrder)
    StartedOrder m_Started;
    StartedOrder m_StartedBefore;
    void         OrderStarted(const std::string& key, unsigned kind, unsigned id);
    void         OrderUnderWay();                   // it fired: the action state it began, if any
    auto         OrderNotStarted() -> bool;         // false when the game refused it already, and said so
    void         TellRefused(StartedOrder& order, uint16 message, const std::string& said);

    // The action itself, no queueing: CL_S_OK when it fired; CL_S_ON_RECAST and
    // CL_S_STANDING_UP for an order to hold; else why not
    auto TryAction(unsigned kind, unsigned mode, unsigned id, EntityId target) -> uint16;
    timer::time_point m_LastHuntLogTime;
    cardian::rest::State m_Rest;
    cardian::rest::Follow m_RestFollow;
    cardian::rest::Order m_RestOrder; // the player's "Rest until N%", or her own Rest row's at 100%; none by default
    int m_RestTicks = 0;
    bool m_RestDeferredPosition = false;
    double m_RestChatAt = 0.0;
    uint32 m_KneelHeldFor = 0;   // the member a kneel would put at risk, as last said; 0: none
    std::string m_RestHeldWhy;   // what keeps her up with a reason to kneel, as last said; empty: nothing
    uint8 m_RestLineKind = 0;           // the rest her queue line shows: CL_AK_NONE, CL_AK_REST (his order) or CL_AK_OWN_REST
    double m_RestCalledOffUntil = 0.0;  // restSeconds: her own kneels held off until then (CallOffRest)
    uint32 m_SaidMpShortFor = 0; // the fight's mob whose "My MP won't last" she has said
    // Magic aggro (#77): a cast of hers that costs MP is held where an
    // aggressive mob's magic detection would hear it, and while the hold
    // lasts her danger map counts the magic circles, so she steps clear
    // before she casts (MagicHearer, HoldForMagic, RefreshDangers)
    auto MagicHearer(const CBattleEntity* PFight) -> CMobEntity*;
    void HoldForMagic(CMobEntity* PHears, std::string_view spell);
    // The moment a mob turns on her, said once (NoteAggro): the mobs on her as of last tick
    void                NoteAggro();
    std::vector<uint32> m_MobsOnHer;
    // A gaze readied at her: her back to the mob until it fires (AvertGaze)
    auto   AvertGaze() -> bool;
    uint32 m_AvertedFrom = 0; // the mob whose gaze she turned from, as last said
    EntityId          m_MagicHold;           // the mob that would hear her casting
    timer::time_point m_MagicHoldUntil{};    // her danger map counts the magic circles until then
    timer::time_point m_MagicHoldSince{};    // since when she has held her spells for it
    timer::time_point m_MagicSaidAt{};       // when she last told the party
    // Sneak Attack or Boost before weapon skills: the weapon skill held one
    // tick while the opener goes out first
    struct HeldWs
    {
        EntityId          target;
        uint16            wsid = 0;
        timer::time_point at;
    };
    std::optional<HeldWs> m_HeldWs;
    // Sneak Attack wants the mob's back: her weapon skill waits while she
    // walks there (SneakPoint), a few seconds at most, and goes without it
    // if she cannot get there or the mob turns on her. No weapon skill (0):
    // the walk is for a naked Sneak Attack, on her next swing
    struct SneakStep
    {
        EntityId          target;
        uint16            wsid = 0;
        timer::time_point since;
    };
    std::optional<SneakStep>         m_SneakStep;
    // After a naked Sneak Attack she stays at the mob's back until her next
    // swing has spent it, a few seconds at most. Spent is the effect seen
    // on her and then gone: the ability lands a tick after it starts
    struct SneakHold
    {
        EntityId          target;
        timer::time_point until;
        bool              landed = false;
    };
    std::optional<SneakHold>         m_SneakHold;
    uint32                           m_SneakGaveUpOn = 0; // the mob whose back she gave up on with Sneak Attack still on her
    // A walk to the mob's back given up, or Sneak Attack refused: she leaves
    // it alone a while, unless the mob turns to someone else first, so a
    // back she cannot reach is not walked at every tick
    struct SneakRest
    {
        timer::time_point until;
        uint32            front = 0; // whom the mob faced
    };
    std::optional<SneakRest>         m_SneakRest;
    void                             RestSneak(const CBattleEntity* PTarget);
    std::optional<timer::time_point> m_SneakReadySince; // Sneak Attack ready in this fight, since: what holding it for her weapon skill costs
    auto                  BoostReady() const -> bool;
    auto                  SneakAttackUsable() const -> bool;
    // A naked Sneak Attack is due (RESEARCH §17.13 item 5): no weapon skill
    // row of hers can take it, no weapon skill is about to spend it (TP
    // under 1000), Sneak Attack can go now, and its back is somewhere she
    // can stand
    auto                  NakedSneakDue(const CBattleEntity* PTarget) -> bool;
    // A naked Sneak Attack, from the mob's back: she holds there for her swing
    void                  NakedSneak(const EntityId target);
    // Behind the mob as the server judges Sneak Attack: in the cone at its
    // back, by its own facing (utils.h behind, the same 64 the hit asks)
    auto                  BehindFor(const CBattleEntity* PTarget) const -> bool;
    auto                  KeepsSneakForBack(const CBattleEntity* PTarget) const -> bool; // Sneak Attack on her, the back not yet hers: no turn to swing
    auto                  SneaksOn(const CBattleEntity* PTarget) const -> bool;          // bound for this mob's back for Sneak Attack, or keeping it there for the swing
    // The swing goes whenever the mob is in her front cone (facing, 64):
    // kept for the back, her heading is held clear of it, within reach
    void                  KeepSwingOff(const CBattleEntity* PTarget);
    // Sneak Attack on her, weapon away, and the mob's back not hers: she
    // walks there before she draws, so no swing spends it from the side; a
    // few seconds from her arrival near it at most, then that back is given
    // up on and she draws
    auto                  TakesBackFirst(const CBattleEntity* PTarget) -> bool;
    struct BackFirst
    {
        EntityId                         target;
        std::optional<timer::time_point> since; // her arrival near the mob: the run in is not the walk round it
    };
    std::optional<BackFirst> m_BackFirst;
    auto                  FireWeaponSkill(EntityId target, uint16 wsid) -> bool;           // the weapon skill itself, said as it goes out
    // The spot straight behind the mob by its own facing, at her fight
    // radius: where the walk for Sneak Attack heads
    auto                  SneakPoint(const CBattleEntity* PTarget) const -> position_t;
    // Sneak Attack, then her weapon skill the moment it lands
    auto                  SneakThenWs(const EntityId target, uint16 wsid) -> bool;
    // The weapon skill without Sneak Attack: Boost first when her row
    // offers it, else the weapon skill now; never beyond the skill's own
    // reach (weaponSkillReach), since the game takes the TP as it starts
    auto                  BoostOrWeaponSkill(const EntityId target, uint16 wsid) -> bool;
    // The player's weapon skill order: it goes now and never walks; with
    // her Sneak Attack row on and Sneak Attack up, a skill that takes it
    // goes with it when she stands behind the mob already (the user,
    // 2026-10-02). It replaces a walk under way
    auto                  OrderedWeaponSkill(const EntityId target, uint16 wsid) -> bool;
    timer::time_point m_LastSurfaceLogTime;
    HeldPoint         m_LeadHeld;
    HeldPoint         m_FollowHeld;
    float             m_CourtesySide = 0.0f; // her side of the line last tick, kept a little cheaper (CourtesyStep)
    timer::time_point m_LastCourtesyTime;
    timer::time_point m_LastCourtesySaid;
    timer::time_point m_LastFormationClipTime;
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

    std::array<std::optional<uint16>, pawn::BehaviorCount> m_Behaviors{};             // the behaviour layer, by pawn::Behavior
    bool                                                    m_PlayerSeenDead = false; // while KO'd: the player has been seen dead since

    // Aggro avoidance state
    bool                m_HasSlot = false;  // this tick's FormationPoint ring, for re-seating a slot in danger
    Anchor            m_Slot{};
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
