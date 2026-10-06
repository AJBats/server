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
#include "view.h"

#include "local_planner.h"
#include "action_keys.h"
#include "cardian_link.h"
#include "formation_math.h"
#include "pawn.h"
#include "world.h"
#include "pawn_danger.h"
#include "pawn_doors.h"
#include "role_support.h"
#include "spell_bank.h"
#include "spell_movement.h"
#include "stake_math.h"
#include "pawn_gambits.h"
#include "pawn_items.h"
#include "pawn_rules.h"

#include "common/settings.h"
#include "enums/char_persist.h"
#include "enums/msg_basic.h"
#include "common/utils.h"
#include "common/xirand.h"

#include <magic_enum/magic_enum.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <limits>
#include <numbers>
#include <sstream>
#include <string>
#include <tuple>
#include <unordered_map>
#include <vector>

#include "ai/ai_container.h"
#include "ai/helpers/pathfind.h"
#include "ai/states/attack_state.h"
#include "ai/states/magic_state.h"
#include "ai/states/weaponskill_state.h"
#include "ai/states/ability_state.h"
#include "ai/states/range_state.h"
#include "ai/states/item_state.h"
#include "ai/states/mobskill_state.h"
#include "data/enums/detects.h"
#include "enmity_container.h"
#include "entities/char_entity.h"
#include "status_effect_container.h"
#include "entities/mob_entity.h"
#include "party_roster.h"
#include "tactics.h"
#include "items/item_weapon.h"
#include "navmesh/navmesh.h"
#include "navmesh/null_navmesh.h"
#include "party.h"
#include "pause/pause.h"
#include "recast_container.h"
#include "packets/s2c/0x05a_motionmes.h"
#include "ability.h"
#include "mobskill.h"
#include "spell.h"
#include "utils/battleutils.h"
#include "weapon_skill.h"
#include "utils/charutils.h"
#include "utils/itemutils.h"
#include "utils/zoneutils.h"
#include "item_container.h"
#include "zone.h"

#include <optional>

namespace
{
    // The zone's navmesh, or none when it has none: a zone without a mesh
    // holds a NullNavMesh, whose every query fails, and there a point is
    // taken as it is
    auto meshOf(const CZone* PZone) -> NavMesh*
    {
        auto* navMesh = PZone != nullptr ? PZone->navMesh() : nullptr;
        return navMesh == nullptr || dynamic_cast<const NullNavMesh*>(navMesh) != nullptr ? nullptr : navMesh;
    }

    // Her route on the navmesh as her own walk finds it (CPathFind): her
    // body's half-width kept off the walls, off-mesh links allowed
    auto routeOf(NavMesh* navMesh, const CBaseEntity* PEntity, const position_t& from, const position_t& to) -> Maybe<PathResult>
    {
        return navMesh->findPath(from, to, PEntity->modelHitboxSize * 0.5f, AvoidLinks{ false });
    }

    // A cardian as her gambit engine sees her (gambit_host.h): her controller's
    // own answers and actions
    class PawnHost final : public pawn::GambitHost
    {
    public:
        explicit PawnHost(CPawnController& controller)
        : m_controller(controller)
        {
        }

        auto OwnClient() const -> bool override
        {
            return false;
        }
        auto Cast(const EntityId target, const SpellID spell) -> bool override
        {
            return m_controller.Cast(target, spell);
        }
        auto CastAssigned(const EntityId target, const SpellID spell) -> bool override
        {
            return m_controller.CastAssigned(target, spell);
        }
        auto FreeToCast(CSpell* /*PSpell*/, CBattleEntity* /*PTarget*/) -> bool override
        {
            return m_controller.RestAllowsAction() && !m_controller.Acting() && !m_controller.QueuedOrderHoldsHer() && m_controller.canAct();
        }
        auto Ability(const EntityId target, const uint16 ability) -> bool override
        {
            return m_controller.Ability(target, ability);
        }
        auto WeaponSkill(const EntityId target, const uint16 skill) -> bool override
        {
            return m_controller.WeaponSkill(target, skill);
        }
        auto RangedAttack(const EntityId target) -> bool override
        {
            return m_controller.RangedAttack(target);
        }
        void ClearGambitBehaviors() override
        {
            m_controller.ClearGambitBehaviors();
        }
        void SetGambitBehavior(const uint16 behavior, const uint16 arg) override
        {
            m_controller.SetGambitBehavior(behavior, arg);
        }
        auto SneakAttackNow(const CBattleEntity* PTarget) -> bool override
        {
            return m_controller.SneakAttackNow(PTarget);
        }
        auto HasQueuedOrder() const -> bool override
        {
            return m_controller.QueuedOrderHoldsHer();
        }
        auto IsRetreating() const -> bool override
        {
            return m_controller.IsRetreating();
        }
        auto RestAllowsAction() const -> bool override
        {
            return m_controller.RestAllowsAction();
        }
        auto RestReadyIn(const double now) const -> double override
        {
            return m_controller.RestReadyIn(now);
        }
        auto RestInterruptionCost() const -> double override
        {
            return m_controller.RestInterruptionCost();
        }
        auto StandsToCast() const -> bool override
        {
            return true;
        }
        auto Acting() const -> bool override
        {
            return m_controller.Acting();
        }
        auto CanAct() -> bool override
        {
            return m_controller.canAct();
        }
        auto PartyPosition() const -> uint8 override
        {
            return m_controller.GetPawnPartyPosition();
        }
        auto IsWorld() const -> bool override
        {
            return m_controller.IsWorld();
        }
        auto GetLivePlayer() const -> CCharEntity* override
        {
            return m_controller.GetLivePlayer();
        }
        auto GetTopEnmity() const -> CBattleEntity* override
        {
            return m_controller.GetTopEnmity();
        }
        auto Anchor() const -> CCharEntity* override
        {
            return m_controller.GetAnchor();
        }
        auto HoldingOff(const CBattleEntity* PFoe) const -> bool override
        {
            return m_controller.HoldingOff(PFoe);
        }
        auto OrderedOnto(const CBattleEntity* PTarget) const -> bool override
        {
            return m_controller.PlayersOrderOn(PTarget);
        }
        auto PartyFightTarget() const -> CBattleEntity* override
        {
            return m_controller.PartyFightTarget();
        }

    private:
        CPawnController& m_controller;
    };
} // namespace

CPawnController::CPawnController(CCharEntity* PPawn)
: CPlayerController(PPawn)
, m_Host(std::make_unique<PawnHost>(*this))
, m_Gambits(std::make_unique<pawn::CGambits>(PPawn, m_Host.get()))
{
}

void CPawnController::ClearGambitBehaviors()
{
    m_Behaviors.fill(std::nullopt);
}

void CPawnController::SetGambitBehavior(const uint16 behavior, const uint16 arg)
{
    cardian::layers::speak(m_Behaviors, behavior, arg);
}

auto CPawnController::Behavior(const pawn::Behavior behavior) const -> std::optional<uint16>
{
    return m_Behaviors[static_cast<uint16>(behavior)];
}

void CPawnController::SetHunting(const bool on)
{
    if (m_Hunting != on)
    {
        ShowInfoFmt("pawn: {} hunt mode {}", POwner->getName(), on ? "on" : "off");
    }
    m_Hunting = on;
}

auto CPawnController::IsHunting() const -> bool
{
    return m_Hunting;
}

void CPawnController::SetWorld(const bool on)
{
    m_World = on;
}

auto CPawnController::IsWorld() const -> bool
{
    return m_World;
}

void CPawnController::SetWaiting(const bool on, const bool ordered, const std::string_view why)
{
    const bool was = m_Waiting;
    m_WarpHold     = false; // HoldForWarp marks its own after this
    if (ordered && !on)
    {
        EndRestOrder("the player's follow order");
        DropQueuedRest("the player's follow order");
    }
    m_Waiting      = on;
    m_WaitOrdered  = on && ordered;
    if (on)
    {
        // A walk in on the player's own Attack outlives the hold: she draws
        // on it from where she stands once it is near enough (ApproachTick).
        // Any other walk in -- the party's fight, her hunt -- ends here
        const bool hisAttack = m_Mode == Mode::Approach && m_Approach.has_value() && m_Approach->kind == ApproachKind::Order;
        if (!hisAttack)
        {
            m_Approach.reset();
        }
        // Holding, she gives up her herd spot: the herd spaces round her as
        // a fixed body where she stands, and she rejoins it from there
        m_Herd    = {};
        m_SeatVia = false;
        if (POwner->PAI->PathFind)
        {
            POwner->PAI->PathFind->Clear();
        }
        // A hold never ends a fight, nor a draw waiting on the player's first
        // strike: told to hold mid-fight, she fights on from where she stands
        // (Move takes no step for her) and holds after it (IdleMode at the
        // fight's exit)
        const bool fighting = m_Mode == Mode::Fight || m_Mode == Mode::Hold || m_Mode == Mode::Attend || hisAttack;
        if ((!was || m_Mode != Mode::Wait) && !fighting)
        {
            Transition(Mode::Wait, why.empty() ? (ordered ? "told to hold position" : "waits where she stands") : why);
        }
    }
    else if (was && m_Mode == Mode::Wait)
    {
        Transition(IdleMode(), why.empty() ? "follows again" : why);
    }
}

auto CPawnController::modeName(const Mode mode) -> const char*
{
    switch (mode)
    {
        case Mode::Follow:
            return "Follow";
        case Mode::Wait:
            return "Wait";
        case Mode::Travel:
            return "Travel";
        case Mode::Walk:
            return "Walk";
        case Mode::Roam:
            return "Roam";
        case Mode::Approach:
            return "Approach";
        case Mode::Hold:
            return "Hold";
        case Mode::Fight:
            return "Fight";
        case Mode::Attend:
            return "Attend";
        case Mode::Retreat:
            return "Retreat";
        case Mode::Maneuver:
            return "Maneuver";
        case Mode::Down:
            return "Down";
    }
    return "?";
}

auto CPawnController::CurrentMode() const -> Mode
{
    return m_Mode;
}

auto CPawnController::GetAnchor() const -> CCharEntity*
{
    if (auto* PPlayer = GetLivePlayer(); PPlayer != nullptr)
    {
        return PPlayer;
    }
    if (!m_World)
    {
        return nullptr;
    }
    const auto leader = pawn::world::campLeaderOf(POwner->id);
    if (leader == 0 || leader == POwner->id)
    {
        return nullptr;
    }
    auto* PLeader = pawn::findPawn(leader);
    return PLeader != nullptr && PLeader->loc.zone == POwner->loc.zone && !PLeader->isDead() ? PLeader : nullptr;
}

auto CPawnController::AttendsFight(CBattleEntity* PTarget) const -> bool
{
    return m_Gambits->AttendsFight(PTarget);
}

auto CPawnController::PlayersOrderOn(const CBattleEntity* PTarget) const -> bool
{
    return PTarget != nullptr && !PTarget->isDead() && m_PlayersOrder.has_value() && m_PlayersOrder->resolve<CBattleEntity>() == PTarget;
}

auto CPawnController::HasPlayersOrder() const -> bool
{
    const auto* PMob = m_PlayersOrder.has_value() ? m_PlayersOrder->resolve<CBattleEntity>() : nullptr;
    return PMob != nullptr && !PMob->isDead();
}

auto CPawnController::JoinBeat(CBattleEntity* PTarget) const -> timer::duration
{
    const auto how = AttendsFight(PTarget) ? cardian::engage::How::Attend : cardian::engage::How::Draw;
    return cardian::engage::waitsBeat(how) ? ReactionBeat() : timer::duration::zero();
}

auto CPawnController::AttendedTarget() const -> CBattleEntity*
{
    return m_Mode == Mode::Attend && m_Attended.has_value() ? m_Attended->resolve<CBattleEntity>() : nullptr;
}

auto CPawnController::Attending(const CBattleEntity* PTarget) const -> bool
{
    return PTarget != nullptr && AttendedTarget() == PTarget;
}

auto CPawnController::AttendedEngaged() const -> bool
{
    auto* PTarget = AttendedTarget();
    return PTarget != nullptr && PTarget->PAI->IsEngaged();
}

auto CPawnController::PartyFightTarget() const -> CBattleEntity*
{
    if (auto* PAttended = AttendedTarget(); PAttended != nullptr)
    {
        return PAttended;
    }
    if (m_Approach.has_value() && m_Approach->kind != ApproachKind::Hunt)
    {
        return m_Approach->target.resolve<CBattleEntity>();
    }
    return nullptr;
}

void CPawnController::Attend(CBattleEntity* PTarget, const std::string_view how)
{
    if (Attending(PTarget))
    {
        return;
    }
    m_Attended        = EntityId(PTarget);
    m_AttendedEngaged = false;
    m_Joining         = false;
    // Attendance observes a fight; the rest policy decides when she stands.
    m_Gambits->Prompt();
    Transition(Mode::Attend, fmt::format("attends the fight on {} from the perimeter ({})", PTarget->getName(), how));
}

auto CPawnController::AttendExitReason() -> std::string
{
    auto* PTarget = m_Attended.has_value() ? m_Attended->resolve<CBattleEntity>() : nullptr;
    if (PTarget == nullptr)
    {
        return "the fight is over (the mob is gone)";
    }
    if (PTarget->isDead())
    {
        return fmt::format("{} is dead", PTarget->getName());
    }
    return fmt::format("the fight is over (nobody of ours is on {})", PTarget->getName());
}

auto CPawnController::IdleMode() const -> Mode
{
    if (m_Retreat)
    {
        return Mode::Retreat;
    }
    if (m_Waiting)
    {
        return Mode::Wait;
    }
    if (m_World)
    {
        // A camp member follows her leader as a cardian follows the player;
        // the leader, and a body on her own, roam
        return GetAnchor() != nullptr ? Mode::Follow : Mode::Roam;
    }
    return Mode::Follow;
}

void CPawnController::Transition(const Mode to, const std::string_view why)
{
    const Mode from = m_Mode;

    // The exits this writer owns. Leaving a fight (Hold counts: she drew)
    // starts the draw cooldown against the mob she had, and clears the
    // herd spot, the close beat and the hold; leaving a walk in drops its
    // target and draw beat. A fight to a fight (a new target) leaves
    // nothing.
    const bool wasEngaged = from == Mode::Fight || from == Mode::Hold;
    const bool nowEngaged = to == Mode::Fight || to == Mode::Hold;
    // A weapon skill held behind its opener lasts the fight it was for: out of
    // it, or down, it is gone, or she would count as acting until the next
    if ((wasEngaged && !nowEngaged) || to == Mode::Down)
    {
        m_HeldWs.reset();
    }
    if (wasEngaged && !nowEngaged)
    {
        m_LeftFightAt   = m_Tick;
        m_Herd          = {};
        m_SeatVia       = false;
        m_Towing        = false;
        m_TowingMob.reset();
        m_HoldForPlayer = false;
        // Sneak Attack ready and never used before the fight ended is part
        // of what holding it costs: booked on the fight it was ready for
        if (m_SneakReadySince.has_value() && m_Gambits->OffersBeforeWs(cardian::tactician::Allowance::SneakAttack))
        {
            pawn::tactics::noteSneakAttack(static_cast<CCharEntity*>(POwner), m_LastFoughtId, cardian::tactics::SneakUse::Unused,
                                           std::chrono::duration<double>(m_Tick - *m_SneakReadySince).count());
        }
        m_SneakStep.reset();
        m_SneakHold.reset();
        m_SneakRest.reset();
        m_SneakReadySince.reset();
    }
    if (from == Mode::Approach && to != Mode::Approach)
    {
        m_Approach.reset();
        m_BackFirst.reset();
        m_Towing = false;
        m_TowingMob.reset();
    }
    // Drawing during an initial receive must not start its clock over.
    // Ending the fight or abandoning the approach does end that receive.
    const bool receivingApproach = to == Mode::Approach && m_Approach.has_value() && m_ReceiveMob.has_value() && m_Approach->target == *m_ReceiveMob;
    if ((wasEngaged && !nowEngaged && !receivingApproach) || (from == Mode::Approach && to != Mode::Approach && !nowEngaged))
    {
        m_ReceiveMob.reset();
        m_Receive = {};
        m_KeepCampFightSpot = false;
        m_CampSettlement = {};
        m_ClosingWithoutHate = false;
    }
    // His own Attack lasts the fight it named: leaving that fight, or the
    // walk in to it, for anything but a fight ends it
    if ((wasEngaged && !nowEngaged) || (from == Mode::Approach && to != Mode::Approach && !nowEngaged))
    {
        m_PlayersOrder.reset();
    }
    if (from == Mode::Attend && to != Mode::Attend)
    {
        m_Attended.reset();
        m_AttendedOrdered = false;
        m_SaidNoSpotFor    = 0;
        m_AttendVerdict    = 0;
    }
    // Leaving a maneuver by whatever door (its finisher, his cancel, his
    // camera off her, her leaving his party, her death) hands her back:
    // her gambit switch as it was, his walk order gone, and his addon
    // told (MANEUVER_STATE, ended) so it lets go of the camera
    if (from == Mode::Maneuver && to != Mode::Maneuver)
    {
        // A composed maneuver ended before its order fired (his cancel, her
        // death) takes the order with it: an order without its route would
        // fire from wherever she stands. The flag drops first, so the drop
        // does not end the maneuver again
        if (m_ManeuverComposed && m_QueuedOrder.has_value())
        {
            m_ManeuverComposed = false;
            ClearQueuedOrders("the maneuver ended first");
        }
        m_Gambits->SetMaster(m_ManeuverPriorMaster);
        pawn::clearWalkOrder(POwner->id);
        if (POwner->PAI->PathFind)
        {
            POwner->PAI->PathFind->Clear();
        }
        if (pawn::maneuverOf(m_ManeuverBy) == POwner->id) // a composed one gave his live slot up already
        {
            pawn::clearManeuver(m_ManeuverBy);
        }
        TellManeuver(CL_MS_ENDED);
        m_ManeuverBy       = 0;
        m_ManeuverComposed = false;
    }
    // KO'd, her line goes: nothing he queued outlives her fall to fire stale
    // at her raise
    if (to == Mode::Down && from != Mode::Down)
    {
        ClearQueuedOrders("she is KO'd");
        EndEnchant("she is KO'd");
    }
    // A pending act belongs to the mode it was scheduled in; only the
    // player's order outlives a change
    if (from != to && m_Pending.has_value() && m_Pending->act != Pending::Act::Order)
    {
        m_Pending.reset();
    }

    m_Mode = to;
    ShowInfoFmt("pawn: {} {} -> {}: {}", POwner->getName(), modeName(from), modeName(to), why);
}

void CPawnController::PlayerZoning()
{
    ClearQueuedOrders("the player is zoning"); // its targets stay behind in this zone
    EndEnchant("the player is zoning");
    StandDown("the player is zoning; stands down");
}

void CPawnController::StandDown(const std::string_view why)
{
    // Cancel delayed joins/orders before changing mode. Otherwise an Order,
    // which normally survives a transition, could restart the old fight.
    m_Pending.reset();
    m_HeldWs.reset();
    m_SneakStep.reset();
    m_SneakHold.reset();
    m_Approach.reset();
    m_BackFirst.reset();
    m_HoldForPlayer = false;
    if (POwner->PAI->PathFind != nullptr)
    {
        POwner->PAI->PathFind->Clear();
    }
    if (POwner->PAI->IsEngaged())
    {
        POwner->PAI->Internal_Disengage();
    }
    if (m_Mode == Mode::Fight || m_Mode == Mode::Hold || m_Mode == Mode::Approach || m_Mode == Mode::Attend)
    {
        Transition(IdleMode(), why);
    }
}

void CPawnController::DropQueuedRest(const std::string_view why)
{
    if (m_QueuedOrder.has_value() && m_QueuedOrder->first.starts_with("rest:"))
    {
        DropQueuedOrder(why);
    }
}

auto CPawnController::ServerExitReason() -> std::string
{
    auto* PTarget = m_LastFought.has_value() ? m_LastFought->resolve<CBattleEntity>() : nullptr;
    if (PTarget == nullptr)
    {
        return "the server ended it (the mob is gone)";
    }
    const auto verdict = cardian::rules::mayFight(EngageFactsFor(PTarget));
    if (verdict.why == "dead")
    {
        return fmt::format("{} is dead", PTarget->getName());
    }
    if (verdict.why == "claimed by another party")
    {
        return fmt::format("the server ended it ({} is claimed by another party)", PTarget->getName());
    }
    if (verdict.why.ends_with(" y away"))
    {
        return fmt::format("the server ended it (lost sight of {}, {})", PTarget->getName(), verdict.why);
    }
    return fmt::format("the server ended it ({})", verdict.ok ? "no reason it knows" : verdict.why);
}

auto CPawnController::IsWaiting() const -> bool
{
    return m_Waiting;
}

auto CPawnController::HoldNow() const -> cardian::hold::Hold
{
    return { m_Waiting, m_WaitOrdered, m_WarpHold };
}

void CPawnController::HoldForWarp(const std::string_view why)
{
    if (cardian::hold::warped(HoldNow()) == HoldNow())
    {
        return; // held already: on his order, or by a warp
    }
    SetWaiting(true, false, why);
    m_WarpHold = true;
}

void CPawnController::Carried()
{
    // The player's magic, seen a moment ago, was this same carry: it must
    // not read as them leaving her behind once she lands. She holds where she
    // lands, and follows again once he is in her zone: there already, or
    // arriving after her
    m_PlayerMagicSeen = timer::time_point::min();
    HoldForWarp("holds where she lands (carried off by a warp)");
}

void CPawnController::ArriveWith(const position_t& landing)
{
    constexpr auto kArrivalWait = 5s;
    m_Arrival                   = Arrival{ .landing = landing, .until = timer::now() + kArrivalWait };
}

auto CPawnController::AwaitsArrival(const Place& place) -> bool
{
    if (!m_Arrival.has_value())
    {
        return false;
    }
    constexpr float kArrivedWithin = 6.0f;
    if (distance(place.position(), m_Arrival->landing) <= kArrivedWithin)
    {
        ShowInfoFmt("pawn: {} sees her player arrive beside her, and follows", POwner->getName());
        m_Arrival.reset();
        return false;
    }
    if (m_Tick >= m_Arrival->until)
    {
        ShowInfoFmt("pawn: {} never saw her player arrive at ({:.1f}, {:.1f}, {:.1f}), and follows where he is", POwner->getName(),
                    m_Arrival->landing.x, m_Arrival->landing.y, m_Arrival->landing.z);
        m_Arrival.reset();
        return false;
    }
    return true;
}

void CPawnController::NotePlayerMagic(const CCharEntity* PPlayer)
{
    if (PPlayer->requestedWarp != WarpRequest::None || PPlayer->StatusEffectContainer->HasStatusEffect(xi::StatusEffect::Teleport))
    {
        m_PlayerMagicSeen = m_Tick;
    }
}

auto CPawnController::SelfDefenceTarget() -> CMobEntity*
{
    CMobEntity* PAttacker = nullptr;
    const auto  answers   = [&](CMobEntity* PMob)
    {
        if (PAttacker == nullptr && PMob->PAI->IsEngaged() && !PMob->isDead() && PMob->GetBattleTarget() == POwner)
        {
            PAttacker = PMob;
        }
    };
    pawn::forEachMobNear(pawn::entitiesAround(POwner), POwner->loc.p, 20.0f, answers);
    return PAttacker;
}

namespace
{
    // The errand's cost of a mob from where she stands: the walk, plus the
    // home pull -- a mob farther from her starting point than she is costs
    // that extra distance again, one nearer earns it back, scaled by how far
    // out she already is (pawn::HuntRules::roam). At home it is the plain walk
    auto errandCost(const pawn::HuntRules& rules, const position_t& from, const position_t& mob) -> float
    {
        const float walk = distance(from, mob);
        if (rules.roam <= 0.0f)
        {
            return walk;
        }
        const float out  = distance(rules.homeAt, from);
        const float pull = out / rules.roam;
        return walk + pull * (distance(rules.homeAt, mob) - out);
    }
} // namespace

// A town seat (ROADMAP D4). Three legs: in from her exit point to her seat,
// the dwell at it (facing her point, kneeling if her pose says, an idle
// emote now and then), out to the exit the world named, where she reports
// and fades. A town body never fights, so this runs ahead of the roamer's
// rest and self-defence. A walk that gets no nearer for WORLD_TOWN_STALL
// seconds is given up where she stands, with a warning naming the leg and
// both ends -- the route is what needs fixing, not her
void CPawnController::TownTick(const pawn::world::TownOrder& order)
{
    auto*       PPathFind = POwner->PAI->PathFind.get();
    const uint8 leg       = order.leaving ? 3 : order.atSeat ? 2 : 1;
    const bool  newGoal   = leg != 2 && std::hypot(order.goal.x - m_TownGoal.x, order.goal.z - m_TownGoal.z) > 0.5f;
    if (leg != m_TownLeg || newGoal)
    {
        m_TownLeg        = leg;
        m_TownGoal       = order.goal;
        m_TownStillSince = m_Tick;
        m_TownLastPos    = POwner->loc.p;
        if (PPathFind != nullptr && PPathFind->IsFollowingPath())
        {
            PPathFind->Clear();
        }
        if (leg == 2 && order.face.has_value())
        {
            POwner->loc.p.rotation = worldAngle(POwner->loc.p, *order.face);
            POwner->updatemask |= UPDATE_POS;
        }
    }
    RestTick(leg == 2, leg == 2 && order.kneel);
    if (leg == 2)
    {
        Move(Intent{});
        if (!order.kneel && !order.chatty && (PPathFind == nullptr || !PPathFind->IsFollowingPath()))
        {
            IdleEmote(nullptr);
        }
        return;
    }
    if (PPathFind == nullptr || POwner->GetSpeed() <= 0)
    {
        return;
    }
    // Arrival is judged on the ground: the point is on the mesh (the world
    // snaps it), but a step's height between her and it is no distance. A
    // waypoint is passed within a yalm and a half; her seat she lands on
    // exactly -- the last stretch is one straight step onto it, so a group's
    // ring is a ring and no two of them overlap
    const position_t& goal   = order.goal;
    const float       flat   = std::hypot(POwner->loc.p.x - goal.x, POwner->loc.p.z - goal.z);
    const bool        atGoal = flat < 1.6f && std::abs(POwner->loc.p.y - goal.y) < 3.0f;
    if (atGoal && (!order.goalIsSeat || flat < 0.35f))
    {
        Move(Intent{});
        pawn::world::noteReached(POwner->id);
        return;
    }
    if (atGoal)
    {
        Intent hop{};
        hop.kind  = Intent::Kind::Hop;
        hop.point = goal;
        Move(hop);
        return;
    }
    if (distance(POwner->loc.p, m_TownLastPos) > 0.5f)
    {
        m_TownLastPos    = POwner->loc.p;
        m_TownStillSince = m_Tick;
    }
    else if (m_Tick - m_TownStillSince > std::chrono::seconds(settings::get<uint32>("pawn.WORLD_TOWN_STALL")))
    {
        ShowWarningFmt("world: {} gets no nearer {} ({:.0f}, {:.0f}, {:.0f}) from ({:.0f}, {:.0f}, {:.0f}) in {}; gives the walk up there",
                       POwner->getName(), leg == 3 ? "her way out" : "her way in", goal.x, goal.y, goal.z, POwner->loc.p.x, POwner->loc.p.y, POwner->loc.p.z,
                       POwner->loc.zone != nullptr ? POwner->loc.zone->getName() : "?");
        Move(Intent{});
        pawn::world::noteReached(POwner->id);
        return;
    }
    // Her lane: the mesh's route walked a step to one side, hers, laid
    // once per leg and followed; the plain path when the mesh has none
    if (PPathFind->IsFollowingPath())
    {
        Intent keep{};
        keep.kind = Intent::Kind::Keep;
        TownStep(keep);
        return;
    }
    if (LanePath(goal))
    {
        return;
    }
    Intent intent{};
    intent.kind      = Intent::Kind::Path;
    intent.point     = goal;
    intent.arrive    = 1.0f;
    intent.tolerance = 1.2f;
    TownStep(intent);
}

// The walk-step jitter. Every body steps on the same zone tick, so the
// client got the same delta for all of them in the same bundle every
// time and drew their run cycles in lockstep; a real player's deltas
// vary from bundle to bundle because her own client's clock drifts
// against yours. So her step varies: a per-body sine over a few ticks,
// WORLD_STEP_JITTER percent either way, whose mean over a period is
// exactly one -- her speed is the norm to the yalm, only the phase is
// hers. Applied through baseSpeed for the step alone; the packet built
// at the end of the tick carries the norm
void CPawnController::TownStep(const Intent& intent)
{
    const float amplitude = settings::get<float>("pawn.WORLD_STEP_JITTER") / 100.0f;
    if (amplitude <= 0.0f)
    {
        Move(intent);
        return;
    }
    const uint32 period = 3 + POwner->id % 4;                                // 3 to 6 ticks, hers
    const float  phase  = static_cast<float>((POwner->id / 4) % 100) / 100.0f; // where in the cycle she started
    const float  scale  = 1.0f + amplitude * std::sin(2.0f * std::numbers::pi_v<float> * (static_cast<float>(m_TownStepCount++ % period) / static_cast<float>(period) + phase));
    const uint8  base   = POwner->baseSpeed;
    POwner->baseSpeed   = static_cast<uint8>(std::clamp(std::lround(static_cast<float>(base) * scale), 1L, 255L));
    Move(intent);
    POwner->baseSpeed = base;
    POwner->UpdateSpeed();
}

// The mesh's route to the point, its interior corners slid sideways by her
// lane (pawn::world::laneOf) and snapped back onto the mesh, or left where
// they were, when the slide lands in a wall -- a crowd sent down one street
// walks it abreast, not in single file. False when the mesh has no route
auto CPawnController::LanePath(const position_t& goal) -> bool
{
    auto* navMesh   = POwner->loc.zone != nullptr ? POwner->loc.zone->navMesh() : nullptr;
    auto* PPathFind = POwner->PAI->PathFind.get();
    if (navMesh == nullptr || PPathFind == nullptr)
    {
        return false;
    }
    const auto found = routeOf(navMesh, POwner, POwner->loc.p, goal);
    if (!found.has_value() || found->points.size() < 2)
    {
        return false;
    }
    const float              lane   = pawn::world::laneOf(POwner->id);
    std::vector<pathpoint_t> points = found->points;
    for (size_t i = 1; i + 1 < points.size(); ++i)
    {
        const auto& prev = points[i - 1].position;
        const auto& next = points[i + 1].position;
        const float dx   = next.x - prev.x;
        const float dz   = next.z - prev.z;
        const float len  = std::hypot(dx, dz);
        if (len < 0.01f)
        {
            continue;
        }
        position_t shifted = points[i].position;
        shifted.x += -dz / len * lane;
        shifted.z += dx / len * lane;
        if (const auto snapped = navMesh->findClosestValidPoint(shifted); snapped.has_value() && std::hypot(snapped->x - shifted.x, snapped->z - shifted.z) < 0.6f)
        {
            points[i].position = *snapped;
        }
    }
    PPathFind->PathThrough(std::move(points), PATHFLAG_RUN);
    return PPathFind->IsFollowingPath();
}

void CPawnController::RoamTick()
{
    m_Gambits->TickBehaviors();

    // A town seat is walked, held and left; nothing else of the roamer's
    // applies to her (ROADMAP D4: no fights in town)
    if (const auto order = pawn::world::townOrder(POwner->id); order.has_value())
    {
        TownTick(*order);
        return;
    }

    auto* PPathFind = POwner->PAI->PathFind.get();

    // Her rest: the world's plain Rest rows (brains.yaml: half HP, low MP)
    // start her own rest order, down until full, and a camp mage's marked
    // Rest row paces her MP by the bank (RESEARCH §17.13). Kneeling, or
    // on her row's order -- stood up by danger or her camp's fight, to
    // kneel again after -- she holds her ground: a mob that has come for
    // her and her camp's fight are answered, and nothing else of the
    // roamer's runs
    const bool down = RestTick(PPathFind == nullptr || !PPathFind->IsFollowingPath());

    // Her ground is hers to hold: a mob that has come for her is answered
    if (auto* PMob = SelfDefenceTarget(); PMob != nullptr)
    {
        Draw(PMob, ApproachKind::Order, "on her, roaming");
        return;
    }
    // Leading a camp, her party's fight is hers too: a member already
    // fighting, or a mob that has come for one of them (D5). The world's
    // own engagement, not her rows': the party's fight as the door scans it
    if (pawn::world::campSizeOf(POwner->id) > 1 && !m_Approach.has_value())
    {
        if (const auto party = PartyFightScan(nullptr, POwner->loc.p); party.target != nullptr && !HoldingOff(party.target))
        {
            if (!Draw(party.target, ApproachKind::Join, party.why, false) && !m_Approach.has_value())
            {
                HoldOff(party.target);
            }
            return;
        }
    }
    if (down || m_RestOrder.active())
    {
        return;
    }

    if (PPathFind == nullptr || POwner->GetSpeed() <= 0)
    {
        return;
    }
    RefreshDangers(nullptr);

    if (!pawn::world::isFarming(POwner->id))
    {
        Move(Intent{});
        if (!PPathFind->IsFollowingPath() && !POwner->PAI->IsCurrentState<CMagicState>())
        {
            m_Gambits->Tick(m_Tick, false);
            IdleEmote(nullptr);
        }
        return;
    }

    // Farming: the party's own hunt, round herself. On the hunt's cadence
    // a mob in her band within the hunt radius is the pick, judged by the
    // pull rules as any hunter's, then the shared walk in (ApproachTick).
    // Alone, her rest above is her pacing; leading a camp (D5) she picks
    // by the party's band and sets off only when the party is ready
    const auto      campSize = pawn::world::campSizeOf(POwner->id);
    pawn::HuntRules rules{};
    rules.minCheck   = settings::get<uint8>(campSize >= 3 ? "pawn.WORLD_TRIO_HUNT_MIN" : campSize == 2 ? "pawn.WORLD_DUO_HUNT_MIN" : "pawn.WORLD_HUNT_MIN");
    rules.maxCheck   = settings::get<uint8>(campSize >= 3 ? "pawn.WORLD_TRIO_HUNT_MAX" : campSize == 2 ? "pawn.WORLD_DUO_HUNT_MAX" : "pawn.WORLD_HUNT_MAX");
    rules.pullFirst  = 1;
    rules.aggressive = false;
    rules.links      = false;
    if (campSize > 1 && !m_Approach.has_value())
    {
        if (const auto why = CampBlocker(); !why.empty())
        {
            Move(Intent{});
            if (!POwner->PAI->IsCurrentState<CMagicState>())
            {
                m_Gambits->Tick(m_Tick, false);
                IdleEmote(nullptr);
            }
            if (m_Tick - m_LastHuntLogTime > 15s)
            {
                m_LastHuntLogTime = m_Tick;
                ShowInfoFmt("world: {} waits for her party ({})", POwner->getName(), why);
            }
            return;
        }
    }
    if (const auto home = pawn::world::homeOf(POwner->id); home.has_value())
    {
        rules.homeAt = home->first;
        rules.roam   = home->second;
    }
    if (!m_Approach.has_value() && m_Tick >= m_WorldNextHunt)
    {
        m_WorldNextHunt = m_Tick + std::chrono::seconds(xirand::GetRandomNumber<uint32>(settings::get<uint32>("pawn.HUNT_CHECK_MIN"), settings::get<uint32>("pawn.HUNT_CHECK_MAX") + 1));
        std::string skipped;
        if (auto* PMob = PickHuntTarget(POwner->loc.p, POwner->GetMLevel(), rules, &skipped); PMob != nullptr)
        {
            const auto beat = ReactionBeat();
            m_HuntBeat      = beat;
            m_Approach      = Approach{ EntityId(PMob), ApproachKind::Hunt };
            m_WorldHeading.reset();
            PPathFind->Clear();
            Transition(Mode::Approach, fmt::format("sets off after {} ({}{})", PMob->getName(),
                                                   magic_enum::enum_name(charutils::CheckMob(POwner->GetMLevel(), PMob)),
                                                   beat > 0s ? fmt::format(", in {:.1f}s", std::chrono::duration<float>(beat).count()) : ""));
            Schedule(Pending::Act::SetOff, PMob, beat);
            return;
        }
    }

    // Nothing in reach: an errand. She looks wider in steps, WORLD_SCAN_MIN
    // to WORLD_SCAN_MAX yalms, takes the nearest mob in her band and heads
    // for a spot WORLD_HEADING_SLOP yalms off it -- not a pull, a direction;
    // whatever she meets on the way is the fight, through the pick above.
    // At the spot with nothing found, or stood still beside one the mesh
    // would not let her reach, she pauses a beat and looks again
    if (m_WorldHeading.has_value())
    {
        if (distance(POwner->loc.p, m_RoamLastPos) > 0.5f)
        {
            m_RoamLastPos    = POwner->loc.p;
            m_RoamStillSince = m_Tick;
        }
        const bool stalled = !PPathFind->IsFollowingPath() && m_Tick - m_RoamStillSince > 3s;
        if (distance(POwner->loc.p, *m_WorldHeading) < 2.0f || stalled)
        {
            if (pawn::world::tickDebug())
            {
                ShowInfoFmt("world: {} {} her heading with nothing found; a beat, then looks again", POwner->getName(), stalled ? "gets no nearer" : "reaches");
            }
            m_WorldHeading.reset();
            m_WorldPauseUntil = m_Tick + std::chrono::seconds(settings::get<uint32>("pawn.WORLD_HEADING_PAUSE"));
            PPathFind->Clear();
            return;
        }
        Intent intent{};
        intent.kind      = Intent::Kind::Path;
        intent.point     = *m_WorldHeading;
        intent.arrive    = 1.5f;
        intent.tolerance = 2.0f;
        Move(intent);
        return;
    }
    m_RoamStillSince = m_Tick;
    m_RoamLastPos    = POwner->loc.p;
    if (m_Tick < m_WorldPauseUntil)
    {
        Move(Intent{});
        if (!POwner->PAI->IsCurrentState<CMagicState>())
        {
            m_Gambits->Tick(m_Tick, false);
            IdleEmote(nullptr);
        }
        return;
    }

    const float scanMin = settings::get<float>("pawn.WORLD_SCAN_MIN");
    const float scanMax = settings::get<float>("pawn.WORLD_SCAN_MAX");
    for (float radius = scanMin; radius <= scanMax + 0.01f; radius += 10.0f)
    {
        if (auto* PMob = NearestPrey(POwner->loc.p, radius, POwner->GetMLevel(), rules); PMob != nullptr)
        {
            const float slop  = settings::get<float>("pawn.WORLD_HEADING_SLOP");
            const float angle = xirand::GetRandomNumber(2.0f * std::numbers::pi_v<float>);
            position_t  spot  = PMob->loc.p;
            spot.x += slop * std::cos(angle);
            spot.z += slop * std::sin(angle);
            m_WorldHeading = spot;
            if (pawn::world::tickDebug())
            {
                ShowInfoFmt("world: {} heads toward {} ({}, {:.0f} y away; scanned {:.0f} y)", POwner->getName(), PMob->getName(), magic_enum::enum_name(charutils::CheckMob(POwner->GetMLevel(), PMob)), distance(POwner->loc.p, PMob->loc.p), radius);
            }
            return;
        }
    }
    // One look across the whole zone for the nearest in her band: a long
    // walk beats standing still (user, 2026-09-07). Nothing even then, and
    // she waits ten beats before looking again
    if (auto* PMob = NearestPreyInZone(POwner->GetMLevel(), rules); PMob != nullptr)
    {
        const float slop  = settings::get<float>("pawn.WORLD_HEADING_SLOP");
        const float angle = xirand::GetRandomNumber(2.0f * std::numbers::pi_v<float>);
        position_t  spot  = PMob->loc.p;
        spot.x += slop * std::cos(angle);
        spot.z += slop * std::sin(angle);
        m_WorldHeading = spot;
        ShowInfoFmt("world: {} finds nothing within {:.0f} y and heads across the zone toward {} ({}, {:.0f} y away)", POwner->getName(), scanMax,
                    PMob->getName(), magic_enum::enum_name(charutils::CheckMob(POwner->GetMLevel(), PMob)), distance(POwner->loc.p, PMob->loc.p));
        return;
    }
    m_WorldPauseUntil = m_Tick + std::chrono::seconds(10 * settings::get<uint32>("pawn.WORLD_HEADING_PAUSE"));
    if (m_Tick - m_LastHuntLogTime > 15s)
    {
        m_LastHuntLogTime = m_Tick;
        ShowInfoFmt("world: {} finds nothing in her band in the whole zone (level {}; band {}..{})", POwner->getName(), POwner->GetMLevel(),
                    magic_enum::enum_name(static_cast<EMobDifficulty>(rules.minCheck)), magic_enum::enum_name(static_cast<EMobDifficulty>(rules.maxCheck)));
    }
}

// The whole zone's nearest mob in the band: the errand's last resort
auto CPawnController::NearestPreyInZone(const uint8 level, const pawn::HuntRules& rules) const -> CMobEntity*
{
    if (POwner->loc.zone == nullptr)
    {
        return nullptr;
    }
    CMobEntity* best     = nullptr;
    float       bestDist = std::numeric_limits<float>::max();
    POwner->loc.zone->ForEachMob([&](CMobEntity* PMob)
    {
        if (!huntable(PMob, level, rules))
        {
            return;
        }
        const float away = errandCost(rules, POwner->loc.p, PMob->loc.p);
        if (away < bestDist)
        {
            best     = PMob;
            bestDist = away;
        }
    });
    return best;
}

// The nearest idle, unclaimed, ordinary mob in the band within the radius of
// the point: the farmer's errand. The pick proper (PickHuntTarget) adds the
// pull rules; this only says which way to walk
auto CPawnController::NearestPrey(const position_t& around, const float radius, const uint8 level, const pawn::HuntRules& rules) const -> CMobEntity*
{
    CMobEntity* best     = nullptr;
    float       bestDist = std::numeric_limits<float>::max();
    const auto  consider = [&](CMobEntity* PMob)
    {
        if (!huntable(PMob, level, rules) || distance(around, PMob->loc.p) > radius)
        {
            return;
        }
        // the cost, not the distance: from far out, prey back toward home wins
        const float away = errandCost(rules, around, PMob->loc.p);
        if (away < bestDist)
        {
            best     = PMob;
            bestDist = away;
        }
    };
    pawn::forEachMobNear(pawn::entitiesAround(POwner), around, radius, consider);
    return best;
}

namespace
{
    // The player has struck: an active enmity entry of theirs on the mob
    auto playerHasEnmity(const CCharEntity* PPlayer, const CBattleEntity* PTarget) -> bool
    {
        const auto* PMob = dynamic_cast<const CMobEntity*>(PTarget);
        if (PPlayer == nullptr || PMob == nullptr)
        {
            return false;
        }
        const auto* enmityList = PMob->PEnmityContainer->GetEnmityList();
        const auto  it         = enmityList->find(PPlayer->id);
        return it != enmityList->end() && it->second.active && (it->second.CE + it->second.VE) > 0;
    }
} // namespace

void CPawnController::SetRetreat(const bool on)
{
    if (m_Retreat != on)
    {
        if (on && POwner->PAI->IsEngaged())
        {
            POwner->PAI->Internal_Disengage();
        }
        // A cast or shot of her own is dropped on the spot, so she runs at
        // once; one the player ordered (begun as OrderStarted stamped it) is
        // his, and goes on
        if (on && (POwner->PAI->IsCurrentState<CMagicState>() || POwner->PAI->IsCurrentState<CRangeState>()))
        {
            bool ordered = false;
            if (!m_Started.name.empty())
            {
                const auto sinceOrder = POwner->PAI->GetCurrentState()->GetStartTime() - m_Started.at;
                ordered               = sinceOrder > -1s && sinceOrder < 1s;
            }
            if (!ordered)
            {
                ShowInfoFmt("pawn: {} drops her cast: retreat", POwner->getName());
                POwner->PAI->InterruptStates();
            }
        }
        m_Retreat = on;
        if (on)
        {
            EndRestOrder("retreat");
            DropQueuedRest("retreat");
            m_Approach.reset();
            Transition(Mode::Retreat, "retreat called");
        }
        else if (m_Mode == Mode::Retreat)
        {
            Transition(IdleMode(), "retreat off");
        }
    }
    m_Retreat = on;
}

auto CPawnController::IsRetreating() const -> bool
{
    return m_Retreat;
}

void CPawnController::SetStake(std::optional<pawn::Stake> stake)
{
    const auto same = [](const std::optional<pawn::Stake>& a, const std::optional<pawn::Stake>& b)
    {
        if (a.has_value() != b.has_value())
        {
            return false;
        }
        return !a.has_value() || (a->zone == b->zone && a->at.x == b->at.x && a->at.y == b->at.y && a->at.z == b->at.z && a->at.rotation == b->at.rotation);
    };
    if (same(m_Stake, stake))
    {
        return;
    }
    const bool was = m_Stake.has_value();
    // A camp set, moved or lifted leaves a kneeling mage down: she rests on
    // where she is, and walks to her new seat once topped up or when a mob
    // comes in (the user, 2026-10-04: "real mages would keep resting").
    // Only a camp in another zone, a trek away, stands her
    if (stake.has_value() && stake->zone != POwner->getZone())
    {
        StandFromRest("the camp is in another zone");
    }
    m_Stake        = std::move(stake);
    m_CampWaitPoint.reset(); // planned afresh for the new camp
    m_CampWaitSlidTo.reset();
    pawn::tactics::resetRestMemory(static_cast<CCharEntity*>(POwner));
    // A new place: her seats aim afresh, and a spot kept after a fight goes
    m_Towing = false;
    m_TowingMob.reset();
    m_KeepCampFightSpot = false;
    m_CampSettlement = {};
    // Relocating a camp remeasures an incoming pull, but cannot make a
    // tank already helping in a fight wait for that fight to arrive again.
    if (!m_Stake.has_value() || !m_Receive.joined)
    {
        m_ReceiveMob.reset();
        m_Receive = {};
    }
    m_ClosingWithoutHate = false;
    m_FollowHeld.has = false;
    m_LeadHeld.has   = false;
    if (was != m_Stake.has_value())
    {
        ShowInfoFmt("pawn: {} {}", POwner->getName(), !m_Stake.has_value() ? "is off the stake" : Staked() ? "keeps to the stake" : "has the stake's orders, from another zone");
    }
}

auto CPawnController::Staked() const -> bool
{
    return m_Stake.has_value() && !m_Retreat && POwner->loc.zone != nullptr && POwner->getZone() == m_Stake->zone;
}

auto CPawnController::Treks() const -> bool
{
    return !m_Waiting && !Staked();
}

auto CPawnController::PlayerPlace::anchor(const float predictScale) const -> Anchor
{
    return PlayerAnchor(PPlayer, predictScale);
}

auto CPawnController::PlayerPlace::position() const -> position_t
{
    return PPlayer->loc.p;
}

auto CPawnController::PlayerPlace::name() const -> std::string
{
    return PPlayer->getName();
}

auto CPawnController::PlayerPlace::fixed() const -> bool
{
    return false;
}

auto CPawnController::PlayerPlace::behind(const float) const -> std::optional<position_t>
{
    return std::nullopt;
}

auto CPawnController::StakePlace::anchor(const float) const -> Anchor
{
    // Fixed: no motion, no prediction, the heading it was set with
    Anchor a;
    a.observed = stake.at;
    a.anchor   = stake.at;
    return a;
}

auto CPawnController::StakePlace::position() const -> position_t
{
    return stake.at;
}

auto CPawnController::StakePlace::name() const -> std::string
{
    return "the stake";
}

auto CPawnController::StakePlace::fixed() const -> bool
{
    return true;
}

auto CPawnController::StakePlace::behind(const float radius) const -> std::optional<position_t>
{
    return nearPosition(stake.at, radius, std::numbers::pi_v<float>);
}

auto CPawnController::CurrentPlace(const CCharEntity* PPlayer) -> const Place*
{
    if (Staked())
    {
        m_StakePlace.stake = *m_Stake;
        return &m_StakePlace;
    }
    if (PPlayer != nullptr)
    {
        m_PlayerPlace.PPlayer = PPlayer;
        return &m_PlayerPlace;
    }
    return nullptr;
}

void CPawnController::EngageOn(CMobEntity* PMob)
{
    if (PMob == nullptr || PMob->isDead())
    {
        return;
    }
    // The beat: the order is taken now, her draw comes a beat later -- the
    // front row first, so the party never draws on one tick
    EndRestOrder("the player's engage order");
    DropQueuedRest("the player's engage order");
    Schedule(Pending::Act::Order, PMob, JoinBeat(PMob));
}

void CPawnController::Schedule(const Pending::Act act, const CBattleEntity* PTarget, const timer::duration beat)
{
    m_Pending = Pending{ act, EntityId(PTarget), m_Tick + beat };
}

auto CPawnController::PendingIs(const Pending::Act act, const CBattleEntity* PTarget) const -> bool
{
    return m_Pending.has_value() && m_Pending->act == act && PTarget != nullptr && m_Pending->target.resolve<CBattleEntity>() == PTarget;
}

auto CPawnController::Due(const Pending::Act act, const CBattleEntity* PTarget) const -> bool
{
    return PendingIs(act, PTarget) && m_Tick >= m_Pending->due;
}

void CPawnController::HoldOff(const CBattleEntity* PTarget)
{
    constexpr auto kRefusalHoldOff = 3s;
    m_HoldOffTarget                = PTarget->id;
    m_HoldOffUntil                 = m_Tick + kRefusalHoldOff;
}

auto CPawnController::HoldingOff(const CBattleEntity* PTarget) const -> bool
{
    return PTarget != nullptr && m_HoldOffTarget == PTarget->id && m_Tick < m_HoldOffUntil;
}

void CPawnController::FireOrderedEngage()
{
    if (!m_Pending.has_value() || m_Pending->act != Pending::Act::Order || m_Tick < m_Pending->due)
    {
        return;
    }
    auto* PMob = m_Pending->target.resolve<CMobEntity>();
    m_Pending.reset();
    if (PMob == nullptr || PMob->isDead())
    {
        return;
    }
    // The order replaces whatever she had set off after, as the party's
    // fight does; farther than she may draw from, she walks in on it
    m_HoldForPlayer = false;
    m_Approach.reset();
    Draw(PMob, ApproachKind::Order, "ordered");
    // Refused outright, neither drawn nor walking in, his own order came to
    // nothing and holds her back from nothing
    const bool taken = m_Mode == Mode::Fight || m_Mode == Mode::Hold || (m_Approach.has_value() && m_Approach->target.resolve<CBattleEntity>() == PMob);
    if (!taken && PlayersOrderOn(PMob))
    {
        m_PlayersOrder.reset();
    }
}

// A cardian cannot talk to the gate guard, so she takes Signet from the
// player: whenever the player has it and she does not (or hers is the
// shorter), she gets it for the player's remaining time. Conquest points,
// crystals and the Easy Prey bonus then land on her as they do on him.
void CPawnController::ShareSignet(CCharEntity* PPlayer)
{
    if (PPlayer == nullptr)
    {
        return;
    }
    const auto* theirs = PPlayer->StatusEffectContainer->GetStatusEffect(xi::StatusEffect::Signet);
    if (theirs == nullptr)
    {
        return;
    }
    const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(theirs->GetStartTime() + theirs->GetDuration() - timer::now());
    if (remaining < 60s)
    {
        return;
    }
    if (const auto* mine = POwner->StatusEffectContainer->GetStatusEffect(xi::StatusEffect::Signet); mine != nullptr)
    {
        const auto left = mine->GetStartTime() + mine->GetDuration() - timer::now();
        if (left + 60s >= remaining)
        {
            return;
        }
        POwner->StatusEffectContainer->DelStatusEffectSilent(xi::StatusEffect::Signet);
    }
    POwner->StatusEffectContainer->AddStatusEffect(xi::StatusEffect::Signet, static_cast<uint16>(xi::StatusEffect::Signet), 0, 0s, remaining);
    ShowInfoFmt("pawn: {} takes Signet with {} ({} min left)", POwner->getName(), PPlayer->getName(), remaining.count() / 60000);
}

auto CPawnController::Acting() const -> bool
{
    return POwner->PAI->IsCurrentState<CMagicState>() || POwner->PAI->IsCurrentState<CWeaponSkillState>() ||
           POwner->PAI->IsCurrentState<CAbilityState>() || POwner->PAI->IsCurrentState<CRangeState>() || POwner->PAI->IsCurrentState<CItemState>();
}

auto CPawnController::ReadyToAct() -> bool
{
    return pawn::readyToAct(static_cast<CCharEntity*>(POwner));
}

auto CPawnController::AbilitiesShutOut() const -> bool
{
    return pawn::abilitiesShutOut(POwner);
}

void CPawnController::NoteOrderedStance(const uint16 ability)
{
    m_Gambits->NoteOrderedStance(ability);
}

auto CPawnController::PlayersBuff(const uint16 ability, const xi::StatusEffect effect) -> bool
{
    return m_Gambits->PlayersBuff(ability, effect);
}

void CPawnController::HeadLook(const CBaseEntity* PAt)
{
    const uint16 want = PAt != nullptr ? PAt->targid : 0;
    if (POwner->m_TargID != want)
    {
        POwner->m_TargID = want;
        POwner->updatemask |= UPDATE_POS;
    }
}

namespace
{
    // The command window's Attack and Disengage, as the addon sends them and her queue holds them
    constexpr std::string_view kAttackOrder    = "attack";
    constexpr std::string_view kDisengageOrder = "disengage";
    constexpr std::string_view kRescueOrder    = "rescue"; // QueueRescue: its target is the player

    // An order's key: the catalogue's kind:mode:id, or item:<id> for an item
    // she uses on herself -- the queue line's own word for an item, read here
    // as kind 5 so it rides the same queue, grace and maneuver ending. Kind 5
    // is reached through item:<id> alone: the item gate (cardianDo) reads that
    // word, so the catalogue's form may not name it
    constexpr unsigned kItemOrder = 5;

    // The catalogue's kind for a weapon skill
    constexpr unsigned kWeaponSkillOrder = 4;

    // How far a weapon skill reaches, as the game judges it when the skill
    // lands (CCharEntity::OnWeaponSkillFinished): its own range -- 3 for a
    // blade, 14 for Gust Slash, 20 for an archery skill -- and both hitboxes,
    // a shade inside. The game takes the TP as the skill starts, so one out of
    // this reach is never tried (CPawnController::WeaponSkill). A skill used
    // on herself reaches wherever she is
    auto weaponSkillReach(const CBattleEntity* PUser, const uint16 wsid, const CBattleEntity* PTarget) -> float
    {
        if (battleutils::isValidSelfTargetWeaponskill(wsid))
        {
            return std::numeric_limits<float>::max();
        }
        const auto* PSkill = battleutils::GetWeaponSkill(wsid);
        return PSkill != nullptr ? PSkill->getRange() + PUser->modelHitboxSize + PTarget->modelHitboxSize - 0.3f : PUser->GetMeleeRange(PTarget) - 0.3f;
    }

    auto parseOrderKey(const std::string& key, unsigned& kind, unsigned& mode, unsigned& id) -> bool
    {
        if (std::sscanf(key.c_str(), "item:%u", &id) == 1)
        {
            kind = kItemOrder;
            mode = 2;
            return true;
        }
        return std::sscanf(key.c_str(), "%u:%u:%u", &kind, &mode, &id) == 3 && kind != kItemOrder;
    }

    // The first stack of an item in her inventory that nothing else holds
    // (a stack in a give or take is busy); nothing when she has none free
    auto inventorySlotOf(CCharEntity* PChar, const uint16 itemId) -> std::optional<uint8>
    {
        const auto* storage = PChar->getStorage(LOC_INVENTORY);
        for (uint8 slot = 1; storage != nullptr && slot <= storage->GetSize(); ++slot)
        {
            if (const CItem* PItem = storage->GetItem(slot); PItem != nullptr && PItem->getID() == itemId && PItem->getQuantity() > 0 && !PItem->isBusy())
            {
                return slot;
            }
        }
        return std::nullopt;
    }
} // namespace

auto CPawnController::DoAction(const std::string& key, CBattleEntity* PTarget) -> uint16
{
    if (key == kAttackOrder)
    {
        return AttackOrder(PTarget);
    }
    if (key == kDisengageOrder)
    {
        return DisengageOrder();
    }
    unsigned kind = 0;
    unsigned mode = 0;
    unsigned id   = 0;
    if (!parseOrderKey(key, kind, mode, id))
    {
        return CL_S_MALFORMED;
    }
    if (kind == kItemOrder)
    {
        // an item is used on herself, and only one she carries
        PTarget = POwner;
        if (!inventorySlotOf(static_cast<CCharEntity*>(POwner), static_cast<uint16>(id)).has_value())
        {
            return CL_S_NOT_CARRIED;
        }
    }
    if (PTarget == nullptr)
    {
        return CL_S_NO_TARGET;
    }
    if (POwner->isDead())
    {
        return CL_S_KNOCKED_OUT;
    }

    // Held (pause/pause.h), nothing starts: the order goes straight to her queue.
    // Out of the action's reach, the order is queued too: she walks in first
    // (OrderApproach). So too an order on its recast, while she acts (a weapon
    // skill held behind its opener included), or given as she gets up from a
    // rest. Holding her position she never
    // walks in: a weapon skill out of reach is refused, and anything else she tries
    // from where she stands, the game's own refusal reaching him as a note. A
    // paused maneuver's route may bring her into reach first, so its order's
    // reach is judged as it fires (FireQueuedOrder)
    const EntityId target(PTarget);

    // Something waits already: the order joins the line behind it, in the
    // order he gives them, kQueueDepth deep in all (the user, 2026-10-05: his
    // commands go as he gave them, his cancel peels the line back). A
    // maneuver's order is its own, one command, as it has always been
    if (m_QueuedOrder.has_value() && !InManeuver())
    {
        return JoinLine(key, target);
    }

    const bool     outOfReach = PTarget != POwner && PTarget->loc.zone == POwner->loc.zone && distance(POwner->loc.p, PTarget->loc.p) > OrderReach(kind, id, PTarget);
    if (outOfReach && m_Waiting && kind == kWeaponSkillOrder && !(cardian::pause::isHeld() && InManeuver()))
    {
        return CL_S_UNREACHED;
    }
    const bool       walksIn = outOfReach && !m_Waiting;
    std::string_view heldFor = cardian::pause::isHeld() ? "paused" : (!ReadyToAct() || m_HeldWs.has_value()) ? "busy" : walksIn ? "out of reach" : "";
    if (heldFor.empty())
    {
        OrderStarted(key, kind, id);
        const auto status = TryAction(kind, mode, id, target);
        if (status != CL_S_OK)
        {
            OrderNotStarted();
        }
        if (status == CL_S_ON_RECAST)
        {
            heldFor = "recast";
        }
        else if (status == CL_S_STANDING_UP)
        {
            heldFor = "standing up";
        }
        else
        {
            if (status == CL_S_OK)
            {
                OrderUnderWay();
                NoteOrderFired();
                SetRunning(key, target);
            }
            return status;
        }
    }

    // Nothing is refused for its timing (the user, 2026-10-05: with a line
    // to wait in, every order joins it): one she cannot start now waits, her
    // cast under way or its own recast however long, until it goes or he
    // takes it back
    SetQueuedOrder(std::make_pair(key, target));
    ShowInfoFmt("pawn: {} queues {} on {} ({})", POwner->getName(), key, PTarget->getName(), heldFor);
    if (InManeuver())
    {
        if (cardian::pause::isHeld())
        {
            // the paused maneuver has its order: walk the route, then this
            MarkComposed(fmt::format("{} at the end of the route", OrderName(kind, id)));
        }
        else
        {
            // a live maneuver's order that must wait (the walk in, her recast, her
            // cast under way) is hers to carry out: he is handed back now, and the
            // tick's mover walks her in, as for any order. It is hers, not the
            // composed maneuver's, so the maneuver's end must not take it with it
            m_ManeuverComposed = false;
            EndManeuver("his order is hers to carry out, the maneuver ends");
        }
    }
    return CL_S_OK;
}

auto CPawnController::OrderName(const unsigned kind, const unsigned id) const -> std::string
{
    switch (kind)
    {
        case 1:
            return "the ranged attack";
        case 2:
        {
            auto* PSpell = spell::GetSpell(static_cast<SpellID>(id));
            return PSpell != nullptr ? PSpell->getName() : "that spell";
        }
        case 3:
        {
            auto* PAbility = ability::GetAbility(static_cast<uint16>(id));
            return PAbility != nullptr ? PAbility->getName() : "that ability";
        }
        case 4:
        {
            auto* PSkill = battleutils::GetWeaponSkill(static_cast<uint16>(id));
            return PSkill != nullptr ? PSkill->getName() : "that weapon skill";
        }
        case kItemOrder:
        {
            const auto* PItem = xi::items::lookup(static_cast<uint16>(id));
            return PItem != nullptr ? PItem->getName() : "that item";
        }
        default:
            return "that";
    }
}

auto CPawnController::JoinLine(const std::string& key, const EntityId target) -> uint16
{
    const auto* PTarget = target.resolve<CBattleEntity>();
    if (!m_QueuedOrder.has_value())
    {
        SetQueuedOrder(std::make_pair(key, target));
        ShowInfoFmt("pawn: {} queues {} on {}", POwner->getName(), key, PTarget != nullptr ? PTarget->getName() : std::string("nobody"));
        return CL_S_OK;
    }
    if (m_QueuedNext.size() + 1 >= kQueueDepth)
    {
        return CL_S_QUEUE_FULL;
    }
    m_QueuedNext.emplace_back(key, target);
    ShowInfoFmt("pawn: {} queues {} on {} behind {} ({} waiting)", POwner->getName(), key, PTarget != nullptr ? PTarget->getName() : std::string("nobody"), m_QueuedOrder->first,
                m_QueuedNext.size() + 1);
    if (const auto owner = pawn::ordersOwnerOf(static_cast<const CCharEntity*>(POwner)); owner != 0)
    {
        cardian::link::send(owner, QueueLine());
    }
    return CL_S_OK;
}

auto CPawnController::QueueRescue(CCharEntity* PPlayer) -> uint16
{
    if (PPlayer == nullptr)
    {
        return CL_S_REFUSED;
    }
    if (POwner->isDead())
    {
        return CL_S_KNOCKED_OUT;
    }
    return JoinLine(std::string(kRescueOrder), EntityId(PPlayer));
}

auto CPawnController::QueuedOrderHoldsHer() const -> bool
{
    if (!m_QueuedOrder.has_value())
    {
        return false;
    }
    // Walking in on it, the order has her: her rows would walk her elsewhere
    if (m_OrderApproaching)
    {
        return true;
    }
    unsigned kind = 0;
    unsigned mode = 0;
    unsigned id   = 0;
    if (!parseOrderKey(m_QueuedOrder->first, kind, mode, id))
    {
        return true;
    }
    auto* PChar = static_cast<CCharEntity*>(POwner);
    if (kind == 2)
    {
        return !PChar->PRecastContainer->HasRecast(RECAST_MAGIC, static_cast<Recast>(id), 0s);
    }
    if (kind == 3)
    {
        const auto* PAbility = ability::GetAbility(static_cast<uint16>(id));
        return PAbility == nullptr || !PChar->PRecastContainer->HasRecast(RECAST_ABILITY, PAbility->getRecastId(), PAbility->getRecastTime());
    }
    return true;
}

void CPawnController::SetQueuedOrder(std::optional<std::pair<std::string, EntityId>> order)
{
    // The first done or let go, the next in line takes its place (the user,
    // 2026-10-05: back to back, as he gave them)
    if (!order.has_value() && !m_QueuedNext.empty())
    {
        order = std::move(m_QueuedNext.front());
        m_QueuedNext.pop_front();
        ShowInfoFmt("pawn: {}'s next queued order comes up: {}", POwner->getName(), order->first);
    }
    if (order.has_value() && !order->first.starts_with("rest:"))
    {
        EndRestOrder("the player's next order");
    }
    // A maneuver's rest lasts exactly as long as its order is queued: met,
    // replaced, cancelled or dropped with the maneuver, the rest is over
    if (m_ManeuverResting && !(order.has_value() && order->first.starts_with("rest:")))
    {
        m_ManeuverResting = false;
        EndRestOrder("its maneuver's order is gone");
    }
    m_QueuedOrder = std::move(order);
    // A new first in line, or none: a walk in was the last one's, and its
    // reach timer with it
    if (m_OrderApproaching)
    {
        m_OrderApproaching = false;
        if (POwner->PAI->PathFind)
        {
            POwner->PAI->PathFind->Clear();
        }
    }
    if (const auto owner = pawn::ordersOwnerOf(static_cast<const CCharEntity*>(POwner)); owner != 0)
    {
        cardian::link::send(owner, QueueLine());
    }
}

void CPawnController::SetRunning(const std::string& key, const EntityId target)
{
    // Only an order with an action to be over: an attack, a disengage, a move,
    // a rest and a rescue have none
    unsigned kind = 0;
    unsigned mode = 0;
    unsigned id   = 0;
    if (!parseOrderKey(key, kind, mode, id))
    {
        return;
    }
    m_Running      = std::make_pair(key, target);
    m_RunningSince = m_Tick;
    m_RunningSeen  = Acting();
    if (const auto owner = pawn::ordersOwnerOf(static_cast<const CCharEntity*>(POwner)); owner != 0)
    {
        cardian::link::send(owner, QueueLine());
    }
}

void CPawnController::UpdateRunning()
{
    if (!m_Running.has_value())
    {
        return;
    }
    // A weapon skill held behind its opener is still hers to carry out
    if (Acting() || m_HeldWs.has_value())
    {
        m_RunningSeen = true;
        return;
    }
    // Over once she has acted and stopped; an action the game refused before
    // it began, or one over within a tick, never shows her acting
    constexpr auto kRunningBeat = 1s;
    if (!m_RunningSeen && m_Tick - m_RunningSince < kRunningBeat)
    {
        return;
    }
    m_Running.reset();
    if (const auto owner = pawn::ordersOwnerOf(static_cast<const CCharEntity*>(POwner)); owner != 0)
    {
        cardian::link::send(owner, QueueLine());
    }
}

auto CPawnController::QueueLine() const -> cl_queue
{
    auto line      = cardian::link::make<cl_queue>();
    line.character = POwner->id;
    if (m_Running.has_value())
    {
        const auto* PRunning  = m_Running->second.resolve<CBattleEntity>();
        line.running.action = pawn::actionOfKey(m_Running->first);
        line.running.target = PRunning != nullptr ? PRunning->targid : uint16{ 0 };
    }
    if (m_QueuedOrder.has_value())
    {
        const auto* PTarget = m_QueuedOrder->second.resolve<CBattleEntity>();
        line.action         = pawn::actionOfKey(m_QueuedOrder->first);
        line.target         = PTarget != nullptr ? PTarget->targid : uint16{ 0 };
        if (m_ManeuverResting)
        {
            line.action.mode = 1; // the maneuver's rest, kneeling now: under way, not waiting
        }
        for (const auto& [key, who] : m_QueuedNext)
        {
            if (line.more >= std::size(line.next))
            {
                break;
            }
            const auto* PNext           = who.resolve<CBattleEntity>();
            line.next[line.more].action = pawn::actionOfKey(key);
            line.next[line.more].target = PNext != nullptr ? PNext->targid : uint16{ 0 };
            ++line.more;
        }
    }
    else if (m_RestLineKind != CL_AK_NONE)
    {
        // Nothing queued, a rest under way (mode 1): his order's, by its
        // percent, or her own (#256)
        line.action = cl_action{ m_RestLineKind, 1, static_cast<uint16_t>(m_RestLineKind == CL_AK_REST ? m_RestOrder.percent : 0) };
        line.target = POwner->targid;
    }
    if (m_Enchant.has_value())
    {
        // the enchanted item, waiting out its delay (mode 0) or in use (mode 1)
        line.lane.action = cl_action{ CL_AK_ITEM, static_cast<uint8_t>(m_Enchant->fired ? 1 : 0), m_Enchant->itemId };
        line.lane.target = POwner->targid;
    }
    return line;
}

auto CPawnController::DropQueuedOrder(const std::string_view why, const uint32 formerOwner) -> bool
{
    if (!m_QueuedOrder.has_value())
    {
        return false;
    }
    ShowInfoFmt("pawn: {} drops the queued {} ({})", POwner->getName(), m_QueuedOrder->first, why);
    SetQueuedOrder(std::nullopt);
    if (InManeuver() && m_ManeuverComposed)
    {
        EndManeuver(fmt::format("its order is gone ({}), the maneuver ends", why));
    }
    TellFormerOwner(formerOwner);
    return true;
}

void CPawnController::TellFormerOwner(const uint32 formerOwner) const
{
    // Out of his party she has no orders owner for SetQueuedOrder to tell: his
    // addon still shows her line, a rest's included, and an empty one clears it
    if (formerOwner != 0 && pawn::ordersOwnerOf(static_cast<const CCharEntity*>(POwner)) == 0)
    {
        auto line      = cardian::link::make<cl_queue>();
        line.character = POwner->id;
        cardian::link::send(formerOwner, line);
    }
}

// His cancel peels her line back from its end, one order a press: mashed, it
// clears the lot (the user, 2026-10-05)
auto CPawnController::CancelQueuedOrder() -> bool
{
    if (!m_QueuedNext.empty())
    {
        ShowInfoFmt("pawn: {} drops the queued {} (the player took it back)", POwner->getName(), m_QueuedNext.back().first);
        m_QueuedNext.pop_back();
        if (const auto owner = pawn::ordersOwnerOf(static_cast<const CCharEntity*>(POwner)); owner != 0)
        {
            cardian::link::send(owner, QueueLine());
        }
        return true;
    }
    return DropQueuedOrder("the player took it back");
}

auto CPawnController::ClearQueuedOrders(const std::string_view why, const uint32 formerOwner) -> bool
{
    m_QueuedNext.clear();
    if (DropQueuedOrder(why, formerOwner))
    {
        return true;
    }
    TellFormerOwner(formerOwner); // nothing queued, but a rest may be on his line
    return false;
}

void CPawnController::OrderStarted(const std::string& key, const unsigned kind, const unsigned id)
{
    if (!m_Started.name.empty())
    {
        m_StartedBefore = std::move(m_Started);
    }
    m_Started = StartedOrder{ OrderName(kind, id), key, m_Tick, timer::time_point::min() };
}

void CPawnController::OrderUnderWay()
{
    // A state that began as it was tried is the order's; one older is
    // whatever she was doing already (her swings)
    if (const auto* PState = POwner->PAI->GetCurrentState(); PState != nullptr && PState->GetStartTime() >= m_Started.at)
    {
        m_Started.stateAt = PState->GetStartTime();
    }
}

auto CPawnController::OrderNotStarted() -> bool
{
    const bool untold = !m_Started.name.empty();
    m_Started         = StartedOrder{};
    return untold;
}

void CPawnController::ToldAfterOrder(const uint16 message, const std::string& said)
{
    constexpr auto kHeels = 2s;
    // The game speaks from the action state it is running: one the order
    // before the last began is still that order's to answer for. Otherwise
    // the word is the last order's
    StartedOrder* order = &m_Started;
    if (const auto* PState = POwner->PAI->GetCurrentState();
        PState != nullptr && !m_StartedBefore.name.empty() && m_StartedBefore.stateAt != timer::time_point::min() &&
        PState->GetStartTime() == m_StartedBefore.stateAt)
    {
        order = &m_StartedBefore;
    }
    if (order->name.empty() || m_Tick - order->at > kHeels)
    {
        return;
    }
    // Turned off the mob to keep a Sneak Attack for its back (KeepSwingOff),
    // her auto-attack is told it cannot see its target every round: her
    // own swing's word, not the order's
    if (static_cast<MsgBasic>(message) == MsgBasic::UnableToSeeTarget && KeepsSneakForBack(POwner->GetBattleTarget()))
    {
        return;
    }
    // Her auto-attack's own word -- a swing at a target beyond her reach,
    // every round while she holds her position out of it -- is no order's
    // refusal. A pet's skill out of range is told the same way, so after a
    // pet's ability it still is one
    if (static_cast<MsgBasic>(message) == MsgBasic::TargetOutOfRange)
    {
        unsigned    kind     = 0;
        unsigned    mode     = 0;
        unsigned    id       = 0;
        const auto* PAbility = parseOrderKey(order->key, kind, mode, id) && kind == 3 ? ability::GetAbility(static_cast<uint16>(id)) : nullptr;
        if (PAbility == nullptr || !PAbility->isPetAbility())
        {
            return;
        }
    }
    TellRefused(*order, message, said);
}

void CPawnController::TellRefused(StartedOrder& order, const uint16 message, const std::string& said)
{
    ShowInfoFmt("pawn: {}'s order {} was refused by the game: {}", POwner->getName(), order.name, said);
    auto note    = cardian::link::make<cl_note>();
    note.kind    = CL_NOTE_REFUSED;
    note.action  = pawn::actionOfKey(order.key);
    note.message = message;
    cardian::link::setText(note.about, said);
    Note(note, CL_S_REFUSED);
    order = StartedOrder{};
}

void CPawnController::Note(cl_note note, const uint16 reason) const
{
    if (const auto owner = pawn::ordersOwnerOf(static_cast<const CCharEntity*>(POwner)); owner != 0)
    {
        note.cardian = POwner->id;
        cardian::link::send(owner, note, reason);
    }
}

auto CPawnController::TryAction(const unsigned kind, const unsigned mode, const unsigned id, const EntityId target) -> uint16
{
    if (!PrepareRestAction(true))
    {
        return CL_S_STANDING_UP;
    }
    bool fired = false;
    switch (kind)
    {
        case 1:
            fired = RangedAttack(target);
            break;
        case 2:
        {
            if (mode != 2)
            {
                return CL_S_MALFORMED;
            }
            const auto spellId = static_cast<SpellID>(id);
            CSpell*    PSpell  = spell::GetSpell(spellId);
            if (PSpell == nullptr)
            {
                return CL_S_MALFORMED;
            }
            if (static_cast<CCharEntity*>(POwner)->PRecastContainer->HasRecast(RECAST_MAGIC, static_cast<Recast>(spellId), 0s))
            {
                return CL_S_ON_RECAST;
            }
            // An order is never second-guessed: straight to the player
            // controller's cast, past the gambit engine's redundancy rule
            // (which declines a cure on a healthy friend). It waits the 2.5 s
            // after her last spell as the player's own cast does (the pacer
            // holds it until then)
            const EntityId castTarget = PSpell->getValidTarget() == TARGET_SELF ? EntityId(POwner) : target;
            FaceTarget(castTarget);
            HeadLook(castTarget.resolve<CBattleEntity>());
            fired = CPlayerController::Cast(castTarget, spellId);
            break;
        }
        case 3:
            if (mode != 2)
            {
                return CL_S_MALFORMED;
            }
            if (const auto* PAbility = ability::GetAbility(static_cast<uint16>(id));
                PAbility != nullptr && static_cast<CCharEntity*>(POwner)->PRecastContainer->HasRecast(RECAST_ABILITY, PAbility->getRecastId(), PAbility->getRecastTime()))
            {
                return CL_S_ON_RECAST;
            }
            fired = Ability(target, static_cast<uint16>(id));
            // His Berserk or Defender is never taken off by her tactician's
            // stance (PlayersBuff)
            if (fired && cardian::tactician::isStanceAbility(id))
            {
                NoteOrderedStance(static_cast<uint16>(id));
            }
            break;
        case 4:
            if (mode != 2)
            {
                return CL_S_MALFORMED;
            }
            fired = OrderedWeaponSkill(target, static_cast<uint16>(id));
            break;
        case kItemOrder:
        {
            // the first stack she carries, through the game's own item use
            auto*      PChar = static_cast<CCharEntity*>(POwner);
            const auto slot  = inventorySlotOf(PChar, static_cast<uint16>(id));
            if (!slot.has_value())
            {
                return CL_S_NOT_CARRIED;
            }
            return pawn::items::useItem(PChar, *slot, LOC_INVENTORY);
        }
        default:
            return CL_S_MALFORMED;
    }
    return fired ? CL_S_OK : CL_S_CANNOT_NOW;
}

void CPawnController::FireQueuedOrder()
{
    if (!m_QueuedOrder.has_value())
    {
        return;
    }
    const auto [key, target] = *m_QueuedOrder;

    // A composed maneuver walks its route first: the order waits at the end of it,
    // its grace running from there. Then, as the gambit engine does, she closes
    // on the target until the order is in reach (the user, 2026-09-22: the
    // route's end is a waypoint; the order is "run to and use")
    if (InManeuver() && m_ManeuverComposed)
    {
        if (cardian::pause::isHeld())
        {
            return;
        }
        if (!RouteWalked())
        {
            return;
        }
        // The move orders (ComposeMove): the route was the order
        if (key == "move" || key == "movewait")
        {
            SetQueuedOrder(std::nullopt);
            ShowInfoFmt("pawn: {} has walked the route", POwner->getName());
            if (key == "movewait")
            {
                SetWaiting(true, true, "the route walked, holding there: the maneuver ends");
            }
            else
            {
                EndManeuver("the route walked, the maneuver ends");
            }
            return;
        }
    }

    // The maneuver's rest (ComposeRest), the route, if one was laid, walked:
    // she kneels there and the maneuver lasts, her queued order still, until
    // the rest order is over -- HP and MP at the percent, or a rest she
    // cannot carry out (RestTick says which)
    if (int percent = 0; std::sscanf(key.c_str(), "rest:%d", &percent) == 1)
    {
        if (!InManeuver())
        {
            SetQueuedOrder(std::nullopt);
            SetRestOrder(percent, "the player's order");
            return;
        }
        if (!m_ManeuverResting)
        {
            m_ManeuverResting = true;
            pawn::clearWalkOrder(POwner->id); // the route's end is where she kneels
            SetRestOrder(percent, "the player's maneuver");
            return;
        }
        if (!m_RestOrder.active())
        {
            SetQueuedOrder(std::nullopt);
            EndManeuver(fmt::format("her rest until {}% is over, the maneuver ends", percent));
        }
        return;
    }

    // His Attack (AttackOrder), held to the release, the route, if one was
    // laid, walked: she takes the fight as the party's engage order has her
    if (key == kAttackOrder)
    {
        SetQueuedOrder(std::nullopt);
        EndManeuver("his attack is away, the maneuver ends");
        auto* PMob = target.resolve<CMobEntity>();
        if (PMob == nullptr || PMob->isDead())
        {
            ShowInfoFmt("pawn: {} lets the queued attack go (its target is gone)", POwner->getName());
            auto note   = cardian::link::make<cl_note>();
            note.kind   = CL_NOTE_LET_GO;
            note.action = pawn::actionOfKey(key);
            Note(note, CL_S_NO_TARGET);
            return;
        }
        ShowInfoFmt("pawn: {} starts the queued attack on {}", POwner->getName(), PMob->getName());
        m_PlayersOrder = EntityId(PMob);
        EngageOn(PMob);
        return;
    }
    // His Disengage (DisengageOrder), held to the release likewise
    if (key == kDisengageOrder)
    {
        SetQueuedOrder(std::nullopt);
        EndManeuver("his disengage is away, the maneuver ends");
        StandDown("the player's order: she sheathes");
        return;
    }
    // His Rescue (QueueRescue): it waits out the rescue's cooldown as an order
    // waits out its recast; any other refusal lets it go with a note
    if (key == kRescueOrder)
    {
        auto*               PPlayer = target.resolve<CCharEntity>();
        pawn::RescueRefusal refusal;
        const auto          status = PPlayer != nullptr ? pawn::rescue(PPlayer, static_cast<CCharEntity*>(POwner), refusal) : uint16{ CL_S_NO_TARGET };
        if (status == CL_S_COOLING_DOWN)
        {
            return;
        }
        SetQueuedOrder(std::nullopt);
        if (status != CL_S_OK)
        {
            ShowInfoFmt("pawn: {} lets the queued rescue go (outcome 0x{:04X})", POwner->getName(), status);
            auto note   = cardian::link::make<cl_note>();
            note.kind   = CL_NOTE_LET_GO;
            note.action = pawn::actionOfKey(key);
            Note(note, status);
        }
        return;
    }

    unsigned kind = 0;
    unsigned mode = 0;
    unsigned id   = 0;
    if (!parseOrderKey(key, kind, mode, id))
    {
        SetQueuedOrder(std::nullopt);
        return;
    }

    // Holding her position she never walks in: a weapon skill out of reach is
    // let go, so it spends no TP on nothing; anything else is tried from where
    // she stands below, and a refusal the game gives reaches him as a note
    const auto beyond = OrderOutOfReach();
    if (beyond.has_value() && m_Waiting && kind == kWeaponSkillOrder)
    {
        ShowInfoFmt("pawn: {} lets the queued {} go (holding position, out of reach of {})", POwner->getName(), key, beyond->first->getName());
        auto note   = cardian::link::make<cl_note>();
        note.kind   = CL_NOTE_LET_GO;
        note.action = pawn::actionOfKey(key);
        note.target = beyond->first->targid;
        cardian::link::setText(note.about, beyond->first->getName());
        SetQueuedOrder(std::nullopt);
        Note(note, CL_S_UNREACHED);
        if (InManeuver() && m_ManeuverComposed)
        {
            m_ManeuverComposed = false;
            EndManeuver("holding position, out of reach: the maneuver ends");
        }
        return;
    }

    // Out of the action's reach: the walk in first (OrderApproach, taken by the
    // tick's mover), the grace waiting, up to kOrderApproachMax of walking
    if (beyond.has_value() && !m_Waiting)
    {
        constexpr auto kOrderApproachMax = 30s;
        if (!m_OrderApproaching)
        {
            m_OrderApproaching    = true;
            m_OrderApproachSince  = m_Tick;
            if (m_ManeuverComposed)
            {
                pawn::clearWalkOrder(POwner->id); // the route is walked: the target is the goal now
            }
            ShowInfoFmt("pawn: {} walks in on {} for {} ({:.1f} y, reach {:.1f})", POwner->getName(), beyond->first->getName(), OrderName(kind, id),
                        distance(POwner->loc.p, beyond->first->loc.p), beyond->second);
        }
        else if (m_Tick - m_OrderApproachSince > kOrderApproachMax)
        {
            ShowInfoFmt("pawn: {} lets the queued {} go (could not get in reach of {})", POwner->getName(), key, beyond->first->getName());
            auto note   = cardian::link::make<cl_note>();
            note.kind   = CL_NOTE_LET_GO;
            note.action = pawn::actionOfKey(key);
            note.target = beyond->first->targid;
            cardian::link::setText(note.about, beyond->first->getName());
            SetQueuedOrder(std::nullopt);
            Note(note, CL_S_UNREACHED);
            if (InManeuver() && m_ManeuverComposed)
            {
                m_ManeuverComposed = false;
                EndManeuver("could not get in reach, the maneuver ends");
            }
            return;
        }
        return;
    }
    if (m_OrderApproaching)
    {
        m_OrderApproaching = false; // in reach: the walk in is over, the order fires below
        POwner->PAI->PathFind->Clear();
    }

    // The pacer: it goes out the first tick the server would take it, her
    // cast under way or its recast waited out however long (nothing in her
    // line is let go for its timing), and never ahead of a weapon skill held
    // behind its opener, which the combat tick fires first
    if (!ReadyToAct() || m_HeldWs.has_value())
    {
        return;
    }

    auto* PTarget = target.resolve<CBattleEntity>();
    if (PTarget == nullptr)
    {
        SetQueuedOrder(std::nullopt);
        return;
    }

    // The action's own target rules decide whether a corpse is valid. A
    // Raise ordered while resting waits here through the same stand gate.
    // Stamped as started first: a refusal the game gives as she tries it is
    // this order's (ToldAfterOrder)
    OrderStarted(key, kind, id);
    const auto status = TryAction(kind, mode, id, target);
    if (status == CL_S_ON_RECAST || status == CL_S_STANDING_UP)
    {
        OrderNotStarted();
        return; // tried again next tick, for as long as it takes
    }
    if (status == CL_S_OK)
    {
        OrderUnderWay();
        // running before the line moves on, so the one QUEUE the move sends shows it
        m_Running      = std::make_pair(key, target);
        m_RunningSince = m_Tick;
        m_RunningSeen  = Acting();
    }
    SetQueuedOrder(std::nullopt);
    if (status != CL_S_OK)
    {
        ShowInfoFmt("pawn: {} lets the queued {} go (outcome 0x{:04X})", POwner->getName(), key, status);
        if (OrderNotStarted())
        {
            auto note   = cardian::link::make<cl_note>();
            note.kind   = CL_NOTE_LET_GO;
            note.action = pawn::actionOfKey(key);
            Note(note, status);
        }
        return;
    }
    // Started, not done: the game may still refuse it on its next step (ToldAfterOrder)
    ShowInfoFmt("pawn: {} starts the queued {} on {}", POwner->getName(), key, PTarget->getName());
    NoteOrderFired();
}

auto CPawnController::CanDrawOn(CBattleEntity* PTarget) -> bool
{
    // Never left a fight: nothing to wait out
    if (m_LeftFightAt == timer::time_point::min())
    {
        return true;
    }

    return m_LeftFightAt + pawn::reengageWait(static_cast<CCharEntity*>(POwner), PTarget, m_LastFoughtId) < m_Tick;
}

auto CPawnController::EngageFactsFor(CBattleEntity* PTarget) -> cardian::rules::EngageFacts
{
    cardian::rules::EngageFacts f;
    if (PTarget == nullptr)
    {
        return f;
    }
    f.exists     = true;
    f.alive      = !PTarget->isDead();
    f.retreating = m_Retreat;
    const auto* PMob = dynamic_cast<const CMobEntity*>(PTarget);
    f.underground = PMob != nullptr ? pawn::isUnderground(PMob) : PTarget->PAI->IsUntargetable();
    f.hostile     = PTarget->objtype == TYPE_MOB && PTarget->allegiance == xi::Allegiance::Mob;
    f.claimable   = static_cast<CCharEntity*>(POwner)->IsMobOwner(PTarget);
    f.cooldown    = !CanDrawOn(PTarget);
    f.distance    = distance(POwner->loc.p, PTarget->loc.p);
    return f;
}

void CPawnController::SayRefusal(const CBattleEntity* PTarget, const std::string& why)
{
    if (m_RefusedTarget == PTarget->id && m_RefusedWhy == why)
    {
        return;
    }
    m_RefusedTarget = PTarget->id;
    m_RefusedWhy    = why;
    ShowInfoFmt("pawn: {} does not draw on {} ({})", POwner->getName(), PTarget->getName(), why);
}

auto CPawnController::Refusal(CBattleEntity* PTarget, const cardian::rules::EngageFacts& facts) const -> std::string
{
    const auto verdict = cardian::rules::mayFight(facts);
    if (!verdict && !cardian::rules::worthWalkingIn(facts))
    {
        return verdict.why;
    }
    if (!PTarget->PAI->IsEngaged() && !pawn::huntRulesOf(pawn::ordersOwnerOf(static_cast<const CCharEntity*>(POwner))).aggressive)
    {
        if (const auto* PMob = dynamic_cast<const CMobEntity*>(PTarget); PMob != nullptr)
        {
            return PullBlocker(PMob);
        }
    }
    return "";
}

auto CPawnController::Draw(CBattleEntity* PTarget, const ApproachKind kind, const std::string_view how, const bool hold) -> bool
{
    // Resting on his order she takes no fight, the party's or her own
    // defence: only his own engage order ends it (EngageOn). Her own Rest
    // row's order is no such hold: a fight calls her up, and she kneels
    // again after it (RestTick)
    if (m_RestOrder.active() && !m_RestOrder.byRow && kind != ApproachKind::Order)
    {
        SayRefusal(PTarget, fmt::format("resting until {}% on the player's order", m_RestOrder.percent));
        return false;
    }
    const auto facts = EngageFactsFor(PTarget);
    if (const auto why = Refusal(PTarget, facts); !why.empty())
    {
        SayRefusal(PTarget, why);
        return false;
    }
    if (kind == ApproachKind::Order && TowsAtStake())
    {
        // An explicit Engage order asks her to close now, even outside
        // automatic camp admission. It never waits for a puller's handoff.
        m_ReceiveMob = EntityId(PTarget);
        m_Receive = {};
        m_Receive.joined = true;
        m_KeepCampFightSpot = false;
        m_CampSettlement = {};
    }
    // A fight she attends (AttendsFight: her rows offer the fight spells,
    // no Attack row of hers claims the mob, and it is not the mob the
    // player's own Attack named) is taken attending, whatever
    // else brought her to the door, the party's engage chord included:
    // distance and the draw cooldown are the melee herd's
    // business, not hers. Attending needs a place to keep cure range to,
    // the player or a stake; without one she draws like anyone on what
    // reaches this door -- an order, the world's own engagement, a row of
    // hers (the engage door hands her no party's fight then). A pull she
    // chose (the hunt) is hers, and she draws on it: an attend there would
    // let it go again. Already drawn, she sheathes first
    if (kind != ApproachKind::Hunt && AttendsFight(PTarget) && (GetAnchor() != nullptr || Staked()))
    {
        m_RefusedTarget = 0;
        m_Approach.reset();
        m_HoldForPlayer = false;
        if (POwner->PAI->IsEngaged())
        {
            POwner->PAI->Internal_Disengage();
        }
        m_AttendedOrdered = kind == ApproachKind::Order;
        Attend(PTarget, how);
        return true;
    }

    // At a camp a damage dealer waits for the pull to come in, as the tank
    // does (CampReceive): until the mob reaches the landing point, turns on
    // her, or stalls outside the camp, she keeps her seat with her weapon
    // away, and only then walks in and draws (the user, 2026-10-04, #253:
    // melee ran out to meet his pull and opened with weapon skills). The
    // tank has her own receive; an order or her own pull closes at once
    if (WaitsForThePull(PTarget, kind))
    {
        if (!m_Approach.has_value() || m_Approach->target.resolve<CBattleEntity>() != PTarget)
        {
            m_Approach = Approach{ EntityId(PTarget), kind };
            Transition(Mode::Approach, fmt::format("waits for {} to come in to the camp", PTarget->getName()));
        }
        return false;
    }

    const auto verdict = cardian::rules::mayFight(facts);
    if (verdict)
    {
        m_RefusedTarget = 0;
        StandFromRest("drawing her weapon");
        if (!RestAllowsAction())
        {
            // Retain the target while the body finishes kneeling and rising.
            m_Approach = Approach{ EntityId(PTarget), kind };
            Transition(Mode::Approach, "standing to engage");
            return false;
        }
        // Sneak Attack on her: its back first, weapon away (TakesBackFirst;
        // the approach walks her there and draws). Not while holding for
        // the player's strike, which keeps her out of reach anyway
        if (!hold && !POwner->PAI->IsEngaged() && TakesBackFirst(PTarget))
        {
            if (!m_Approach.has_value() || m_Approach->target.resolve<CBattleEntity>() != PTarget)
            {
                m_Approach = Approach{ EntityId(PTarget), kind };
                Transition(Mode::Approach, fmt::format("takes {}'s back before she draws", PTarget->getName()));
            }
            return false;
        }
        m_Approach.reset();
        if (POwner->PAI->IsEngaged())
        {
            POwner->PAI->Internal_ChangeTarget(EntityId(PTarget));
        }
        else
        {
            POwner->PAI->Internal_Engage(EntityId(PTarget));
        }
        m_HoldForPlayer = hold;
        Transition(hold ? Mode::Hold : Mode::Fight, fmt::format("draws on {} ({})", PTarget->getName(), how));
        return true;
    }

    // Only the distance, or the draw's own wait, in the way: she walks in
    // with her weapon away and draws when the rules allow (the approach
    // in DoRoamTick). The walk is said once; the door's beat was served,
    // so it starts at once
    if (cardian::rules::worthWalkingIn(facts))
    {
        // A draw that would hold for his strike, near enough and waiting on
        // nothing but her own draw cooldown, is not walked into: the hold
        // keeps her back by him anyway. She stays, and the door draws her
        // where she stands once the wait is served
        if (hold && cardian::rules::onlyCooldown(facts))
        {
            ShowInfoFmt("pawn: {} waits out her draw cooldown before holding on {}", POwner->getName(), PTarget->getName());
            return false;
        }
        if (!m_Approach.has_value() || m_Approach->target.resolve<CBattleEntity>() != PTarget)
        {
            m_Approach = Approach{ EntityId(PTarget), kind };
            Transition(Mode::Approach, kind == ApproachKind::Join && TowsAtStake() ? fmt::format("waits for {} at the stake ({})", PTarget->getName(), verdict.why) :
                                                                                     fmt::format("walks in on {} ({})", PTarget->getName(), verdict.why));
        }
        return false;
    }

    SayRefusal(PTarget, verdict.why);
    return false;
}

auto CPawnController::PullBlocker(const CMobEntity* PMob) const -> std::string
{
    // The party's own mob is no danger to the pull (exclude), and the
    // circles are the pick's: every danger, worst case, padded
    const auto dangers = pawn::danger::around(pawn::entitiesAround(POwner), POwner->loc.p, settings::get<float>("pawn.AVOID_SCAN"),
                                              pawn::danger::Profile::party(static_cast<CCharEntity*>(POwner)), PMob);
    // Only the guards that matter to the way in (forWalk): a mob behind a
    // wall is no company
    const auto seen  = pawn::danger::forWalk(dangers, POwner->loc.p, PMob->loc.p, [this](const auto& d, const auto& p) { return Sees(d, p); });
    const auto block = cardian::rules::pullBlocked(cardian::rules::padded(seen), POwner->loc.p.x, POwner->loc.p.z, PMob->loc.p.x, PMob->loc.p.z);
    if (!block.has_value())
    {
        return "";
    }
    const auto* guard = seen[block->circle].mob;
    return block->targetInside ? fmt::format("inside {}'s circle", guard->getName())
                               : fmt::format("the way in crosses {}'s circle", guard->getName());
}

void CPawnController::RefreshDangers(const CBattleEntity* PIgnore)
{
    m_Dangers.clear();
    m_SightMemo.clear();
    if (!IsAvoiding())
    {
        return;
    }
    // The party's own mob is never a danger to keep out of: a freshly
    // pulled aggressive mob is not fighting anyone yet, and its circle
    // would hold her at the rim of the very mob she is meant to hit, or
    // walk up to
    auto* PPawn   = static_cast<CCharEntity*>(POwner);
    auto  profile = pawn::danger::Profile::of(PPawn, IsAvoidingAggro(), IsAvoidingLinks());
    // A spell held for magic aggro (#77): the circles count as though she
    // were casting, so the walk takes her clear of them before she does
    profile.casting = profile.casting || m_Tick < m_MagicHoldUntil;
    m_Dangers       = pawn::danger::around(pawn::entitiesAround(POwner), POwner->loc.p, settings::get<float>("pawn.AVOID_SCAN"), profile, PIgnore);
}

auto CPawnController::MagicHearer(const CBattleEntity* PFight) -> CMobEntity*
{
    // The mob whose circle holds her only because she would be casting:
    // its magic detection, not its eyes or ears, would bring it. Nothing
    // when she does not avoid aggro; the fight's own mob is no danger
    if (!IsAvoidingAggro())
    {
        return nullptr;
    }
    auto* PPawn    = static_cast<CCharEntity*>(POwner);
    auto  quiet    = pawn::danger::Profile::of(PPawn, true, false);
    quiet.casting  = false;
    auto loud      = quiet;
    loud.casting   = true;
    const auto  me = POwner->loc.p;
    const float scan     = settings::get<float>("pawn.AVOID_SCAN");
    auto*       entities = pawn::entitiesAround(POwner);
    const auto  heard    = pawn::danger::around(entities, me, scan, loud, PFight);
    const auto  seen     = pawn::danger::around(entities, me, scan, quiet, PFight);
    for (const auto& d : heard)
    {
        const bool inside = cardian::formation::depthInside(d, me.x, me.z) > 0.0f;
        const bool anyway = std::any_of(seen.begin(), seen.end(), [&](const auto& q) { return q.mob == d.mob && cardian::formation::depthInside(q, me.x, me.z) > 0.0f; });
        if (inside && !anyway)
        {
            return d.mob;
        }
    }
    return nullptr;
}

void CPawnController::HoldForMagic(CMobEntity* PHears, const std::string_view spell)
{
    // Held: her danger map counts the magic circles for a while after each
    // held cast, so she steps clear and does not walk straight back in for
    // range once a cast has gone through; only a cast actually held renews
    // it. Held again and again for this long, nowhere clear and in range can
    // be had, and the party is told once a minute (the user, 2026-10-04)
    constexpr auto kHold   = std::chrono::seconds(10);
    constexpr auto kStuck  = std::chrono::seconds(8);
    const bool     stretch = m_MagicHold.resolve<CMobEntity>() == PHears && m_Tick < m_MagicHoldUntil;
    if (!stretch)
    {
        m_MagicHoldSince = m_Tick;
        ShowInfoFmt("pawn: {} holds {}: {} would hear her casting here (magic detection {} y); she steps clear first", POwner->getName(), spell,
                    PHears->getName(), PHears->getMobMod(xi::MobMod::MagicRange));
    }
    m_MagicHold      = EntityId(PHears);
    m_MagicHoldUntil = m_Tick + kHold;
    if (stretch && m_Tick - m_MagicHoldSince >= kStuck && (m_MagicSaidAt == timer::time_point{} || m_Tick - m_MagicSaidAt >= std::chrono::seconds(60)))
    {
        m_MagicSaidAt = m_Tick;
        std::string name(PHears->getName());
        std::replace(name.begin(), name.end(), '_', ' ');
        pawn::tactics::role::sayParty(static_cast<CCharEntity*>(POwner), fmt::format("The {} would hear my spells here, so I'm holding them until I can step clear.", name));
    }
}

auto CPawnController::ReachOf(CMobEntity* PMob) -> cardian::perimeter::Reach
{
    // The live skill list -- a family script swaps it mid-fight -- read
    // once per list per mob
    const auto listId = static_cast<uint16>(PMob->getMobMod(xi::MobMod::SkillList));
    if (m_Reach.mob == PMob->id && m_Reach.list == listId)
    {
        return m_Reach.reach;
    }
    const auto&                           ids = battleutils::GetMobSkillList(listId);
    std::vector<cardian::perimeter::Move> moves;
    moves.reserve(ids.size());
    for (const auto id : ids)
    {
        if (auto* PSkill = battleutils::GetMobSkill(id); PSkill != nullptr)
        {
            moves.push_back({ .aoe      = PSkill->getAoe(),
                              .distance = PSkill->getDistance(),
                              .radius   = PSkill->getRadius(),
                              .hostile  = (PSkill->getValidTargets() & TARGET_ENEMY) != 0,
                              .name     = PSkill->getName() });
        }
    }
    m_Reach = { PMob->id, listId, cardian::perimeter::reachOf(moves, PMob->GetMeleeRange(POwner), settings::get<float>("pawn.PERIMETER_MARGIN")) };
    return m_Reach.reach;
}

auto CPawnController::CastRange() const -> float
{
    // The cure's range from the spell table, which every tier shares; read
    // once
    static const float range = []
    {
        const auto* PSpell = spell::GetSpell(SpellID::Cure);
        return PSpell != nullptr ? PSpell->getRange() : 20.0f;
    }();
    return range;
}

auto CPawnController::AttendIntent(CMobEntity* PMob, const Place* place) -> Intent
{
    // Out of the mob's reach, in cure range of the tank: a crescent of safe
    // spots. Out of it she walks to its nearest point; in it she holds
    // where she stands, so the tank's small moves never drag her round the
    // fight (the user, 2026-09-17). The mob on her lifts the ring: running
    // with a mob on her is kiting, and the emergency cure covers her HP
    RestoreNormalSpeed();
    m_HasSlot = false; // a crescent point is no formation slot for the vet to re-seat
    Intent intent;
    intent.kind = Intent::Kind::Stand;

    // The milestone log: the crescent's verdict, said when it changes, in
    // numbers (the user, 2026-09-17: the timing of the shifts, no flavour)
    const auto note = [&](const uint8 verdict, const std::string& text)
    {
        if (m_AttendVerdict != verdict)
        {
            m_AttendVerdict = verdict;
            ShowInfoFmt("pawn: {} attend: {}", POwner->getName(), text);
        }
    };

    const CBattleEntity* PTank = PMob->GetBattleTarget();
    if (PTank == POwner)
    {
        note(1, fmt::format("{} is on her: stands", PMob->getName()));
        return intent;
    }
    // Whoever the mob is on, else the player: her cure target and the side
    // she keeps to
    if (PTank == nullptr)
    {
        PTank = GetLivePlayer();
    }
    if (PTank == nullptr || PTank == POwner)
    {
        note(2, "nobody to keep cure range to: stands");
        return intent;
    }

    if (place != nullptr && place->fixed())
    {
        return CampAttendIntent(PMob, *place, PTank);
    }

    // The spell's own range to the tank, stretched: the hitboxes are the
    // slack, and a landing check that fails is on the log's record (the
    // user, 2026-09-17). Distances planar, as the spot finder's are
    const position_t& me        = POwner->loc.p;
    const position_t& mob       = PMob->loc.p;
    const position_t& tank      = PTank->loc.p;
    const float       range     = CastRange();
    const auto        reach     = ReachOf(PMob);
    const float       mobToTank = distance(mob, tank, true);
    const float       ring      = cardian::perimeter::ringOf(reach, mobToTank);
    m_AttendRing                = ring;
    const float       toMob     = distance(me, mob, true);
    const float       toTank    = distance(me, tank, true);
    // The aim sits inside the crescent's edges, past the walker's stop
    // distance so arriving short stays safe. Thin crescents keep usable
    // width between the padded edges and use a tighter arrival tolerance.
    constexpr float kInset = 2.5f;
    const float     width  = mobToTank + range - ring; // along the ray from the mob through the tank
    const float     inset  = cardian::perimeter::crescentInset(width);

    // What her spot keeps when the claim board slides it off another
    // member's (ClaimSpot): round the mob at its distance, so it stays
    // outside the reach, and within cure range of the tank, the inset's
    // half her cover for arriving short
    const auto purpose = [&](Intent& i, const float within)
    {
        i.slideRound = mob;
        i.keepTo     = tank;
        i.keepWithin = within;
    };
    const float keep = range - inset / 2.0f;

    // The attending mages before her in the party order, on this mob: she
    // stands with them, a body's width apart (the user, 2026-10-03: the
    // mages stick together, never on one pixel). The first one's claim
    // seeds a later mage's spot, so the two never take opposite sides of
    // the fight (#250)
    const CCharEntity*                  PFirst = nullptr;
    position_t                          firstAt{};
    std::vector<cardian::claims::Point> mates;
    for (const auto& claim : ClaimsBefore())
    {
        const auto* PPeer = claim.PMember->PAI != nullptr ? dynamic_cast<const CPawnController*>(claim.PMember->PAI->GetController()) : nullptr;
        if (PPeer != nullptr && PPeer->Attending(PMob))
        {
            if (PFirst == nullptr)
            {
                PFirst  = claim.PMember;
                firstAt = claim.at;
            }
            mates.push_back({ claim.at.x, claim.at.z });
        }
    }
    // Together is within kTogether of one of them; walking to the first
    // one's side, she walks on until she is beside her
    const bool inCrescent = toMob >= ring && toTank <= range;
    const bool together   = mates.empty() || cardian::claims::together({ me.x, me.z }, mates, m_Joining ? cardian::claims::kBeside : cardian::claims::kTogether);
    m_Joining             = false;

    // In the crescent and with the mages before her, she holds where she
    // stands, so the tank's small moves never drag her round the fight
    const auto stands = [&]
    {
        note(3, fmt::format("in the crescent: stands (mob {:.1f} y, ring {:.1f}; tank {} {:.1f} y, cure {:.0f})", toMob, ring, PTank->getName(), toTank, range));
        intent.holdsSpot = true;
        purpose(intent, keep);
        return intent;
    };
    if (inCrescent && together)
    {
        return stands();
    }
    // The search's seed: the spot of the crescent nearest to it. Where she
    // stands, following; at a place with a heading, its backline -- the 6
    // o'clock behind the stake at the ring's distance, 90 degrees from the
    // tank's 3 o'clock and out of the mob's cones (the user, 2026-09-17).
    // Following, a later mage's seed is the first mage's claim, and the
    // board takes her beside it; a fight's far side is reached round the
    // ring, a leg at a time, never through the mob's reach
    const auto backline = place != nullptr ? place->behind(ring) : std::nullopt;
    if (!backline.has_value() && PFirst != nullptr)
    {
        if (const auto spot = cardian::perimeter::safeSpot(mob.x, mob.z, tank.x, tank.z, ring + inset, range - inset, firstAt.x, firstAt.z); spot.has_value())
        {
            Intent join;
            join.kind  = Intent::Kind::Path;
            join.point = position_t(spot->first, me.y, spot->second, 0, me.rotation);
            purpose(join, keep);
            // Beside her, or nowhere: with no room near her, she seeks her
            // own spot as a mage alone would
            if (const auto room = GiveWay(join.point, join); room.PBy == nullptr || room.to.has_value())
            {
                // Round the tank's side when the crescent does not reach
                // behind the mob: the far side is out of cure range
                constexpr float      kLegTurn = std::numbers::pi_v<float> / 6.0f;
                const position_t     beside   = room.to.value_or(join.point);
                std::optional<float> never;
                if (mobToTank > 0.01f && ring + inset + mobToTank > range - inset)
                {
                    never = std::atan2(mob.z - tank.z, mob.x - tank.x);
                }
                const auto leg = cardian::claims::roundTheRing({ mob.x, mob.z }, { me.x, me.z }, { beside.x, beside.z }, ring, kLegTurn, never);
                const bool       aLeg    = std::hypot(leg.x - beside.x, leg.z - beside.z) > 0.01f;
                m_Joining                = true;
                intent.kind              = Intent::Kind::Path;
                intent.point             = aLeg ? position_t(leg.x, me.y, leg.z, 0, me.rotation) : beside;
                intent.arrive            = std::min(intent.arrive, room.to.has_value() ? cardian::claims::kArrive : inset / 2.0f);
                intent.tolerance         = std::min(intent.tolerance, room.to.has_value() ? cardian::claims::kArrive : inset / 2.0f);
                purpose(intent, keep);
                if (aLeg)
                {
                    note(11, fmt::format("goes round {} to {}'s side: walks a leg to ({:.1f}, {:.1f}), for ({:.1f}, {:.1f}) (mob {:.1f} y, ring {:.1f}; tank {} {:.1f} y)", PMob->getName(), PFirst->getName(),
                                         leg.x, leg.z, beside.x, beside.z, toMob, ring, PTank->getName(), toTank));
                }
                else
                {
                    note(10, fmt::format("joins {}: walks to ({:.1f}, {:.1f}), {:.1f} y from {}'s claim (mob {:.1f} y, ring {:.1f}; tank {} {:.1f} y, cure {:.0f})", PFirst->getName(), beside.x, beside.z,
                                         distance(beside, firstAt, true), PFirst->getName(), toMob, ring, PTank->getName(), toTank, range));
                }
                return intent;
            }
        }
    }
    if (inCrescent)
    {
        return stands();
    }
    const position_t seed = backline.value_or(me);
    if (const auto spot = cardian::perimeter::safeSpot(mob.x, mob.z, tank.x, tank.z, ring + inset, range - inset, seed.x, seed.z); spot.has_value())
    {
        intent.kind      = Intent::Kind::Path;
        intent.point     = position_t(spot->first, me.y, spot->second, 0, me.rotation);
        intent.arrive    = std::min(intent.arrive, inset / 2.0f);
        intent.tolerance = std::min(intent.tolerance, inset / 2.0f);
        purpose(intent, keep);
        note(4, fmt::format("out of the crescent: walks to ({:.1f}, {:.1f}){} (mob {:.1f} y, ring {:.1f}; tank {} {:.1f} y, cure {:.0f}; width {:.1f}, inset {:.1f})",
                            spot->first, spot->second, backline.has_value() ? fmt::format(", the spot nearest the backline ({:.1f}, {:.1f})", backline->x, backline->z) : "",
                            toMob, ring, PTank->getName(), toTank, range, width, inset));
        return intent;
    }

    // The crescent is empty: no spot clears the reach and keeps cure range.
    // She stays in, as close to the tank as the range asks and no closer,
    // said once per fight naming the move that binds; only cure range moves
    // her from here, so the tank's shuffle never does
    if (m_SaidNoSpotFor != PMob->id)
    {
        m_SaidNoSpotFor      = PMob->id;
        const bool  byTank   = reach.target > 0.0f && mobToTank + reach.target > reach.mob;
        const auto  named    = byTank ? reach.targetBy : reach.mobBy;
        std::string move     = named.empty() ? std::string("its melee") : std::string(named);
        std::replace(move.begin(), move.end(), '_', ' ');
        if (!named.empty())
        {
            move[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(move[0])));
        }
        const float moveRange = (byTank ? reach.target : reach.mob) - settings::get<float>("pawn.PERIMETER_MARGIN");
        ShowInfoFmt("pawn: {}: {} reaches {:.0f} y round {}; no spot with walking clearance within her cure range ({:.0f} y): she stays in", POwner->getName(), move, moveRange,
                    byTank ? PTank->getName() : PMob->getName(), range);
        pawn::tactics::role::sayParty(static_cast<CCharEntity*>(POwner), fmt::format("{} reaches {:.0f} yalms; I can't safely stay clear and keep curing, so I'm staying in.", move, moveRange));
    }
    if (toTank > range)
    {
        // Straight back the way she faces the mob when the tank stands on it
        const float fallback = 2.0f * std::numbers::pi_v<float> - rotationToRadian(me.rotation) + std::numbers::pi_v<float>;
        const auto [x, z]    = cardian::perimeter::atRange(mob.x, mob.z, tank.x, tank.z, mobToTank + range - kInset, fallback);
        intent.kind          = Intent::Kind::Path;
        intent.point         = position_t(x, me.y, z, 0, me.rotation);
        purpose(intent, range);
        note(5, fmt::format("crescent empty (width {:.1f}): walks in to cure range at ({:.1f}, {:.1f}) (mob {:.1f} y, ring {:.1f}; tank {} {:.1f} y)", width, x, z, toMob, ring, PTank->getName(), toTank));
        return intent;
    }
    note(6, fmt::format("crescent empty (width {:.1f}): stays in (mob {:.1f} y, ring {:.1f}; tank {} {:.1f} y, cure {:.0f})", width, toMob, ring, PTank->getName(), toTank, range));
    intent.holdsSpot = true;
    purpose(intent, range);
    return intent;
}

auto CPawnController::RearCampRoute(const position_t& point, const position_t& camp) const -> std::optional<std::vector<pathpoint_t>>
{
    const auto forward = [&](const position_t& p) { return cardian::stake::forwardOf(camp.x, camp.z, camp.rotation, p.x, p.z); };
    if (forward(point) > 0.05f)
    {
        return std::nullopt;
    }
    // If avoidance left her in front, she can walk back. Once behind the
    // line, an AoE reposition cannot route round a wall through the front.
    const float limit   = std::max(0.0f, forward(POwner->loc.p)) + 0.05f;
    auto*       navMesh = meshOf(POwner->loc.zone);
    if (navMesh == nullptr)
    {
        return std::vector<pathpoint_t>{ pathpoint_t{ .position = point, .wait = {}, .setRotation = false } };
    }
    auto path = routeOf(navMesh, POwner, POwner->loc.p, point);
    if (!path.has_value() || path->isPartial)
    {
        return std::nullopt;
    }
    for (const auto& step : path->points)
    {
        if (forward(step.position) > limit)
        {
            return std::nullopt;
        }
    }
    return std::move(path->points);
}

auto CPawnController::CampAttendIntent(CMobEntity* PMob, const Place& place, const CBattleEntity* PTank) -> Intent
{
    const auto camp      = place.position();
    const bool preparing = !PMob->PAI->IsEngaged();
    // An unpulled mob is not yet an AoE source at its current position.
    // Prepare for the camp's landing point; use live geometry once it fights.
    position_t mob  = preparing ? nearPosition(camp, cardian::stake::kMobAhead, 0.0f) : PMob->loc.p;
    position_t tank = PTank->loc.p;
    if (preparing)
    {
        const float reach = std::max(1.0f, PMob->GetMeleeRange(POwner) - 0.3f);
        tank = nearPosition(mob, reach, std::numbers::pi_v<float> / 2.0f);
    }
    const float ring = cardian::perimeter::ringOf(ReachOf(PMob), distance(mob, tank, true));
    m_CampRing       = ring; // what the camp's next pull is planned for (CampWaitIntent)
    m_AttendRing     = ring;
    return CampSpot(place, mob, tank, ring, preparing ? "arrival" : "live");
}

auto CPawnController::CampSpot(const Place& place, const position_t& mob, const position_t& tank, const float ring, const std::string_view geometry) -> Intent
{
    const auto  camp   = place.position();
    const auto  me     = POwner->loc.p;
    const float range  = CastRange();
    const float radius = std::clamp(ring + 1.0f - cardian::stake::kMobAhead, 3.0f, std::max(3.0f, range - 2.0f - cardian::stake::kMobAhead));
    auto*       navMesh = meshOf(POwner->loc.zone);
    const auto clipped = [&](const position_t& point) -> std::optional<position_t>
    {
        if (navMesh == nullptr)
        {
            return point;
        }
        const auto end = navMesh->findFurthestValidPoint(camp, point);
        return end.has_value() ? std::optional<position_t>(*end) : std::nullopt;
    };
    const auto  forward   = [&](const position_t& p) { return cardian::stake::forwardOf(camp.x, camp.z, camp.rotation, p.x, p.z); };
    const auto  rear      = clipped(nearPosition(camp, radius, std::numbers::pi_v<float>));
    const float rearDepth = rear.has_value() ? std::max(0.0f, -forward(*rear)) : radius;
    const auto cost = [&](const position_t& p)
    {
        const float side = cardian::stake::forwardOf(camp.x, camp.z, static_cast<uint8>(camp.rotation + 64), p.x, p.z);
        return cardian::perimeter::campCost(forward(p), side, rearDepth, distance(p, mob, true), ring, distance(p, tank, true), range);
    };

    // Small rear arc search; mesh clipping naturally compresses it against
    // a wall. Staying is always an option, including when every spot has AoE.
    struct Candidate
    {
        position_t point;
        float      score;
    };
    std::vector<Candidate> candidates;
    candidates.reserve(46);
    const auto add = [&](const position_t& raw)
    {
        const auto point = clipped(raw);
        if (point.has_value() && forward(*point) <= 0.05f && IsClear(point->x, point->z))
        {
            candidates.push_back({ *point, cost(*point) });
        }
    };
    for (const float depth : { radius, radius + 4.0f, radius + 8.0f, radius * 0.75f, radius * 0.5f })
    {
        for (int turn = -4; turn <= 4; ++turn)
        {
            add(nearPosition(camp, depth, std::numbers::pi_v<float> + turn * std::numbers::pi_v<float> / 8.0f));
        }
    }
    if (const auto nearest = cardian::perimeter::safeSpot(mob.x, mob.z, tank.x, tank.z, ring + 1.0f, range - 1.0f, me.x, me.z); nearest.has_value())
    {
        add(position_t(nearest->first, me.y, nearest->second, 0, me.rotation));
    }
    std::stable_sort(candidates.begin(), candidates.end(), [](const auto& a, const auto& b) { return a.score < b.score; });
    const float currentCost = cost(me);
    float best = currentCost - 1.5f;
    Intent intent;
    for (const auto& candidate : candidates)
    {
        if (candidate.score >= best)
        {
            break;
        }
        const auto route = RearCampRoute(candidate.point, camp);
        if (!route.has_value())
        {
            continue;
        }
        float      walk     = 0.0f;
        position_t previous = me;
        for (const auto& step : *route)
        {
            walk += distance(previous, step.position);
            previous = step.position;
        }
        const float score = candidate.score + 0.25f * walk;
        if (score < best && cardian::perimeter::worthwhileCampMove(currentCost, candidate.score, walk))
        {
            best                = score;
            intent.kind         = Intent::Kind::Path;
            intent.point        = candidate.point;
            intent.arrive       = 0.5f;
            intent.tolerance    = 0.75f;
            intent.rearBoundary = camp;
        }
    }
    // Every mage of the camp's search picks the same best spot: the claim
    // board (ClaimSpot) slides a later one's off an earlier one's, round
    // the mob at its distance, behind the flag and in cure range, and
    // slides her off a spot she holds that an earlier mage has come to
    intent.slideRound   = mob;
    intent.keepTo       = tank;
    intent.keepWithin   = range;
    intent.rearBoundary = camp;
    intent.holdsSpot    = intent.kind == Intent::Kind::Stand;
    const bool  moves   = intent.kind == Intent::Kind::Path;
    const bool  exposed = distance(me, mob, true) < ring;
    const uint8 verdict = moves ? 7 : exposed ? 8 : 9;
    if (!geometry.empty() && m_AttendVerdict != verdict)
    {
        m_AttendVerdict = verdict;
        ShowInfoFmt("pawn: {} camp backline: {}{} (mob {:.1f} y, ring {:.1f}; cure {:.1f}/{:.0f}; {} geometry)", POwner->getName(),
                    moves ? fmt::format("repositions to ({:.1f}, {:.1f})", intent.point.x, intent.point.z) : "holds her spot",
                    !moves && exposed ? "; accepts AoE exposure" : "", distance(me, mob, true), ring, distance(me, tank, true), range, geometry);
    }
    return intent;
}

auto CPawnController::WaitsAtCampSpot() const -> bool
{
    return pawn::tactics::offersSpells(POwner) && !TakesFights();
}

auto CPawnController::CampWaitIntent(const Place& place) -> Intent
{
    // A kneel is planned to last into the next pull, so it is taken where
    // that pull will be attended from: she walks there, then kneels, and
    // rises close to her place in the fight (the user, 2026-10-03)
    m_HasSlot       = false; // a camp spot is no formation slot for the vet to re-seat
    RestoreNormalSpeed();    // no catch-up sprint carried onto her walk to it
    const auto camp = place.position();
    // The pull lands at the flag (kMobAhead), its tank at its 3 o'clock at
    // an ordinary mob's melee reach; its AoE the last ring seen at a camp,
    // else an ordinary melee mob's TP reach. A search that finds nothing
    // better than where she stands keeps her there
    constexpr float kOrdinaryReach = 3.0f;
    constexpr float kOrdinaryRing  = 12.0f;
    const auto      landing        = nearPosition(camp, cardian::stake::kMobAhead, 0.0f);
    if (!m_CampWaitPoint.has_value())
    {
        const auto  tank  = nearPosition(landing, kOrdinaryReach, std::numbers::pi_v<float> / 2.0f);
        const float ring  = m_CampRing > 0.0f ? m_CampRing : kOrdinaryRing;
        const auto  spot  = CampSpot(place, landing, tank, ring, "");
        m_CampWaitPoint   = spot.kind == Intent::Kind::Path ? spot.point : POwner->loc.p;
        m_CampWaitBest    = std::numeric_limits<float>::max();
        m_CampWaitBestAt  = m_Tick;
        ShowInfoFmt("pawn: {} plans to wait for the camp's pulls at ({:.1f}, {:.1f}), {:.1f} y from the flag (ring {:.1f}{})", POwner->getName(),
                    m_CampWaitPoint->x, m_CampWaitPoint->z, distance(*m_CampWaitPoint, camp, true), ring, m_CampRing > 0.0f ? ", the last seen at a camp" : ", an ordinary mob's");
    }
    // The mages wait together, never on one spot (#250): every mage's
    // search picks the same best spot, and the claim board slides a plan
    // that a member before her has claimed -- where she stands, where she
    // is going, or the spot she plans to wait on -- round the flag at its
    // distance, behind it and in cure range of the landing point. The
    // plan moves with it, so the walk there is judged on the spot she
    // will stand on
    const auto withPurpose = [&](Intent& i)
    {
        i.slideRound   = camp;
        i.keepTo       = landing;
        i.keepWithin   = CastRange();
        i.rearBoundary = camp;
    };
    Intent plan;
    plan.kind  = Intent::Kind::Path;
    plan.point = *m_CampWaitPoint;
    withPurpose(plan);
    ClaimSpot(plan);
    if (distance(plan.point, *m_CampWaitPoint, true) > 0.01f)
    {
        m_CampWaitPoint  = plan.point;
        m_CampWaitSlidTo = plan.point;
        m_CampWaitBest   = std::numeric_limits<float>::max();
        m_CampWaitBestAt = m_Tick;
    }
    // A slid plan is walked to as closely as any slide: arriving short
    // still leaves her clear of the claim it gave way to
    const bool  slid      = m_CampWaitSlidTo.has_value() && distance(*m_CampWaitSlidTo, *m_CampWaitPoint, true) < 0.01f;
    const float arrive    = slid ? cardian::claims::kArrive : 0.5f;
    const float tolerance = slid ? cardian::claims::kArrive : 0.75f;
    // Standing, she walks there before she kneels; kneeling when the camp
    // was set, she rests on where she is and walks over once topped up
    // (the walk is a routine step her rest puts off)
    Intent intent;
    const float away = distance(POwner->loc.p, *m_CampWaitPoint, true);
    if (away > tolerance)
    {
        // Best effort, no cooldown: a walk that gains nothing for a few
        // seconds -- no route from where she stands -- ends where she is,
        // which becomes her spot. A kneel puts the walk off, and is no stall
        if (POwner->StatusEffectContainer->HasStatusEffect(xi::StatusEffect::Healing) || away < m_CampWaitBest - 0.5f)
        {
            m_CampWaitBest   = std::min(m_CampWaitBest, away);
            m_CampWaitBestAt = m_Tick;
        }
        else if (m_Tick - m_CampWaitBestAt > 3s)
        {
            ShowInfoFmt("pawn: {} cannot get to her camp spot ({:.1f} y off): she waits where she stands", POwner->getName(), away);
            m_CampWaitPoint = POwner->loc.p;
            return intent;
        }
        intent.kind      = Intent::Kind::Path;
        intent.point     = *m_CampWaitPoint;
        intent.arrive    = arrive;
        intent.tolerance = tolerance;
        withPurpose(intent);
    }
    return intent;
}

auto CPawnController::Sees(const pawn::danger::Danger& danger, const position_t& point) const -> bool
{
    // Memoised per tick, to the yalm: the mob's own line-of-sight cache is
    // eight entries deep and one tick's questions from a party would
    // thrash it
    const uint32 mob = danger.mob != nullptr ? danger.mob->id : 0;
    const int    qx  = static_cast<int>(std::lround(point.x));
    const int    qz  = static_cast<int>(std::lround(point.z));
    for (const auto& m : m_SightMemo)
    {
        if (m.mob == mob && m.x == qx && m.z == qz)
        {
            return m.seen;
        }
    }
    const bool seen = pawn::danger::sees(danger, point);
    m_SightMemo.push_back(SightMemo{ mob, qx, qz, seen });
    return seen;
}

void CPawnController::FocusDangers(const position_t& from, const position_t& to)
{
    const auto sight = [this](const auto& d, const auto& p) { return Sees(d, p); };
    m_ActiveDangers  = pawn::danger::forWalk(m_Dangers, from, to, sight);
    m_ActivePadded   = cardian::rules::padded(m_ActiveDangers);
    m_EscapeDangers  = pawn::danger::forWalk(m_Dangers, from, from, sight);
    m_EscapePadded   = cardian::rules::padded(m_EscapeDangers);
}

auto CPawnController::IsClear(const float x, const float z) const -> bool
{
    // A spot is clear when the walk to it, vetted as Move will vet it,
    // ends outside every padded circle that matters
    const position_t spot(x, POwner->loc.p.y, z, 0, 0);
    const auto       matter = pawn::danger::forWalk(m_Dangers, POwner->loc.p, spot, [this](const auto& d, const auto& p) { return Sees(d, p); });
    return !cardian::formation::insideAny(cardian::rules::padded(matter), x, z);
}

auto CPawnController::InsideDanger() const -> bool
{
    const auto matter = pawn::danger::forWalk(m_Dangers, POwner->loc.p, POwner->loc.p, [this](const auto& d, const auto& p) { return Sees(d, p); });
    return cardian::formation::insideAny(matter, POwner->loc.p.x, POwner->loc.p.z);
}

auto CPawnController::ApproachIntent(const CBattleEntity* PTarget) const -> Intent
{
    Intent intent;
    intent.kind      = Intent::Kind::Path;
    intent.point     = PTarget->loc.p;
    intent.arrive    = RoamDistance;
    intent.tolerance = RoamDistance;
    intent.target    = PTarget;
    intent.fighting  = true;
    return intent;
}

auto CPawnController::SpotClaims() const -> std::vector<position_t>
{
    // Walking, the walker's destination; standing, her body. A destination
    // the walker set a while ago belongs to a walk some other tick has taken
    // over (an order's route, a trek), and her body speaks for her then
    std::vector<position_t> out;
    const auto*             PPathFind = POwner->PAI != nullptr ? POwner->PAI->PathFind.get() : nullptr;
    const bool              walking   = m_WalkTo.has_value() && PPathFind != nullptr && PPathFind->IsFollowingPath() && timer::now() - m_WalkToAt < 2s;
    out.push_back(walking ? *m_WalkTo : POwner->loc.p);
    // Waiting at a camp, the spot she plans to wait on is hers too: kneeling
    // elsewhere, her rest puts the walk there off, and the spot is still taken
    if (m_CampWaitPoint.has_value() && Staked() && AttendedTarget() == nullptr && distance(*m_CampWaitPoint, out.front(), true) >= cardian::claims::kSpacing)
    {
        out.push_back(*m_CampWaitPoint);
    }
    return out;
}

auto CPawnController::ClaimsBefore() const -> std::vector<BoardClaim>
{
    // The party's cardians in her zone, alive, before her in the party
    // order: the same order for everyone, so of any two only the later
    // gives way. A cardian another controller drives (a possession) claims
    // her body
    std::vector<BoardClaim> out;
    const auto*             PParty = static_cast<const CCharEntity*>(POwner)->PParty;
    if (PParty == nullptr)
    {
        return out;
    }
    for (const auto* PMember : PParty->members)
    {
        if (PMember == POwner)
        {
            break;
        }
        const auto* PChar = dynamic_cast<const CCharEntity*>(PMember);
        if (PChar == nullptr || !pawn::isPawn(PChar) || PChar->loc.zone != POwner->loc.zone || PChar->isDead())
        {
            continue;
        }
        const auto* PPeer = PChar->PAI != nullptr ? dynamic_cast<const CPawnController*>(PChar->PAI->GetController()) : nullptr;
        if (PPeer == nullptr)
        {
            out.push_back({ PChar, PChar->loc.p });
            continue;
        }
        for (const auto& at : PPeer->SpotClaims())
        {
            out.push_back({ PChar, at });
        }
    }
    return out;
}

auto CPawnController::GiveWay(const position_t& spot, const Intent& intent) -> GaveWay
{
    namespace claims = cardian::claims;
    GaveWay    out;
    const auto before = ClaimsBefore();
    if (before.empty())
    {
        return out;
    }
    std::vector<claims::Point> board;
    board.reserve(before.size());
    for (const auto& claim : before)
    {
        board.push_back({ claim.at.x, claim.at.z });
    }

    // The mover's purpose: round the centre it names, else the party's
    // place -- the player, or the stake -- else round herself
    position_t centre = POwner->loc.p;
    if (intent.slideRound.has_value())
    {
        centre = *intent.slideRound;
    }
    else if (const auto* place = CurrentPlace(GetAnchor()); place != nullptr)
    {
        centre = place->position();
    }
    claims::Purpose purpose{ .centre = { centre.x, centre.z } };
    if (intent.keepTo.has_value())
    {
        purpose.keepTo = claims::Point{ intent.keepTo->x, intent.keepTo->z };
        purpose.within = intent.keepWithin;
    }

    // Asked again this tick on the same spot, purpose and board -- the
    // roam tick's pass, the walker's, a mover's own question -- the answer
    // is the one already worked out: a crowded spot with nothing clear
    // near would otherwise walk the mesh and the danger map for every
    // candidate each time
    const auto&                 rear = intent.rearBoundary;
    const std::array<float, 12> asked{ spot.x, spot.z, POwner->loc.p.x, POwner->loc.p.z, centre.x, centre.z,
                                       purpose.keepTo.has_value() ? purpose.keepTo->x : 0.0f, purpose.keepTo.has_value() ? purpose.keepTo->z : 0.0f,
                                       purpose.keepTo.has_value() ? purpose.within : -1.0f, rear.has_value() ? rear->x : 0.0f, rear.has_value() ? rear->z : 0.0f,
                                       rear.has_value() ? static_cast<float>(rear->rotation) : -1.0f };
    if (m_GiveWayTick != m_Tick)
    {
        m_GiveWayTick = m_Tick;
        m_GiveWayMemo.clear();
    }
    for (const auto& memo : m_GiveWayMemo)
    {
        if (memo.asked == asked && memo.board == board)
        {
            return memo.verdict;
        }
    }
    const auto remember = [&](const GaveWay& verdict)
    {
        m_GiveWayMemo.push_back({ asked, board, verdict });
        return verdict;
    };

    const auto verdict = claims::giveWay(board, board.size(), { spot.x, spot.z }, { POwner->loc.p.x, POwner->loc.p.z }, purpose);
    if (!verdict.by.has_value())
    {
        return remember(out);
    }
    out.PBy = before[*verdict.by].PMember;
    out.at  = before[*verdict.by].at;

    // The world has the last word on each candidate, nearest first: on the
    // mesh and reached along it from the spot (not past a wall or an
    // edge), behind the camp's line when the mover keeps one, and clear of
    // the danger map. A zone with no mesh takes the candidate as it is
    auto* navMesh = meshOf(POwner->loc.zone);
    for (const auto& candidate : verdict.to)
    {
        position_t point(candidate.x, spot.y, candidate.z, 0, spot.rotation);
        if (navMesh != nullptr)
        {
            const auto end = navMesh->findFurthestValidPoint(spot, point);
            if (!end.has_value() || std::hypot(end->x - point.x, end->z - point.z) > 0.3f)
            {
                continue;
            }
            point.y = end->y;
        }
        if (intent.rearBoundary.has_value() &&
            cardian::stake::forwardOf(intent.rearBoundary->x, intent.rearBoundary->z, intent.rearBoundary->rotation, point.x, point.z) > 0.05f)
        {
            continue;
        }
        if (!IsClear(point.x, point.z))
        {
            continue;
        }
        out.to = point;
        break;
    }
    return remember(out);
}

void CPawnController::ClaimSpot(Intent& intent)
{
    // The fight's own movers keep their own spacing -- a seat (the herd,
    // the mob's back, the camp tank's spot), a walk in on a mob, the
    // player's order -- and a hop is a step too short to plan. The
    // formation's followers keep their seats, which are spaced already
    // and arrived at loosely: a slide there would chase them round the
    // player as he moves. A walk to a point goes through the board, and so
    // does a stand she was placed on. Asked once: the roam tick asks before
    // the rest decides, and the walker then takes its answer
    if (intent.claimed || intent.seat || intent.target != nullptr)
    {
        return;
    }
    intent.claimed    = true;
    const bool stands = intent.kind == Intent::Kind::Stand && intent.holdsSpot;
    if (!stands && intent.kind != Intent::Kind::Path)
    {
        return;
    }
    const position_t spot = stands ? POwner->loc.p : intent.point;
    // A spot on its own centre (a walk straight at a spell's target) has no
    // circle to slide round, and the target may well be the member it meets
    if (intent.slideRound.has_value() && distance(spot, *intent.slideRound, true) < cardian::claims::kMinRadius)
    {
        return;
    }
    const auto way = GiveWay(spot, intent);
    if (way.PBy == nullptr)
    {
        return;
    }
    // Said when it changes: a new member, a new spot, or nowhere to go
    if (!way.to.has_value())
    {
        if (m_GaveWayTo != way.PBy->id || !m_GaveWayKept)
        {
            m_GaveWayTo   = way.PBy->id;
            m_GaveWayKept = true;
            ShowInfoFmt("pawn: {} gives way to {}, but nothing clear is near ({:.1f}, {:.1f}), {:.1f} y from {}'s claim: she keeps her spot", POwner->getName(), way.PBy->getName(),
                        spot.x, spot.z, distance(spot, way.at, true), way.PBy->getName());
        }
        return;
    }
    if (m_GaveWayTo != way.PBy->id || m_GaveWayKept || distance(m_GaveWayAt, *way.to, true) > 1.0f)
    {
        m_GaveWayTo   = way.PBy->id;
        m_GaveWayKept = false;
        m_GaveWayAt   = *way.to;
        ShowInfoFmt("pawn: {} gives way to {}: {} ({:.1f}, {:.1f}) is {:.1f} y from {}'s claim; slides to ({:.1f}, {:.1f}), {:.1f} y from it", POwner->getName(), way.PBy->getName(),
                    stands ? "where she stands" : "her spot", spot.x, spot.z, distance(spot, way.at, true), way.PBy->getName(), way.to->x, way.to->z, distance(*way.to, way.at, true));
    }
    // Close enough to the slid spot that arriving short leaves her clear
    intent.kind      = Intent::Kind::Path;
    intent.point     = *way.to;
    intent.arrive    = std::min(intent.arrive, cardian::claims::kArrive);
    intent.tolerance = std::min(intent.tolerance, cardian::claims::kArrive);
}

auto CPawnController::Move(Intent intent) -> std::optional<AvoidAction>
{
    // Stay stationary during spells and ranged attacks to avoid interrupting them.
    if (POwner->PAI->IsCurrentState<CMagicState>() || POwner->PAI->IsCurrentState<CRangeState>())
    {
        return AvoidAction::None;
    }

    // Holding her position (SetWaiting), she takes no step of her own: no
    // chase, no herd spot, no perimeter, no walk into a spell's range, no
    // step out of an aggro circle, and no walk in for the player's order.
    // She keeps her target in front of her, and whatever needs no step runs
    // from where she stands. The hold lasts until he lifts it or moves it
    // with a maneuver; a maneuver's route is his walk order, not a step of
    // hers, and is never stopped here
    if (m_Waiting)
    {
        Intent stand;
        stand.target   = intent.target;
        stand.fighting = intent.fighting;
        stand.vet      = false;
        return Walk(std::move(stand));
    }

    // The player's order walks her in ahead of everything (OrderApproach)
    if (auto order = OrderApproach(); order.has_value())
    {
        return Walk(std::move(*order));
    }

    // Spell approaches seek range and line of sight through the shared
    // avoidance checks. They may leave the camp's formation boundary.
    if (!m_Retreat && !QueuedOrderHoldsHer() && m_Gambits->MasterOn() && RestAllowsAction())
    {
        const bool engaged = (POwner->PAI->IsEngaged() && !m_HoldForPlayer) || AttendedEngaged();
        const auto cast    = pawn::tactics::assignment(static_cast<CCharEntity*>(POwner), engaged);
        if (cast.has_value() && cast->approach)
        {
            auto* target = pawn::tactics::entity(static_cast<CCharEntity*>(POwner), cast->target);
            auto* PSpell = spell::GetSpell(cast->spell);
            const float reach = pawn::tactics::bank::castRange(POwner, PSpell, target);
            if (target != nullptr && target->loc.zone == POwner->loc.zone && reach > 0.0f && !HoldsFireOn(target))
            {
                // Use the same line-of-sight requirement as spell validation.
                const bool sight = !POwner->loc.zone->CanUseMisc(xi::ZoneMisc::LosPlayerBlock) || POwner->CanSeeTarget(target);
                intent.kind = Intent::Kind::Stand;
                intent.fallback.reset();
                // The approach's purpose, for the claim board: the target at
                // the distance the approach chose, so a slide keeps her range
                intent.slideRound = target->loc.p;
                intent.keepTo.reset();
                intent.holdsSpot = false;
                intent.claimed   = false;
                if (const auto goal = cardian::casting::approach(POwner->loc.p, target->loc.p, reach, sight); goal.has_value())
                {
                    intent.kind = Intent::Kind::Path;
                    intent.point = *goal;
                    intent.arrive = cardian::casting::kArrival;
                    intent.tolerance = cardian::casting::kTolerance;
                    intent.fallback = target->loc.p;
                }
                intent.seat = false;
                intent.rearBoundary.reset();
            }
        }
    }

    return Walk(std::move(intent));
}

auto CPawnController::Walk(Intent intent) -> std::optional<AvoidAction>
{
    // The claim board first (claim_board.h): no two party members aim for
    // one spot, whichever mover proposed it
    ClaimSpot(intent);

    auto*      PPathFind    = POwner->PAI->PathFind.get();
    position_t point        = intent.point;
    float      followMax    = intent.tolerance;
    float      followTarget = intent.arrive;
    float      declump      = intent.declump;

    // The vet: the danger map over the proposal, the same for every mover.
    // A proposal with no point of its own (stand, keep) is vetted only
    // for the circle she may be standing in; one the party waved through
    // (aggressive company allowed) is not vetted at all
    const bool  proposes = intent.kind != Intent::Kind::Stand && intent.kind != Intent::Kind::Keep;
    AvoidAction action   = AvoidAction::None;
    if (intent.vet && IsAvoiding() && (proposes || InsideDanger()))
    {
        if (!proposes)
        {
            point = POwner->loc.p;
        }
        // The circles that matter to this walk: their mobs see her, the
        // point, or the way between. A wall makes the rest no danger
        FocusDangers(POwner->loc.p, point);
        action = Avoid(point, followMax, followTarget, declump, intent.fighting);
    }

    // A path produced by avoidance or another mover cannot be kept as
    // though it still led to the seat. Re-plan that seat when it resumes.
    if (action != AvoidAction::None || intent.kind != Intent::Kind::Keep)
    {
        m_SeatPathActive = action == AvoidAction::None && intent.seat && intent.kind == Intent::Kind::Path;
    }

    // Moving uses the same stand transition as spells and orders. A danger
    // escape can interrupt a rest; merely keeping an idle path cannot.
    const bool takesStep = action != AvoidAction::None ||
        (intent.kind != Intent::Kind::Stand && intent.kind != Intent::Kind::Keep && distance(POwner->loc.p, point) > followMax) ||
        (intent.kind == Intent::Kind::Keep && PPathFind->IsFollowingPath());
    if (takesStep)
    {
        StandFromRest(action == AvoidAction::Escape ? "escaping danger" : "moving");
        if (!RestAllowsAction())
        {
            PPathFind->Clear();
            return action;
        }
    }

    // The step
    if (action != AvoidAction::None)
    {
        // What the vet asked for: a short hop straight to its point, a path
        // when it is far, a path dropped when she is there
        if (IsShortHop(point, followMax))
        {
            PPathFind->Clear();
            PPathFind->StepTo(point);
        }
        else if (const float away = distance(POwner->loc.p, point); away > followMax)
        {
            if (!PathToward(point, followTarget))
            {
                NotePathFailure(action, point, away);
            }
        }
        else if (PPathFind->IsFollowingPath())
        {
            PPathFind->Clear();
        }
    }
    else
    {
        switch (intent.kind)
        {
            case Intent::Kind::Stand:
                if (PPathFind->IsFollowingPath())
                {
                    PPathFind->Clear();
                }
                break;
            case Intent::Kind::Keep:
                break;
            case Intent::Kind::Hop:
                PPathFind->Clear();
                PPathFind->StepTo(point);
                break;
            case Intent::Kind::Path:
                if (distance(POwner->loc.p, point) > followMax)
                {
                    if (!PathToward(point, followTarget, intent.rearBoundary.has_value() ? &*intent.rearBoundary : nullptr))
                    {
                        if (intent.fallback.has_value())
                        {
                            // Retry toward the spell target through the same avoidance
                            // checks. Range and sight determine when to stop each tick.
                            intent.point = *intent.fallback;
                            intent.arrive = 0.0f;
                            intent.tolerance = 0.0f;
                            intent.fallback.reset();
                            intent.claimed = false;
                            return Walk(std::move(intent));
                        }
                        if (intent.herdSpot)
                        {
                            HerdPin("her way there failed");
                        }
                    }
                }
                else if (PPathFind->IsFollowingPath())
                {
                    PPathFind->Clear();
                }
                break;
            case Intent::Kind::Formation:
            {
                const float currentDistance = distance(POwner->loc.p, point);
                if (currentDistance > followMax)
                {
                    // Warp only when pathing genuinely fails; a pawn arriving
                    // at a zone gate runs to its player like anyone else would
                    if (!PathToward(point, followTarget) && intent.warpIfLost && currentDistance > WarpDistance)
                    {
                        PPathFind->WarpTo(point);
                        return std::nullopt;
                    }
                }
                else if (currentDistance < declump)
                {
                    if (!PPathFind->IsFollowingPath())
                    {
                        PathToward(point, followTarget + 0.5f);
                    }
                }
                else if (PPathFind->IsFollowingPath())
                {
                    PPathFind->Clear();
                }
                break;
            }
        }
    }

    // Her claim on the board while she walks: the point the walker sent
    // her to, or the end of the path she keeps (SpotClaims)
    if (PPathFind->IsFollowingPath())
    {
        // A path she keeps may be another mover's by now: its own end
        m_WalkTo   = action != AvoidAction::None || intent.kind != Intent::Kind::Keep ? point : PPathFind->GetDestination();
        m_WalkToAt = m_Tick;
    }
    else
    {
        m_WalkTo.reset();
    }

    PPathFind->FollowPath(m_Tick);

    // Lock-on: within melee reach of the mob, her heading goes back onto
    // it after every step, the way a locked-on player strafes. A step
    // points her down her path, and the swing asks that she face her
    // target, so without this a cardian moving to her seat -- or stuck in
    // a tight spot beside the mob -- never swings. (The client draws other
    // characters facing their heading; the strafe itself is not animated
    // for them, a quirk of the protocol. The swings are real.)
    if (intent.target != nullptr && KeepsSneakForBack(intent.target))
    {
        KeepSwingOff(intent.target);
    }
    else if (intent.target != nullptr && distance(POwner->loc.p, intent.target->loc.p) <= POwner->GetMeleeRange(intent.target) + LockOnSlack)
    {
        PPathFind->LookAt(intent.target->loc.p);
    }
    return action;
}

auto CPawnController::SneaksOn(const CBattleEntity* PTarget) const -> bool
{
    return PTarget != nullptr && ((m_SneakStep.has_value() && m_SneakStep->target.resolve<CBattleEntity>() == PTarget) ||
                                  (m_SneakHold.has_value() && m_SneakHold->target.resolve<CBattleEntity>() == PTarget));
}

auto CPawnController::KeepsSneakForBack(const CBattleEntity* PTarget) const -> bool
{
    // Sneak Attack is spent by her next swing and counts only from the
    // mob's back, and a swing asks that she face the mob: with it on her and
    // bound for this mob's back she does not turn to the mob -- held for her
    // swing (m_SneakHold), until she is there; for a weapon skill (a sneak
    // step with one), until the weapon skill turns her and goes out in one
    // tick (SneakThenWs). The hold's or the step's end -- the mob on her, its
    // back out of reach for too long -- lets her swing as she would
    if (PTarget == nullptr || !POwner->StatusEffectContainer->HasStatusEffect(xi::StatusEffect::SneakAttack) || PTarget->GetBattleTarget() == POwner)
    {
        return false;
    }
    const bool forWs   = m_SneakStep.has_value() && m_SneakStep->wsid != 0 && m_SneakStep->target.resolve<CBattleEntity>() == PTarget;
    const bool forSwing = m_SneakHold.has_value() && m_SneakHold->target.resolve<CBattleEntity>() == PTarget && !BehindFor(PTarget);
    return forWs || forSwing;
}

void CPawnController::KeepSwingOff(const CBattleEntity* PTarget)
{
    // Not turning to the mob is not enough: walking round it in reach, or
    // drawn as it walked into her, her heading can hold it in the cone a
    // swing needs. Held 60 degrees off it, on the side she already leans
    constexpr int16 kClear = 43;
    if (PTarget == nullptr || distance(POwner->loc.p, PTarget->loc.p) > POwner->GetMeleeRange(PTarget) + LockOnSlack)
    {
        return;
    }
    const int16 off = facingAngle(POwner->loc.p, PTarget->loc.p);
    if (std::abs(off) >= kClear)
    {
        return;
    }
    POwner->loc.p.rotation = relativeAngle(worldAngle(POwner->loc.p, PTarget->loc.p), off >= 0 ? -kClear : kClear);
    POwner->updatemask |= UPDATE_POS;
}

auto CPawnController::TakesBackFirst(const CBattleEntity* PTarget) -> bool
{
    // Sneak Attack still on her from a mob that died before her swing: she
    // takes the next one's back before she draws, so her first swing is
    // from there (the user, 2026-10-04: position, then engage). Not on a
    // mob that is on her, nor one whose back she gave up on, nor while she
    // holds her position or tows at the stake
    constexpr auto  kBackWalk = 6s;
    constexpr float kNear     = 3.0f; // yalms beyond her reach: she has arrived, and the walk round it begins
    if (PTarget == nullptr || !POwner->StatusEffectContainer->HasStatusEffect(xi::StatusEffect::SneakAttack) || PTarget->GetBattleTarget() == POwner ||
        PTarget->id == m_SneakGaveUpOn || m_Waiting || TowsAtStake() || !POwner->PAI->PathFind->ValidPosition(SneakPoint(PTarget)))
    {
        m_BackFirst.reset();
        return false;
    }
    const float gap = distance(POwner->loc.p, PTarget->loc.p) - POwner->GetMeleeRange(PTarget);
    if (gap <= 0.0f && BehindFor(PTarget))
    {
        // There: she draws. The walk's clock is kept, so a mob that turns
        // during the draw's beat buys no fresh walk round it
        return false;
    }
    if (!m_BackFirst.has_value() || m_BackFirst->target.resolve<CBattleEntity>() != PTarget)
    {
        m_BackFirst = BackFirst{ EntityId(PTarget), std::nullopt };
        ShowInfoFmt("tactics: {} takes {}'s back before she draws, for the Sneak Attack still on her", POwner->getName(), PTarget->getName());
    }
    if (!m_BackFirst->since.has_value() && gap <= kNear)
    {
        m_BackFirst->since = m_Tick;
    }
    if (m_BackFirst->since.has_value() && m_Tick - *m_BackFirst->since > kBackWalk)
    {
        m_SneakGaveUpOn = PTarget->id;
        m_BackFirst.reset();
        ShowInfoFmt("tactics: {} gives up on {}'s back and draws; the Sneak Attack on her goes on her next swing", POwner->getName(), PTarget->getName());
        return false;
    }
    return true;
}

void CPawnController::WalkToward(CBattleEntity* PTarget)
{
    if (!POwner->PAI->CanFollowPath() || POwner->GetSpeed() <= 0)
    {
        return;
    }
    RefreshDangers(PTarget);
    Move(ApproachIntent(PTarget));
}

void CPawnController::IdleEmote(const CCharEntity* PPlayer)
{
    if (POwner->StatusEffectContainer->HasStatusEffect(xi::StatusEffect::Healing))
    {
        return;
    }
    // The first idle moment only sets the clock: no fidget on arrival
    if (m_NextIdleEmoteTime == timer::time_point::min() || m_Tick >= m_NextIdleEmoteTime)
    {
        const bool due      = m_NextIdleEmoteTime != timer::time_point::min();
        m_NextIdleEmoteTime = m_Tick + std::chrono::seconds(xirand::GetRandomNumber(45, 120));
        // Alone (waiting somewhere), she fidgets too; with the player far
        // off she does not, and a stare needs someone to stare at
        if (!due || POwner->PAI->IsCurrentState<CMagicState>() || POwner->loc.zone == nullptr || (PPlayer != nullptr && distance(POwner->loc.p, PPlayer->loc.p) > 20.0f))
        {
            return;
        }

        static constexpr std::array<Emote, 4> kFidgets{ Emote::Think, Emote::Sigh, Emote::Huh, Emote::Stare };
        auto                                  emote = kFidgets[static_cast<std::size_t>(xirand::GetRandomNumber(0, static_cast<int>(kFidgets.size())))];
        if (emote == Emote::Stare && PPlayer == nullptr)
        {
            emote = Emote::Think;
        }
        const CBaseEntity* PAt = emote == Emote::Stare ? static_cast<const CBaseEntity*>(PPlayer) : POwner;

        const auto* PPawn = static_cast<const CCharEntity*>(POwner);
        POwner->loc.zone->PushPacket(POwner, CHAR_INRANGE_SELF, std::make_unique<GP_SERV_COMMAND_MOTIONMES>(PPawn, PAt->id, PAt->targid, emote, EmoteMode::Motion, 0));
    }
}

void CPawnController::TidyBag()
{
    if (m_Tick - m_LastTidyTime < 15s)
    {
        return;
    }
    m_LastTidyTime = m_Tick;

    if (const auto merges = pawn::items::tidyStacks(static_cast<CCharEntity*>(POwner)); merges > 0)
    {
        ShowInfoFmt("pawn: {} stacks her bag ({} merges)", POwner->getName(), merges);
    }
}

auto CPawnController::HatedByAnyMob() const -> bool
{
    bool        hated = false;
    const float reach = settings::get<float>("pawn.HUNT_LEASH");
    const auto  hates = [&](CMobEntity* PMob)
    {
        if (hated || !PMob->isAlive() || !isWithinDistance(POwner->loc.p, PMob->loc.p, reach))
        {
            return;
        }
        const auto* enmityList = PMob->PEnmityContainer->GetEnmityList();
        const auto  it         = enmityList->find(POwner->id);
        hated                  = it != enmityList->end() && it->second.active;
    };
    pawn::forEachMobNear(pawn::entitiesAround(POwner), POwner->loc.p, reach, hates);
    return hated;
}

auto CPawnController::FormationSlot() const -> pawn::Slot
{
    const auto slot = static_cast<pawn::Slot>(Behavior(pawn::Behavior::Formation).value_or(static_cast<uint16>(pawn::Slot::Follow)));

    // The lead's point ahead is a hunter's stance and silent in town: a
    // Lead row reads as auto (a seat on the ring) while her zone is a
    // city and takes effect again on the field. The row itself stands.
    if (slot == pawn::Slot::Lead && !settings::get<bool>("pawn.FORMATION_LEAD_IN_TOWN") &&
        POwner->loc.zone != nullptr && (POwner->loc.zone->GetTypeMask() & xi::ZoneType::City) != xi::ZoneType::Unknown)
    {
        return pawn::Slot::Follow;
    }
    return slot;
}

auto CPawnController::IsAvoidingAggro() const -> bool
{
    return !m_Retreat && Behavior(pawn::Behavior::AvoidAggro).value_or(0) != 0;
}

auto CPawnController::IsAvoidingLinks() const -> bool
{
    return !m_Retreat && Behavior(pawn::Behavior::AvoidLinks).value_or(0) != 0;
}

auto CPawnController::IsAvoiding() const -> bool
{
    return IsAvoidingAggro() || IsAvoidingLinks();
}

auto CPawnController::RestsWithPlayer() const -> bool
{
    return Behavior(pawn::Behavior::RestWithPlayer).value_or(0) != 0;
}

auto CPawnController::RestsByRow() const -> bool
{
    // A plain Self -> Rest row whose conditions hold this tick: it starts
    // her own rest order, down until full (RestTick). The marked one is
    // the tactician's and never speaks here
    return Behavior(pawn::Behavior::Rest).value_or(0) != 0;
}

auto CPawnController::RestRowDue() const -> bool
{
    // Her Rest row asks for its order: it speaks, she has something to
    // rest for, and no rest order is on yet. RestTick takes it where she
    // could kneel now; the party's hunt pick stands aside for it
    return !m_RestOrder.active() && m_Gambits->MasterOn() && RestsByRow() &&
           (POwner->health.hp < POwner->GetMaxHP() || POwner->health.mp < POwner->GetMaxMP());
}

auto CPawnController::HomePointsWithPlayer() const -> bool
{
    return Behavior(pawn::Behavior::HomePointWithPlayer).value_or(0) != 0;
}

CPawnController::~CPawnController() = default;

auto CPawnController::Gambits() -> pawn::CGambits&
{
    return *m_Gambits;
}

auto CPawnController::Tick(const timer::time_point tick) -> Task<void>
{
    TracyZoneScoped;
    TracyZoneString(POwner->getName());

    m_Tick = tick;
    m_Gambits->TickBehaviors();
    m_Rest.observe(POwner->StatusEffectContainer->HasStatusEffect(xi::StatusEffect::Healing),
                   std::chrono::duration<double>(tick.time_since_epoch()).count());
    // A one-shot wake request (such as moving camp) survives the kneel.
    if (m_Rest.standPending)
    {
        StandFromRest("kneeling finished after a wake request");
    }
    // This tick's cost, for the world's load line (always) and the per-zone
    // detail under pawn.WORLD_TICK_DEBUG
    struct BrainClock
    {
        const CBattleEntity* owner;
        realtime::time_point start;
        ~BrainClock()
        {
            if (owner->loc.zone != nullptr)
            {
                pawn::world::noteBrainTick(static_cast<uint16>(owner->loc.zone->GetID()), realtime::now() - start);
            }
        }
    } const brainClock{ POwner, realtime::now() };
    std::ignore = brainClock;

    // A zone change meant for the client protocol -- a warp, a teleport --
    // is hers to carry by transfer, ahead of the zone's own check
    if (pawn::carryZoning(static_cast<CCharEntity*>(POwner)))
    {
        co_return;
    }

    const bool engaged = POwner->PAI->IsEngaged();

    // Her party's fight log, advanced once a tick by whoever asks first --
    // a KO'd cardian included, or a wipe would never close
    pawn::tactics::tick(static_cast<CCharEntity*>(POwner), tick);

    // The server's attack state, reconciled with the mode: a fight the
    // server ended (the mob died, she lost sight of it, another party's
    // claim) is her exit from Fight, said with the server's reason; a
    // fight begun by a hand not hers (a script, a possession) is her
    // entry, said as such. Neither is ever silent.
    const bool thinksEngaged = m_Mode == Mode::Fight || m_Mode == Mode::Hold;
    if (thinksEngaged && !engaged)
    {
        if (!ResumeCampReceive())
        {
            Transition(IdleMode(), ServerExitReason());
        }
    }
    else if (!thinksEngaged && engaged && m_Mode != Mode::Down && m_Mode != Mode::Maneuver && POwner->GetBattleTarget() != nullptr)
    {
        Transition(Mode::Fight, fmt::format("engaged on {} by a hand not hers", POwner->GetBattleTarget()->getName()));
    }

    UpdateRestLine();
    if (POwner->isDead())
    {
        if (m_Mode != Mode::Down)
        {
            Transition(Mode::Down, "KO'd");
        }
        WatchPlayerHomePoint();
        UpdateRunning();
    }
    else
    {
        if (m_Mode == Mode::Down)
        {
            Transition(IdleMode(), "back on her feet");
        }
        m_PlayerSeenDead = false;
        CheckBrain();
        UpdateRunning(); // an order over lets its line go before the enchanted item's use begins
        EnchantTick();   // the enchanted item's use goes ahead of her line, holding no place in it
        FireQueuedOrder();
        UpdateRunning(); // after the line moves: an order over and the next away in one tick is one QUEUE
        FireOrderedEngage();
        FoodTick(); // her food: eaten with the player, a body of the world's topped up

        // Mobs check a character for aggro only when that character's client
        // sends a position or action packet (CZoneEntities::tapMobAggro). A
        // cardian sends neither, so she asks on her own, at a player's cadence
        if (POwner->loc.zone != nullptr)
        {
            POwner->loc.zone->SpawnMOBs(static_cast<CCharEntity*>(POwner));
        }
    }

    // A closed door on her way opens as she walks up, the way the client
    // opens one for a player: the navmesh knows no doors (pawn_doors.h).
    // Her facing is the way to her next waypoint; a door within two yalms
    // of that line, out to DOOR_REACH, is hers to open
    if (!POwner->isDead() && POwner->PAI->PathFind && POwner->PAI->PathFind->IsFollowingPath())
    {
        pawn::doors::openAhead(static_cast<CCharEntity*>(POwner), settings::get<float>("pawn.DOOR_REACH"), 2.0f);
    }

    // A maneuver (docs/maneuvers.md): the player drives her, so neither
    // tick runs -- no fight door, no formation, no perimeter; her walk
    // order alone moves her. A finisher that engaged her (a weapon skill,
    // a Provoke) leaves her engaged for the tick after the maneuver, which
    // reconciles it into a fight as any hand not hers
    if (m_Mode == Mode::Maneuver)
    {
        ManeuverTick();
        co_return;
    }

    // A debugging line for the player's own party: the world's bodies fight
    // all day, and their aggro would bury his
    if (!POwner->isDead() && GetLivePlayer() != nullptr)
    {
        NoteAggro();
    }

    // A gaze readied with her before it: her back to it, and nothing else
    // this tick, every action being a turn back to her target
    if (!POwner->isDead() && AvertGaze())
    {
        co_return;
    }

    if (engaged)
    {
        co_await DoCombatTick(tick);
    }
    else if (!POwner->isDead())
    {
        co_await DoRoamTick(tick);
    }

    co_return;
}

namespace
{
    // A gaze move: its script lands only on one who faces the mob --
    // xi.mobskills.mobGazeMove, or its own isFacing test (belly_dance,
    // frigid_shuffle). Read from its script once per skill
    auto isGaze(CMobSkill* PSkill) -> bool
    {
        static auto& known = *new std::unordered_map<uint16, bool>();
        if (const auto it = known.find(PSkill->getID()); it != known.end())
        {
            return it->second;
        }
        std::ifstream     file(fmt::format("./scripts/actions/mobskills/{}.lua", PSkill->getName()));
        std::stringstream text;
        text << file.rdbuf();
        const auto script = text.str();
        const bool gaze   = script.find("mobGazeMove") != std::string::npos || script.find("isFacing(") != std::string::npos;
        known.emplace(PSkill->getID(), gaze);
        return gaze;
    }
} // namespace

auto CPawnController::AvertGaze() -> bool
{
    // A gaze lands only on one who faces the mob (xi.mobskills.mobGazeMove:
    // target:isFacing(mob) and mob:isInfront(target), which the engine's
    // infront(A, B) -- "A before B" -- makes the same test), whichever way
    // the mob faces. So while a mob readies one that could take her she
    // turns her back on it and holds still; when it has fired she turns back
    // to her target as her tick does (the user, 2026-10-04: Baleful Gaze,
    // and every gaze dodged by looking away). Per fight, every cardian the
    // world's included: a single-target gaze only when she is its target,
    // an area one only from a mob on her party and with her in its reach --
    // never another party's mob
    CMobEntity* PGazer = nullptr;
    CMobSkill*  PSkill = nullptr;
    pawn::forEachMobNear(pawn::entitiesAround(POwner), POwner->loc.p, 30.0f, [&](CMobEntity* PMob)
    {
        const auto* state = PGazer == nullptr && !PMob->isDead() ? dynamic_cast<CMobSkillState*>(PMob->PAI->GetCurrentState()) : nullptr;
        auto*       skill = state != nullptr ? state->GetSkill() : nullptr;
        if (skill == nullptr || !isGaze(skill))
        {
            return;
        }
        const auto* POn     = PMob->GetBattleTarget();
        const bool  atHer   = POn == POwner;
        const bool  onParty = atHer || (POn != nullptr && POwner->PParty != nullptr && POn->PParty == POwner->PParty);
        const float reach   = std::max(skill->getDistance(), skill->getRadius()) + 2.0f;
        if (skill->isAoE() ? onParty && distance(PMob->loc.p, POwner->loc.p) <= reach : atHer)
        {
            PGazer = PMob;
            PSkill = skill;
        }
    });
    if (PGazer == nullptr)
    {
        m_AvertedFrom = 0;
        return false;
    }
    const uint8 away = worldAngle(PGazer->loc.p, POwner->loc.p);
    if (POwner->loc.p.rotation != away)
    {
        POwner->loc.p.rotation = away;
        POwner->updatemask |= UPDATE_POS;
    }
    HeadLook(nullptr);
    if (POwner->PAI->PathFind != nullptr && POwner->PAI->PathFind->IsFollowingPath())
    {
        POwner->PAI->PathFind->Clear(); // a step would turn her back round
    }
    if (m_AvertedFrom != PGazer->id)
    {
        m_AvertedFrom = PGazer->id;
        ShowInfoFmt("pawn: {} turns her back on {}'s {}", POwner->getName(), PGazer->getName(), PSkill->getName());
    }
    return true;
}

void CPawnController::NoteAggro()
{
    // The moment a mob turns on her, said once with what the log otherwise
    // never holds (the user, 2026-10-04, a goblin on a mage who avoided
    // aggro): how far off it was, whether it was walking, whether she was
    // casting -- rooted, her avoidance cannot step -- what it detects by,
    // and whether her danger map counted it
    std::vector<uint32> onHer;
    pawn::forEachMobNear(pawn::entitiesAround(POwner), POwner->loc.p, 50.0f, [&](CMobEntity* PMob)
    {
        if (PMob->isDead() || PMob->GetBattleTarget() != POwner)
        {
            return;
        }
        onHer.push_back(PMob->id);
        if (std::find(m_MobsOnHer.begin(), m_MobsOnHer.end(), PMob->id) != m_MobsOnHer.end())
        {
            return;
        }
        const auto  detects = static_cast<xi::Detects>(PMob->getMobMod(xi::MobMod::Detection));
        std::string by;
        const auto  add = [&](const xi::Detects flag, const std::string_view word)
        {
            if ((detects & flag) != xi::Detects::None)
            {
                by += fmt::format("{}{}", by.empty() ? "" : ", ", word);
            }
        };
        add(xi::Detects::Sight, "sight");
        add(xi::Detects::Hearing, "hearing");
        add(xi::Detects::Magic, "magic");
        add(xi::Detects::Lowhp, "low HP");
        const bool counted = std::any_of(m_Dangers.begin(), m_Dangers.end(), [&](const auto& d) { return d.mob == PMob; });
        ShowInfoFmt("pawn: {}: {} comes for her, {:.1f} y off ({}; she {}; it detects by {}; {})", POwner->getName(), PMob->getName(),
                    distance(POwner->loc.p, PMob->loc.p), PMob->PAI->PathFind != nullptr && PMob->PAI->PathFind->IsFollowingPath() ? "it was walking" : "it was standing",
                    POwner->PAI->IsCurrentState<CMagicState>() ? "was casting, rooted" : "was free to move", by.empty() ? "nothing" : by,
                    !IsAvoidingAggro() ? "her Avoid aggro row is off" : counted ? "her danger map counted it" : "her danger map did not count it");
    });
    m_MobsOnHer = std::move(onHer);
}

void CPawnController::WatchPlayerHomePoint()
{
    if (!HomePointsWithPlayer())
    {
        return;
    }
    auto* PPlayer = zoneutils::GetChar(pawn::summonerOf(POwner->id));
    if (PPlayer == nullptr)
    {
        return;
    }
    if (PPlayer->isDead())
    {
        m_PlayerSeenDead = true;
        return;
    }
    if (!m_PlayerSeenDead)
    {
        return;
    }

    // Back from the dead at their home point: they home pointed. A raise in
    // place leaves them where they fell.
    const auto& home   = PPlayer->profile.home_point;
    const bool  atHome = PPlayer->loc.zone != nullptr && PPlayer->loc.zone->GetID() == home.destination &&
                        distance(PPlayer->loc.p, home.p) < 20.0f;
    if (!atHome)
    {
        return;
    }
    m_PlayerSeenDead = false;
    ShowInfoFmt("pawn: {} home points with {}", POwner->getName(), PPlayer->getName());
    pawn::homePoint(static_cast<CCharEntity*>(POwner));
}

void CPawnController::CheckBrain()
{
    // Her rows once; a job change leaves them as they are
    if (!m_BrainLoaded)
    {
        m_BrainLoaded = true;
        pawn::loadBrain(static_cast<CCharEntity*>(POwner));
    }
}

auto CPawnController::DoCombatTick(const timer::time_point tick) -> Task<void>
{
    TracyZoneScoped;

    m_Gambits->TickBehaviors();

    CCharEntity* PPlayer = GetAnchor();
    const Place* place   = CurrentPlace(PPlayer);

    // The party's place gone means stand down: the player left the zone
    // with no stake holding her here. Their weapon going down does not
    // call the party off a fight that has started -- it runs until the
    // mob dies or an order ends it -- but it does end a hold
    // (below), which the party only drew for.
    if (place == nullptr && !m_Waiting && !m_World)
    {
        Transition(IdleMode(), "the player left the zone");
        POwner->PAI->Internal_Disengage();
        co_return;
    }
    if (PPlayer != nullptr)
    {
        NotePlayerMagic(PPlayer);
    }

    if (POwner->PAI->IsCurrentState<CMagicState>() || POwner->PAI->IsCurrentState<CRangeState>())
    {
        co_return;
    }

    CBattleEntity* PTarget = POwner->GetBattleTarget();
    if (PTarget == nullptr || PTarget->isDead())
    {
        Transition(IdleMode(), PTarget == nullptr ? std::string("no target") : fmt::format("{} is dead", PTarget->getName()));
        POwner->PAI->Internal_Disengage();
        co_return;
    }

    // Holding for the player's strike, she drew on their word alone. Their
    // target moved: the hold follows it when her rows take the new one now
    // (EngageChoice) and the rules let her draw on it outright, and holds
    // on the new one until they strike or it comes; otherwise she stands
    // down, and the door takes her next fight, the hold worked out again
    // from the mob -- a hold is not a begun fight, and she never holds on
    // a mob they have left (engage_math.h holdStep). A fight that has begun
    // is not called off by a switch -- it runs until the mob dies or the
    // player zones, and only the chord's engage moves the party (M3.9; the
    // user, 2026-09-17)
    if (m_HoldForPlayer && PPlayer != nullptr && PPlayer->PAI->IsEngaged())
    {
        auto*      PSwitched = PPlayer->GetBattleTarget();
        const bool moved     = PSwitched != nullptr && PSwitched != PTarget;
        if (moved)
        {
            const auto rows    = EngageChoice(PPlayer, place != nullptr && !m_Waiting ? place->position() : POwner->loc.p);
            const auto facts   = EngageFactsFor(PSwitched);
            const bool mayDraw = cardian::rules::mayFight(facts) && Refusal(PSwitched, facts).empty();
            const auto step    = cardian::engage::holdStep(moved, rows.target == PSwitched, mayDraw);
            const bool hold    = !playerHasEnmity(PPlayer, PSwitched) && !PSwitched->PAI->IsEngaged();
            if (step == cardian::engage::HoldStep::Follow &&
                Draw(PSwitched, ApproachKind::Join, fmt::format("{} switched to it; {}", PPlayer->getName(), rows.why), hold))
            {
                co_return;
            }
            Transition(IdleMode(), fmt::format("stands down ({}'s target moved to {})", PPlayer->getName(), PSwitched->getName()));
            POwner->PAI->Internal_Disengage();
            co_return;
        }
    }

    // A target gone underground with no fight on is let go, not waited on
    if (auto* PMobTarget = dynamic_cast<CMobEntity*>(PTarget); PMobTarget != nullptr && pawn::isUnderground(PMobTarget) && !PMobTarget->PAI->IsEngaged())
    {
        Transition(IdleMode(), fmt::format("lets {} go (underground)", PTarget->getName()));
        POwner->PAI->Internal_Disengage();
        co_return;
    }

    // Her tactician's melee -- a marked Attack row took this fight, and no
    // order claims the mob -- gives way to her rest the moment her recovery
    // is due: she leaves the fight, the door has her attend it, and she
    // rests as her rest policy says until it stands her (tactician_line.h
    // leavesToRest; RESEARCH §14.12 decision 19). An order, or the player's
    // own Attack, keeps her in. A tank's recovery is never due
    // (RecoveryDue): she leaves no fight to rest
    if (const bool runs = TacticianRuns(), due = RecoveryDue();
        due && cardian::tactician::leavesToRest(runs, due, ClaimingRowAs(PTarget, false).has_value(), ClaimingRowAs(PTarget, true).has_value(), PlayersOrderOn(PTarget)))
    {
        StandDown(fmt::format("leaves the fight on {} to rest (her recovery is due)", PTarget->getName()));
        co_return;
    }

    // Whoever she is fighting is the mob the draw cooldown will measure
    // against once this fight ends, and the one the server's exit reason
    // is read from
    m_LastFoughtId = PTarget->id;
    m_LastFought   = EntityId(PTarget);

    // The weapon skill held behind its opener (Sneak Attack, Boost) goes
    // out the first tick the pacer allows -- the opener landed and let go
    // of her -- before anything else can spend it; given up after a few
    // ticks, and never tried beyond its reach (BoostOrWeaponSkill). Behind
    // Sneak Attack it goes through Boost when her row offers that too:
    // Sneak Attack, Boost, the weapon skill
    if (m_HeldWs.has_value() && m_Tick > m_HeldWs->at)
    {
        const auto held  = *m_HeldWs;
        auto*      PHeld = held.target.resolve<CBattleEntity>();
        if (PHeld == nullptr || PHeld->isDead() || m_Tick - held.at > 2s)
        {
            m_HeldWs.reset();
        }
        else if (ReadyToAct())
        {
            m_HeldWs.reset();
            FaceTarget(held.target);
            if (BoostOrWeaponSkill(held.target, held.wsid))
            {
                co_return;
            }
            m_HeldWs = held; // refused: tried again until the few ticks are up
        }
    }

    // How long Sneak Attack has stood ready in this fight: what holding it
    // for her weapon skill costs, said as it goes out (SneakThenWs) and
    // booked unused as the fight ends. Not usable (spent by any road, his
    // order included, or on recast), the clock starts over
    if (SneakAttackUsable())
    {
        if (!m_SneakReadySince.has_value())
        {
            m_SneakReadySince = m_Tick;
        }
    }
    else
    {
        m_SneakReadySince.reset();
    }

    // The walk to the mob's back for Sneak Attack (WeaponSkill, or a naked
    // one: NakedSneakDue): it goes the first tick she is behind, in reach
    // and free to act; given up if the mob turns on her or the walk runs
    // long, her weapon skill then going alone
    if (m_SneakStep.has_value())
    {
        constexpr auto kSneakWalk = 5s;
        const auto     step       = *m_SneakStep;
        auto*          PStep      = step.target.resolve<CBattleEntity>();
        const bool     onHer      = PStep != nullptr && PStep->GetBattleTarget() == POwner;
        if (PStep == nullptr || PStep->isDead() || PStep != PTarget)
        {
            m_SneakStep.reset();
        }
        else if (onHer || m_Tick - step.since > kSneakWalk)
        {
            if (ReadyToAct())
            {
                m_SneakStep.reset();
                RestSneak(PStep);
                // A Sneak Attack already on her is given up with the back: no
                // hold or fresh walk for it on this mob, it goes on her next
                // swing (m_SneakGaveUpOn)
                if (POwner->StatusEffectContainer->HasStatusEffect(xi::StatusEffect::SneakAttack))
                {
                    m_SneakGaveUpOn = PStep->id;
                }
                ShowInfoFmt("tactics: {} gives up on Sneak Attack ({}){}", POwner->getName(),
                            onHer ? fmt::format("{} is on her", PStep->getName()) : fmt::format("she could not reach {}'s back", PStep->getName()),
                            step.wsid != 0 ? "; her weapon skill goes alone" : "");
                // Turned off the mob for the walk (KeepSwingOff): the weapon
                // skill asks that she face it
                if (step.wsid != 0)
                {
                    FaceTarget(step.target);
                    if (BoostOrWeaponSkill(step.target, step.wsid))
                    {
                        co_return;
                    }
                }
            }
        }
        else if (ReadyToAct() && distance(POwner->loc.p, PStep->loc.p) <= POwner->GetMeleeRange(PStep) && BehindFor(PStep))
        {
            m_SneakStep.reset();
            if (step.wsid == 0)
            {
                NakedSneak(step.target);
                co_return;
            }
            if (SneakThenWs(step.target, step.wsid))
            {
                co_return;
            }
        }
    }

    // A Sneak Attack still on her -- primed for a mob that died before her
    // swing -- is the next mob's: she takes its back before her first
    // swing, and turns to swing only there (KeepsSneakForBack; the user,
    // 2026-10-04: one primed on a goblin was spent on a lizard's flank).
    // Not while a weapon skill waits behind a Sneak Attack just used, not on
    // a mob whose back she has given up on already, and not where she could
    // not walk there anyway -- holding her position, towing at the stake, its
    // back off the mesh -- or the hold would only keep her from swinging
    const bool sneakUp = POwner->StatusEffectContainer->HasStatusEffect(xi::StatusEffect::SneakAttack);
    if (!sneakUp)
    {
        m_SneakGaveUpOn = 0;
    }
    if (sneakUp && !m_SneakStep.has_value() && !m_SneakHold.has_value() && !m_HeldWs.has_value() && PTarget->GetBattleTarget() != POwner &&
        PTarget->id != m_SneakGaveUpOn && !m_Waiting && !TowsAtStake() && POwner->PAI->PathFind->ValidPosition(SneakPoint(PTarget)))
    {
        m_SneakHold = SneakHold{ .target = EntityId(PTarget), .until = m_Tick + 15s, .landed = true };
        ShowInfoFmt("tactics: {} takes {}'s back for the Sneak Attack still on her", POwner->getName(), PTarget->getName());
    }

    // After a naked Sneak Attack she keeps the mob's back until her swing
    // has spent it (the effect seen on her, then gone), the mob turns on
    // her, or a few seconds pass
    if (m_SneakHold.has_value())
    {
        auto*      PHeld = m_SneakHold->target.resolve<CBattleEntity>();
        const bool up    = POwner->StatusEffectContainer->HasStatusEffect(xi::StatusEffect::SneakAttack);
        m_SneakHold->landed = m_SneakHold->landed || up;
        if (PHeld == nullptr || PHeld != PTarget || PHeld->GetBattleTarget() == POwner || m_Tick > m_SneakHold->until || (m_SneakHold->landed && !up))
        {
            // Its back out of reach this long with Sneak Attack still on her:
            // she swings as she would, and takes no new hold on that mob
            if (PHeld != nullptr && m_Tick > m_SneakHold->until && up)
            {
                m_SneakGaveUpOn = PHeld->id;
                ShowInfoFmt("tactics: {} gives up on {}'s back; the Sneak Attack on her goes on her next swing", POwner->getName(), PHeld->getName());
            }
            m_SneakHold.reset();
        }
    }

    // A naked Sneak Attack when no weapon skill row of hers can take one
    if (!m_SneakStep.has_value() && !m_SneakHold.has_value() && NakedSneakDue(PTarget))
    {
        m_SneakStep = SneakStep{ .target = EntityId(PTarget), .wsid = 0, .since = m_Tick };
        ShowInfoFmt("tactics: {} steps round to {}'s back for a Sneak Attack on her next swing (her weapon skill row cannot take one)", POwner->getName(), PTarget->getName());
    }

    // The hold ends the moment the player has struck or the mob has come,
    // and says which: without it, a cardian closing on her own is a
    // mystery in the log
    if (m_HoldForPlayer && PPlayer == nullptr)
    {
        // Also covers an unexpected loss of the player without SendToZone.
        PlayerZoning();
        co_return;
    }
    if (m_HoldForPlayer && PPlayer != nullptr)
    {
        const bool struck = playerHasEnmity(PPlayer, PTarget);
        const bool came   = PTarget->PAI->IsEngaged();
        if (struck || came)
        {
            // The beat: the hold's end is seen now, the close comes a beat
            // later -- the lead first
            if (!PendingIs(Pending::Act::Close, PTarget))
            {
                Schedule(Pending::Act::Close, PTarget, ReactionBeat());
            }
        }
        if (Due(Pending::Act::Close, PTarget))
        {
            m_Pending.reset();
            m_HoldForPlayer = false;
            Transition(Mode::Fight, fmt::format("closes on {} ({})", PTarget->getName(),
                                                struck ? fmt::format("{} has its attention", PPlayer->getName())
                                                       : fmt::format("{} is fighting {}", PTarget->getName(),
                                                                     PTarget->GetBattleTarget() != nullptr ? PTarget->GetBattleTarget()->getName() : "someone")));
        }
    }

    // Still holding and the player has put their weapon away: they thought
    // better of it before a blow was struck, and the party drew on that
    // word alone -- so it stands down with them
    if (m_HoldForPlayer && PPlayer != nullptr && !PPlayer->PAI->IsEngaged())
    {
        Transition(IdleMode(), fmt::format("stands down ({} thought better of {})", PPlayer->getName(), PTarget->getName()));
        POwner->PAI->Internal_Disengage();
        co_return;
    }

    if (TowsAtStake() && CampReceive(PTarget) == cardian::stake::ReceiveAction::Outside)
    {
        Transition(IdleMode(), fmt::format("lets {} go (left camp before the receive)", PTarget->getName()));
        POwner->PAI->Internal_Disengage();
        co_return;
    }

    // Holding, she walks with the player, and the formation's pace and
    // perch are hers. Fighting, neither is: the speed limit is never
    // broken once a fight starts, and a perch belongs to a slot
    if (!m_HoldForPlayer)
    {
        RestoreNormalSpeed();
        m_AvoidPerch.reset();
        m_AvoidItch = 0.0f;
    }

    // Her head tracks the target -- except through an action, when it
    // stays on the action's own target (a cure on a friend mid-fight);
    // her body turns only when the target leaves her front arc, or when
    // an action fires -- a body snapped to the target every tick is the
    // mob tell
    if (!Acting())
    {
        HeadLook(PTarget);
    }

    std::optional<AvoidAction> moved;
    if (POwner->PAI->CanFollowPath() && POwner->GetSpeed() > 0)
    {
        if (KeepsSneakForBack(PTarget))
        {
            KeepSwingOff(PTarget);
        }
        else if (!facing(POwner->loc.p, PTarget->loc.p, 64))
        {
            POwner->PAI->PathFind->LookAt(PTarget->loc.p);
        }

        // The tick's danger map, once, for every mover and the vet
        RefreshDangers(PTarget);

        std::optional<Intent> intent;
        if (m_Waiting)
        {
            // Holding her position: no seat, no tow, no step back -- Move
            // would take none of them -- only her target kept in front of her
            intent           = Intent{};
            intent->target   = PTarget;
            intent->fighting = true;
        }
        else if (m_HoldForPlayer && PPlayer != nullptr)
        {
            // Walking in with the player, in formation, never within reach
            // of the mob: the strike is the player's, and the pounce after
            // it is a few yalms from a slot. No lock-on: she walks with
            // them
            intent = FormationIntent(*place, PPlayer, PTarget);
        }
        else
        {
            m_HasSlot = false;
            // Her place on the mob: a bearing round it in the herd
            // (HerdBearing). Bound for the mob's back for Sneak Attack, or
            // keeping it for the swing that spends a naked one, she stands
            // there instead, reached round the mob's side. The tank at a
            // stake takes neither: she tows (TowIntent)
            const bool           tows     = TowsAtStake();
            const bool           sneaking = !tows && SneaksOn(PTarget);
            std::optional<float> bearing;
            if (!tows && !sneaking)
            {
                bearing = HerdBearing(PTarget);
                if (HerdAvoids(PTarget, *bearing))
                {
                    bearing = cardian::herd::bearingOf(PTarget->loc.p.x, PTarget->loc.p.z, POwner->loc.p.x, POwner->loc.p.z);
                }
            }
            const position_t point = sneaking ? SneakPoint(PTarget) : bearing.has_value() ? HerdPoint(PTarget, *bearing) : PTarget->loc.p;

            // An idle target is a pull on its way in, judged by the pull rule
            // the pick used (PullBlocker), never by the shape of her own
            // avoidance: a detour round a circle is a walk, not a reason.
            // Unclean, it is let go when the party wants clean pulls and
            // fetched -- the vet waived -- when it allows aggressive company
            // (the door refused it by the same rule, so this catches only a
            // guard that has roamed over since). A target that is fighting
            // is held at the rim: the tank brings it
            bool vet = true;
            if (!PTarget->PAI->IsEngaged())
            {
                if (pawn::huntRulesOf(pawn::ordersOwnerOf(static_cast<const CCharEntity*>(POwner))).aggressive)
                {
                    vet = false;
                }
                else if (auto* PMobTarget = dynamic_cast<CMobEntity*>(PTarget); PMobTarget != nullptr)
                {
                    if (const auto unclean = PullBlocker(PMobTarget); !unclean.empty())
                    {
                        Transition(IdleMode(), fmt::format("lets {} go ({})", PTarget->getName(), unclean));
                        POwner->PAI->Internal_Disengage();
                        co_return;
                    }
                }
            }

            // The camp tank tows; anyone else is offered the step back
            // first (off unless MELEE_STEP_BACK), then her herd spot or the
            // mob's back. Reach is the distance alone -- the server's
            // CanAttack would say it, but it disengages her as a side
            // effect on a claimed or far target, and counts a walking
            // cardian as out of reach
            const bool inReach = distance(POwner->loc.p, PTarget->loc.p) <= POwner->GetMeleeRange(PTarget);
            intent             = tows ? std::optional<Intent>(TowIntent(PTarget)) : StepBackIntent(PTarget);
            if (!intent.has_value())
            {
                if (sneaking && inReach && BehindFor(PTarget))
                {
                    intent = Intent{}; // in the cone at its back and in reach: she stands, a swing from there is the point
                }
                else if (bearing.has_value() && !inReach && HerdPinned(PTarget))
                {
                    intent = ApproachIntent(PTarget); // her way to her spot failed: she closes on the mob itself
                }
                else
                {
                    if (bearing.has_value())
                    {
                        NoteHerd(PTarget, *bearing, inReach, point);
                    }
                    intent = SeatIntent(PTarget, point, inReach, false, !sneaking);
                }
            }
            intent->target   = PTarget;
            intent->fighting = true;
            intent->vet      = vet;
        }

        moved = Move(*intent);
    }

    // Her think runs whether she stands or walks: a cast she wants stops
    // the walk (CastAndStop) -- except the step out of an aggro circle,
    // which a cast would undo. Holding, only what she would do between
    // fights -- cures and buffs -- since a nuke is a first hit too
    if (moved.value_or(AvoidAction::None) != AvoidAction::Escape)
    {
        m_Gambits->Tick(tick, !m_HoldForPlayer);
    }

    co_return;
}

// The walk in on a mob, weapon away, and the draw at the door: shared by
// the party's pawns (the anchor is the player, the pacing theirs) and the
// world's bodies (the anchor her route point, no pacing but her own rest).
// True when the tick was hers; false when the walk was dropped and the
// tick goes on
auto CPawnController::ApproachTick(const position_t& anchor, const uint8 level, const std::string& pacing, const bool hunting, CBattleEntity* PPartyTarget) -> bool
{
    // Walking in on a mob, weapon away -- her own pull, the party's fight
    // farther than she may draw from, or the player's order: she committed
    // the moment it was chosen and closes; only the draw waits, on the
    // rules (pawn_rules.h) and the re-engage timer, so the party moves on
    // at once after a kill and the pause is at the draw, not before the
    // choice. Every reason to let the mob go is judged here, ahead of the
    // party's rest
    if (m_Approach.has_value())
    {
        auto*      PMob = m_Approach->target.resolve<CMobEntity>();
        const bool hunt = m_Approach->kind == ApproachKind::Hunt;
        const bool join = m_Approach->kind == ApproachKind::Join;
        if (PMob == nullptr || PMob->isDead())
        {
            Transition(IdleMode(), PMob == nullptr ? std::string("the mob is gone") : fmt::format("{} is dead", PMob->getName()));
        }
        // Her own pull is judged as it was picked: idle, hunted, inside the
        // radius, and the party paced for it -- the player sat, a member
        // fell -- else the choice goes too, to be made afresh
        else if (hunt && (PMob->PAI->IsEngaged() || !hunting || distance(anchor, PMob->loc.p) > settings::get<float>("pawn.HUNT_RADIUS")))
        {
            Transition(IdleMode(), fmt::format("lets {} go ({})", PMob->getName(),
                                               PMob->PAI->IsEngaged() ? "it is fighting already" : !hunting ? "the hunt is off" : "beyond the hunt radius"));
        }
        else if (const auto blocker = hunt ? pacing : std::string(); !blocker.empty())
        {
            Transition(IdleMode(), fmt::format("lets {} go ({})", PMob->getName(), blocker));
        }
        // The party's fight she was walking in on ended, or moved to
        // another mob. After receiving she owns this fight, including
        // a weapon-away pursuit after the engine's sight-range sheathe.
        else if (join && PPartyTarget != PMob &&
                 !(TowsAtStake() && m_Receive.joined && m_ReceiveMob.has_value() && *m_ReceiveMob == PMob))
        {
            Transition(IdleMode(), fmt::format("lets {} go (the party moved on)", PMob->getName()));
        }
        else
        {
            // her eye is on it the whole way in, and while she stands there
            // waiting to draw
            HeadLook(PMob);
            // A hunt's walk starts a beat after the pick
            if (PendingIs(Pending::Act::SetOff, PMob))
            {
                if (!Due(Pending::Act::SetOff, PMob))
                {
                    return true;
                }
                m_Pending.reset();
            }

            // At a camp a damage dealer keeps her seat, weapon away, until
            // the pull comes in (WaitsForThePull, as the Draw door has it)
            if (WaitsForThePull(PMob, m_Approach->kind))
            {
                if (const auto* place = CurrentPlace(GetAnchor()); place != nullptr && POwner->PAI->CanFollowPath() && POwner->GetSpeed() > 0)
                {
                    RefreshDangers(PMob);
                    Move(FormationIntent(*place, GetAnchor(), nullptr));
                }
                return true;
            }

            // The rules gate the DRAW, not the arrival: allowed, she draws
            // where she stands and charges in with her weapon out; still
            // too far, or the draw's wait unserved, she walks in with it
            // away and draws the moment the rules allow, wherever that
            // catches her. Anything else in the way (claimed, gone under)
            // ends the walk.
            // The beat again at a hunt's draw: the wait is served for the
            // whole party at once, and without it they would all draw on
            // one tick. A join's or an order's draw waits no beat: its
            // beat was served at the door
            const auto facts = EngageFactsFor(PMob);
            const auto ready = cardian::rules::mayFight(facts);
            if (ready)
            {
                // Bound for its back before she draws (the door's
                // TakesBackFirst): she walks round to it, weapon away, and
                // draws below once she stands there or gives the back up
                if (m_BackFirst.has_value() && m_BackFirst->target.resolve<CBattleEntity>() == PMob && !POwner->PAI->IsEngaged() && TakesBackFirst(PMob))
                {
                    if (POwner->PAI->CanFollowPath() && POwner->GetSpeed() > 0)
                    {
                        RefreshDangers(PMob);
                        auto intent     = SeatIntent(PMob, SneakPoint(PMob), distance(POwner->loc.p, PMob->loc.p) <= POwner->GetMeleeRange(PMob), false, false);
                        intent.target   = PMob;
                        intent.fighting = true;
                        Move(intent);
                    }
                    return true;
                }
                if (!PendingIs(Pending::Act::Draw, PMob))
                {
                    Schedule(Pending::Act::Draw, PMob, hunt ? m_HuntBeat : timer::duration{});
                }
                if (Due(Pending::Act::Draw, PMob))
                {
                    m_Pending.reset();
                    m_HoldForPlayer = false;
                    // A join's draw on the player's word alone holds, as the
                    // door's does: until they strike, or the mob comes to us
                    CCharEntity* PPlayer = GetAnchor();
                    const bool   hold    = join && PPlayer != nullptr && PPlayer->PAI->IsEngaged() && PPlayer->GetBattleTarget() == PMob &&
                                        !playerHasEnmity(PPlayer, PMob) && !PMob->PAI->IsEngaged();
                    const std::string how = hunt                  ? std::string(magic_enum::enum_name(charutils::CheckMob(level, PMob))) :
                                            hold                  ? fmt::format("holding for {}'s strike", PPlayer->getName()) :
                                            join && TowsAtStake() ? std::string("it came within reach") :
                                            join && Staked()      ? std::string("it came in to the camp") :
                                                                    std::string("walked in");
                    Draw(PMob, m_Approach->kind, how, hold);
                }
                return true;
            }
            if (!cardian::rules::worthWalkingIn(facts))
            {
                Transition(IdleMode(), fmt::format("lets {} go ({})", PMob->getName(), ready.why));
                return true;
            }
            // A pull that has turned unclean on the way in (a guard roamed
            // over it) is let go, as the fight lets it go -- whoever chose
            // it, and said, so she never stands at a rim with no reason
            if (!PMob->PAI->IsEngaged() && !pawn::huntRulesOf(pawn::ordersOwnerOf(static_cast<const CCharEntity*>(POwner))).aggressive)
            {
                if (const auto unclean = PullBlocker(PMob); !unclean.empty())
                {
                    Transition(IdleMode(), fmt::format("lets {} go ({})", PMob->getName(), unclean));
                    return true;
                }
            }
            // Receive with the weapon away too: wait while the pull comes
            // in, then close if it stalls. Offensive gambits still run in
            // DoRoamTick, so Provoke need not wait for the receive or draw.
            if (join && TowsAtStake())
            {
                if (POwner->PAI->CanFollowPath() && POwner->GetSpeed() > 0)
                {
                    RefreshDangers(PMob);
                    Move(TowIntent(PMob));
                }
                return true;
            }
            WalkToward(PMob);
            return true;
        }
    }

    return false;
}

auto CPawnController::DoRoamTick(const timer::time_point tick) -> Task<void>
{
    TracyZoneScoped;

    if (!POwner->PAI->CanFollowPath())
    {
        co_return;
    }

    // Resting on his order, or her Rest row's, she finishes before she sets
    // out after him. A trek to meet him follows him, and ends where he
    // already is (meetTrek)
    if (!m_RestOrder.active() && pawn::meetTrek(static_cast<const CCharEntity*>(POwner)).has_value())
    {
        if (m_Mode != Mode::Travel)
        {
            Transition(Mode::Travel, "ordered to another zone");
        }
        TravelTick();
        co_return;
    }

    if (pawn::walkOrderOf(POwner->id).has_value())
    {
        if (m_Mode != Mode::Walk)
        {
            Transition(Mode::Walk, "walked by the player");
        }
        WalkTick();
        co_return;
    }
    if (m_Mode == Mode::Walk)
    {
        Transition(IdleMode(), "the walk is over");
    }

    // A world body on her own, or leading her camp: nobody to follow. Her
    // walk in on a mob is the shared one, anchored on herself and paced by
    // nothing but her own rest; the rest of her idle is Roam (RoamTick). A
    // camp member has a leader, and takes the party path below on her; so
    // does a body in a real player's party (any party of hers that is not
    // her camp's -- the member list empties while the player crosses a
    // zone line, the party does not), whether or not they are in her zone:
    // invited from another city she holds where she stands until gathered,
    // and roams for nobody
    const bool playersParty = static_cast<CCharEntity*>(POwner)->PParty != nullptr && pawn::world::campLeaderOf(POwner->id) == 0;
    if (m_World && GetAnchor() == nullptr && !playersParty)
    {
        if (m_Approach.has_value() && ApproachTick(POwner->loc.p, POwner->GetMLevel(), std::string(), pawn::world::isFarming(POwner->id), nullptr))
        {
            co_return;
        }
        if (m_Mode == Mode::Follow || m_Mode == Mode::Wait || m_Mode == Mode::Travel)
        {
            Transition(Mode::Roam, "on her own");
        }
        if (m_Mode == Mode::Roam)
        {
            RoamTick();
        }
        co_return;
    }

    // The player she is with, in her zone and her party; nobody otherwise.
    // Idle is the floor: with nobody here and nothing sending her anywhere,
    // the idle tick below runs where she stands, its parts about the player
    // skipped -- just spawned, signed in, out of the party, or told to wait
    CCharEntity* PPlayer = GetAnchor();
    if (PPlayer == nullptr)
    {
        // Gone by magic a moment ago -- a warp, a teleport -- she waits
        // where she stands and says so; gone on foot, she follows through
        // the zone line as ever
        if (Treks() && m_PlayerMagicSeen != timer::time_point::min() && m_Tick - m_PlayerMagicSeen < 5s)
        {
            m_PlayerMagicSeen = timer::time_point::min();
            HoldForWarp(fmt::format("waits in {} (the player warped away)", POwner->loc.zone != nullptr ? POwner->loc.zone->getName() : "?"));
        }
        // In the player's party with the player in another zone, she goes to
        // them, unless told to wait. A player out of the world is loading
        // between zones -- their character is gone from every zone and from
        // the party's list until they land -- and she keeps to the trek.
        // The one she follows is the real player in her party, wherever he
        // is: in another zone she goes to him, owned or wild alike. While he
        // loads between zones the party's list has nobody real in it for a
        // moment, and a trek already under way keeps to it
        const auto* PParty    = static_cast<CCharEntity*>(POwner)->PParty;
        const auto* PPlayer   = pawn::partyPlayer(static_cast<const CCharEntity*>(POwner));
        const bool  elsewhere = PPlayer != nullptr && PPlayer->loc.zone != nullptr && PPlayer->getZone() != POwner->getZone();
        if (PPlayer != nullptr)
        {
            m_NoPlayerSince = {};
        }
        else if (m_NoPlayerSince == timer::time_point{})
        {
            m_NoPlayerSince = tick;
        }
        const bool loading = PPlayer == nullptr && m_Mode == Mode::Travel && tick - m_NoPlayerSince < 20s;
        if (Treks() && PParty != nullptr && (elsewhere || loading) && !m_RestOrder.active())
        {
            if (m_Mode != Mode::Travel)
            {
                Transition(Mode::Travel, "the player is in another zone");
            }
            TravelTick();
            co_return;
        }
        if (m_Mode == Mode::Travel)
        {
            Transition(IdleMode(), "nowhere to go");
        }
    }
    else
    {
        NotePlayerMagic(PPlayer);

        // The automatic hold a warp set lifts with the player back in her
        // zone; one he ordered holds until he lifts it
        if (cardian::hold::reunited(HoldNow()) != HoldNow())
        {
            SetWaiting(false, false, fmt::format("follows again ({} is back)", PPlayer->getName()));
        }
        if (m_Mode == Mode::Travel)
        {
            Transition(IdleMode(), fmt::format("with {} again", PPlayer->getName()));
        }

        ShareSignet(PPlayer);
        // Walking in on a mob, her eye is on it (below), not the player
        if (!Acting() && !m_Approach.has_value())
        {
            HeadLook(distance(POwner->loc.p, PPlayer->loc.p) < 40.0f ? PPlayer : nullptr);
        }
    }

    // Somewhere to go: her place in formation round the party's place --
    // the player, or the stake -- and the hunt round the player. Waiting,
    // she has neither, whatever the party's plan. The hunt is the party's
    // strategy, not a gambit, so her gambit switch does not gate it; a
    // mage whose rows take no fight never pulls, since she attends
    // (RESEARCH §12.15), and one with an Attack row fights her own pull
    const Place* place         = CurrentPlace(PPlayer);
    const bool   somewhereToGo = place != nullptr && !m_Waiting;
    const bool   hunting       = somewhereToGo && PPlayer != nullptr && IsHunting() &&
                         cardian::engage::huntsForParty(pawn::tactics::offersSpells(POwner), TakesFights());

    TidyBag();
    m_Gambits->TickBehaviors();

    // The engage door (engage_math.h): the fight her Attack rows take
    // (EngageChoice), drawn on through the one door (Draw) -- the rules,
    // then the draw, or the walk in when it is farther than she may draw
    // from; with none, a mage with spells to offer and a place to keep cure
    // range to attends the party's fight (PartyFightScan) from the
    // perimeter, so by default she attends without engaging monsters. With
    // her gambits off neither is hers: she takes no fight of her own, and
    // only an order (EngageOn, the command window's Attack) sends her in.
    // A walk in already under way passed the door once; the approach
    // below draws. Fights are looked for round the party's place -- or,
    // holding her position, round her own: she can draw only on what she
    // can reach from where she stands, and a mob on her far from the
    // player is still hers
    namespace engage        = cardian::engage;
    const position_t from   = place != nullptr && !m_Waiting ? place->position() : POwner->loc.p;
    const bool       spells = pawn::tactics::offersSpells(POwner);
    FightPick        party;
    engage::How      how = engage::How::Draw;
    // An attendance she has committed to -- the mob engaged, or the
    // player's order -- is kept until it ends or an order changes it, as a
    // drawn fighter keeps hers, while she still attends it. The leash
    // chooses new fights, not this one's continued attendance, and
    // PlayerZoning explicitly ends the commitment. A row that now claims
    // the same mob draws her onto it (the melee-mage upgrade); no role and
    // no row -- her gambits switched off mid-fight -- and she stops
    // attending, rather than drawing on it
    if (auto* PAttended = AttendedTarget(); PAttended != nullptr && (AttendedEngaged() || m_AttendedOrdered))
    {
        const auto claim = ClaimingRow(PAttended);
        switch (engage::keptAttendance(spells, claim.has_value()))
        {
            case engage::Kept::Attend:
                party = { PAttended, "the fight she is attending" };
                how   = engage::How::Attend;
                break;
            case engage::Kept::Draw:
                party = { PAttended, fmt::format("{}, {}", FoeWhy(claim->finder, PAttended, PPlayer), pawn::rowLabel(claim->row)), claim->row.index };
                how   = engage::How::Draw;
                break;
            case engage::Kept::Stop:
                Transition(IdleMode(), fmt::format("stops attending {} (her rows no longer offer the fight a spell)", PAttended->getName()));
                break;
        }
    }
    if (party.target == nullptr)
    {
        // A camp member in the wild follows her camp's leader (GetAnchor);
        // the party's fight backs up her own rows there, so her camp never
        // stands idle through a fight (engage_math.h doorAnswer)
        const bool campMember = m_World && PPlayer != nullptr && pawn::world::campLeaderOf(POwner->id) == PPlayer->id;
        party                 = EngageChoice(PPlayer, from);
        const auto fight      = party.target == nullptr && (spells || campMember) ? PartyFightScan(PPlayer, from) : FightPick{};
        const auto answer     = engage::doorAnswer(party.target != nullptr, fight.target != nullptr, spells, place != nullptr, campMember);
        if (party.target == nullptr && answer.has_value())
        {
            party = fight;
        }
        how = answer.value_or(engage::How::Draw);
    }
    CBattleEntity* PPartyTarget = party.target;
    // Walking in on the party's fight, or on the player's order round its
    // mob to the back before she draws (TakesBackFirst): the approach has
    // her, and the door does not draw her again every beat
    const bool     walkingIn    = m_Approach.has_value() &&
                              (m_Approach->kind == ApproachKind::Join || (m_Approach->kind == ApproachKind::Order && m_BackFirst.has_value()));
    // The door's state, once a second, while a fight is on around her and she
    // has not passed it: what stands in the way of this tick
    if (settings::get<bool>("pawn.FORMATION_DEBUG") && m_Tick - m_DoorSaidAt >= 1s)
    {
        const auto around      = PartyFightScan(PPlayer, from);
        const bool fightAround = (PPlayer != nullptr && PPlayer->PAI->IsEngaged()) || around.target != nullptr || PPartyTarget != nullptr;
        if (fightAround)
        {
            m_DoorSaidAt = m_Tick;
            ShowInfoFmt("pawn: door {}: target {}{}{}, party's fight {}, walking in {}, held off {}, pending {}{}", POwner->getName(),
                        PPartyTarget != nullptr ? PPartyTarget->getName() : "none",
                        PPartyTarget != nullptr ? fmt::format(" ({})", party.why) : "",
                        PPartyTarget != nullptr && how == engage::How::Attend ? ", attends" : "",
                        around.target != nullptr ? around.target->getName() : "none",
                        walkingIn, PPartyTarget != nullptr && HoldingOff(PPartyTarget),
                        PPartyTarget != nullptr && PendingIs(Pending::Act::Join, PPartyTarget) ? (Due(Pending::Act::Join, PPartyTarget) ? "due" : "waiting") : "none",
                        PPartyTarget != nullptr ? fmt::format(", leash {:.0f} y from ({:.0f}, {:.0f})", settings::get<float>("pawn.HUNT_LEASH"), from.x, from.z) : "");
        }
    }
    if (PPartyTarget != nullptr && !walkingIn && !HoldingOff(PPartyTarget) && !(Attending(PPartyTarget) && how == engage::How::Attend))
    {
        const auto facts = EngageFactsFor(PPartyTarget);
        if (const auto why = Refusal(PPartyTarget, facts); !why.empty())
        {
            // A refusal that stands (claimed by another party, an idle
            // target the party would not pull) is said once, the target
            // left alone for a while, and she keeps to the formation
            SayRefusal(PPartyTarget, why);
            HoldOff(PPartyTarget);
        }
        else
        {
            // The beat: the party's fight is seen now, her draw comes a
            // beat later, her eyes on it in the meantime; a fight she
            // attends waits none. Due, the rules run again at the door
            // (Draw): a refusal then is said and holds the target off
            if (!PendingIs(Pending::Act::Join, PPartyTarget))
            {
                Schedule(Pending::Act::Join, PPartyTarget, engage::waitsBeat(how) ? ReactionBeat() : timer::duration::zero());
            }
            if (!Due(Pending::Act::Join, PPartyTarget))
            {
                HeadLook(PPartyTarget);
                co_return;
            }
            m_Pending.reset();

            // Drawn on the player's word alone: hold until they strike, or
            // the mob comes to us. A pull, an answer to aggro, or an order
            // closes.
            const bool hold = PPlayer != nullptr && PPlayer->PAI->IsEngaged() && PPartyTarget == PPlayer->GetBattleTarget() &&
                              !playerHasEnmity(PPlayer, PPartyTarget) && !PPartyTarget->PAI->IsEngaged();
            if (!Draw(PPartyTarget, ApproachKind::Join, hold ? fmt::format("holding for {}'s strike", PPlayer->getName()) : party.why, hold) &&
                !m_Approach.has_value())
            {
                HoldOff(PPartyTarget);
            }
            co_return;
        }
    }

    // Attending, her exit is her mob's end -- dead or gone -- or the door
    // having no fight for her at all, said with what became of the mob, or
    // that she no longer attends fights
    if (m_Mode == Mode::Attend)
    {
        auto* PAttended = m_Attended.has_value() ? m_Attended->resolve<CBattleEntity>() : nullptr;
        if (PPartyTarget == nullptr || PAttended == nullptr || PAttended->isDead())
        {
            // At a place that never moves, the spot she attended from is
            // her seat until the place is set again: no tightening up
            // between fights (the user, 2026-09-17)
            auto& held = FormationSlot() == pawn::Slot::Lead ? m_LeadHeld : m_FollowHeld;
            if (place != nullptr && place->fixed() && held.has)
            {
                ShowInfoFmt("pawn: {} keeps her spot at ({:.1f}, {:.1f}) after the fight ({:.1f} y from her seat)", POwner->getName(), POwner->loc.p.x, POwner->loc.p.z, distance(POwner->loc.p, held.point, true));
                held.point = POwner->loc.p;
            }
            // Where she waits for the next pull (CampWaitIntent): the spot she
            // attended from, when it lies behind the flag and within her cure
            // range of where the next pull lands; one the fight pushed her
            // into at the flag, or dragged far off, is planned afresh
            if (place != nullptr && place->fixed())
            {
                const auto camp    = place->position();
                const auto landing = nearPosition(camp, cardian::stake::kMobAhead, 0.0f);
                if (cardian::stake::forwardOf(camp.x, camp.z, camp.rotation, POwner->loc.p.x, POwner->loc.p.z) <= -2.0f &&
                    distance(POwner->loc.p, landing, true) <= CastRange())
                {
                    m_CampWaitPoint = POwner->loc.p;
                }
                else
                {
                    m_CampWaitPoint.reset();
                }
            }
            Transition(IdleMode(), !spells && PAttended != nullptr && !PAttended->isDead()
                                       ? fmt::format("stops attending {} (her rows no longer offer the fight a spell)", PAttended->getName())
                                       : AttendExitReason());
        }
    }

    // The player has drawn on a burrowed mob: the party waits for it to
    // surface, and says so now and then
    if (PPlayer != nullptr && PPlayer->PAI->IsEngaged())
    {
        if (auto* PMob = dynamic_cast<CMobEntity*>(PPlayer->GetBattleTarget());
            PMob != nullptr && pawn::isUnderground(PMob) && !PMob->PAI->IsEngaged() && m_Tick - m_LastSurfaceLogTime > 5s)
        {
            m_LastSurfaceLogTime = m_Tick;
            ShowInfoFmt("pawn: {} waits for {} to surface", POwner->getName(), PMob->getName());
        }
    }

    // Walking in on a mob (ApproachTick): the party's place is the anchor
    // -- the player or the stake, or with neither here, herself -- and the
    // band is judged at the player's level
    if (m_Approach.has_value())
    {
        const position_t anchorAt = place != nullptr ? place->position() : POwner->loc.p;
        const uint8      level    = PPlayer != nullptr ? PPlayer->GetMLevel() : POwner->GetMLevel();
        if (ApproachTick(anchorAt, level, PPlayer != nullptr ? PacingBlocker(PPlayer) : std::string(), hunting, PPartyTarget))
        {
            // Walking in on the party's fight, she thinks on the way: a
            // Provoke or a spell goes out as she comes, once the mob is on
            // us. A hunt's walk in is the pull itself, and thinks nothing
            if (m_Approach.has_value() && m_Approach->kind != ApproachKind::Hunt && !Acting())
            {
                if (auto* PMob = m_Approach->target.resolve<CBattleEntity>(); PMob != nullptr && PMob->PAI->IsEngaged())
                {
                    m_Gambits->Tick(tick, true);
                }
            }
            co_return;
        }
    }

    // A hunter picks the party's next fight itself, the moment it is free
    // to: the choice is never throttled, only the draw. Resting on an
    // order, or with her Rest row asking for one (RestTick, below the
    // pick), she picks nothing
    if (hunting && !m_Approach.has_value() && !m_RestOrder.active() && !RestRowDue())
    {
        const auto blocker = HuntBlocker(PPlayer);
        if (blocker.empty())
        {
            std::string skipped;
            if (auto* PMob = PickHuntTarget(PPlayer, &skipped); PMob != nullptr)
            {
                // The beat: the choice is made now, the walk starts a beat
                // later, her eyes on it in the meantime
                const auto beat = ReactionBeat();
                m_HuntBeat      = beat;
                m_Approach      = Approach{ EntityId(PMob), ApproachKind::Hunt };
                Transition(Mode::Approach, fmt::format("sets off after {} ({}{})", PMob->getName(),
                                                       magic_enum::enum_name(charutils::CheckMob(PPlayer->GetMLevel(), PMob)),
                                                       beat > 0s ? fmt::format(", in {:.1f}s", std::chrono::duration<float>(beat).count()) : ""));
                Schedule(Pending::Act::SetOff, PMob, beat);
                co_return;
            }
            // A quiet hunt says why, now and then: the band is judged
            // against the player's level, and a low zone has nothing in it
            if (m_Tick - m_LastHuntLogTime > 15s)
            {
                m_LastHuntLogTime = m_Tick;
                const auto rules = pawn::huntRulesOf(pawn::ordersOwnerOf(static_cast<const CCharEntity*>(POwner)));
                ShowInfoFmt("pawn: {} finds nothing to hunt within {} y of {} (level {}; band {}..{}, idle and unclaimed{}{}){}",
                            POwner->getName(), settings::get<float>("pawn.HUNT_RADIUS"), PPlayer->getName(), PPlayer->GetMLevel(),
                            magic_enum::enum_name(static_cast<EMobDifficulty>(rules.minCheck)), magic_enum::enum_name(static_cast<EMobDifficulty>(rules.maxCheck)),
                            rules.aggressive ? "" : ", aggressive company avoided", rules.links ? "" : ", links avoided",
                            skipped.empty() ? "" : "; skipped " + skipped);
            }
        }
        else if (m_Tick - m_LastHuntLogTime > 15s)
        {
            m_LastHuntLogTime = m_Tick;
            ShowInfoFmt("pawn: {} hunt waits: {}", POwner->getName(), blocker);
        }
    }

    // The tick's danger map, then her proposal through the walker: her place
    // in formation, paced by the formation's catch-up; or with nowhere to
    // go, where she stands at her normal speed -- vetted like any other, so
    // a circle nudges her clear and she stays where it leaves her. Nothing
    // when the tick went on a warp
    // The fight turning on -- the first strike -- is a new think: her rows
    // are read that tick, not on the cadence
    const bool attendedEngaged = AttendedEngaged();
    if (attendedEngaged && !m_AttendedEngaged)
    {
        m_Gambits->Prompt();
    }
    m_AttendedEngaged = attendedEngaged;

    RefreshDangers(AttendedTarget());
    Intent proposal;
    if (auto* PAttended = dynamic_cast<CMobEntity*>(AttendedTarget()); PAttended != nullptr)
    {
        proposal = AttendIntent(PAttended, place);
        // A kneeling mage puts her move to her attend spot off until the mob
        // comes in: within its ring and this many yalms more of her, the
        // seconds it takes to cover them hers to reach the spot (the user,
        // 2026-10-04: rest until topped up or a monster comes in)
        constexpr float kComesIn = 8.0f;
        proposal.comesIn         = m_AttendRing > 0.0f && distance(POwner->loc.p, PAttended->loc.p, true) < m_AttendRing + kComesIn;
    }
    else if (somewhereToGo && !AwaitsArrival(*place) && !m_RestOrder.active()) // resting on his order, or her Rest row's, she stays where she kneels
    {
        // At a camp an attending mage's seat is where she will attend the next pull from
        proposal = place->fixed() && WaitsAtCampSpot() ? CampWaitIntent(*place) : FormationIntent(*place, PPlayer, nullptr);
    }
    else
    {
        RestoreNormalSpeed();
        proposal.kind = Intent::Kind::Keep;
    }
    // The claim board's slide before the rest's judgement, so a mage that
    // must give way is not kneeled on a spot another member has claimed,
    // and one beside it is judged on the spot she will stand on; the
    // walker's own pass then finds it clear
    ClaimSpot(proposal);
    const bool stationary = (proposal.kind == Intent::Kind::Stand || proposal.kind == Intent::Kind::Keep ||
                             distance(POwner->loc.p, proposal.point) <= proposal.tolerance) &&
                            !POwner->PAI->PathFind->IsFollowingPath();
    // RestTick may defer a proposed seat adjustment during support recovery.
    // An active path is already movement, and still requires a stand.
    // RestTick first checks urgent healing, danger and orders. Only when it
    // keeps her down do we suppress the proposal; Move still vets her current
    // position for aggro/link danger and may escape it.
    // A kneeling mage puts a routine step off only while the party's place
    // -- the player, or the stake -- is within her cure range: from there
    // she still heals everyone as she rests. Farther, the party has moved
    // on, and she gets up to follow (the user, 2026-10-04: Gabriol rested
    // on 200 yalms behind after a new stake)
    const bool nearPlace = place == nullptr || distance(POwner->loc.p, place->position(), true) <= std::max(CastRange(), 10.0f);
    if (RestTick(stationary, false, !POwner->PAI->PathFind->IsFollowingPath() && !proposal.comesIn && nearPlace))
    {
        proposal.kind      = Intent::Kind::Stand;
        proposal.seat      = false;
        proposal.holdsSpot = false;
    }
    const auto avoidAction = Move(proposal);
    if (!avoidAction.has_value())
    {
        co_return;
    }

    // Her think runs whether she stands or walks: a cast she wants stops
    // the walk (CastAndStop), never the other way round (the user,
    // 2026-09-17) -- except the step out of an aggro circle, the one walk
    // a cast would undo by rooting her inside it. Attending an engaged mob,
    // the fight's spells too. The emote, standing still only
    if (!POwner->PAI->IsCurrentState<CMagicState>() && *avoidAction != AvoidAction::Escape)
    {
        m_Gambits->Tick(tick, attendedEngaged);
        if (!POwner->PAI->PathFind->IsFollowingPath() && *avoidAction != AvoidAction::Detour)
        {
            IdleEmote(PPlayer);
        }
    }

    co_return;
}

auto CPawnController::FormationIntent(const Place& place, const CCharEntity* PPlayer, const CBattleEntity* PStandOff) -> Intent
{
    // A quiet mage keeps her last spot, even far from camp. Incoming aggro
    // outside the leash sends her home instead: release that distant spot
    // and let ordinary formation movement and target acquisition take over.
    auto& held = FormationSlot() == pawn::Slot::Lead ? m_LeadHeld : m_FollowHeld;
    const float leash = settings::get<float>("pawn.HUNT_LEASH");
    if (place.fixed() && held.has && !isWithinDistance(place.position(), POwner->loc.p, leash) && !isWithinDistance(place.position(), held.point, leash))
    {
        if (const auto* PAttacker = SelfDefenceTarget(); PAttacker != nullptr)
        {
            held.has = false;
            ShowInfoFmt("pawn: {} leaves her kept spot and returns to camp ({} is on her outside the leash)", POwner->getName(), PAttacker->getName());
        }
    }

    // Where this pawn belongs: the lead holds a point ahead of the place,
    // everyone else a seat on the ring around it. FormationPoint sets
    // m_HasSlot for the avoidance pass.
    m_HasSlot = false;
    position_t followPoint{};

    // No point within reach of a mob the party is holding on: pushed out
    // to the ring, and round to the place's side from behind it
    const auto standOff = [&](position_t point) -> position_t
    {
        if (PStandOff == nullptr)
        {
            return point;
        }
        const auto  ours   = place.position();
        const float radius = POwner->GetMeleeRange(PStandOff) + settings::get<float>("pawn.FORMATION_STANDOFF");
        const auto [x, z]  = cardian::formation::standOff(PStandOff->loc.p.x, PStandOff->loc.p.z, ours.x, ours.z, radius, point.x, point.z);
        if (x != point.x || z != point.z)
        {
            point.x = x;
            point.y = PStandOff->loc.p.y;
            point.z = z;
        }
        return point;
    };

    if (FormationSlot() == pawn::Slot::Lead)
    {
        followPoint = standOff(LeadPoint(place, PPlayer));
        RampCatchUp(m_PlayerMoving, followPoint);
    }
    else
    {
        // Everyone else follows the place itself, in a seat on the ring
        // around it: the same fresh position the lead uses, with a gentle
        // prediction, parked and held the way the lead holds its point. (A
        // seat, not the place: a fresh position would put her right on top
        // of the player.)
        const auto slot   = RingSlot();
        const auto seat   = SeatOf(slot);
        const auto anchor = place.anchor(settings::get<float>("pawn.FORMATION_FOLLOW_PREDICT_SCALE"));
        followPoint       = standOff(FormationPoint(anchor, seat.offset, seat.angle, m_FollowHeld));
        RampCatchUp(anchor.moving, followPoint);
        FormationDebug(cardian::formation::slotName(slot), PPlayer, anchor, followPoint);
    }

    Intent intent;
    intent.kind       = Intent::Kind::Formation;
    intent.point      = followPoint;
    intent.arrive     = 1.0f;
    intent.tolerance  = 2.0f;
    intent.warpIfLost = true;

    return intent;
}

auto CPawnController::CourtesyStep(const position_t& point) -> position_t
{
    const float bodyCost = settings::get<float>("pawn.COURTESY_BODY_COST");
    const auto* PPlayer  = GetLivePlayer();
    auto*       navMesh  = POwner->loc.zone != nullptr ? POwner->loc.zone->navMesh() : nullptr;
    if (bodyCost <= 0.0f || PPlayer == nullptr || PPlayer == POwner || PPlayer->loc.zone != POwner->loc.zone || navMesh == nullptr)
    {
        return point;
    }

    // The field: the player's body, and their wake -- a lane along the way
    // they move, long while they move, short while they stand. Built
    // where the player is on their own screen when it shows her where
    // the server has her now: the streamed position carried by the
    // prediction (0.7 s, about the lag of her position on their screen),
    // along the way they move; standing, the streamed position and the
    // way they face. Two lags stack on the player's screen -- their own
    // position is live there, hers is the server's last word -- and at
    // run speed that is a few yalms along the line of motion, which a
    // field built on the server's positions alone leaves out
    const auto       a      = PlayerAnchor(PPlayer, 1.0f);
    const bool       moving = a.moving;
    const position_t p      = moving ? a.anchor : a.observed;
    position_t       ahead  = nearPosition(p, 1.0f, 0.0f);
    if (moving && a.ahead > 0.1f)
    {
        ahead = position_t(p.x + (a.anchor.x - a.observed.x) / a.ahead, p.y, p.z + (a.anchor.z - a.observed.z) / a.ahead, 0, 0);
    }
    const auto& me   = POwner->loc.p;
    const float wake = settings::get<float>(moving ? "pawn.COURTESY_WAKE_RUN" : "pawn.COURTESY_WAKE_STANDING");

    // The wake in two bands, wider and dearer the further ahead: a narrow
    // one from a body's radius ahead of the player (so the seats beside
    // and behind them lie outside it), and the full-width one from its
    // own radius ahead. Where they overlap -- the player's own line ahead
    // -- the cost is the sum: the dearest cells she can step on
    const float body     = settings::get<float>("pawn.COURTESY_BODY_RADIUS");
    const float width    = settings::get<float>("pawn.COURTESY_WAKE_RADIUS");
    const float wakeCost = settings::get<float>("pawn.COURTESY_WAKE_COST");
    const auto  alongWay = [&](const float yalms)
    {
        return std::pair{ p.x + (ahead.x - p.x) * yalms, p.z + (ahead.z - p.z) * yalms };
    };
    cardian::planner::Field field;
    field.discs.push_back(cardian::planner::Disc{ p.x, p.z, body, bodyCost });
    for (const auto [from, radius] : { std::pair{ body, body + 0.5f }, std::pair{ width, width } })
    {
        if (wake > from)
        {
            const auto [ax, az] = alongWay(from);
            const auto [bx, bz] = alongWay(wake);
            field.capsules.push_back(cardian::planner::Capsule{ ax, az, bx, bz, radius, wakeCost });
        }
    }
    // The local goal: the destination when it is within the window, else
    // the mesh's own way to it, at the window's edge
    constexpr int   kWindow = 12;
    constexpr float kReach  = 6.0f; // this tick's step target, that far along the plan: a tick's run at catch-up speed, with the arrive distance to spare
    const float     edge    = static_cast<float>(kWindow) - 1.0f;
    position_t      goal    = point;
    if (distance(me, point, true) > edge)
    {
        const float span = distance(me, point, true);
        goal             = position_t(me.x + (point.x - me.x) * edge / span, point.y, me.z + (point.z - me.z) * edge / span, 0, 0);
        if (const auto path = routeOf(navMesh, POwner, me, point); path.has_value() && !path->points.empty())
        {
            float      walked = 0.0f;
            position_t prev   = me;
            for (const auto& pp : path->points)
            {
                const float seg = distance(prev, pp.position, true);
                if (walked + seg >= edge)
                {
                    const float t = (edge - walked) / std::max(seg, 0.01f);
                    goal          = position_t(prev.x + (pp.position.x - prev.x) * t, pp.position.y, prev.z + (pp.position.z - prev.z) * t, 0, 0);
                    break;
                }
                walked += seg;
                prev = pp.position;
                goal = pp.position;
            }
        }
    }

    // Only a walk that would pass through the field is planned; the rest
    // go straight, as they always did
    if (!field.reaches(me.x, me.z, goal.x, goal.z))
    {
        m_CourtesySide = 0.0f;
        return point;
    }

    cardian::planner::Query q;
    q.sx       = me.x;
    q.sz       = me.z;
    q.gx       = goal.x;
    q.gz       = goal.z;
    q.window   = kWindow;
    q.sideBias = m_Tick - m_LastCourtesyTime <= 1s ? m_CourtesySide : 0.0f;

    const auto walkable = [&](const float x, const float z)
    {
        return navMesh->validPosition(position_t(x, me.y, z, 0, 0));
    };
    const auto plan = cardian::planner::plan(q, field, walkable);
    if (!plan.has_value() || plan->points.size() < 2)
    {
        return point;
    }

    // Her side of the line, kept for next tick's tie
    const auto& early = plan->points[std::min<std::size_t>(2, plan->points.size() - 1)];
    const float cross = (goal.x - me.x) * (early.second - me.z) - (goal.z - me.z) * (early.first - me.x);
    if (cross != 0.0f)
    {
        m_CourtesySide = cross > 0.0f ? 1.0f : -1.0f;
    }
    m_LastCourtesyTime = m_Tick;
    if (plan->length > plan->straight + 0.5f && m_Tick - m_LastCourtesySaid > 3s)
    {
        m_LastCourtesySaid = m_Tick;
        ShowInfoFmt("pawn: {} goes round {} ({:.1f}y further, {})", POwner->getName(), PPlayer->getName(), plan->length - plan->straight, moving ? "their wake" : "standing");
    }

    // The step target that far along the plan; a plan not much longer than
    // that ends at the goal itself, so the arrive distance is measured
    // from the goal and not from a point a yalm short of it
    const auto [x, z] = cardian::planner::along(*plan, plan->length <= kReach + 1.5f ? plan->length : kReach);
    return position_t(x, me.y, z, 0, 0);
}

auto CPawnController::InManeuver() const -> bool
{
    return m_Mode == Mode::Maneuver;
}

auto CPawnController::BeginManeuver(CCharEntity* PBy, uint32* other) -> uint16
{
    if (PBy == nullptr)
    {
        return CL_S_REFUSED;
    }
    if (InManeuver())
    {
        if (m_ManeuverBy != PBy->id)
        {
            return CL_S_ANOTHER_PLAYERS;
        }
        return m_ManeuverComposed ? CL_S_MANEUVER_COMPOSED : CL_S_OK;
    }
    if (POwner->isDead())
    {
        return CL_S_KNOCKED_OUT;
    }
    // A fight is no bar (docs/maneuvers.md: the boss pull that went
    // sideways). She stays engaged through it, the tick leaves an engaged
    // maneuver alone, and its end hands her straight back to the fight
    if (cardian::view::origin(PBy) != POwner)
    {
        return CL_S_NOT_LOOKING;
    }
    if (const auto driven = pawn::maneuverOf(PBy->id); driven != 0 && driven != POwner->id)
    {
        if (other != nullptr)
        {
            *other = driven;
        }
        return CL_S_ONE_MANEUVER;
    }

    // Kneeling, she stays down, and a rest he ordered goes on: the wheel
    // taken is no reason to rise. The ring's first step stands her
    // (WalkOrderTick), an order he composes replaces the rest
    // (SetQueuedOrder), an action it fires stands her as any does, and a
    // maneuver that rests her keeps her ticks (the user, 2026-10-03: Zapp
    // stood to take "Rest until 100%" and lost his)
    // The wheel taken replaces her line: the maneuver is his command now, and
    // an order left in it would fire mid-drive or be lost under the
    // maneuver's own (his commands win)
    ClearQueuedOrders("he takes the wheel");
    m_ManeuverBy          = PBy->id;
    m_ManeuverComposed    = false;
    m_ManeuverResting     = false;
    m_ManeuverPriorMaster = m_Gambits->MasterOn();
    m_Gambits->SetMaster(false);
    pawn::setManeuver(PBy->id, POwner->id);
    Transition(Mode::Maneuver, fmt::format("{} takes the wheel", PBy->getName()));
    TellManeuver(CL_MS_LIVE);
    return CL_S_OK;
}

// The player whose maneuver it is hears every change of it
void CPawnController::TellManeuver(const uint8 state) const
{
    auto msg    = cardian::link::make<cl_maneuver_state>();
    msg.cardian = POwner->id;
    msg.state   = state;
    cardian::link::send(m_ManeuverBy, msg);
}

void CPawnController::EndManeuver(const std::string_view why)
{
    if (InManeuver())
    {
        Transition(IdleMode(), why);
    }
}

// His order has left her: the maneuver's end, and his hand-back. What the
// game makes of the order from here is hers to carry out (the user,
// 2026-09-22: "she does the rest"); a refusal reaches him as a note
void CPawnController::NoteOrderFired()
{
    EndManeuver("his order is away, the maneuver ends");
}

auto CPawnController::ManeuverComposed() const -> bool
{
    return InManeuver() && m_ManeuverComposed;
}

auto CPawnController::ManeuverBy() const -> uint32
{
    return InManeuver() ? m_ManeuverBy : 0;
}

auto CPawnController::OwnMaster() const -> bool
{
    return InManeuver() ? m_ManeuverPriorMaster : m_Gambits->MasterOn();
}

void CPawnController::SetOwnMaster(const bool on)
{
    if (InManeuver())
    {
        m_ManeuverPriorMaster = on; // the maneuver's end restores it
        return;
    }
    m_Gambits->SetMaster(on);
}

// Move here, or Move here and hold: paused, the route the ring laid; live,
// the point the ring stands on now (OPEN_ISSUES #273: a live maneuver's "go
// here and wait"). Either way her one queued command, and the maneuver
// lasts until she has walked it
auto CPawnController::ComposeMove(const bool wait) -> uint16
{
    if (!InManeuver())
    {
        return CL_S_NO_MANEUVER;
    }
    if (m_ManeuverComposed)
    {
        return CL_S_MANEUVER_COMPOSED;
    }
    if (!pawn::walkOrderOf(POwner->id).has_value())
    {
        return CL_S_NO_ROUTE;
    }
    SetQueuedOrder(std::make_pair(std::string(wait ? "movewait" : "move"), EntityId(POwner)));
    MarkComposed(fmt::format("walk the route{}", wait ? ", then hold there" : ""));
    return CL_S_OK;
}

// A maneuver's order is given -- held, or a rest either way: composed, it
// needs his eye no more. His one live maneuver is free again, for the next
// cardian's: a pause queues one per cardian (the user, 2026-09-23)
void CPawnController::MarkComposed(const std::string_view what)
{
    m_ManeuverComposed = true;
    if (pawn::maneuverOf(m_ManeuverBy) == POwner->id)
    {
        pawn::clearManeuver(m_ManeuverBy);
    }
    ShowInfoFmt("pawn: {}'s maneuver is composed: {}", POwner->getName(), what);
    TellManeuver(CL_MS_COMPOSED);
}

auto CPawnController::ComposeRest(const int percent) -> uint16
{
    if (!InManeuver())
    {
        return CL_S_NO_MANEUVER;
    }
    if (m_ManeuverComposed)
    {
        return CL_S_MANEUVER_COMPOSED;
    }
    if (percent < 1 || percent > 100)
    {
        return CL_S_MALFORMED;
    }
    if (cardian::rest::Order{ .percent = percent }.metBy(POwner->health.hp, POwner->GetMaxHP(), POwner->health.mp, POwner->GetMaxMP()))
    {
        return CL_S_ALREADY_RESTED;
    }
    // Live, she rests where she stands: the point she was steered toward is let go
    if (!cardian::pause::isHeld())
    {
        pawn::clearWalkOrder(POwner->id);
    }
    SetQueuedOrder(std::make_pair(fmt::format("rest:{}", percent), EntityId(POwner)));
    MarkComposed(fmt::format("{}rest until {}%", pawn::walkOrderOf(POwner->id).has_value() ? "walk the route, then " : "", percent));
    return CL_S_OK;
}

auto CPawnController::AttackOrder(CBattleEntity* PTarget) -> uint16
{
    auto* PMob = dynamic_cast<CMobEntity*>(PTarget);
    if (PMob == nullptr || PMob->isDead())
    {
        return CL_S_NOT_A_MONSTER;
    }
    if (POwner->isDead())
    {
        return CL_S_KNOCKED_OUT;
    }
    if (cardian::pause::isHeld())
    {
        m_QueuedNext.clear(); // his attack replaces her line, as it does unpaused
        SetQueuedOrder(std::make_pair(std::string(kAttackOrder), EntityId(PMob)));
        ShowInfoFmt("pawn: {} queues the attack on {} (paused)", POwner->getName(), PMob->getName());
        if (InManeuver())
        {
            MarkComposed(fmt::format("{}attack {}", pawn::walkOrderOf(POwner->id).has_value() ? "walk the route, then " : "", PMob->getName()));
        }
        return CL_S_OK;
    }
    EndManeuver("his order is away, she attacks: the maneuver ends");
    ClearQueuedOrders("his attack order replaces it"); // her newest order is the one she carries out
    m_PlayersOrder = EntityId(PMob);
    EngageOn(PMob);
    return CL_S_OK;
}

auto CPawnController::DisengageOrder() -> uint16
{
    if (POwner->isDead())
    {
        return CL_S_KNOCKED_OUT;
    }
    if (cardian::pause::isHeld())
    {
        m_QueuedNext.clear(); // his disengage replaces her line, as his attack does
        SetQueuedOrder(std::make_pair(std::string(kDisengageOrder), EntityId(POwner)));
        ShowInfoFmt("pawn: {} queues the disengage (paused)", POwner->getName());
        if (InManeuver())
        {
            MarkComposed(fmt::format("{}disengage", pawn::walkOrderOf(POwner->id).has_value() ? "walk the route, then " : ""));
        }
        return CL_S_OK;
    }
    const bool fighting = POwner->PAI->IsEngaged() || m_Mode == Mode::Fight || m_Mode == Mode::Hold || m_Mode == Mode::Approach || m_Mode == Mode::Attend;
    if (!fighting)
    {
        return CL_S_NOT_FIGHTING;
    }
    EndManeuver("his order is away, she disengages: the maneuver ends");
    ClearQueuedOrders("his disengage replaces it");
    StandDown("the player's order: she sheathes");
    return CL_S_OK;
}

// How close an order needs her: the spell's or ability's own range, a weapon
// skill's own range and both hitboxes (weaponSkillReach), the ranged attack's
// distance; a shade inside each, the game's own check being the judge
auto CPawnController::OrderReach(const unsigned kind, const unsigned id, const CBattleEntity* PTarget) const -> float
{
    switch (kind)
    {
        case 1:
            return 24.0f;
        case 2:
        {
            const auto* PSpell = spell::GetSpell(static_cast<SpellID>(id));
            return PSpell != nullptr && PSpell->getRange() > 0.0f ? PSpell->getRange() - 0.5f : 19.5f;
        }
        case 3:
        {
            const auto* PAbility = ability::GetAbility(static_cast<uint16>(id));
            const float range    = PAbility != nullptr ? PAbility->getRange() : 0.0f;
            return range > 0.0f ? range - 0.5f : POwner->GetMeleeRange(PTarget) - 0.3f;
        }
        case kWeaponSkillOrder:
            return weaponSkillReach(POwner, static_cast<uint16>(id), PTarget);
        default:
            return POwner->GetMeleeRange(PTarget) - 0.3f;
    }
}

auto CPawnController::OrderOutOfReach() const -> std::optional<std::pair<CBattleEntity*, float>>
{
    if (!m_QueuedOrder.has_value())
    {
        return std::nullopt;
    }
    unsigned kind = 0;
    unsigned mode = 0;
    unsigned id   = 0;
    if (!parseOrderKey(m_QueuedOrder->first, kind, mode, id))
    {
        return std::nullopt;
    }
    auto* PTarget = m_QueuedOrder->second.resolve<CBattleEntity>();
    if (PTarget == nullptr || PTarget == POwner || PTarget->loc.zone != POwner->loc.zone || PTarget->isDead())
    {
        return std::nullopt; // a dead target is the action's own rules' to refuse, not a walk
    }
    const float reach = OrderReach(kind, id, PTarget);
    if (distance(POwner->loc.p, PTarget->loc.p) <= reach)
    {
        return std::nullopt;
    }
    return std::make_pair(PTarget, reach);
}

auto CPawnController::OrderApproach() -> std::optional<Intent>
{
    if (!m_OrderApproaching)
    {
        return std::nullopt;
    }
    const auto beyond = OrderOutOfReach();
    if (!beyond.has_value())
    {
        return std::nullopt;
    }
    // The player's word walks her up to the mob he named: the vet has no say
    // over the company he chose for her
    Intent intent;
    intent.kind      = Intent::Kind::Path;
    intent.point     = beyond->first->loc.p;
    intent.arrive    = std::max(1.0f, beyond->second - 1.0f);
    intent.tolerance = 0.0f;
    intent.target    = beyond->first;
    intent.vet       = false;
    intent.fallback  = beyond->first->loc.p;
    return intent;
}

namespace
{
    // A walk order's end counts as reached this near: past the walker's "no
    // forward step" arrival (1.5 y), so a point beside a wall she stops short
    // of still counts
    constexpr float kRouteReached = 1.6f;
} // namespace

auto CPawnController::RouteWalked() const -> bool
{
    const auto point = pawn::walkOrderOf(POwner->id);
    if (!point.has_value())
    {
        return true;
    }
    return !pawn::routeFront(POwner->id).has_value() && distance(POwner->loc.p, *point) < kRouteReached;
}

// The maneuver's tick: it lives as long as its driver looks through her --
// or, composed under a hold, as long as its order waits -- and his walk
// order is all that moves her
void CPawnController::ManeuverTick()
{
    if (m_ManeuverComposed)
    {
        if (!m_QueuedOrder.has_value())
        {
            EndManeuver("nothing left to do, the maneuver ends");
            return;
        }
        // its rest, under way (FireQueuedOrder): down until the percent
        if (m_ManeuverResting)
        {
            RestTick(true);
            return;
        }
        // the route by the walk order; the walk into reach by the mover, as
        // any order's (OrderApproach) -- neither tick runs here to call it
        if (auto order = OrderApproach(); order.has_value())
        {
            Move(std::move(*order));
            return;
        }
        WalkTick();
        return;
    }
    const auto* PBy = zoneutils::GetChar(m_ManeuverBy);
    if (PBy == nullptr || cardian::view::origin(PBy) != POwner)
    {
        EndManeuver("the player looks away, the maneuver ends");
        return;
    }
    WalkTick();
}

void CPawnController::WalkTick()
{
    WalkOrderTick(m_Tick);
    if (!cardian::view::timersArmed() && POwner->PAI->PathFind->IsFollowingPath())
    {
        POwner->PAI->PathFind->FollowPath(m_Tick);
    }
}

// The walk order as it stands: arrived (within 0.2 y) she stands on the
// point and the order stays, so a steered walk whose ring she has caught
// resumes the instant the ring moves on; a point she already paths to
// keeps its path; a new point is pathed now. A point the mesh cannot
// reach is dropped -- unless she is all but on it (the walker refuses a
// route with no forward step), which is arrival too. The order lives as
// long as its giver looks through her: his camera off her, his addon
// gone, his logout all end it here. Called from the logic tick and, every
// kSteerPeriodMs, from the steer tick.
void CPawnController::WalkOrderTick(const timer::time_point now)
{
    const auto endPoint = pawn::walkOrderOf(POwner->id);
    if (!endPoint.has_value())
    {
        return;
    }
    const auto* PBy = zoneutils::GetChar(pawn::walkOrderedBy(POwner->id));
    if (!m_ManeuverComposed && (PBy == nullptr || cardian::view::origin(PBy) != POwner))
    {
        ShowInfoFmt("pawn: walk {}: the player looks away, order ends", POwner->getName());
        pawn::clearWalkOrder(POwner->id);
        POwner->PAI->PathFind->Clear();
        return;
    }
    auto* PPathFind = POwner->PAI->PathFind.get();
    // A laid route is walked crumb by crumb, each let go as she comes near
    auto crumb = pawn::routeFront(POwner->id);
    while (crumb.has_value() && distance(POwner->loc.p, *crumb) < 0.6f)
    {
        pawn::popRoute(POwner->id);
        crumb = pawn::routeFront(POwner->id);
    }
    const auto  goal  = crumb.has_value() ? crumb : endPoint;
    const auto  point = &*goal;
    const float away  = distance(POwner->loc.p, *point);
    if (away < 0.2f)
    {
        PPathFind->Clear();
        m_WalkPoint    = *point;
        m_LastWalkStep = now; // standing on the ring: the next step is a period's worth, not the time she stood
        return;
    }
    // Kneeling, she stays down on an end point the route already counts as
    // reached (RouteWalked): a ring that has not left her, its point snapped
    // to the mesh a little off her feet, is no reason to stand, and a rest
    // ordered there finds her still kneeling
    const bool kneeling = POwner->StatusEffectContainer->HasStatusEffect(xi::StatusEffect::Healing);
    if (kneeling && !crumb.has_value() && away < kRouteReached)
    {
        PPathFind->Clear();
        m_WalkPoint    = *point;
        m_LastWalkStep = now;
        return;
    }
    // Kneeling, she rises before she walks: the path moves her, and only
    // Move's stand would otherwise lift her off her knees
    if (kneeling)
    {
        EndRestOrder("a walk order");
        StandFromRest("a walk order");
    }
    if (!RestAllowsAction())
    {
        return;
    }
    if (PPathFind->IsFollowingPath() && m_WalkPoint.has_value() && distance(*m_WalkPoint, *point) < 0.15f)
    {
        return;
    }
    m_WalkPoint = *point;
    if (!PathToward(*point, 0.0f))
    {
        if (away < 1.5f)
        {
            PPathFind->Clear();
            return;
        }
        ShowInfoFmt("pawn: walk {}: cannot path to ({:.1f}, {:.1f}, {:.1f}) {:.1f}y away, order dropped", POwner->getName(), point->x, point->y, point->z, away);
        pawn::clearWalkOrder(POwner->id);
    }
}

void CPawnController::WalkStep()
{
    // A held game (pause/pause.h) holds her too: the AI tick stands down
    // under a hold, and so must this timer, which runs on real time
    if ((m_Mode != Mode::Walk && m_Mode != Mode::Maneuver) || !POwner->PAI->CanFollowPath() || cardian::pause::isHeld())
    {
        return;
    }
    const auto now = timer::now();
    WalkOrderTick(now);
    auto* PPathFind = POwner->PAI->PathFind.get();
    if (!PPathFind->IsFollowingPath())
    {
        // A re-path tick (the path is not "following" until the next one)
        // keeps the clock running, so the next step covers the time
        return;
    }
    // The fraction of a logic tick's step the time since the last step is
    // worth, in microseconds (whole milliseconds would truncate a 16 ms
    // interval): a tick that fires late takes a longer step, so her speed
    // is exact on average whatever the timer's jitter. Capped at a quarter
    // step; the first step after a stand is a period's worth.
    const auto since = m_LastWalkStep == timer::time_point{} ? std::chrono::microseconds(cardian::view::kSteerPeriodMs * 1000) : std::chrono::duration_cast<std::chrono::microseconds>(now - m_LastWalkStep);
    m_LastWalkStep   = now;
    const float scale = std::min(0.25f, static_cast<float>(since.count()) / 400000.0f);
    const auto  before = POwner->loc.p;
    PPathFind->SetStepScale(scale);
    PPathFind->FollowPath(now);
    PPathFind->SetStepScale(1.0f);
    // The walk's accounting, every five seconds in the map log while she is
    // walked: her ground speed (the server's truth, against the client's
    // meter), the timer's steps, and the time the cap threw away
    m_WalkStats.steps++;
    m_WalkStats.elapsed += since;
    m_WalkStats.lost += std::max(std::chrono::microseconds(0), since - std::chrono::microseconds(100000));
    m_WalkStats.moved += distance(before, POwner->loc.p);
    if (m_WalkStats.since == timer::time_point{})
    {
        m_WalkStats.since = now;
    }
    else if (now - m_WalkStats.since >= 5s)
    {
        const float secs = std::chrono::duration<float>(now - m_WalkStats.since).count();
        ShowInfoFmt("pawn: walk {}: {:.2f} y/s over {:.1f}s, {} steps ({:.1f}/s), elapsed {}ms, {}ms lost to the cap, speed {}",
                    POwner->getName(), m_WalkStats.moved / secs, secs, m_WalkStats.steps, m_WalkStats.steps / secs,
                    m_WalkStats.elapsed.count() / 1000, m_WalkStats.lost.count() / 1000, POwner->GetSpeed());
        m_WalkStats = {};
        m_WalkStats.since = now;
    }
    // Her position goes out now, not at the logic tick's PostTick (400 ms):
    // the viewer's client hears every step. The packet LEADS her by what the
    // client takes to ease an entity onto a packet position (STEER_LEAD_MS
    // of her run), along her facing, never past the ring: the ring is where
    // the player means her to be, and the lead only shows her on the way
    // there sooner. Her real position -- aggro, range, everything the
    // server judges -- is untouched; only the packet is built from the led
    // point, and the position is put back at once.
    if (POwner->loc.zone != nullptr)
    {
        const auto  point   = pawn::walkOrderOf(POwner->id);
        const float toRing  = point.has_value() ? distance(POwner->loc.p, *point) : 0.0f;
        // Her run in yalms a second: upstream's step, speed / 17 (pathfind.cpp)
        const float leadMax = settings::get<float>("pawn.STEER_LEAD_MS") / 1000.0f * static_cast<float>(POwner->GetSpeed()) / 17.0f;
        const float lead    = std::min(leadMax, toRing);
        const auto  real    = POwner->loc.p;
        if (lead > 0.05f)
        {
            const float radians = 2.0f * std::numbers::pi_v<float> - rotationToRadian(real.rotation); // the walker's own convention (pathfind_step.cpp)
            POwner->loc.p.x     = real.x + std::cos(radians) * lead;
            POwner->loc.p.z     = real.z + std::sin(radians) * lead;
        }
        POwner->loc.zone->UpdateEntityPacket(POwner, ENTITY_UPDATE, UPDATE_POS);
        POwner->loc.p = real;
    }
}

void CPawnController::TravelTick()
{
    const bool narrate = m_Tick - m_LastTravelDebugTime > 5s;
    if (narrate)
    {
        m_LastTravelDebugTime = m_Tick;
    }

    const auto order = pawn::travelOrderOf(POwner->id);

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
        // The player, in her party and another zone: DoRoamTick sends her
        // only then, or while they load between zones
        const CCharEntity* PPlayer = pawn::partyPlayer(static_cast<const CCharEntity*>(POwner));
        if (PPlayer == nullptr || PPlayer->loc.zone == nullptr)
        {
            if (narrate)
            {
                ShowInfoFmt("pawn: travel {}: the player is not in the world (loading?), idling", POwner->getName());
            }
            return;
        }
        targetZone = PPlayer->getZone();

        // Recruited from afar on his shout: the teleport the travel code
        // falls back on carries her to his side, not a walk across the
        // world (RESEARCH §18.6). While he is in his Mog House, or leaving
        // his zone, she waits where she is, and goes once he is out
        if (pawn::carriedToPlayer(static_cast<const CCharEntity*>(POwner)))
        {
            if (PPlayer->inMogHouse() || PPlayer->requestedZoneChange)
            {
                if (narrate)
                {
                    ShowInfoFmt("pawn: travel {}: from afar, waits while {} is {}", POwner->getName(), PPlayer->getName(),
                                PPlayer->inMogHouse() ? "in his Mog House" : "zoning");
                }
                return;
            }
            if (narrate)
            {
                ShowInfoFmt("pawn: travel {}: from afar, {} -> {} by the teleport to {}", POwner->getName(), static_cast<uint16>(POwner->getZone()),
                            static_cast<uint16>(targetZone), PPlayer->getName());
            }
            pawn::requestTransfer(POwner->id, std::nullopt);
            return;
        }
    }

    // The line chosen as she sets out, and kept while she walks it: of the
    // lines that lead the way, the one nearest where her player walked
    // through, when she follows him out of the zone he left; else the one
    // nearest her
    constexpr auto kHopForgotten = 10s;
    if (!(m_TravelHop.has_value() && m_TravelHopFrom == POwner->getZone() && m_TravelHopTarget == targetZone && m_Tick - m_TravelHopAt < kHopForgotten))
    {
        const auto*      PLeader = pawn::partyPlayer(static_cast<const CCharEntity*>(POwner));
        const auto       left    = PLeader != nullptr ? pawn::playerExit(PLeader->id) : std::nullopt;
        const position_t closeTo = left.has_value() && left->from == POwner->getZone() && left->to == targetZone ? left->at : POwner->loc.p;
        m_TravelHop       = pawn::travel::nextHop(POwner->getZone(), targetZone, closeTo);
        m_TravelHopFrom   = POwner->getZone();
        m_TravelHopTarget = targetZone;
    }
    m_TravelHopAt  = m_Tick;
    const auto hop = m_TravelHop;
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

auto CPawnController::Cast(const EntityId target, const SpellID spellid) -> bool
{
    CSpell* PSpell = spell::GetSpell(spellid);
    if (PSpell == nullptr)
    {
        return false;
    }

    const EntityId castTarget = PSpell->getValidTarget() == TARGET_SELF ? EntityId(POwner) : target;

    if (PartyAlreadyCasting(PSpell, castTarget.resolve<CBattleEntity>()))
    {
        return false;
    }

    auto* PTarget = castTarget.resolve<CBattleEntity>();
    if (PTarget == nullptr || PTarget->loc.zone != POwner->loc.zone || HoldsFireOn(PTarget) ||
        distance(POwner->loc.p, PTarget->loc.p) > pawn::tactics::bank::castRange(POwner, PSpell, PTarget))
    {
        return false;
    }
    FaceTarget(castTarget);
    HeadLook(PTarget);
    return CastAndStop(castTarget, spellid);
}

auto CPawnController::CastAndStop(const EntityId target, const SpellID spellid) -> bool
{
    // A cast of hers that costs MP is held where an aggressive mob's magic
    // detection would hear it (#77): she steps clear first and casts from
    // there. Her rows and her tactician come through here, first aid too;
    // the player's own order does not, his call to make
    if (auto* PSpell = spell::GetSpell(spellid); PSpell != nullptr && PSpell->getMPCost() > 0)
    {
        const auto* PFight = AttendedTarget() != nullptr ? AttendedTarget() : POwner->GetBattleTarget();
        if (auto* PHears = MagicHearer(PFight); PHears != nullptr)
        {
            HoldForMagic(PHears, PSpell->getName());
            return false;
        }
    }
    // A cast she wants stops whatever walk she is on: the path is dropped
    // as the cast begins, or the pathfinder's next step would interrupt it
    if (!PrepareRestAction())
    {
        return false;
    }
    const bool cast = CPlayerController::Cast(target, spellid);
    if (cast && POwner->PAI->PathFind != nullptr)
    {
        POwner->PAI->PathFind->Clear();
    }
    return cast;
}

auto CPawnController::CastAssigned(const EntityId target, const SpellID spellid) -> bool
{
    CSpell* PSpell = spell::GetSpell(spellid);
    if (PSpell == nullptr)
    {
        return false;
    }
    const EntityId castTarget = PSpell->getValidTarget() == TARGET_SELF ? EntityId(POwner) : target;
    auto* PTarget = castTarget.resolve<CBattleEntity>();
    if (PTarget == nullptr || PTarget->loc.zone != POwner->loc.zone || HoldsFireOn(PTarget) ||
        distance(POwner->loc.p, PTarget->loc.p) > pawn::tactics::bank::castRange(POwner, PSpell, PTarget))
    {
        return false;
    }
    FaceTarget(castTarget);
    HeadLook(PTarget);
    return CastAndStop(castTarget, spellid);
}

// Boost is spent by the next blow, so it is worth nothing unless the
// weapon skill follows at once: her marked Boost row offers it (the
// tactician's tool, RESEARCH §17.13), ready, known, not already up
auto CPawnController::BoostReady() const -> bool
{
    if (!m_Gambits->OffersBeforeWs(cardian::tactician::Allowance::Boost))
    {
        return false;
    }
    const auto* PBoost = ability::GetAbility(cardian::tactician::kBoost);
    auto*       PChar  = static_cast<CCharEntity*>(POwner);
    return PBoost != nullptr && !AbilitiesShutOut() && charutils::hasAbility(PChar, cardian::tactician::kBoost) &&
           !PChar->StatusEffectContainer->HasStatusEffect(xi::StatusEffect::Boost) && !PChar->PRecastContainer->HasRecast(RECAST_ABILITY, PBoost->getRecastId(), 0s);
}

// Sneak Attack is hers to use now: known, not shut out, not up already,
// off its recast -- whatever her rows say
auto CPawnController::SneakAttackUsable() const -> bool
{
    const auto* PSneak = ability::GetAbility(cardian::tactician::kSneakAttack);
    auto*       PChar  = static_cast<CCharEntity*>(POwner);
    return PSneak != nullptr && charutils::hasAbility(PChar, cardian::tactician::kSneakAttack) && !AbilitiesShutOut() &&
           !PChar->StatusEffectContainer->HasStatusEffect(xi::StatusEffect::SneakAttack) && !PChar->PRecastContainer->HasRecast(RECAST_ABILITY, PSneak->getRecastId(), 0s);
}

auto CPawnController::SneakAttackReady() const -> bool
{
    return m_Gambits->OffersBeforeWs(cardian::tactician::Allowance::SneakAttack) && SneakAttackUsable();
}

auto CPawnController::SneakAttackNow(const CBattleEntity* PTarget) -> bool
{
    if (PTarget == nullptr || !POwner->PAI->IsEngaged())
    {
        return false;
    }
    const auto* PFront = PTarget->GetBattleTarget();
    if (PFront == nullptr || PFront == POwner)
    {
        return false;
    }
    if (m_SneakRest.has_value() && m_Tick < m_SneakRest->until && m_SneakRest->front == PFront->id)
    {
        return false;
    }
    return SneakAttackReady();
}

void CPawnController::RestSneak(const CBattleEntity* PTarget)
{
    constexpr auto kSneakRest = 10s;
    const auto*    PFront     = PTarget != nullptr ? PTarget->GetBattleTarget() : nullptr;
    m_SneakRest               = SneakRest{ .until = m_Tick + kSneakRest, .front = PFront != nullptr ? PFront->id : 0 };
}

auto CPawnController::TakesSneakAttack(const uint16 wsid) -> bool
{
    // Read once per weapon skill, off its own script: the answer cannot
    // change while the server runs
    static std::unordered_map<uint16, bool> known;
    if (const auto it = known.find(wsid); it != known.end())
    {
        return it->second;
    }
    bool takes = false;
    if (auto* PSkill = battleutils::GetWeaponSkill(wsid); PSkill != nullptr)
    {
        std::ifstream     file(fmt::format("./scripts/actions/weaponskills/{}.lua", PSkill->getName()));
        const std::string source((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
        takes = cardian::tactician::scriptTakesSneakAttack(source);
    }
    known.emplace(wsid, takes);
    return takes;
}

auto CPawnController::NakedSneakDue(const CBattleEntity* PTarget) -> bool
{
    // Cheapest first: Sneak Attack hers now and no weapon skill due (one
    // would spend it, whatever it is); then her rows, read each think (no
    // weapon skill row that can take it: CGambits::NakedSneak); then the
    // fight (SneakAttackNow), and the mob's back somewhere she can stand
    if (PTarget == nullptr || POwner->health.tp >= 1000 || !SneakAttackUsable() || !m_Gambits->NakedSneak() || !SneakAttackNow(PTarget))
    {
        return false;
    }
    // Holding her position she walks to no back: only one she stands at
    const bool there = distance(POwner->loc.p, PTarget->loc.p) <= POwner->GetMeleeRange(PTarget) && BehindFor(PTarget);
    return there || (!m_Waiting && POwner->PAI->PathFind->ValidPosition(SneakPoint(PTarget)));
}

void CPawnController::NakedSneak(const EntityId target)
{
    auto* PTarget = target.resolve<CBattleEntity>();
    if (!CPlayerController::Ability(POwner->entityId(), cardian::tactician::kSneakAttack))
    {
        RestSneak(PTarget);
        return;
    }
    m_SneakHold = SneakHold{ .target = target, .until = m_Tick + 5s };
    m_SneakReadySince.reset();
    ShowInfoFmt("tactics: {} sneak attacks from {}'s back for her next swing (naked: no weapon skill row of hers can take it)", POwner->getName(),
                PTarget != nullptr ? PTarget->getName() : "?");
    pawn::tactics::noteSneakAttack(static_cast<CCharEntity*>(POwner), PTarget != nullptr ? PTarget->id : 0, cardian::tactics::SneakUse::Naked, 0.0);
}

auto CPawnController::BehindFor(const CBattleEntity* PTarget) const -> bool
{
    return PTarget != nullptr && ::behind(POwner->loc.p, PTarget->loc.p, 64);
}

auto CPawnController::SneakPoint(const CBattleEntity* PTarget) const -> position_t
{
    return nearPosition(PTarget->loc.p, FightRadius(PTarget), std::numbers::pi_v<float>);
}

auto CPawnController::WeaponSkill(const EntityId target, const uint16 wsid) -> bool
{
    if (!PrepareRestAction())
    {
        return false;
    }
    auto* PTarget = target.resolve<CBattleEntity>();

    // Sneak Attack first, from the mob's back (her Sneak Attack row), when
    // this weapon skill takes it and it can go now (SneakAttackNow: not
    // while the mob is on her or has nobody to face): at once when she
    // stands behind the mob in reach, else after the walk there (the
    // combat tick's sneak step), which her weapon skill waits on -- unless
    // its back is off the mesh (against a wall), or she holds her position
    // and walks nowhere, when the weapon skill goes alone. Her rows' weapon
    // skill only, on the mob she fights: his orders go through
    // OrderedWeaponSkill
    if (m_SneakStep.has_value() && m_SneakStep->wsid != 0 && PTarget != nullptr && m_SneakStep->target == PTarget)
    {
        return true; // on her way already, not turned to the mob (KeepsSneakForBack)
    }
    // A Sneak Attack already on her -- primed for a mob that died before her
    // swing -- and the back not hers yet: the weapon skill waits for the
    // back and goes out the tick she reaches it, before her first swing can
    // spend it (the user, 2026-10-04: position, engage, slam the macro)
    if (PTarget != nullptr && PTarget == POwner->GetBattleTarget() && TakesSneakAttack(wsid) &&
        POwner->StatusEffectContainer->HasStatusEffect(xi::StatusEffect::SneakAttack) && PTarget->GetBattleTarget() != POwner && !BehindFor(PTarget) &&
        PTarget->id != m_SneakGaveUpOn && !m_Waiting && POwner->PAI->PathFind->ValidPosition(SneakPoint(PTarget)))
    {
        m_SneakStep = SneakStep{ .target = target, .wsid = wsid, .since = m_Tick };
        ShowInfoFmt("tactics: {} steps round to {}'s back for the Sneak Attack still on her; weapon skill {} waits", POwner->getName(), PTarget->getName(), wsid);
        return true;
    }
    FaceTarget(target);
    HeadLook(PTarget);
    if (PTarget != nullptr && PTarget == POwner->GetBattleTarget() && TakesSneakAttack(wsid) && SneakAttackNow(PTarget))
    {
        if (distance(POwner->loc.p, PTarget->loc.p) <= POwner->GetMeleeRange(PTarget) && BehindFor(PTarget))
        {
            return SneakThenWs(target, wsid);
        }
        if (!m_Waiting && POwner->PAI->PathFind->ValidPosition(SneakPoint(PTarget)))
        {
            m_SneakStep = SneakStep{ .target = target, .wsid = wsid, .since = m_Tick };
            ShowInfoFmt("tactics: {} steps round to {}'s back for Sneak Attack (her Sneak Attack row); weapon skill {} waits", POwner->getName(), PTarget->getName(), wsid);
            return true;
        }
    }
    return BoostOrWeaponSkill(target, wsid);
}

auto CPawnController::OrderedWeaponSkill(const EntityId target, const uint16 wsid) -> bool
{
    if (!PrepareRestAction())
    {
        return false;
    }
    FaceTarget(target);
    HeadLook(target.resolve<CBattleEntity>());
    m_SneakStep.reset();
    auto* PTarget = target.resolve<CBattleEntity>();
    if (PTarget != nullptr && TakesSneakAttack(wsid) && SneakAttackNow(PTarget) && distance(POwner->loc.p, PTarget->loc.p) <= POwner->GetMeleeRange(PTarget) &&
        BehindFor(PTarget))
    {
        return SneakThenWs(target, wsid);
    }
    return BoostOrWeaponSkill(target, wsid);
}

auto CPawnController::SneakThenWs(const EntityId target, const uint16 wsid) -> bool
{
    auto* PTarget = target.resolve<CBattleEntity>();
    // Sneak Attack already on her: no second one, and no Boost's beat -- she
    // turns to the mob and the weapon skill goes this tick, before a swing
    if (POwner->StatusEffectContainer->HasStatusEffect(xi::StatusEffect::SneakAttack))
    {
        FaceTarget(target);
        return FireWeaponSkill(target, wsid);
    }
    if (!CPlayerController::Ability(POwner->entityId(), cardian::tactician::kSneakAttack))
    {
        RestSneak(PTarget);
        return BoostOrWeaponSkill(target, wsid);
    }
    m_HeldWs = HeldWs{ .target = target, .wsid = wsid, .at = m_Tick };

    // How long it stood ready for this weapon skill: the cost of holding it
    // rather than spending it on a plain hit (RESEARCH §17.13, the Thief)
    const double waited = m_SneakReadySince.has_value() ? std::chrono::duration<double>(m_Tick - *m_SneakReadySince).count() : 0.0;
    m_SneakReadySince.reset();
    ShowInfoFmt("tactics: {} sneak attacks from {}'s back (her Sneak Attack row), ready {:.0f} s before her weapon skill; weapon skill {} follows as it lands",
                POwner->getName(), PTarget != nullptr ? PTarget->getName() : "?", waited, wsid);
    pawn::tactics::noteSneakAttack(static_cast<CCharEntity*>(POwner), PTarget != nullptr ? PTarget->id : 0, cardian::tactics::SneakUse::BeforeWs, waited);
    return true;
}

auto CPawnController::BoostOrWeaponSkill(const EntityId target, const uint16 wsid) -> bool
{
    // The game takes her TP as a weapon skill starts and judges its reach
    // only as it lands: one beyond its reach (weaponSkillReach) is never
    // tried -- her rows', his order's, or one held behind its opener -- so it
    // never spends her TP on nothing
    auto* PTarget = target.resolve<CBattleEntity>();
    if (PTarget != nullptr && PTarget != POwner && distance(POwner->loc.p, PTarget->loc.p) > weaponSkillReach(POwner, wsid, PTarget))
    {
        return false;
    }
    // Boost first, the weapon skill the moment Boost has landed and lets go
    // of her, two ticks on (DoCombatTick) -- only in reach of the mob, or
    // the walk in spends it on a punch
    // (in reach and engaged is enough: a member repositions all fight long,
    // and a standing-still gate never opened for her)
    const bool inReach = PTarget != nullptr && POwner->PAI->IsEngaged() && distance(POwner->loc.p, PTarget->loc.p) <= POwner->GetMeleeRange(PTarget);
    if (inReach && BoostReady() && CPlayerController::Ability(POwner->entityId(), cardian::tactician::kBoost))
    {
        m_HeldWs = HeldWs{ .target = target, .wsid = wsid, .at = m_Tick };
        ShowInfoFmt("tactics: {} boosts (her Boost row); weapon skill {} follows as it lands", POwner->getName(), wsid);
        return true;
    }
    return FireWeaponSkill(target, wsid);
}

auto CPawnController::FireWeaponSkill(const EntityId target, const uint16 wsid) -> bool
{
    auto* PTarget = target.resolve<CBattleEntity>();
    if (PTarget != nullptr && PTarget != POwner && distance(POwner->loc.p, PTarget->loc.p) > weaponSkillReach(POwner, wsid, PTarget))
    {
        return false;
    }
    const int16 tp = POwner->health.tp;
    if (!CPlayerController::WeaponSkill(target, wsid))
    {
        return false;
    }
    // Said as it goes out (the user, 2026-10-04: a weapon skill is seen as
    // it lands, never as it starts): her TP, and Sneak Attack with whether
    // she stands behind the mob, the one place it counts (weaponskills.lua)
    const bool sneak = POwner->StatusEffectContainer->HasStatusEffect(xi::StatusEffect::SneakAttack);
    auto*       PWs  = battleutils::GetWeaponSkill(wsid);
    ShowInfoFmt("tactics: {} uses {} on {} at {} TP{}", POwner->getName(), PWs != nullptr ? PWs->getName() : fmt::format("weapon skill {}", wsid),
                PTarget != nullptr ? PTarget->getName() : "?", tp,
                !sneak ? "" : PTarget != nullptr && behind(POwner->loc.p, PTarget->loc.p, 64) ? ", Sneak Attack on, from its back" : ", Sneak Attack on but NOT from its back (wasted)");
    return true;
}

auto CPawnController::Ability(const EntityId target, const uint16 abilityid) -> bool
{
    if (!PrepareRestAction())
    {
        return false;
    }
    FaceTarget(target);
    HeadLook(target.resolve<CBattleEntity>());
    return CPlayerController::Ability(target, abilityid);
}

auto CPawnController::RangedAttack(const EntityId target) -> bool
{
    if (!PrepareRestAction())
    {
        return false;
    }
    timer::duration rangedDelay = 10s;
    if (const auto* PRange = dynamic_cast<CItemWeapon*>(POwner->m_Weapons[SLOT_RANGED]))
    {
        rangedDelay = std::chrono::milliseconds(PRange->getDelay());
    }

    if (m_Tick - m_LastRangedAttackTime < rangedDelay)
    {
        return false;
    }

    FaceTarget(target);
    if (!CPlayerController::RangedAttack(target))
    {
        return false;
    }

    // A shot that begins drops the walk she was on, as a cast does
    if (POwner->PAI->PathFind != nullptr)
    {
        POwner->PAI->PathFind->Clear();
    }
    m_LastRangedAttackTime = m_Tick;
    return true;
}

void CPawnController::FaceTarget(const EntityId target) const
{
    if (const auto* PTarget = target.resolve(); PTarget != nullptr && PTarget != POwner)
    {
        POwner->PAI->PathFind->LookAt(PTarget->loc.p);
    }
}

auto CPawnController::PartyAlreadyCasting(CSpell* PSpell, const CBattleEntity* PTarget) const -> bool
{
    return pawn::partyAlreadyCasting(static_cast<CCharEntity*>(POwner), PSpell, PTarget);
}

// The engage door and the tactician's say over her fights are her
// engine's (CGambits), the same for every character whose rows run
auto CPawnController::FoeFacts(CBattleEntity* PFoe, const CCharEntity* PLeader) const -> cardian::engage::Foe
{
    return m_Gambits->FoeFacts(PFoe, PLeader);
}

auto CPawnController::FoeOfKind(const cardian::engage::Finder finder, CBattleEntity* PFoe) const -> bool
{
    return m_Gambits->FoeOfKind(finder, PFoe);
}

auto CPawnController::FoeWhy(const cardian::engage::Finder finder, CBattleEntity* PFoe, const CCharEntity* PLeader) const -> std::string
{
    return m_Gambits->FoeWhy(finder, PFoe, PLeader);
}

auto CPawnController::FoesAround(CCharEntity* PLeader, const position_t& from) const -> std::vector<CBattleEntity*>
{
    return m_Gambits->FoesAround(PLeader, from);
}

auto CPawnController::PartyFightScan(CCharEntity* PLeader, const position_t& from) const -> FightPick
{
    return m_Gambits->PartyFightScan(PLeader, from);
}

auto CPawnController::EngageChoice(CCharEntity* PLeader, const position_t& from) const -> FightPick
{
    return m_Gambits->EngageChoice(PLeader, from);
}

auto CPawnController::ClaimingRow(CBattleEntity* PTarget) const -> std::optional<RowClaim>
{
    return m_Gambits->ClaimingRow(PTarget);
}

auto CPawnController::ClaimingRowAs(CBattleEntity* PTarget, const bool melee) const -> std::optional<RowClaim>
{
    return m_Gambits->ClaimingRowAs(PTarget, melee);
}

auto CPawnController::TakesFights() const -> bool
{
    return m_Gambits->TakesFights();
}

auto CPawnController::TacticianRuns() const -> bool
{
    return m_Gambits->TacticianRuns();
}

auto CPawnController::RecoveryDue() const -> bool
{
    return m_Gambits->RecoveryDue();
}

auto CPawnController::TacticianMelee() const -> bool
{
    return m_Gambits->TacticianMelee();
}

auto CPawnController::HuntBlocker(const CCharEntity* PPlayer) const -> std::string
{
    // Pull only from the player's side: the player drives, the hunter scouts
    if (const float away = distance(POwner->loc.p, PPlayer->loc.p); away > 10.0f)
    {
        return fmt::format("{:.0f} y from {}", away, PPlayer->getName());
    }
    return PacingBlocker(PPlayer);
}

namespace
{
    // A member attending the fight from the perimeter is in it, weapon or
    // no weapon: the blockers that wait for a fight to end wait for her too
    auto isAttending(const CBattleEntity* PMember) -> bool
    {
        const auto* PController = PMember != nullptr && PMember->PAI != nullptr ? dynamic_cast<const CPawnController*>(PMember->PAI->GetController()) : nullptr;
        return PController != nullptr && PController->CurrentMode() == CPawnController::Mode::Attend;
    }
} // namespace

auto CPawnController::PacingBlocker(const CCharEntity* PPlayer) const -> std::string
{
    if (PPlayer->animation == xi::Animation::Healing)
    {
        return fmt::format("{} resting", PPlayer->getName());
    }

    // No HP or MP gate: the player paces the party and rests it when it
    // needs resting. A member down or still fighting is not pacing.
    const auto* PPawn = static_cast<const CCharEntity*>(POwner);
    if (PPawn->PParty == nullptr)
    {
        return "";
    }
    for (auto* PMember : PPawn->PParty->members)
    {
        if (PMember->loc.zone != POwner->loc.zone)
        {
            continue;
        }
        if (PMember->isDead())
        {
            return fmt::format("{} down", PMember->getName());
        }
        if (PMember->PAI->IsEngaged() || isAttending(PMember))
        {
            return fmt::format("{} engaged", PMember->getName());
        }
        // A cardian already walking in on her own pull: one pull at a time
        if (const auto* PController = dynamic_cast<const CPawnController*>(PMember->PAI->GetController());
            PController != nullptr && PController != this && PController->m_Approach.has_value() && PController->m_Approach->kind == ApproachKind::Hunt)
        {
            return fmt::format("{} pulling", PMember->getName());
        }
    }
    return "";
}

// The camp leader's pacing (D5): nobody sets off while a member of her
// party is down, kneeling, fighting or already walking in on a pull
auto CPawnController::CampBlocker() const -> std::string
{
    const auto* PPawn = static_cast<const CCharEntity*>(POwner);
    if (PPawn->PParty == nullptr)
    {
        return "";
    }
    for (auto* PMember : PPawn->PParty->members)
    {
        if (PMember == POwner || PMember->loc.zone != POwner->loc.zone)
        {
            continue;
        }
        if (PMember->isDead())
        {
            return fmt::format("{} down", PMember->getName());
        }
        if (PMember->StatusEffectContainer->HasStatusEffect(xi::StatusEffect::Healing))
        {
            return fmt::format("{} resting", PMember->getName());
        }
        if (PMember->PAI->IsEngaged() || isAttending(PMember))
        {
            return fmt::format("{} engaged", PMember->getName());
        }
        if (const auto* PController = dynamic_cast<const CPawnController*>(PMember->PAI->GetController());
            PController != nullptr && PController->m_Approach.has_value())
        {
            return fmt::format("{} walking in", PMember->getName());
        }
    }
    return "";
}

auto CPawnController::PickHuntTarget(const position_t& around, const uint8 level, const pawn::HuntRules& rules, std::string* skipped) const -> CMobEntity*
{
    // What was in the band but not pulled, and why: a few, for the log
    int        skips = 0;
    const auto skip  = [&](const CMobEntity* PWhom, const std::string& why)
    {
        if (skipped == nullptr || skips >= 3)
        {
            return;
        }
        if (!skipped->empty())
        {
            *skipped += ", ";
        }
        *skipped += fmt::format("{} ({})", PWhom->getName(), why);
        ++skips;
    };

    const auto  radius      = settings::get<float>("pawn.HUNT_RADIUS");
    const auto  cleanRadius = settings::get<float>("pawn.HUNT_CLEAN_RADIUS");
    auto*       entities    = pawn::entitiesAround(POwner);

    // One danger scan per hunt check, wide enough to cover every candidate's
    // circle and every approach from the hunter. Judged for the whole party
    // that will fight beside the target, not for the hunter's own buffs
    const auto dangers = pawn::danger::around(entities, around, radius + std::max(cleanRadius, distance(POwner->loc.p, around)),
                                              pawn::danger::Profile::party(static_cast<CCharEntity*>(POwner)));

    // An idle, unclaimed, ordinary field mob in the band, within the hunt
    // radius of the anchor
    const auto eligible = [&](CMobEntity* PMob) -> bool
    {
        return huntable(PMob, level, rules) && distance(around, PMob->loc.p) <= radius;
    };

    // The pull rule's circles (pawn_rules.h) for one candidate: the
    // dangers that matter to the hunter's way in (forWalk: they see the
    // player at the scan's centre, the candidate, the hunter, or the way
    // between -- a mob behind a wall is no company), padded as the fight
    // pads them. The pick's set holds everything the fight's does, so the
    // fight never drops what the pick approved. The candidate's own
    // circle, when it is a danger itself, is no reason against pulling it
    std::vector<pawn::danger::Danger> seen;
    cardian::rules::Circles           paddedCircles;
    std::size_t                       ownCircle = cardian::rules::npos;
    const auto                        focusOn   = [&](const CMobEntity* PMob)
    {
        seen          = pawn::danger::forWalk(dangers, POwner->loc.p, PMob->loc.p, [this](const auto& d, const auto& p) { return Sees(d, p); });
        paddedCircles = cardian::rules::padded(seen);
        ownCircle     = cardian::rules::npos;
        for (std::size_t i = 0; i < seen.size(); ++i)
        {
            if (seen[i].mob == PMob)
            {
                ownCircle = i;
            }
        }
    };

    // A linking family member (aggressive or not) within the clean radius
    const auto linked = [&](const CMobEntity* PMob) -> bool
    {
        bool       found = false;
        const auto kin   = [&](CMobEntity* POther)
        {
            if (!found && POther != PMob && POther->m_Link != 0 && POther->m_Family == PMob->m_Family &&
                POther->isAlive() && POther->PMaster == nullptr &&
                isWithinDistance(POther->loc.p, PMob->loc.p, cleanRadius))
            {
                found = true;
            }
        };
        pawn::forEachMobNear(entities, PMob->loc.p, cleanRadius, kin);
        return found;
    };

    // The pull order: easiest first (the lowest thing that still pays --
    // the signet sweet spot), nearest, or toughest; distance breaks ties
    const auto keyOf = [&](const CMobEntity* PMob) -> int
    {
        switch (rules.pullFirst)
        {
            case 1:
                return PMob->GetMLevel();
            case 2:
                return -PMob->GetMLevel();
            default:
                return 0;
        }
    };

    CMobEntity* best     = nullptr;
    float       bestDist = radius; // the hunter's own walk is capped too
    int         bestKey  = 0;

    const auto consider = [&](CMobEntity* PMob)
    {
        if (!eligible(PMob))
        {
            return;
        }

        // The pull rule (pawn_rules.h): prey standing in an aggressive
        // mob's circle is not the pull -- the guard is, when the party
        // allows aggressive company and the guard is itself fair game;
        // otherwise the prey is skipped. With aggressive company avoided,
        // a circle across the hunter's way in skips it too. The player
        // can still sneak behind a guard and pull it to the party waiting
        // outside its circle
        CMobEntity* pick = PMob;
        focusOn(PMob);
        if (const auto block = cardian::rules::pullBlocked(paddedCircles, POwner->loc.p.x, POwner->loc.p.z, PMob->loc.p.x, PMob->loc.p.z, ownCircle); block.has_value())
        {
            CMobEntity* guard = seen[block->circle].mob;
            if (!rules.aggressive)
            {
                skip(PMob, block->targetInside ? fmt::format("inside {}'s circle", guard->getName())
                                               : fmt::format("the way in crosses {}'s circle", guard->getName()));
                return;
            }
            if (block->targetInside)
            {
                if (!eligible(guard))
                {
                    skip(PMob, fmt::format("its guard {} is no pull", guard->getName()));
                    return;
                }
                pick = guard;
            }
        }
        if (!rules.links && pick->m_Link != 0 && linked(pick))
        {
            skip(pick, "kin nearby");
            return;
        }

        const float toHunter = distance(POwner->loc.p, pick->loc.p);
        const int   key      = keyOf(pick);
        if (toHunter >= radius || (best != nullptr && (key > bestKey || (key == bestKey && toHunter >= bestDist))))
        {
            return;
        }

        best     = pick;
        bestDist = toHunter;
        bestKey  = key;
    };
    pawn::forEachMobNear(entities, around, radius, consider);

    return best;
}

// An idle, unclaimed, ordinary field mob whose check against the level
// falls in the rules' band: the hunt's eligibility, shared by the pick and
// the farmer's errand
auto CPawnController::huntable(CMobEntity* PMob, const uint8 level, const pawn::HuntRules& rules) -> bool
{
    const bool special = (PMob->m_Type & xi::MobType::Event) != xi::MobType::Normal ||
                         (PMob->m_Type & xi::MobType::Fished) != xi::MobType::Normal ||
                         (PMob->m_Type & xi::MobType::Battlefield) != xi::MobType::Normal ||
                         (PMob->m_Type & xi::MobType::Notorious) != xi::MobType::Normal;
    if (special || PMob->PMaster != nullptr || !PMob->isAlive() || pawn::isUnderground(PMob) ||
        PMob->PAI->IsEngaged() || PMob->allegiance != xi::Allegiance::Mob || !PMob->PEnmityContainer->GetEnmityList()->empty())
    {
        return false;
    }
    const auto check = static_cast<uint8>(charutils::CheckMob(level, PMob));
    return check >= rules.minCheck && check <= rules.maxCheck;
}

auto CPawnController::PickHuntTarget(const CCharEntity* PPlayer, std::string* skipped) const -> CMobEntity*
{
    return PickHuntTarget(PPlayer->loc.p, PPlayer->GetMLevel(), pawn::huntRulesOf(pawn::ordersOwnerOf(static_cast<const CCharEntity*>(POwner))), skipped);
}

auto CPawnController::GetTopEnmity() const -> CBattleEntity*
{
    return pawn::topEnmityOf(POwner);
}

namespace
{
    constexpr auto kHealthSaveEvery = 30s;
} // namespace

void CPawnController::NoteForSaving()
{
    auto*      PPawn = static_cast<CCharEntity*>(POwner);
    const auto now   = timer::now();
    if (!m_SaveSeeded)
    {
        // What she loaded with is what the row holds: nothing to write yet
        m_SavedHp       = PPawn->health.hp;
        m_SavedMp       = PPawn->health.mp;
        m_SavedAt       = PPawn->loc.p;
        m_HealthSavedAt = now;
        m_SaveSeeded    = true;
        return;
    }
    if (PPawn->loc.p.x != m_SavedAt.x || PPawn->loc.p.y != m_SavedAt.y || PPawn->loc.p.z != m_SavedAt.z || PPawn->loc.p.rotation != m_SavedAt.rotation)
    {
        PPawn->setPersist(CharPersist::Position);
        m_SavedAt = PPawn->loc.p;
    }
    if ((m_SavedHp != PPawn->health.hp || m_SavedMp != PPawn->health.mp) && now - m_HealthSavedAt >= kHealthSaveEvery)
    {
        charutils::SaveCharStats(PPawn);
        m_SavedHp       = PPawn->health.hp;
        m_SavedMp       = PPawn->health.mp;
        m_HealthSavedAt = now;
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
        // A Mog House stay keeps the player in the zone, parked at its origin
        // and out of sight: not someone to follow, fight beside or rest with
        if (auto* PChar = dynamic_cast<CCharEntity*>(PMember);
            PChar != nullptr && PChar->PSession != nullptr && PChar->loc.zone == POwner->loc.zone && !PChar->inMogHouse() && !PChar->requestedZoneChange)
        {
            return PChar;
        }
    }
    return nullptr;
}

namespace
{
    // The pawn in the lead slot holds a point ahead of the player and is
    // off the ring
    auto isLead(const CCharEntity* PChar) -> bool
    {
        const auto* PController = dynamic_cast<const CPawnController*>(PChar->PAI->GetController());
        return PController != nullptr && PController->FormationSlot() == pawn::Slot::Lead;
    }
} // namespace

auto CPawnController::PlayerAnchor(const CCharEntity* PPlayer, const float predictScale) -> Anchor
{
    Anchor a;
    a.observed = PPlayer->loc.p;
    // The position packet's MoveFlame counter plus real displacement since the last packet
    a.moving = PPlayer->loc.p.moving != 0 || PPlayer->m_lastMoveDistance > 0.05f;
    a.anchor = a.observed;

    const auto streamed = cardian::link::freshPositionOf(PPlayer->id);
    if (!streamed.has_value())
    {
        return a;
    }

    a.observed  = position_t(streamed->x, streamed->y, streamed->z, 0, streamed->rotation);
    a.anchor    = a.observed;
    a.moving    = streamed->moving;
    a.streamed  = true;
    a.streamAge = streamed->age;

    // Prediction: aim at where the player will be once the rest of the loop
    // (pawn tick, travel, the client's own render cadence) has played out
    // -- the arithmetic lives in formation_math.h with its tests. A stop
    // zeroes the stream velocity within one sample, so the prediction
    // collapses at once and the pawn walks back (the rubber band).
    const auto predicted = cardian::formation::predictAhead(
        cardian::formation::Motion{ streamed->vx, streamed->vz, streamed->yawRate }, a.moving,
        settings::get<float>("pawn.FORMATION_PREDICT_MS") / 1000.0f * predictScale,
        settings::get<float>("pawn.FORMATION_PREDICT_MAX") * predictScale);
    a.anchor.x += predicted.dx;
    a.anchor.z += predicted.dz;
    a.ahead = predicted.ahead;
    return a;
}

void CPawnController::RampCatchUp(const bool playerMoving, const position_t& point)
{
    const float normalSpeed = settings::get<float>("pawn.PAWN_SPEED");
    float       wanted      = cardian::formation::catchUpSpeed(distance(POwner->loc.p, point), playerMoving, normalSpeed,
                                                               settings::get<float>("pawn.FORMATION_CATCHUP_SPEED"),
                                                               settings::get<float>("pawn.FORMATION_CATCHUP_DISTANCE"));
    // Never a step faster than the player while a mob holds hate on her
    if (wanted > normalSpeed && HatedByAnyMob())
    {
        wanted = normalSpeed;
    }

    const auto wantedSpeed = static_cast<uint8>(std::lround(wanted));
    if (wantedSpeed != POwner->baseSpeed)
    {
        m_Sprinting       = wantedSpeed > normalSpeed;
        POwner->baseSpeed = wantedSpeed;
        POwner->UpdateSpeed();
    }
}

void CPawnController::RestoreNormalSpeed()
{
    const auto normalSpeed = settings::get<uint8>("pawn.PAWN_SPEED");
    if (POwner->baseSpeed != normalSpeed)
    {
        POwner->baseSpeed = normalSpeed;
        POwner->UpdateSpeed();
    }
    m_Sprinting = false;
}

void CPawnController::FormationDebug(const char* role, const CCharEntity* PPlayer, const Anchor& a, const position_t& point)
{
    // The subject is the player's stream; a stake has none to score
    if (!settings::get<bool>("pawn.FORMATION_DEBUG") || PPlayer == nullptr)
    {
        return;
    }

    // Score the previous prediction once its horizon has elapsed: how far
    // from the predicted point did the player actually turn up?
    if (a.streamed)
    {
        if (m_Prediction.valid && m_Tick - m_Prediction.at >= m_Prediction.horizon)
        {
            m_LastPredictionError = distance(m_Prediction.point, a.observed);
            m_Prediction.valid    = false;
        }
        if (a.ahead > 0.0f && !m_Prediction.valid)
        {
            m_Prediction = { a.anchor, m_Tick, std::chrono::milliseconds(static_cast<int64>(settings::get<float>("pawn.FORMATION_PREDICT_MS"))), true };
        }
    }

    if (m_Tick - m_LastLeadDebugTime < 1s)
    {
        return;
    }
    m_LastLeadDebugTime = m_Tick;

    // Freshness of the stream vs the packet, the packet's positional lag,
    // the prediction applied and how wrong the last one was, how far the
    // pawn sits from its point, and from the player
    const auto packetAge = pawn::positionPacketAge(PPlayer->id);
    ShowInfoFmt("pawn: {} {} on {}: source={} stream={} packet={} gap={:.1f}y moving={} pred=+{:.1f}y predErr={} track={:.1f}y dist={:.1f}y speed={}",
                role, POwner->getName(), PPlayer->getName(),
                a.streamed ? "stream" : "packet",
                a.streamed ? fmt::format("{}ms", a.streamAge.count()) : "none",
                packetAge.has_value() ? fmt::format("{}ms", packetAge->count()) : "none",
                a.streamed ? distance(a.observed, PPlayer->loc.p) : 0.0f,
                a.moving ? 1 : 0,
                a.ahead,
                m_LastPredictionError >= 0.0f ? fmt::format("{:.1f}y", m_LastPredictionError) : "n/a",
                distance(POwner->loc.p, point),
                distance(POwner->loc.p, a.observed),
                POwner->baseSpeed);
}

auto CPawnController::WalkLength(const position_t& to) const -> std::optional<float>
{
    auto* navMesh = POwner->loc.zone != nullptr ? POwner->loc.zone->navMesh() : nullptr;
    if (navMesh == nullptr)
    {
        return distance(POwner->loc.p, to);
    }
    const auto path = routeOf(navMesh, POwner, POwner->loc.p, to);
    if (!path.has_value() || path->isPartial)
    {
        return std::nullopt;
    }
    float      length = 0.0f;
    position_t prev   = POwner->loc.p;
    for (const auto& point : path->points)
    {
        length += distance(prev, point.position);
        prev = point.position;
    }
    return length;
}

auto CPawnController::ReachableFormationPoint(const Anchor& a, const float offset, const float angle) -> position_t
{
    auto* navMesh = POwner->loc.zone != nullptr ? POwner->loc.zone->navMesh() : nullptr;

    // The ray from the player (where they are, not where they are
    // predicted to be: a prediction can run through a wall) to the
    // projected point, clipped where the mesh ends -- a wall, a cliff's
    // edge, a doorway's frame. Most cases end here, on the player's side
    const position_t from    = a.observed;
    position_t       point   = nearPosition(a.anchor, offset, angle);
    bool             clipped = false;
    if (navMesh != nullptr && offset > 0.0f)
    {
        // Judged on the ground (planar): the mesh's height is the point's
        // whenever it has one, and a slope is no clip
        if (const auto end = navMesh->findFurthestValidPoint(from, point); end.has_value())
        {
            clipped = distance(*end, point, true) > 0.5f;
            point   = *end;
        }
    }

    // Then priced by the walk: a point on the mesh but only reachable the
    // long way round (a thin wall with its opening far off, a ledge joined
    // by a ramp) is brought in toward the player, a third of the way at a
    // time, the last step a yalm from them -- within the tick, so she
    // never starts the long walk. The measure is the walk to the point
    // against the walk to the player and on: when she is the one behind
    // the wall every point costs the maze and none is brought in (she has
    // the maze to walk either way), and when she stands off the mesh (a
    // push-out, a ledge lip) no walk can be priced and the point stands
    bool             walkedIn = false;
    const position_t farEnd   = point;
    if (const auto toPlayer = navMesh != nullptr ? WalkLength(from) : std::nullopt; toPlayer.has_value())
    {
        const float span = distance(from, farEnd);
        for (int step = 0; step <= 3; ++step)
        {
            if (step > 0)
            {
                const float t = std::max(1.0f - step / 3.0f, span > 0.0f ? 1.0f / span : 0.0f);
                point         = position_t(from.x + (farEnd.x - from.x) * t, from.y + (farEnd.y - from.y) * t, from.z + (farEnd.z - from.z) * t, 0, 0);
                if (const auto onMesh = navMesh->findClosestValidPoint(point); onMesh.has_value())
                {
                    point = *onMesh;
                }
                walkedIn = true;
            }
            const auto walk = WalkLength(point);
            if (walk.has_value() && cardian::formation::worthTheWalk(*walk, *toPlayer + distance(from, point)))
            {
                break;
            }
        }
    }

    if ((clipped || walkedIn) && m_Tick - m_LastFormationClipTime >= 5s)
    {
        m_LastFormationClipTime = m_Tick;
        ShowInfoFmt("pawn: {} brings her point in ({:.1f}y of {:.1f}y: {}{}{})", POwner->getName(), distance(from, point), offset,
                    clipped ? "the mesh ends" : "", clipped && walkedIn ? ", " : "", walkedIn ? "a long walk round" : "");
    }
    return point;
}

auto CPawnController::FormationPoint(const Anchor& a, const float offset, const float angle, HeldPoint& held) -> position_t
{
    const position_t projected = nearPosition(a.anchor, offset, angle);

    // Remembered so Avoid() can re-seat the slot on the same ring
    m_HasSlot    = true;
    m_Slot       = a;
    m_SlotOffset = offset;
    m_SlotAngle  = angle;

    // A moving player is re-aimed every tick; the deadband only absorbs
    // the coarse position/heading updates of a player standing still, and
    // is judged on the raw projection (HeldPoint)
    if (!held.has || a.moving || distance(projected, held.raw) > settings::get<float>("pawn.FORMATION_DEADBAND"))
    {
        held.raw   = projected;
        held.point = ReachableFormationPoint(a, offset, angle);
        held.has   = true;
    }
    return held.point;
}

auto CPawnController::IsShortHop(const position_t& point, const float followMax) const -> bool
{
    // Small seating and avoidance adjustments use a direct step on the mesh.
    constexpr float directStepLimit = 1.2f;
    const float     hop             = distance(POwner->loc.p, point);
    return hop > followMax && hop < directStepLimit && POwner->PAI->PathFind->ValidPosition(point);
}

void CPawnController::NotePathFailure(const AvoidAction action, const position_t& point, const float away)
{
    if (action == AvoidAction::None || m_Tick - m_LastPathFailTime < 1s)
    {
        return;
    }
    m_LastPathFailTime = m_Tick;
    ShowInfoFmt("pawn: {} cannot path to her {} point ({:.1f}y away, at {:.1f} {:.1f} {:.1f}, on mesh: {})", POwner->getName(),
                magic_enum::enum_name(action), away, point.x, point.y, point.z, POwner->PAI->PathFind->ValidPosition(point) ? "yes" : "no");
}

auto CPawnController::Avoid(position_t& point, float& followMax, float& followTarget, float& declumpDistance, const bool fighting) -> AvoidAction
{
    using namespace cardian::formation;

    // The tick's danger map, focused on this walk (FocusDangers): the
    // circles whose mobs can see her, the point, or the way between
    const auto& dangers = m_ActiveDangers;

    // The margins that keep the boundary from being slippery: every point
    // she walks to is planned against the circles padded by kClearance, so
    // a 400 ms step (about two yalms) and the circle's breathing as she
    // walks over bumps (the height slice) cannot land her inside. Only the
    // escape test uses the true circle, and it pushes her a little further
    // out than the padding. A hold stands anywhere within kHoldBand of the
    // padded ring.
    constexpr float kClearance   = cardian::rules::kClearance;
    constexpr float kEscapeExtra = 0.5f;
    constexpr float kHoldBand    = 1.0f;
    constexpr float kDetourArc   = 3.0f;

    AvoidAction action  = AvoidAction::None;
    bool        perched = false; // this tick used or took a perch
    if (!dangers.empty())
    {
        const auto&      padded  = m_ActivePadded;  // the planning circles: every point she walks to
        const auto&      seeing  = m_EscapeDangers; // the true circles whose mobs see her: is she inside one
        const auto&      escape  = m_EscapePadded;  // and the way out of them
        const position_t me      = POwner->loc.p;

        // A point she is sent to must be on the mesh and clear: an off-mesh
        // point is snapped to the nearest walkable one, which counts only if
        // it is still outside every padded circle -- otherwise the walk
        // would snap her straight into the bubble (seen at a rim by a wall)
        const auto onMeshClear = [&](float& x, float& z) -> bool
        {
            if (POwner->PAI->PathFind->ValidPosition(position_t(x, me.y, z, 0, 0)))
            {
                return true;
            }
            const auto* navMesh = POwner->loc.zone != nullptr ? POwner->loc.zone->navMesh() : nullptr;
            if (navMesh == nullptr)
            {
                return false;
            }
            const auto snapped = navMesh->findClosestValidPoint(position_t(x, me.y, z, 0, 0));
            if (!snapped.has_value() || insideAny(padded, snapped->x, snapped->z))
            {
                return false;
            }
            x = snapped->x;
            z = snapped->z;
            return true;
        };
        const auto standAndSay = [&](const char* why)
        {
            point = me;
            if (m_Tick - m_LastPathFailTime >= 1s)
            {
                m_LastPathFailTime = m_Tick;
                ShowInfoFmt("pawn: {} stands: {}", POwner->getName(), why);
            }
        };

        // A perch sits on the padded ring by construction, so only a circle
        // that has grown well over it (half a yalm) takes it away; float
        // error and the ring's breathing do not
        const auto overgrown = [&](const position_t& perch)
        {
            return std::ranges::any_of(padded, [&](const Circle& c)
                                       {
                                           return planarDistance(c.x, c.z, perch.x, perch.z) < c.radius - 0.5f;
                                       });
        };

        if (insideAny(seeing, me.x, me.z))
        {
            // Pushed away: the minimum proximity is never violated, whatever
            // the formation wanted. Straight out is the first choice; with a
            // wall at her back, other directions around the deepest circle
            // are tried for an on-mesh, clear point.
            auto [x, z] = pushOut(escape, me.x, me.z, kEscapeExtra);
            if (!POwner->PAI->PathFind->ValidPosition(position_t(x, me.y, z, 0, 0)))
            {
                const auto deepest = std::max_element(escape.begin(), escape.end(), [&](const auto& a, const auto& b)
                                                      {
                                                          return depthInside(a, me.x, me.z) < depthInside(b, me.x, me.z);
                                                      });
                const float base = std::atan2(me.z - deepest->z, me.x - deepest->x);
                for (const float turn : { 0.5f, -0.5f, 1.0f, -1.0f, 1.6f, -1.6f, 2.2f, -2.2f, 3.1f })
                {
                    const float cx = deepest->x + std::cos(base + turn) * (deepest->radius + kEscapeExtra);
                    const float cz = deepest->z + std::sin(base + turn) * (deepest->radius + kEscapeExtra);
                    if (!insideAny(escape, cx, cz) && POwner->PAI->PathFind->ValidPosition(position_t(cx, me.y, cz, 0, 0)))
                    {
                        x = cx;
                        z = cz;
                        break;
                    }
                }
            }
            point           = position_t(x, me.y, z, 0, me.rotation);
            followMax       = 0.0f;
            followTarget    = 0.3f;
            declumpDistance = 0.0f;
            action          = AvoidAction::Escape;
        }
        else
        {
            if (insideAny(padded, point.x, point.z))
            {
                if (fighting)
                {
                    // Hold: the target is in danger and is not approached. At
                    // the boundary already she stands where she is -- a player
                    // hangs at the edge rather than pacing round it -- else she
                    // walks up to it along her own line to the target
                    bool                                   stay = false;
                    std::optional<std::pair<float, float>> rim;
                    float                                  rimDistance = 0.0f;
                    for (const auto& c : padded)
                    {
                        if (planarDistance(c.x, c.z, point.x, point.z) >= c.radius)
                        {
                            continue;
                        }
                        if (planarDistance(c.x, c.z, me.x, me.z) <= c.radius + kHoldBand)
                        {
                            stay = true;
                            break;
                        }
                        if (const auto p = approachRim(c, me.x, me.z, point.x, point.z); p.has_value())
                        {
                            const float d = planarDistance(me.x, me.z, p->first, p->second);
                            if (!rim.has_value() || d < rimDistance)
                            {
                                rim         = p;
                                rimDistance = d;
                            }
                        }
                    }
                    if (stay || !rim.has_value())
                    {
                        point = me;
                    }
                    else
                    {
                        auto [x, z] = pushOut(padded, rim->first, rim->second);
                        if (onMeshClear(x, z))
                        {
                            point = position_t(x, me.y, z, 0, me.rotation);
                        }
                        else
                        {
                            standAndSay("the boundary nearest her target is off the mesh");
                        }
                    }
                    followMax       = 1.0f;
                    followTarget    = 0.5f;
                    declumpDistance = 0.0f;
                    action          = AvoidAction::Hold;
                }
                else
                {
                    // The slot is in danger: the nearest clear angle on its own
                    // ring (the sandwich -- as close to the ideal spot as safety
                    // allows), else the point pushed straight out, is the best
                    // spot on offer. The re-seat is a formation point like any
                    // other: the world clips and prices it the same way
                    const position_t ideal  = point;
                    bool             seated = false;
                    if (m_HasSlot)
                    {
                        const auto onRing = [&](const float angle)
                        {
                            const position_t p = nearPosition(m_Slot.anchor, m_SlotOffset, angle);
                            return std::pair{ p.x, p.z };
                        };
                        if (const auto angle = safestAngleOnRing(padded, m_SlotAngle, onRing); angle.has_value())
                        {
                            point  = ReachableFormationPoint(m_Slot, m_SlotOffset, *angle);
                            seated = true;
                        }
                    }
                    if (!seated)
                    {
                        const auto [x, z] = pushOut(padded, point.x, point.z);
                        point.x           = x;
                        point.z           = z;
                    }
                    action = seated ? AvoidAction::Slot : AvoidAction::PushedSlot;
                    {
                        float cx = point.x;
                        float cz = point.z;
                        if (onMeshClear(cx, cz))
                        {
                            point.x = cx;
                            point.z = cz;
                        }
                        else
                        {
                            standAndSay("no clear spot for her slot on the mesh");
                        }
                    }

                    // The settle rule: she does not chase that spot every tick.
                    // On arrival she takes it and perches; after that the itch
                    // grows with how much better the spot on offer has become
                    // than her perch, beyond what she tolerates, and drains
                    // otherwise -- and only when it crosses her patience does
                    // she move, once, in one go. A perch a circle has grown
                    // well over is given up at once.
                    const position_t candidate = point;
                    const float      dt        = std::clamp(std::chrono::duration<float>(m_Tick - m_LastItchTick).count(), 0.0f, 1.0f);
                    m_LastItchTick             = m_Tick;
                    if (m_AvoidPerch.has_value() && !overgrown(*m_AvoidPerch))
                    {
                        const float improvement = planarDistance(m_AvoidPerch->x, m_AvoidPerch->z, ideal.x, ideal.z) -
                                                  planarDistance(candidate.x, candidate.z, ideal.x, ideal.z);
                        m_AvoidItch = itchAfter(m_AvoidItch, improvement, settings::get<float>("pawn.AVOID_ITCH_TOLERANCE"), dt);
                        if (m_AvoidItch >= settings::get<float>("pawn.AVOID_ITCH_PATIENCE"))
                        {
                            ShowInfoFmt("pawn: {} moves her perch ({:.1f}y closer to her spot)", POwner->getName(), improvement);
                            m_AvoidPerch = candidate;
                            m_AvoidItch  = 0.0f;
                        }
                        else
                        {
                            point  = *m_AvoidPerch;
                            action = AvoidAction::Perch;
                        }
                    }
                    else
                    {
                        if (!m_AvoidPerch.has_value())
                        {
                            ShowInfoFmt("pawn: {} perches {:.1f}y off her spot", POwner->getName(), planarDistance(candidate.x, candidate.z, ideal.x, ideal.z));
                        }
                        m_AvoidPerch = candidate;
                        m_AvoidItch  = 0.0f;
                    }
                    perched = true;
                }
            }
            else if (!fighting && m_AvoidPerch.has_value() && !overgrown(*m_AvoidPerch) &&
                     planarDistance(point.x, point.z, m_AvoidPerch->x, m_AvoidPerch->z) <= settings::get<float>("pawn.AVOID_ITCH_TOLERANCE"))
            {
                // Her spot hovers just outside the bubble, within the tolerance
                // of the perch: not worth leaving it for
                point   = *m_AvoidPerch;
                action  = AvoidAction::Perch;
                perched = true;
            }

            // The way to the point (as it now stands) cuts into a circle: go
            // round the NEAREST such one first (the list is unordered), with
            // the waypoint itself pushed clear of any other circle; next tick
            // re-plans
            const Circle* nearestCrossing = nullptr;
            float         nearestDist     = 0.0f;
            for (const auto& c : padded)
            {
                if (!segmentEnters(c, me.x, me.z, point.x, point.z))
                {
                    continue;
                }
                const float d = planarDistance(c.x, c.z, me.x, me.z);
                if (nearestCrossing == nullptr || d < nearestDist)
                {
                    nearestCrossing = &c;
                    nearestDist     = d;
                }
            }
            if (nearestCrossing != nullptr)
            {
                // A detour that makes no progress is no detour: at the clear
                // point already, the short way round yields a waypoint she is
                // standing on. Then the long way round is tried only when the
                // straight walk would reach the true circle; a shallow clip
                // is walked. No way round a deep one: she stands and says so.
                constexpr float kDetourStep = 0.3f;
                const auto      waypoint    = [&](const float preferDir)
                {
                    auto [x, z]    = detourAround(*nearestCrossing, me.x, me.z, point.x, point.z, 0.0f, kDetourArc, kHoldBand, preferDir);
                    std::tie(x, z) = pushOut(padded, x, z);
                    return std::pair{ x, z };
                };
                const auto usable = [&](float& x, float& z)
                {
                    return planarDistance(me.x, me.z, x, z) > kDetourStep && onMeshClear(x, z);
                };
                auto [x, z]     = waypoint(0.0f);
                bool progress   = usable(x, z);
                const bool deep = segmentClosest(*nearestCrossing, me.x, me.z, point.x, point.z) < nearestCrossing->radius - kClearance;
                if (!progress && deep)
                {
                    for (const float dir : { 1.0f, -1.0f })
                    {
                        auto [dx, dz] = waypoint(dir);
                        if (usable(dx, dz))
                        {
                            x        = dx;
                            z        = dz;
                            progress = true;
                            break;
                        }
                    }
                }
                if (progress)
                {
                    point           = position_t(x, me.y, z, 0, me.rotation);
                    followMax       = kDetourStep;
                    followTarget    = kDetourStep;
                    declumpDistance = 0.0f;
                    action          = AvoidAction::Detour;
                }
                else if (deep)
                {
                    standAndSay("boxed in, the way to her point crosses a circle and no way round is on the mesh");
                }
            }
        }
    }

    if (!perched)
    {
        m_AvoidPerch.reset();
        m_AvoidItch    = 0.0f;
        m_LastItchTick = m_Tick;
    }

    // A change of action is always worth a line; the full picture once a
    // second under FORMATION_DEBUG. The nearest danger is found only when a
    // line is actually printed.
    const bool changed = m_LastAvoidAction != action && action != AvoidAction::None;
    const bool debug   = settings::get<bool>("pawn.FORMATION_DEBUG") && m_Tick - m_LastAvoidDebugTime >= 1s;
    m_LastAvoidAction  = action;

    if (changed || debug)
    {
        const auto nearest = std::min_element(dangers.begin(), dangers.end(), [](const auto& a, const auto& b)
                                              {
                                                  return a.distance < b.distance;
                                              });
        if (changed)
        {
            ShowInfoFmt("pawn: {} avoids {}{} ({}: {:.1f}y of a {:.1f}y circle)", POwner->getName(),
                        nearest->mob->getName(), nearest->linked ? " [link]" : "", magic_enum::enum_name(action), nearest->distance, nearest->radius);
        }
        if (debug)
        {
            m_LastAvoidDebugTime = m_Tick;
            if (dangers.empty())
            {
                ShowInfoFmt("pawn: avoid {}: dangers=0", POwner->getName());
            }
            else
            {
                ShowInfoFmt("pawn: avoid {}: dangers={} nearest={}{} d={:.1f}y r={:.1f}y action={} pt={:.1f}y itch={:.1f} perch={}", POwner->getName(), dangers.size(),
                            nearest->mob->getName(), nearest->linked ? " [link]" : "", nearest->distance, nearest->radius, magic_enum::enum_name(action),
                            distance(POwner->loc.p, point), m_AvoidItch, m_AvoidPerch.has_value() ? fmt::format("{:.1f}y", distance(POwner->loc.p, *m_AvoidPerch)) : "-");
            }
        }
    }
    return action;
}

auto CPawnController::LeadPoint(const Place& place, const CCharEntity* PPlayer) -> position_t
{
    const auto a          = place.anchor(1.0f);
    m_PlayerMoving        = a.moving;
    m_LastPredictionAhead = a.ahead;

    float lead = settings::get<float>("pawn.FORMATION_LEAD_DISTANCE");
    if (a.moving)
    {
        lead += settings::get<float>("pawn.FORMATION_LEAD_MOVING_BONUS");
    }

    const auto point = FormationPoint(a, lead, 0.0f, m_LeadHeld);
    FormationDebug("lead", PPlayer, a, point);
    return point;
}

auto CPawnController::RingSlot() const -> pawn::Slot
{
    using cardian::formation::isRingSlot;
    using cardian::formation::Seat;

    const auto seatOf = [](const CCharEntity* PChar, const pawn::Slot slot) -> Seat
    {
        return { pawn::isMeleeJob(PChar->GetMJob()), isRingSlot(slot) ? std::optional<pawn::Slot>{ slot } : std::nullopt };
    };

    // The party's cardians in this zone, alive and not leading, in party
    // order -- the same list for everyone, so the seats agree
    std::vector<Seat> seats;
    std::size_t       mine  = 0;
    bool              found = false;

    const auto* PPawn = static_cast<const CCharEntity*>(POwner);
    if (PPawn->PParty != nullptr)
    {
        for (const auto* PMember : PPawn->PParty->members)
        {
            const auto* PChar = dynamic_cast<const CCharEntity*>(PMember);
            if (PChar == nullptr || !pawn::isPawn(PChar) || PChar->loc.zone != POwner->loc.zone || PChar->isDead())
            {
                continue;
            }
            const auto* PController = dynamic_cast<const CPawnController*>(PChar->PAI->GetController());
            if (PController == nullptr || PController->FormationSlot() == pawn::Slot::Lead)
            {
                continue;
            }
            if (PChar == POwner)
            {
                mine  = seats.size();
                found = true;
            }
            seats.push_back(seatOf(PChar, PController->FormationSlot()));
        }
    }

    if (!found)
    {
        seats = { seatOf(PPawn, FormationSlot()) };
        mine  = 0;
    }
    return cardian::formation::assignSlots(seats)[mine];
}

auto CPawnController::SeatOf(const pawn::Slot slot) -> SeatGeometry
{
    // nearPosition's angle runs to the player's right for positive values
    // (checked in play against the debug line's seat name)
    constexpr float kRight = 1.0f;
    constexpr float kPi    = static_cast<float>(M_PI);
    const float     flank  = settings::get<float>("pawn.FORMATION_FLANK_ANGLE_DEG") * kPi / 180.0f;
    const float     rear   = kPi - settings::get<float>("pawn.FORMATION_FOLLOW_ANGLE_DEG") * kPi / 180.0f;

    switch (slot)
    {
        case pawn::Slot::FlankRight:
            return { settings::get<float>("pawn.FORMATION_FLANK_DISTANCE"), kRight * flank };
        case pawn::Slot::FlankLeft:
            return { settings::get<float>("pawn.FORMATION_FLANK_DISTANCE"), -kRight * flank };
        case pawn::Slot::RearLeft:
            return { settings::get<float>("pawn.FORMATION_FOLLOW_DISTANCE"), -kRight * rear };
        case pawn::Slot::Behind:
            return { settings::get<float>("pawn.FORMATION_REAR_DISTANCE"), kPi };
        case pawn::Slot::RearRight:
        default:
            return { settings::get<float>("pawn.FORMATION_FOLLOW_DISTANCE"), kRight * rear };
    }
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
            PChar != nullptr && pawn::isPawn(PChar) && !isLead(PChar))
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

namespace
{
    // The herd pass for one mob, kept for the tick it was worked out in so
    // every cardian on the mob reads the same pass, and when the ring was
    // last evened out
    struct HerdPass
    {
        timer::time_point                 tick{};
        timer::time_point                 evenedAt{};
        std::unordered_map<uint32, float> bearings; // by character id
    };
    std::unordered_map<uint32, HerdPass> herdPasses; // by mob id
} // namespace

auto CPawnController::HerdFixed(const CBattleEntity* PMob) const -> bool
{
    // The Tank places herself and the herd spaces round her; a cardian
    // holding her position stays put; a Thief on her way to the mob's back
    // is on an errand; a bad spot pins her a while; one who cannot move
    // (asleep, bound, casting) is where she stands
    return m_Waiting || SneaksOn(PMob) || HerdPinned(PMob) || !POwner->PAI->CanFollowPath() || POwner->GetSpeed() == 0 ||
           (m_Gambits->MasterOn() && pawn::roster::roleOf(static_cast<CCharEntity*>(POwner)) == cardian::party::Role::Tank);
}

auto CPawnController::HerdBody(const CBattleEntity* PMob) const -> std::optional<cardian::herd::Body>
{
    if (PMob == nullptr || m_Mode != Mode::Fight || POwner->GetBattleTarget() != PMob || POwner->isDead())
    {
        return std::nullopt;
    }
    const auto& me   = POwner->loc.p;
    const float here = cardian::herd::bearingOf(PMob->loc.p.x, PMob->loc.p.z, me.x, me.z);
    if (HerdFixed(PMob))
    {
        return cardian::herd::Body{ .bearing = here, .fixed = true };
    }
    // Walking to a new spot in reach, the pass sees her where she is going;
    // otherwise where she stands, settled once she has taken her spot
    if (m_Herd.mob == PMob->id && m_Herd.walking)
    {
        return cardian::herd::Body{ .bearing = m_Herd.bearing };
    }
    return cardian::herd::Body{ .bearing = here, .settled = m_Herd.mob == PMob->id && m_Herd.taken };
}

auto CPawnController::HerdBearing(const CBattleEntity* PMob) -> float
{
    using namespace cardian::herd;
    constexpr float kPi = std::numbers::pi_v<float>;

    auto& pass = herdPasses[PMob->id];
    if (pass.tick != m_Tick)
    {
        // A mob nobody has asked about for half a minute is let go
        std::erase_if(herdPasses, [&](const auto& entry)
                      {
                          return entry.first != PMob->id && entry.second.tick + 30s < m_Tick;
                      });
        pass.tick = m_Tick;
        pass.bearings.clear();

        // Everyone in the melee ring: the party's cardians fighting it, as
        // each reads herself (HerdBody), and the player near enough to be
        // in it -- spaced round, never moved
        std::vector<uint32> ids;
        std::vector<Body>   bodies;
        const auto*         PPawn = static_cast<const CCharEntity*>(POwner);
        if (PPawn->PParty != nullptr)
        {
            for (auto* PMember : PPawn->PParty->members)
            {
                auto* PChar = dynamic_cast<CCharEntity*>(PMember);
                if (PChar == nullptr || PChar->loc.zone != POwner->loc.zone || PChar->isDead())
                {
                    continue;
                }
                if (pawn::isPawn(PChar))
                {
                    const auto* PController = dynamic_cast<const CPawnController*>(PChar->PAI->GetController());
                    if (const auto body = PController != nullptr ? PController->HerdBody(PMob) : std::nullopt; body.has_value())
                    {
                        ids.push_back(PChar->id);
                        bodies.push_back(*body);
                    }
                }
                else if (distance(PChar->loc.p, PMob->loc.p) <= PChar->GetMeleeRange(PMob) + 1.0f)
                {
                    ids.push_back(PChar->id);
                    bodies.push_back(Body{ .bearing = bearingOf(PMob->loc.p.x, PMob->loc.p.z, PChar->loc.p.x, PChar->loc.p.z), .fixed = true });
                }
            }
        }
        if (std::ranges::find(ids, POwner->id) == ids.end())
        {
            if (const auto body = HerdBody(PMob); body.has_value())
            {
                ids.push_back(POwner->id);
                bodies.push_back(*body);
            }
        }

        // Spaced to the gap; every HERD_BEAT evened out a step, in yalms of
        // walk at her distance from the mob, and spaced again (a step of 0,
        // the default: never)
        const float gap  = settings::get<float>("pawn.HERD_GAP_DEG") * kPi / 180.0f;
        auto        out  = spread(bodies, gap);
        const auto  beat = std::chrono::duration_cast<timer::duration>(std::chrono::duration<float>(settings::get<float>("pawn.HERD_BEAT")));
        if (m_Tick >= pass.evenedAt + beat)
        {
            pass.evenedAt      = m_Tick;
            const float radius = std::max(FightRadius(PMob), 0.5f);
            for (std::size_t i = 0; i < bodies.size(); ++i)
            {
                bodies[i].bearing = out[i];
            }
            out = evenStep(bodies, settings::get<float>("pawn.HERD_EVEN_STEP") / radius, settings::get<float>("pawn.HERD_MOVE_MIN") / radius);
            for (std::size_t i = 0; i < bodies.size(); ++i)
            {
                bodies[i].bearing = out[i];
            }
            out = spread(bodies, gap);
        }
        for (std::size_t i = 0; i < ids.size(); ++i)
        {
            pass.bearings[ids[i]] = out[i];
        }
    }
    if (const auto it = pass.bearings.find(POwner->id); it != pass.bearings.end())
    {
        return it->second;
    }
    const auto& me = POwner->loc.p;
    return bearingOf(PMob->loc.p.x, PMob->loc.p.z, me.x, me.z);
}

auto CPawnController::HerdPoint(const CBattleEntity* PMob, const float bearing) const -> position_t
{
    const float radius = FightRadius(PMob);
    position_t  point  = PMob->loc.p;
    point.x += std::cos(bearing) * radius;
    point.z += std::sin(bearing) * radius;
    return point;
}

void CPawnController::NoteHerd(const CBattleEntity* PMob, const float bearing, const bool inReach, const position_t& point)
{
    using cardian::herd::compassRounded;

    if (m_Herd.mob != PMob->id)
    {
        m_Herd     = {};
        m_Herd.mob = PMob->id;
    }
    m_Herd.bearing = bearing;
    if (!inReach)
    {
        // Towed: out of reach she closes from where she is, and takes her
        // spot again on arrival
        m_Herd.taken   = false;
        m_Herd.walking = false;
        return;
    }
    // Set off once the spot is HERD_MOVE_MIN off her; once walking, carry
    // on until she is on it (the mover's arrival), so a step is a step
    if (distance(POwner->loc.p, point) <= HerdDeadband())
    {
        if (!m_Herd.taken)
        {
            ShowInfoFmt("pawn: {} engages {} at {}°", POwner->getName(), PMob->getName(), compassRounded(bearing));
        }
        else if (m_Herd.walking)
        {
            ShowInfoFmt("pawn: {} settles round {} at {}°", POwner->getName(), PMob->getName(), compassRounded(bearing));
        }
        m_Herd.taken   = true;
        m_Herd.walking = false;
        return;
    }
    // In reach and off her spot: on arrival she walks onto it; once taken,
    // a spot the herd has moved is a walk round the mob
    if (m_Herd.taken && !m_Herd.walking)
    {
        ShowInfoFmt("pawn: {} moves round {} to {}° (the herd)", POwner->getName(), PMob->getName(), compassRounded(bearing));
        m_Herd.walkingSince = m_Tick;
    }
    m_Herd.walking = m_Herd.taken;
    // A walk round the mob that has not arrived in a few seconds -- held at
    // a danger's rim, or a path that ends short -- is given up where she is
    if (m_Herd.walking && m_Tick > m_Herd.walkingSince + 6s)
    {
        HerdPin("her spot is out of her way");
    }
}

auto CPawnController::HerdDeadband() const -> float
{
    return m_Herd.walking ? kSpotArrive : settings::get<float>("pawn.HERD_MOVE_MIN");
}

void CPawnController::HerdPin(const std::string_view why)
{
    if (m_Herd.mob == 0)
    {
        return;
    }
    if (m_Tick >= m_Herd.pinnedUntil)
    {
        ShowInfoFmt("pawn: {} holds where she stands ({}); the herd spaces round her a while", POwner->getName(), why);
    }
    m_Herd.pinnedUntil  = m_Tick + 3s;
    m_Herd.walking      = false;
    m_Herd.blocked      = m_Herd.bearing;
    m_Herd.blockedUntil = m_Tick + 20s;
}

auto CPawnController::HerdPinned(const CBattleEntity* PMob) const -> bool
{
    return PMob != nullptr && m_Herd.mob == PMob->id && m_Tick < m_Herd.pinnedUntil;
}

auto CPawnController::HerdAvoids(const CBattleEntity* PMob, const float bearing) -> bool
{
    // The pass sending her back to a spot that failed her lately (a wall,
    // a path that never arrives): she holds where she stands instead, pinned
    // again without a word, until the spot has been left alone a while
    constexpr float kNear = 15.0f * std::numbers::pi_v<float> / 180.0f;
    if (PMob == nullptr || m_Herd.mob != PMob->id || m_Tick >= m_Herd.blockedUntil || HerdPinned(PMob) ||
        std::abs(cardian::herd::shortest(m_Herd.blocked, bearing)) > kNear)
    {
        return false;
    }
    m_Herd.pinnedUntil = m_Tick + 3s;
    m_Herd.walking     = false;
    return true;
}

auto CPawnController::FightRadius(const CBattleEntity* PTarget) const -> float
{
    // Inside its reach, always: a target it cannot reach is one it walks
    // onto again
    return std::min(RoamDistance, POwner->GetMeleeRange(PTarget) - settings::get<float>("pawn.MELEE_BACKOFF_MARGIN"));
}

auto CPawnController::FightClearance(const CBattleEntity* PTarget) const -> float
{
    return cardian::formation::meleeClearance(FightRadius(PTarget), settings::get<float>("pawn.MELEE_BACKOFF_TRIGGER"));
}

auto CPawnController::TowsAtStake() const -> bool
{
    // The party's Tank tows, with her gambits on: the seat is what she is
    // for (RESEARCH §17.13), and with the master switch off nothing of her
    // rows or her seat moves her
    return Staked() && m_Gambits->MasterOn() && pawn::roster::roleOf(static_cast<CCharEntity*>(POwner)) == cardian::party::Role::Tank;
}

auto CPawnController::HoldsFireOn(const CBattleEntity* PTarget) -> bool
{
    // Held while the tank's own receive rule (CampReceive) says the pull is
    // still on its way: not at the landing point, not on her, not stalled
    if (PTarget == nullptr || PTarget->objtype != TYPE_MOB || !Staked() || TowsAtStake() || PTarget->GetBattleTarget() == POwner ||
        CampReceive(PTarget) == cardian::stake::ReceiveAction::Join)
    {
        return false;
    }
    if (m_HeldFireOn != PTarget->id)
    {
        m_HeldFireOn = PTarget->id;
        ShowInfoFmt("pawn: {} holds her spells on {} until it comes in to the camp", POwner->getName(), PTarget->getName());
    }
    return true;
}

auto CPawnController::WaitsForThePull(const CBattleEntity* PTarget, const ApproachKind kind) -> bool
{
    return kind == ApproachKind::Join && Staked() && !TowsAtStake() && CampReceive(PTarget) != cardian::stake::ReceiveAction::Join;
}

auto CPawnController::CampReceive(const CBattleEntity* PTarget) -> cardian::stake::ReceiveAction
{
    if (!m_ReceiveMob.has_value() || !(*m_ReceiveMob == PTarget))
    {
        m_ReceiveMob = EntityId(PTarget);
        m_Receive = {};
        m_KeepCampFightSpot = false;
        m_CampSettlement = {};
    }
    const auto& stake = m_Stake->at;
    const auto home = nearPosition(stake, cardian::stake::kMobAhead, 0.0f);
    const cardian::stake::ReceiveConfig config{
        settings::get<float>("pawn.CAMP_RECEIVE_IMMEDIATE"),
        settings::get<double>("pawn.CAMP_RECEIVE_SECONDS_PER_YALM"),
        settings::get<double>("pawn.CAMP_RECEIVE_MAX_WAIT"),
        settings::get<float>("pawn.CAMP_RECEIVE_PROGRESS"),
        settings::get<double>("pawn.CAMP_RECEIVE_WINDOW"),
    };
    const bool joined = m_Receive.joined;
    const auto result = m_Receive.update(std::chrono::duration<double>(m_Tick.time_since_epoch()).count(),
                                        distance(PTarget->loc.p, home, true),
                                        isWithinDistance(stake, PTarget->loc.p, settings::get<float>("pawn.HUNT_LEASH")),
                                        PTarget->GetBattleTarget() == POwner, PTarget->PAI->IsEngaged(), config);
    if (!joined && m_Receive.joined)
    {
        ShowInfoFmt("pawn: {} receives {} ({:.1f} y from landing point, {})", POwner->getName(), PTarget->getName(),
                    distance(PTarget->loc.p, home, true), PTarget->GetBattleTarget() == POwner ? "has hate" : "closes to help");
    }
    return result;
}

auto CPawnController::ResumeCampReceive() -> bool
{
    // The engine sheathes beyond its 30-y sight range. A flyby can cross
    // that line while still inside camp's admission radius; preserve its
    // clock (or committed pursuit) through a weapon-away approach.
    auto* PMob = TowsAtStake() && m_ReceiveMob.has_value() ? m_ReceiveMob->resolve<CMobEntity>() : nullptr;
    if (PMob == nullptr || POwner->isDead() || m_Waiting || (!m_Receive.sampled && !m_Receive.joined) || !PMob->PAI->IsEngaged())
    {
        return false;
    }
    const auto facts = EngageFactsFor(PMob);
    if (facts.distance <= cardian::rules::kDrawRange || !cardian::rules::worthWalkingIn(facts) ||
        (!m_Receive.joined && !isWithinDistance(m_Stake->at, PMob->loc.p, settings::get<float>("pawn.HUNT_LEASH"))))
    {
        return false;
    }
    m_Approach = Approach{ EntityId(PMob), ApproachKind::Join };
    Transition(Mode::Approach, fmt::format("keeps receiving {} with weapon away (beyond draw range)", PMob->getName()));
    return true;
}

auto CPawnController::TowIntent(CBattleEntity* PTarget) -> Intent
{
    // The flag is the frontline. Aim just ahead of it, with a small
    // arrival allowance; any intrusion behind it calls for a correction.
    // The ordinary avoidance pass still has the final word on movement.
    const auto& stake      = m_Stake->at;
    const auto  home       = nearPosition(stake, cardian::stake::kMobAhead, 0.0f);
    const float reach      = std::max(1.0f, PTarget->GetMeleeRange(POwner) - 0.3f);
    const float mobToHome  = distance(PTarget->loc.p, home, true);
    const float mobToFlag  = distance(PTarget->loc.p, stake, true);
    const float forward   = cardian::stake::forwardOf(stake.x, stake.z, stake.rotation, PTarget->loc.p.x, PTarget->loc.p.z);
    const bool  newMob     = !m_TowingMob.has_value() || !(*m_TowingMob == PTarget);
    const bool  wasTowing  = !newMob && m_Towing;
    const bool  wasClosing = !newMob && m_ClosingWithoutHate;
    const bool  received   = CampReceive(PTarget) == cardian::stake::ReceiveAction::Join;
    const bool  hasHate    = PTarget->GetBattleTarget() == POwner;
    const bool  wasKept    = m_KeepCampFightSpot;
    const bool  pathing    = PTarget->PAI->PathFind != nullptr && PTarget->PAI->PathFind->IsFollowingPath();
    auto* const victim    = PTarget->GetBattleTarget();
    // Ordinary melee state excludes pauses to cast, shoot or use a TP move.
    // A failed route or a wall must not be mistaken for reaching the fight.
    // Once accepted, these action checks do not revoke the established spot.
    const bool meleeReady = !wasKept && !pathing && PTarget->isAlive() && victim != nullptr && victim->isAlive() &&
        victim->loc.zone == PTarget->loc.zone && PTarget->PAI->IsCurrentState<CAttackState>() &&
        !PTarget->StatusEffectContainer->HasPreventActionEffect() &&
        isWithinDistance(PTarget->loc.p, victim->loc.p, PTarget->GetMeleeRange(victim)) && PTarget->CanSeeTarget(victim);
    const bool settled = m_CampSettlement.observe(PTarget->PAI->getTick().time_since_epoch().count(),
        PTarget->PAI->getPrevTick().time_since_epoch().count(), PTarget->loc.p.x, PTarget->loc.p.y, PTarget->loc.p.z, meleeReady);
    m_KeepCampFightSpot    = cardian::stake::keepsFightSpot(wasKept, settled, mobToFlag, forward);
    // Do not start towing on the first plausible stop and thereby move the
    // mob before its confirming update. The normal walker still vets Stand.
    const bool confirming = received && hasHate && !m_KeepCampFightSpot && meleeReady &&
        cardian::stake::keepsFightSpot(false, true, mobToFlag, forward);
    m_ClosingWithoutHate  = received && !hasHate;
    // During receive, stay ready at the landing point. Once committed,
    // melee a mob on somebody else; only its actual target can tow it.
    // A stopped pull in the front half belongs to the player: gaining hate
    // must not drag that fight to the exact landing point. No dwell timer.
    m_Towing              = !received || (hasHate && !m_KeepCampFightSpot && !confirming && cardian::stake::frontlineTowing(newMob || wasClosing, m_Towing, mobToHome, forward));
    m_TowingMob           = EntityId(PTarget);
    m_HasSlot             = false;
    if (m_KeepCampFightSpot != wasKept)
    {
        ShowInfoFmt("pawn: {} {} {}'s fight spot ({:.1f} y from flag, {:.1f} y forward)", POwner->getName(),
                    m_KeepCampFightSpot ? "accepts" : "releases", PTarget->getName(), distance(PTarget->loc.p, stake, true), forward);
    }
    if (newMob || wasTowing != m_Towing || wasClosing != m_ClosingWithoutHate)
    {
        m_TowRoute.reset();
        m_SeatPathActive = false;
    }

    position_t point;
    if (confirming)
    {
        point = POwner->loc.p;
    }
    else if (m_Towing)
    {
        // The tank can stand behind the line to receive an incoming pull;
        // it is the monster's intended stopping point that stays in front.
        const auto [x, z] = cardian::stake::towPoint(PTarget->loc.p.x, PTarget->loc.p.z, home.x, home.z, reach, stake.rotation);
        point             = position_t(x, stake.y, z, 0, 0);
        if (!wasTowing)
        {
            ShowInfoFmt("pawn: {} {} {} ahead of the camp frontline ({:.1f} y from landing point, {:.1f} y forward): waits at ({:.1f}, {:.1f})", POwner->getName(), hasHate ? "tows" : "waits to receive", PTarget->getName(), mobToHome, forward, x, z);
        }
    }
    else
    {
        // The stake's 3 o'clock, on the mob: the right-hand side of the
        // stake's heading (the right flank seat's sign), at her reach
        position_t frame = PTarget->loc.p;
        frame.rotation   = stake.rotation;
        point            = nearPosition(frame, FightRadius(PTarget), std::numbers::pi_v<float> / 2.0f);
        if (wasTowing && !m_ClosingWithoutHate)
        {
            ShowInfoFmt("pawn: {} has {} settled ahead of camp ({:.1f} y from landing point, {:.1f} y forward) and plans its 3 o'clock seat", POwner->getName(), PTarget->getName(), mobToHome, forward);
        }
    }
    const bool inReach = distance(POwner->loc.p, PTarget->loc.p) <= POwner->GetMeleeRange(PTarget);
    auto       intent = confirming ? Intent{} : SeatIntent(PTarget, point, inReach && !m_Towing, true);
    // The mob being still does not mean she has reached 3 o'clock yet.
    // Only polish an arrived seat, never a receive/tow or an unfinished path.
    // Keep observing while positioning so an old settle timer cannot survive it.
    const bool seated = !confirming && !m_Towing && inReach && intent.kind == Intent::Kind::Stand &&
                        isWithinDistance(POwner->loc.p, point, settings::get<float>("pawn.FIGHT_SEAT_DEADBAND")) &&
                        !POwner->PAI->PathFind->IsFollowingPath();
    if (auto backoff = StepBackIntent(PTarget, seated); backoff.has_value())
    {
        intent = *backoff;
    }
    intent.target     = PTarget;
    intent.fighting   = true;
    intent.vet        = PTarget->PAI->IsEngaged() || !pawn::huntRulesOf(pawn::ordersOwnerOf(static_cast<const CCharEntity*>(POwner))).aggressive;
    if (settings::get<bool>("pawn.FORMATION_DEBUG") && m_Tick - m_LastTowRouteDebug >= 1s)
    {
        m_LastTowRouteDebug = m_Tick;
        ShowInfoFmt("pawn: {} camp tank {}: mob ({:.1f}, {:.1f}), tank ({:.1f}, {:.1f}), goal ({:.1f}, {:.1f}), move {}, step ({:.1f}, {:.1f})",
                    POwner->getName(), confirming ? "confirm stop" : !received ? "receive" : m_ClosingWithoutHate ? "melee" : m_Towing ? "tow" : "seat", PTarget->loc.p.x, PTarget->loc.p.z,
                    POwner->loc.p.x, POwner->loc.p.z, point.x, point.z, magic_enum::enum_name(intent.kind), intent.point.x, intent.point.z);
    }
    return intent;
}

auto CPawnController::SeatIntent(const CBattleEntity* PTarget, const position_t& seat, const bool inReach, const bool campRoute, const bool ownSeat) -> Intent
{
    using cardian::formation::Circle;
    using cardian::formation::segmentCrosses;

    const auto* PPathFind = POwner->PAI->PathFind.get();
    const auto& me        = POwner->loc.p;
    const float off       = distance(me, seat);
    Intent      intent;

    // A moved mob invalidates the old detour before the cached-path check.
    // Keep the chosen side through small shifts to avoid left-right wiggling.
    if (campRoute && m_TowRoute.observe(PTarget->loc.p.x, PTarget->loc.p.z))
    {
        m_SeatPathActive = false;
        if (settings::get<bool>("pawn.FORMATION_DEBUG"))
        {
            ShowInfoFmt("pawn: {} replans her camp route around {} (mob moved {:.1f} y or more)",
                        POwner->getName(), PTarget->getName(), cardian::stake::Route::kMobDrift);
        }
    }

    // On her spot, or near enough while in reach: she stands. Her herd
    // spot waits until it is HERD_MOVE_MIN off her (HerdDeadband); the
    // mob's back and the camp tank's spot keep FIGHT_SEAT_DEADBAND
    const float deadband = !inReach                ? 0.5f
                           : ownSeat && !campRoute ? HerdDeadband()
                                                   : settings::get<float>("pawn.FIGHT_SEAT_DEADBAND");
    if (off <= deadband)
    {
        m_SeatVia = false;
        if (campRoute)
        {
            m_TowRoute.reset();
        }
        return intent;
    }

    // A small seat adjustment (IsShortHop): straight at it, on the mesh.
    if (off < 1.2f)
    {
        if (PPathFind->ValidPosition(seat))
        {
            if (campRoute)
            {
                m_TowRoute.reset();
            }
            intent.kind  = Intent::Kind::Hop;
            intent.point = seat;
            return intent;
        }
        // The spot has left the mesh (the mob against a wall): she holds
        // where she stands, and the herd spaces round her a while. Not her
        // spot (the mob's back for Sneak Attack): she stands, and the
        // walk's own clock gives it up
        if (ownSeat && !campRoute)
        {
            HerdPin("her spot is off the mesh");
        }
        return intent;
    }

    // A path already on the way stands until the seat has drifted a yalm
    // from where it was planned against
    if (m_SeatPathActive && PPathFind->IsFollowingPath() && distance(seat, m_SeatDestination) <= 1.0f)
    {
        intent.kind = Intent::Kind::Keep;
        return intent;
    }

    // A far spot is reached round the mob's side, never through it: a way
    // there crossing the mob goes first to the middle of the arc, a quarter
    // turn at most, on the side of the shorter turn, at the spot's distance
    position_t   goal = seat;
    const Circle body{ PTarget->loc.p.x, PTarget->loc.p.z, PTarget->modelHitboxSize + 0.8f };
    if (campRoute)
    {
        const auto [x, z] = m_TowRoute.point(body.x, body.z, FightClearance(PTarget), me.x, me.z, seat.x, seat.z);
        goal.x = x;
        goal.z = z;
    }
    else if (segmentCrosses(body, me.x, me.z, seat.x, seat.z))
    {
        using cardian::herd::bearingOf;
        constexpr float kQuarter = std::numbers::pi_v<float> / 2.0f;
        const float     from     = bearingOf(body.x, body.z, me.x, me.z);
        const float     turn     = cardian::herd::shortest(from, bearingOf(body.x, body.z, seat.x, seat.z));
        const float     viaAt    = from + std::clamp(turn / 2.0f, -kQuarter, kQuarter);
        const float     reach    = std::hypot(seat.x - body.x, seat.z - body.z);
        position_t      via      = seat;
        via.x                    = body.x + std::cos(viaAt) * reach;
        via.z                    = body.z + std::sin(viaAt) * reach;
        if (distance(me, via) > 1.0f)
        {
            goal = via;
            if (!m_SeatVia)
            {
                ShowInfoFmt("pawn: {} goes round {} ({})", POwner->getName(), PTarget->getName(), turn > 0.0f ? "anticlockwise" : "clockwise");
            }
            m_SeatVia = true;
        }
    }

    m_SeatDestination = seat;
    // Choose the movement method from the next waypoint's distance.
    intent.kind       = IsShortHop(goal, 0.0f) ? Intent::Kind::Hop : Intent::Kind::Path;
    intent.point      = goal;
    intent.arrive     = kSpotArrive;
    intent.tolerance  = 0.0f;
    intent.seat       = true;
    intent.herdSpot   = ownSeat && !campRoute;
    return intent;
}

auto CPawnController::ReactionBeat() const -> timer::duration
{
    // The row: the lead at once, then the flanks, the rear quarters, behind
    int beats = 0;
    switch (FormationSlot() == pawn::Slot::Lead ? pawn::Slot::Lead : RingSlot())
    {
        case pawn::Slot::Lead:
            break;
        case pawn::Slot::FlankLeft:
        case pawn::Slot::FlankRight:
            beats = settings::get<uint8>("pawn.REACTION_BEATS_FLANK");
            break;
        case pawn::Slot::RearLeft:
        case pawn::Slot::RearRight:
            beats = settings::get<uint8>("pawn.REACTION_BEATS_REAR");
            break;
        default:
            beats = settings::get<uint8>("pawn.REACTION_BEATS_BEHIND");
            break;
    }
    beats += xirand::GetRandomNumber<int>(0, static_cast<int>(settings::get<uint8>("pawn.REACTION_JITTER")) + 1);
    return std::chrono::duration_cast<timer::duration>(std::chrono::duration<float>(settings::get<float>("pawn.REACTION_BEAT") * static_cast<float>(beats)));
}

auto CPawnController::StepBackIntent(const CBattleEntity* PTarget, const bool positioned) -> std::optional<Intent>
{
    TracyZoneScoped;

    // Off unless MELEE_STEP_BACK: mobs stop short of their target now, and
    // the herd keeps her at her distance (ROADMAP A item 9)
    if (!settings::get<bool>("pawn.MELEE_STEP_BACK"))
    {
        return std::nullopt;
    }

    // The rest clock: the target is settled once it is off its path and on
    // the same spot as last tick; any move restarts the clock
    const bool pathing = PTarget->PAI->PathFind != nullptr && PTarget->PAI->PathFind->IsFollowingPath();
    const bool settled = !pathing && PTarget->id == m_TargetRestId && isWithinDistance(PTarget->loc.p, m_TargetRestPos, 0.1f);
    const auto seconds = [](const char* key)
    {
        return std::chrono::duration_cast<timer::duration>(std::chrono::duration<float>(settings::get<float>(key)));
    };
    if (!positioned || !settled)
    {
        m_TargetRestId    = PTarget->id;
        m_TargetRestPos   = PTarget->loc.p;
        m_TargetRestSince = m_Tick;
        if (PendingIs(Pending::Act::StepBack, PTarget))
        {
            m_Pending.reset(); // the rest restarted
        }
        return std::nullopt;
    }
    // The step back waits the rest delay and her beat; never over the
    // player's order or the hold's close, which are about to act
    if (m_Pending.has_value() && m_Pending->act != Pending::Act::StepBack)
    {
        return std::nullopt;
    }
    if (!PendingIs(Pending::Act::StepBack, PTarget))
    {
        Schedule(Pending::Act::StepBack, PTarget, seconds("pawn.MELEE_BACKOFF_DELAY") + ReactionBeat());
    }
    if (!Due(Pending::Act::StepBack, PTarget) || m_Tick < m_LastStepBackAt + seconds("pawn.MELEE_BACKOFF_COOLDOWN"))
    {
        return std::nullopt;
    }

    const float away = distance(POwner->loc.p, PTarget->loc.p);
    if (away >= FightClearance(PTarget))
    {
        return std::nullopt;
    }

    // Inside its reach, always (FightRadius): a target it cannot reach is
    // one it walks onto again, and that chase is what this exists to avoid
    const float radius = FightRadius(PTarget);
    if (radius <= away)
    {
        return std::nullopt;
    }

    const position_t& me  = POwner->loc.p;
    const position_t& mob = PTarget->loc.p;

    // Standing on the mob there is no bearing to keep: she backs away the
    // way she faces it, turned round
    const float facingRadians = 2.0f * std::numbers::pi_v<float> - rotationToRadian(me.rotation);
    auto [x, z]               = cardian::formation::backOff(mob.x, mob.z, me.x, me.z, radius, facingRadians + std::numbers::pi_v<float>);

    // A spot is a spot when it is on the mesh and clear of every circle.
    // A wall or a circle at her back: round the mob a little, either side
    const auto usable = [&](const float px, const float pz)
    {
        return POwner->PAI->PathFind->ValidPosition(position_t(px, me.y, pz, 0, 0)) && IsClear(px, pz);
    };
    if (!usable(x, z))
    {
        const float base  = std::atan2(z - mob.z, x - mob.x);
        bool        found = false;
        for (const float turn : { 0.8f, -0.8f, 1.6f, -1.6f })
        {
            const float cx = mob.x + std::cos(base + turn) * radius;
            const float cz = mob.z + std::sin(base + turn) * radius;
            if (usable(cx, cz))
            {
                x     = cx;
                z     = cz;
                found = true;
                break;
            }
        }
        if (!found)
        {
            m_LastStepBackAt = m_Tick;
            ShowInfoFmt("pawn: {} has nowhere to step back from {} ({:.1f}y; off the mesh or inside a circle all round)", POwner->getName(), PTarget->getName(), away);
            return std::nullopt;
        }
    }

    // Step straight to the point; the walker's lock-on turns her face
    // back to the mob.
    m_LastStepBackAt = m_Tick;
    m_Pending.reset();
    Intent intent;
    intent.kind  = Intent::Kind::Hop;
    intent.point = position_t(x, me.y, z, 0, me.rotation);
    ShowInfoFmt("pawn: {} steps back from {} ({:.1f}y -> {:.1f}y)", POwner->getName(), PTarget->getName(), away, distance(intent.point, mob));
    return intent;
}

auto CPawnController::PathToward(const position_t& point, const float closeTo, const position_t* rearBoundary) -> bool
{
    auto* PPathFind = POwner->PAI->PathFind.get();

    if (rearBoundary != nullptr)
    {
        // Use the validated waypoints themselves: PathAround could choose a
        // different route through the forward half. Courtesy can shorten the
        // step, but cannot turn an AoE reposition into a frontline crossing.
        auto route = RearCampRoute(CourtesyStep(point), *rearBoundary);
        if (!route.has_value())
        {
            route = RearCampRoute(point, *rearBoundary);
        }
        if (!route.has_value() || route->empty())
        {
            PPathFind->Clear();
            return false;
        }
        return PPathFind->PathThrough(std::move(*route), PATHFLAG_RUN);
    }

    // Allow short navmesh requests for precise Cardian positioning.
    constexpr uint8 pathFlags = PATHFLAG_RUN | PATHFLAG_CARDIAN;
    const auto request = [&](const position_t& goal)
    {
        // Let the caller decide where to stop when no arrival margin is requested.
        return closeTo == 0.0f ? PPathFind->PathTo(goal, pathFlags) : PPathFind->PathAround(goal, closeTo, pathFlags);
    };
    // Adjust the route to respect the player's personal space.
    if (request(CourtesyStep(point)))
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
        if (request(*snapped))
        {
            return true;
        }
    }

    // We may be off-mesh ourselves (knockback, legacy stepping)
    navMesh->snapToValidPosition(POwner->loc.p);
    return request(point);
}
