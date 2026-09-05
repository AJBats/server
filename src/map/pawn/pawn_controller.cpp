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
#include "cardian_link.h"
#include "formation_math.h"
#include "pawn.h"
#include "pawn_danger.h"
#include "pawn_doors.h"
#include "pawn_gambits.h"
#include "pawn_items.h"

#include "common/settings.h"
#include "common/utils.h"
#include "common/xirand.h"

#include <magic_enum/magic_enum.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <numbers>
#include <tuple>
#include <vector>

#include "ai/ai_container.h"
#include "ai/helpers/pathfind.h"
#include "ai/states/magic_state.h"
#include "ai/states/weaponskill_state.h"
#include "ai/states/ability_state.h"
#include "ai/states/range_state.h"
#include "enmity_container.h"
#include "entities/char_entity.h"
#include "status_effect_container.h"
#include "entities/mob_entity.h"
#include "items/item_weapon.h"
#include "navmesh/navmesh.h"
#include "party.h"
#include "recast_container.h"
#include "packets/s2c/0x05a_motionmes.h"
#include "spell.h"
#include "utils/charutils.h"
#include "utils/zoneutils.h"
#include "zone.h"

CPawnController::CPawnController(CCharEntity* PPawn)
: CPlayerController(PPawn)
, m_Gambits(std::make_unique<pawn::CGambits>(PPawn, this))
{
}

void CPawnController::ClearGambitBehaviors()
{
    m_Behaviors.fill(std::nullopt);
}

void CPawnController::SetGambitBehavior(const uint16 behavior, const uint16 arg)
{
    if (behavior < pawn::BehaviorCount && !m_Behaviors[behavior].has_value())
    {
        m_Behaviors[behavior] = arg;
    }
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

void CPawnController::SetWaiting(const bool on, const bool ordered)
{
    m_Waiting     = on;
    m_WaitOrdered = on && ordered;
    if (on)
    {
        m_HuntApproach.reset();
        m_HoldForPlayer = false;
        if (POwner->PAI->PathFind)
        {
            POwner->PAI->PathFind->Clear();
        }
    }
}

auto CPawnController::IsWaiting() const -> bool
{
    return m_Waiting;
}

void CPawnController::Carried(const bool withPlayer)
{
    // The player's magic, seen a moment ago, was this same carry: it must
    // not read as them leaving her behind once she lands
    m_PlayerMagicSeen = timer::time_point::min();
    if (withPlayer)
    {
        if (m_Waiting && !m_WaitOrdered)
        {
            SetWaiting(false, false);
        }
    }
    else
    {
        SetWaiting(true, false);
        ShowInfoFmt("pawn: {} will wait where she lands", POwner->getName());
    }
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

void CPawnController::WaitTick(CCharEntity* PPlayer)
{
    m_Gambits->TickBehaviors();

    // Her ground is hers to hold: a mob that has come for her is answered
    if (auto* PMob = SelfDefenceTarget(); PMob != nullptr)
    {
        ShowInfoFmt("pawn: {} answers {} (waiting)", POwner->getName(), PMob->getName());
        POwner->StatusEffectContainer->DelStatusEffectSilent(xi::StatusEffect::Healing);
        POwner->PAI->Internal_Engage(EntityId(PMob));
        return;
    }

    if (PPlayer != nullptr)
    {
        ShareSignet(PPlayer);
        TidyBag();
        if (!Acting())
        {
            HeadLook(distance(POwner->loc.p, PPlayer->loc.p) < 40.0f ? PPlayer : nullptr);
        }
    }

    if (!POwner->PAI->IsCurrentState<CMagicState>())
    {
        m_Gambits->Tick(m_Tick, false);
        IdleEmote(PPlayer);
    }
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
        ShowInfoFmt("pawn: {} retreat {}", POwner->getName(), on ? "on" : "off");
        if (on && POwner->PAI->IsEngaged())
        {
            POwner->PAI->Internal_Disengage();
        }
    }
    m_Retreat = on;
}

auto CPawnController::IsRetreating() const -> bool
{
    return m_Retreat;
}

void CPawnController::EngageOn(CMobEntity* PMob)
{
    if (PMob == nullptr || PMob->isDead())
    {
        return;
    }
    // The beat: the order is taken now, her draw comes a beat later -- the
    // front row first, so the party never draws on one tick
    m_Order   = EntityId(PMob);
    m_OrderAt = m_Tick + ReactionBeat();
}

void CPawnController::FireOrderedEngage()
{
    if (!m_Order.has_value() || m_Tick < m_OrderAt)
    {
        return;
    }
    auto* PMob = m_Order->resolve<CMobEntity>();
    m_Order.reset();
    if (PMob == nullptr || PMob->isDead())
    {
        return;
    }
    // The order replaces whatever she had set off after, as the party's
    // fight does
    m_HoldForPlayer = false;
    m_HuntApproach.reset();
    POwner->StatusEffectContainer->DelStatusEffectSilent(xi::StatusEffect::Healing);
    if (POwner->PAI->IsEngaged())
    {
        POwner->PAI->Internal_ChangeTarget(EntityId(PMob));
    }
    else
    {
        POwner->PAI->Internal_Engage(EntityId(PMob));
    }
    ShowInfoFmt("pawn: {} draws on {} (ordered)", POwner->getName(), PMob->getName());
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
           POwner->PAI->IsCurrentState<CAbilityState>() || POwner->PAI->IsCurrentState<CRangeState>();
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

auto CPawnController::DoAction(const std::string& key, CBattleEntity* PTarget) -> std::string
{
    unsigned kind = 0;
    unsigned mode = 0;
    unsigned id   = 0;
    if (std::sscanf(key.c_str(), "%u:%u:%u", &kind, &mode, &id) != 3)
    {
        return "bad action";
    }
    if (PTarget == nullptr)
    {
        return "no target";
    }
    if (POwner->isDead())
    {
        return "KO'd";
    }

    const EntityId target(PTarget);
    const auto     err = Acting() ? std::string("busy") : TryAction(kind, mode, id, target);
    if (err == "busy" || err == "recast")
    {
        m_QueuedOrder         = std::make_pair(key, target);
        m_QueuedOrderDeadline = m_Tick + 30s;
        ShowInfoFmt("pawn: {} queues {} on {} ({})", POwner->getName(), key, PTarget->getName(), err);
        return "";
    }
    return err;
}

auto CPawnController::TryAction(const unsigned kind, const unsigned mode, const unsigned id, const EntityId target) -> std::string
{
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
                return "pick a spell";
            }
            const auto spellId = static_cast<SpellID>(id);
            CSpell*    PSpell  = spell::GetSpell(spellId);
            if (PSpell == nullptr)
            {
                return "no such spell";
            }
            if (static_cast<CCharEntity*>(POwner)->PRecastContainer->HasRecast(RECAST_MAGIC, static_cast<Recast>(spellId), 0s))
            {
                return "recast";
            }
            // An order is never second-guessed: straight to the base
            // controller's cast, past the gambit engine's redundancy rule
            // (which declines a cure on a healthy friend) and past the
            // player controller's 2.5 s post-spell delay. The magic state
            // is the only gate, as it is for a player.
            const EntityId castTarget = PSpell->getValidTarget() == TARGET_SELF ? EntityId(POwner) : target;
            FaceTarget(castTarget);
            HeadLook(castTarget.resolve<CBattleEntity>());
            fired = CController::Cast(castTarget, spellId);
            break;
        }
        case 3:
            if (mode != 2)
            {
                return "pick an ability";
            }
            fired = Ability(target, static_cast<uint16>(id));
            break;
        case 4:
            if (mode != 2)
            {
                return "pick a weapon skill";
            }
            fired = WeaponSkill(target, static_cast<uint16>(id));
            break;
        default:
            return "bad action";
    }
    return fired ? "" : "cannot do that now";
}

void CPawnController::FireQueuedOrder()
{
    if (!m_QueuedOrder.has_value() || Acting())
    {
        return;
    }
    const auto [key, target] = *m_QueuedOrder;
    if (m_Tick > m_QueuedOrderDeadline)
    {
        ShowInfoFmt("pawn: {} lets the queued {} go (30 s without a chance)", POwner->getName(), key);
        m_QueuedOrder.reset();
        return;
    }

    auto* PTarget = target.resolve<CBattleEntity>();
    if (PTarget == nullptr || PTarget->isDead())
    {
        m_QueuedOrder.reset();
        return;
    }

    unsigned kind = 0;
    unsigned mode = 0;
    unsigned id   = 0;
    if (std::sscanf(key.c_str(), "%u:%u:%u", &kind, &mode, &id) != 3)
    {
        m_QueuedOrder.reset();
        return;
    }

    const auto err = TryAction(kind, mode, id, target);
    if (err == "recast")
    {
        return; // the timer has not run out: next tick
    }
    m_QueuedOrder.reset();
    ShowInfoFmt("pawn: {} fires the queued {} on {}{}", POwner->getName(), key, PTarget->getName(), err.empty() ? "" : " -- " + err);
}

auto CPawnController::CanDrawOn(CBattleEntity* PTarget) -> bool
{
    // Never left a fight: nothing to wait out
    if (m_LeftFightAt == timer::time_point::min())
    {
        return true;
    }

    const bool same = PTarget != nullptr && PTarget->id == m_LastFoughtId;
    const auto wait = same
                          ? std::chrono::milliseconds(static_cast<CCharEntity*>(POwner)->GetWeaponDelay(false))
                          : std::chrono::milliseconds(static_cast<int64>(settings::get<float>("cardian.REENGAGE_SWITCH_DELAY") * 1000.0f));
    return m_LeftFightAt + wait < m_Tick;
}

void CPawnController::IdleEmote(const CCharEntity* PPlayer)
{
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

auto CPawnController::RestsWithPlayer() const -> bool
{
    return Behavior(pawn::Behavior::RestWithPlayer).value_or(0) != 0;
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

    // A zone change meant for the client protocol -- a warp, a teleport --
    // is hers to carry by transfer, ahead of the zone's own check
    if (pawn::carryZoning(static_cast<CCharEntity*>(POwner)))
    {
        co_return;
    }

    const bool engaged = POwner->PAI->IsEngaged();

    // Leaving a fight starts the draw cooldown; who it was with decides
    // how long (CanDrawOn)
    if (m_WasEngaged && !engaged)
    {
        m_LeftFightAt = tick;
        m_FightSeat   = {};
        m_SeatVia     = false;
        m_CloseAt     = timer::time_point::min();
    }
    m_WasEngaged = engaged;

    if (POwner->isDead())
    {
        WatchPlayerHomePoint();
    }
    else
    {
        m_PlayerSeenDead = false;
        CheckBrain();
        FireQueuedOrder();
        FireOrderedEngage();

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
    // The default rows once; a job change keeps the player's edits
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

    CCharEntity* PPlayer = GetLivePlayer();

    // The player is the party's anchor: gone from the zone means stand down.
    // Their weapon going down does not call the party off a fight that has
    // started -- it runs until the mob dies or drifts past the leash -- but
    // it does end a hold (below), which the party only drew for.
    if (PPlayer == nullptr && !m_Waiting)
    {
        ShowInfoFmt("pawn: {} disengaging (player gone)", POwner->getName());
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
        POwner->PAI->Internal_Disengage();
        co_return;
    }

    // A target gone underground with no fight on is let go, not waited on
    if (auto* PMobTarget = dynamic_cast<CMobEntity*>(PTarget); PMobTarget != nullptr && pawn::isUnderground(PMobTarget) && !PMobTarget->PAI->IsEngaged())
    {
        ShowInfoFmt("pawn: {} lets {} go (underground)", POwner->getName(), PTarget->getName());
        POwner->PAI->Internal_Disengage();
        co_return;
    }

    // Whoever she is fighting is the mob the draw cooldown will measure
    // against once this fight ends
    m_LastFoughtId = PTarget->id;

    // The hold ends the moment the player has struck or the mob has come,
    // and says which: without it, a cardian closing on her own is a
    // mystery in the log
    if (m_HoldForPlayer && PPlayer != nullptr)
    {
        const bool struck = playerHasEnmity(PPlayer, PTarget);
        const bool came   = PTarget->PAI->IsEngaged();
        if (struck || came)
        {
            // The beat: the hold's end is seen now, the close comes a beat
            // later -- the lead first
            if (m_CloseAt == timer::time_point::min())
            {
                m_CloseAt = m_Tick + ReactionBeat();
            }
        }
        if (m_CloseAt != timer::time_point::min() && m_Tick >= m_CloseAt)
        {
            m_HoldForPlayer = false;
            m_CloseAt       = timer::time_point::min();
            ShowInfoFmt("pawn: {} closes on {} ({})", POwner->getName(), PTarget->getName(),
                        struck ? fmt::format("{} has its attention", PPlayer->getName())
                               : fmt::format("{} is fighting {}", PTarget->getName(),
                                             PTarget->GetBattleTarget() != nullptr ? PTarget->GetBattleTarget()->getName() : "someone"));
        }
    }

    // Still holding and the player has put their weapon away: they thought
    // better of it before a blow was struck, and the party drew on that
    // word alone -- so it stands down with them
    if (m_HoldForPlayer && PPlayer != nullptr && !PPlayer->PAI->IsEngaged())
    {
        ShowInfoFmt("pawn: {} stands down ({} thought better of {})", POwner->getName(), PPlayer->getName(), PTarget->getName());
        m_HoldForPlayer = false;
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

    auto avoidAction = AvoidAction::None;
    if (POwner->PAI->CanFollowPath() && POwner->GetSpeed() > 0)
    {
        if (!facing(POwner->loc.p, PTarget->loc.p, 64))
        {
            POwner->PAI->PathFind->LookAt(PTarget->loc.p);
        }

        if (m_HoldForPlayer && PPlayer != nullptr)
        {
            // Walking in with the player, in formation, never within reach
            // of the mob: the strike is the player's, and the pounce after
            // it is a few yalms from a slot
            avoidAction = FollowFormation(PPlayer, PTarget).value_or(AvoidAction::None);
        }
        else
        {
            // The same avoidance pass as roaming, with the target as the
            // point: inside a circle she steps out mid-fight, and a target
            // parked inside another mob's circle is not approached -- she
            // waits at the rim for the tank to bring it
            m_HasSlot = false;
            // Her place on the mob: a seat on the fight ring, or, as its
            // target, wherever she stands -- the front
            const auto seat            = TakeFightSeat(PTarget);
            position_t point           = seat.has_value() ? SeatPoint(PTarget, *seat) : PTarget->loc.p;
            float      followMax       = RoamDistance;
            float      followTarget    = RoamDistance;
            float      declumpDistance = 0.0f;
            if (IsAvoidingAggro())
            {
                avoidAction = Avoid(point, followMax, followTarget, declumpDistance, PTarget, true);
            }

            // Holding at the rim only makes sense for a target that is
            // coming (fighting someone). An idle target parked in another
            // mob's circle is fetched when the party allows aggressive
            // company, and let go when it does not -- never waited on
            if (avoidAction != AvoidAction::None && !PTarget->PAI->IsEngaged())
            {
                if (pawn::huntRulesOf(pawn::summonerOf(POwner->id)).aggressive)
                {
                    avoidAction = AvoidAction::None;
                }
                else
                {
                    ShowInfoFmt("pawn: {} lets {} go (idle inside another mob's circle)", POwner->getName(), PTarget->getName());
                    POwner->PAI->Internal_Disengage();
                    co_return;
                }
            }

            if (avoidAction != AvoidAction::None)
            {
                if (IsShortHop(point, followMax))
                {
                    POwner->PAI->PathFind->Clear();
                    POwner->PAI->PathFind->StepTo(point);
                }
                else if (const float away = distance(POwner->loc.p, point); away > followMax)
                {
                    if (!PathToward(point, followTarget))
                    {
                        NotePathFailure(avoidAction, point, away);
                    }
                }
                else if (POwner->PAI->PathFind->IsFollowingPath())
                {
                    POwner->PAI->PathFind->Clear();
                }
            }
            else
            {
                // Melee archetype: to her seat on the ring, or, at the
                // front, into reach. In reach, a path still under her feet
                // is dropped: a path ends against the mob's position when
                // it was planned, and a mob that has moved since would be
                // walked past
                std::unique_ptr<CBasicPacket> err;
                const bool                    inReach = POwner->CanAttack(PTarget, err);
                if (seat.has_value())
                {
                    WalkToSeat(PTarget, point, inReach);
                }
                else if (inReach)
                {
                    if (POwner->PAI->PathFind->IsFollowingPath())
                    {
                        POwner->PAI->PathFind->Clear();
                    }
                }
                else if (distance(POwner->loc.p, PTarget->loc.p) > RoamDistance)
                {
                    PathToward(PTarget->loc.p, RoamDistance);
                }

                // The seat is her declump; the front has only the sidestep
                if (!POwner->PAI->PathFind->IsFollowingPath() && !StepBack(PTarget) && !seat.has_value())
                {
                    Declump(PTarget);
                }
            }
        }

        POwner->PAI->PathFind->FollowPath(m_Tick);
    }

    // Never a cast in a tick spent stepping to safety: it would root her
    // inside the circle. Holding, only what she would do between fights
    // -- cures and buffs -- since a nuke is a first hit too
    if (avoidAction != AvoidAction::Escape && avoidAction != AvoidAction::Detour)
    {
        m_Gambits->Tick(tick, !m_HoldForPlayer);
    }

    co_return;
}

auto CPawnController::DoRoamTick(const timer::time_point tick) -> Task<void>
{
    TracyZoneScoped;

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
        // Gone by magic a moment ago -- a warp, a teleport -- she waits
        // where she stands and says so; gone on foot, she follows through
        // the zone line as ever
        if (!m_Waiting && m_PlayerMagicSeen != timer::time_point::min() && m_Tick - m_PlayerMagicSeen < 5s)
        {
            m_PlayerMagicSeen = timer::time_point::min();
            SetWaiting(true, false);
            ShowInfoFmt("pawn: {} waits in {} (the player warped away)", POwner->getName(), POwner->loc.zone != nullptr ? POwner->loc.zone->getName() : "?");
        }
        if (m_Waiting)
        {
            WaitTick(nullptr);
            co_return;
        }
        TravelTick();
        co_return;
    }

    NotePlayerMagic(PPlayer);

    // An automatic wait ends with the player back in her zone; an ordered
    // one holds until told otherwise
    if (m_Waiting && !m_WaitOrdered)
    {
        SetWaiting(false, false);
        ShowInfoFmt("pawn: {} follows again ({} is back)", POwner->getName(), PPlayer->getName());
    }
    if (m_Waiting)
    {
        WaitTick(PPlayer);
        co_return;
    }

    ShareSignet(PPlayer);
    TidyBag();
    // Walking in on a hunt, her eye is on the mob (below), not the player
    if (!Acting() && !m_HuntApproach.has_value())
    {
        HeadLook(distance(POwner->loc.p, PPlayer->loc.p) < 40.0f ? PPlayer : nullptr);
    }
    m_Gambits->TickBehaviors();

    if (auto* PTarget = PartyEngageTarget(PPlayer); PTarget != nullptr)
    {
        // The beat: the party's fight is seen now, her draw comes a beat
        // later, her eyes on it in the meantime
        if (m_EngageBeatMob != PTarget->id)
        {
            m_EngageBeatMob = PTarget->id;
            m_EngageAt      = m_Tick + ReactionBeat();
        }
        if (m_Tick < m_EngageAt)
        {
            HeadLook(PTarget);
            co_return;
        }
        m_EngageBeatMob = 0;

        // Drawn on the player's word alone: hold until they strike, or the
        // mob comes to us. A pull, an answer to aggro, or an order closes.
        m_HoldForPlayer = PPlayer->PAI->IsEngaged() && PTarget == PPlayer->GetBattleTarget() &&
                          !playerHasEnmity(PPlayer, PTarget) && !PTarget->PAI->IsEngaged();
        if (m_HoldForPlayer)
        {
            ShowInfoFmt("pawn: {} draws on {} (holding for {}'s strike)", POwner->getName(), PTarget->getName(), PPlayer->getName());
        }
        // The party's fight replaces whatever she had set off after: she
        // chooses again when this one is over, rather than walking back to
        // a mob picked before it started
        m_HuntApproach.reset();
        POwner->StatusEffectContainer->DelStatusEffectSilent(xi::StatusEffect::Healing);
        POwner->PAI->Internal_Engage(EntityId(PTarget));
        co_return;
    }
    m_EngageBeatMob = 0;

    // The player has drawn on a burrowed mob: the party waits for it to
    // surface, and says so now and then
    if (PPlayer->PAI->IsEngaged())
    {
        if (auto* PMob = dynamic_cast<CMobEntity*>(PPlayer->GetBattleTarget());
            PMob != nullptr && pawn::isUnderground(PMob) && !PMob->PAI->IsEngaged() && m_Tick - m_LastSurfaceLogTime > 5s)
        {
            m_LastSurfaceLogTime = m_Tick;
            ShowInfoFmt("pawn: {} waits for {} to surface", POwner->getName(), PMob->getName());
        }
    }

    // A hunter walking to the mob she chose: she committed the moment it
    // was picked and closes with her weapon away, drawing only once the
    // re-engage timer allows -- so the party moves on at once after a
    // kill and the pause is at the draw, not before the choice. Every
    // reason to let the mob go is judged here, ahead of the party's rest
    if (m_HuntApproach.has_value())
    {
        auto* PMob = m_HuntApproach->resolve<CMobEntity>();
        if (PMob == nullptr || PMob->isDead() || PMob->PAI->IsEngaged() || !IsHunting() ||
            distance(PPlayer->loc.p, PMob->loc.p) > settings::get<float>("pawn.HUNT_RADIUS"))
        {
            m_HuntApproach.reset();
        }
        // The party is no longer paced for a pull -- the player sat, a
        // member fell -- so the choice goes too, to be made afresh
        else if (const auto blocker = PacingBlocker(PPlayer); !blocker.empty())
        {
            ShowInfoFmt("pawn: {} lets {} go ({})", POwner->getName(), PMob->getName(), blocker);
            m_HuntApproach.reset();
        }
        else
        {
            // her eye is on it the whole way in, and while she stands there
            // waiting to draw
            HeadLook(PMob);
            if (m_Tick < m_SetOffAt)
            {
                co_return;
            }

            // The cooldown gates the DRAW, not the arrival: ready, she
            // draws where she stands and charges in with her weapon out,
            // as she always did; still waiting, she walks in with it away
            // and draws the moment the wait is served, wherever that
            // catches her
            // The beat again at the draw: the wait is served for the whole
            // party at once, and without it they would all draw on one tick
            const bool ready = CanDrawOn(PMob);
            if (ready && m_DrawAt == timer::time_point::min())
            {
                m_DrawAt = m_Tick + m_HuntBeat;
            }
            if (ready && m_Tick >= m_DrawAt)
            {
                ShowInfoFmt("pawn: {} draws on {} ({})", POwner->getName(), PMob->getName(),
                            magic_enum::enum_name(charutils::CheckMob(PPlayer->GetMLevel(), PMob)));
                m_HuntApproach.reset();
                m_HoldForPlayer = false;
                POwner->PAI->Internal_Engage(EntityId(PMob));
            }
            else if (distance(POwner->loc.p, PMob->loc.p) > RoamDistance)
            {
                if (PathToward(PMob->loc.p, RoamDistance))
                {
                    POwner->PAI->PathFind->FollowPath(m_Tick);
                }
            }
            co_return;
        }
    }

    // Rest with the player: the Healing status is the real thing -- kneel
    // animation and resting regen ticks -- so the party sits down together
    const bool playerResting = RestsWithPlayer() && PPlayer->animation == xi::Animation::Healing;
    const bool resting       = POwner->StatusEffectContainer->HasStatusEffect(xi::StatusEffect::Healing);
    if (playerResting && !resting && distance(POwner->loc.p, PPlayer->loc.p) < 10.0f && !POwner->PAI->PathFind->IsFollowingPath())
    {
        const auto healingTickDelay = std::chrono::seconds(settings::get<uint8>("map.HEALING_TICK_DELAY"));
        POwner->StatusEffectContainer->AddStatusEffect(xi::StatusEffect::Healing, 0, 0, healingTickDelay, 0s);
        co_return;
    }
    if (!playerResting && resting)
    {
        POwner->StatusEffectContainer->DelStatusEffectSilent(xi::StatusEffect::Healing);
    }
    if (resting && playerResting)
    {
        co_return;
    }

    // A hunter picks the party's next fight itself, the moment it is free
    // to: the choice is never throttled, only the draw
    if (IsHunting() && !m_HuntApproach.has_value())
    {
        const auto blocker = HuntBlocker(PPlayer);
        if (blocker.empty())
        {
            if (auto* PMob = PickHuntTarget(PPlayer); PMob != nullptr)
            {
                // The beat: the choice is made now, the walk starts a beat
                // later, her eyes on it in the meantime
                const auto beat = ReactionBeat();
                m_HuntBeat      = beat;
                m_SetOffAt      = m_Tick + beat;
                m_DrawAt        = timer::time_point::min();
                ShowInfoFmt("pawn: {} sets off after {} ({}{})", POwner->getName(), PMob->getName(),
                            magic_enum::enum_name(charutils::CheckMob(PPlayer->GetMLevel(), PMob)),
                            beat > 0s ? fmt::format(", in {:.1f}s", std::chrono::duration<float>(beat).count()) : "");
                m_HuntApproach = EntityId(PMob);
                co_return;
            }
            // A quiet hunt says why, now and then: the band is judged
            // against the player's level, and a low zone has nothing in it
            if (m_Tick - m_LastHuntLogTime > 15s)
            {
                m_LastHuntLogTime = m_Tick;
                const auto rules = pawn::huntRulesOf(pawn::summonerOf(POwner->id));
                ShowInfoFmt("pawn: {} finds nothing to hunt within {} y of {} (level {}; band {}..{}, idle and unclaimed{}{})",
                            POwner->getName(), settings::get<float>("pawn.HUNT_RADIUS"), PPlayer->getName(), PPlayer->GetMLevel(),
                            magic_enum::enum_name(static_cast<EMobDifficulty>(rules.minCheck)), magic_enum::enum_name(static_cast<EMobDifficulty>(rules.maxCheck)),
                            rules.aggressive ? "" : ", aggressive company avoided", rules.links ? "" : ", links avoided");
            }
        }
        else if (m_Tick - m_LastHuntLogTime > 15s)
        {
            m_LastHuntLogTime = m_Tick;
            ShowInfoFmt("pawn: {} hunt waits: {}", POwner->getName(), blocker);
        }
    }

    const auto avoidAction = FollowFormation(PPlayer, nullptr);
    if (!avoidAction.has_value())
    {
        co_return;
    }

    if (POwner->PAI->PathFind->IsFollowingPath())
    {
        POwner->PAI->PathFind->FollowPath(m_Tick);
    }
    else if (!POwner->PAI->IsCurrentState<CMagicState>() && *avoidAction != AvoidAction::Escape && *avoidAction != AvoidAction::Detour)
    {
        // Between fights, standing still: cures, raises, buffs -- but never
        // in a tick spent stepping to safety, since a cast would root the
        // pawn inside the circle
        m_Gambits->Tick(tick, false);
        IdleEmote(PPlayer);
    }

    co_return;
}

auto CPawnController::FollowFormation(CCharEntity* PPlayer, const CBattleEntity* PStandOff) -> std::optional<AvoidAction>
{
    // Where this pawn belongs: the lead holds a point ahead of the player,
    // everyone else a seat on the ring around them. FormationPoint sets
    // m_HasSlot for the avoidance pass.
    m_HasSlot = false;
    position_t followPoint{};
    float      declumpDistance = 0.0f;
    float      followMax       = 2.0f;
    float      followTarget    = 1.0f;

    // No point within reach of a mob the party is holding on: pushed out
    // to the ring, and round to the player's side from behind it
    const auto standOff = [&](position_t point) -> position_t
    {
        if (PStandOff == nullptr)
        {
            return point;
        }
        const float radius = POwner->GetMeleeRange(PStandOff) + settings::get<float>("pawn.FORMATION_STANDOFF");
        const auto [x, z]  = cardian::formation::standOff(PStandOff->loc.p.x, PStandOff->loc.p.z, PPlayer->loc.p.x, PPlayer->loc.p.z, radius, point.x, point.z);
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
        followPoint = standOff(LeadPoint(PPlayer));
        RampCatchUp(m_PlayerMoving, followPoint);
    }
    else
    {
        // Everyone else follows the player themself, in a seat on the ring
        // around them: the same fresh position the lead uses, with a gentle
        // prediction, parked and held the way the lead holds its point. (A
        // seat, not the player: a fresh position would put her right on top
        // of them.)
        const auto slot   = RingSlot();
        const auto seat   = SeatOf(slot);
        const auto anchor = PlayerAnchor(PPlayer, settings::get<float>("pawn.FORMATION_FOLLOW_PREDICT_SCALE"));
        followPoint       = standOff(FormationPoint(anchor, seat.offset, seat.angle, m_FollowPoint, m_HasFollowPoint));
        RampCatchUp(anchor.moving, followPoint);
        FormationDebug(cardian::formation::slotName(slot), PPlayer, anchor, followPoint);
    }

    auto avoidAction = AvoidAction::None;
    if (IsAvoidingAggro())
    {
        avoidAction = Avoid(followPoint, followMax, followTarget, declumpDistance, PStandOff, false);
    }

    const float currentDistance = distance(POwner->loc.p, followPoint);

    if (avoidAction != AvoidAction::None && IsShortHop(followPoint, followMax))
    {
        POwner->PAI->PathFind->Clear();
        POwner->PAI->PathFind->StepTo(followPoint);
    }
    else if (currentDistance > followMax)
    {
        // Warp only when pathing genuinely fails; a pawn arriving at a zone
        // gate runs to its player like anyone else would
        if (!PathToward(followPoint, followTarget))
        {
            if (currentDistance > WarpDistance)
            {
                POwner->PAI->PathFind->WarpTo(followPoint);
                return std::nullopt;
            }
            NotePathFailure(avoidAction, followPoint, currentDistance);
        }
    }
    else if (currentDistance < declumpDistance)
    {
        if (!POwner->PAI->PathFind->IsFollowingPath())
        {
            PathToward(followPoint, followTarget + 0.5f);
        }
    }
    else if (POwner->PAI->PathFind->IsFollowingPath())
    {
        POwner->PAI->PathFind->Clear();
    }

    return avoidAction;
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

        // In their Mog House: the party waits at the door
        if (PSummoner->loc.zone == POwner->loc.zone && PSummoner->inMogHouse())
        {
            if (narrate)
            {
                ShowInfoFmt("pawn: travel {}: {} is in their Mog House, waiting", POwner->getName(), PSummoner->getName());
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

    FaceTarget(castTarget);
    HeadLook(castTarget.resolve<CBattleEntity>());
    return CPlayerController::Cast(castTarget, spellid);
}

auto CPawnController::WeaponSkill(const EntityId target, const uint16 wsid) -> bool
{
    FaceTarget(target);
    HeadLook(target.resolve<CBattleEntity>());
    return CPlayerController::WeaponSkill(target, wsid);
}

auto CPawnController::Ability(const EntityId target, const uint16 abilityid) -> bool
{
    FaceTarget(target);
    HeadLook(target.resolve<CBattleEntity>());
    return CPlayerController::Ability(target, abilityid);
}

auto CPawnController::RangedAttack(const EntityId target) -> bool
{
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
    auto* PPawn    = static_cast<CCharEntity*>(POwner);
    bool  redundant = false;

    PPawn->ForParty([&](const CBattleEntity* PMember)
                    {
                        if (redundant || PMember == POwner || !PMember->PAI->IsCurrentState<CMagicState>())
                        {
                            return;
                        }

                        auto*       MState  = static_cast<CMagicState*>(PMember->PAI->GetCurrentState());
                        auto*       MSpell  = MState->GetSpell();
                        const auto* MTarget = MState->target().resolve();
                        if (MSpell == nullptr)
                        {
                            return;
                        }

                        const bool sameFamily = PSpell->getSpellFamily() == MSpell->getSpellFamily();
                        const bool weakerOrSame = PSpell->getID() <= MSpell->getID();

                        if ((PSpell->isBuff() || PSpell->isDebuff()) && sameFamily && weakerOrSame)
                        {
                            redundant = true;
                        }
                        else if (PSpell->isCure() && PTarget != nullptr && PTarget == MTarget && PTarget->GetHPP() > 50)
                        {
                            redundant = true;
                        }
                        else if (PSpell->isNa() && sameFamily && PSpell->getID() == MSpell->getID())
                        {
                            redundant = true;
                        }
                    });

    return redundant;
}

auto CPawnController::PartyEngageTarget(CCharEntity* PPlayer) const -> CBattleEntity*
{
    // Retreat: the party's fight is nobody's, whoever swings or aggroes
    if (m_Retreat)
    {
        return nullptr;
    }

    // The player's engagement comes first: a weapon drawn on a mob commits
    // the party. The cardians draw too, and hold their ground until the
    // player has struck or the mob comes to them (m_HoldForPlayer, set
    // where they engage)
    if (PPlayer != nullptr && PPlayer->PAI->IsEngaged())
    {
        if (auto* PMob = dynamic_cast<CMobEntity*>(PPlayer->GetBattleTarget()); PMob != nullptr && !PMob->isDead())
        {
            // Underground with no fight on, it is not the party's fight yet:
            // the party waits, weapons away, and draws when it surfaces (the
            // combat tick lets such a target go)
            if (pawn::isUnderground(PMob) && !PMob->PAI->IsEngaged())
            {
                return nullptr;
            }
            return PMob;
        }
    }

    // A pawn already fighting pulls the rest of the party in -- how a
    // hunter's pull propagates without the player tagging anything
    const auto* PPawn = static_cast<CCharEntity*>(POwner);
    if (PPawn->PParty == nullptr)
    {
        return nullptr;
    }

    for (auto* PMember : PPawn->PParty->members)
    {
        auto* PChar = dynamic_cast<CCharEntity*>(PMember);
        if (PChar == nullptr || PChar == POwner || !pawn::isPawn(PChar) ||
            PChar->loc.zone != POwner->loc.zone || !PChar->PAI->IsEngaged())
        {
            continue;
        }

        if (auto* PTarget = PChar->GetBattleTarget(); PTarget != nullptr && !PTarget->isDead())
        {
            return PTarget;
        }
    }

    // Self-defence: a mob that has chosen a member of this party is the
    // party's fight, whether or not anyone has swung yet -- aggro on a
    // cardian, or on the player, is answered
    const float    leash    = settings::get<float>("pawn.HUNT_LEASH");
    CBattleEntity* attacker = nullptr;
    const auto     answers  = [&](CMobEntity* PMob)
    {
        if (attacker != nullptr || !PMob->PAI->IsEngaged() || PMob->isDead() ||
            !isWithinDistance(POwner->loc.p, PMob->loc.p, leash))
        {
            return;
        }
        auto* PVictim = PMob->GetBattleTarget();
        if (PVictim != nullptr && PVictim->PParty == PPawn->PParty)
        {
            attacker = PMob;
            ShowInfoFmt("pawn: {} answers {} (on {})", POwner->getName(), PMob->getName(), PVictim->getName());
        }
    };
    pawn::forEachMobNear(pawn::entitiesAround(POwner), POwner->loc.p, leash, answers);
    return attacker;
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
        if (PMember->PAI->IsEngaged())
        {
            return fmt::format("{} engaged", PMember->getName());
        }
    }
    return "";
}

auto CPawnController::PickHuntTarget(const CCharEntity* PPlayer) const -> CMobEntity*
{
    const auto  rules       = pawn::huntRulesOf(pawn::summonerOf(POwner->id));
    const auto  minCheck    = rules.minCheck;
    const auto  maxCheck    = rules.maxCheck;
    const auto  radius      = settings::get<float>("pawn.HUNT_RADIUS");
    const auto  cleanRadius = settings::get<float>("pawn.HUNT_CLEAN_RADIUS");
    auto*       entities    = pawn::entitiesAround(POwner);

    // One danger scan per hunt check, wide enough to cover every candidate's
    // circle and every approach from the hunter. Judged for the whole party
    // that will fight beside the target, not for the hunter's own buffs
    const auto dangers = pawn::danger::around(entities, PPlayer->loc.p, radius + std::max(cleanRadius, distance(POwner->loc.p, PPlayer->loc.p)),
                                              pawn::danger::Profile::worstCase());

    // An idle, unclaimed, ordinary field mob in the band, within the hunt
    // radius of the player
    const auto eligible = [&](CMobEntity* PMob) -> bool
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
        if (distance(PPlayer->loc.p, PMob->loc.p) > radius)
        {
            return false;
        }
        const auto check = static_cast<uint8>(charutils::CheckMob(PPlayer->GetMLevel(), PMob));
        return check >= minCheck && check <= maxCheck;
    };

    // The aggressive mob whose detection circle a candidate stands in
    const auto guardOf = [&](const CMobEntity* PMob) -> const pawn::danger::Danger*
    {
        for (const auto& d : dangers)
        {
            if (d.mob != PMob && isWithinDistance(d.mob->loc.p, PMob->loc.p, d.radius))
            {
                return &d;
            }
        }
        return nullptr;
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

        // Prey standing in an aggressive mob's circle is not the
        // pull: the guard is, when the party allows aggressive
        // company and the guard is itself fair game; otherwise the
        // prey is skipped. The player can still sneak behind the
        // guard and pull it to the party waiting outside its circle
        CMobEntity* pick = PMob;
        if (const auto* guard = guardOf(PMob); guard != nullptr)
        {
            if (!rules.aggressive || !eligible(guard->mob))
            {
                return;
            }
            pick = guard->mob;
        }

        // Aggressive company avoided: no danger circle across the
        // hunter's approach either
        if (!rules.aggressive)
        {
            for (const auto& d : dangers)
            {
                if (d.mob != pick && cardian::formation::segmentCrosses(d, POwner->loc.p.x, POwner->loc.p.z, pick->loc.p.x, pick->loc.p.z))
                {
                    return;
                }
            }
        }
        if (!rules.links && pick->m_Link != 0 && linked(pick))
        {
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
    pawn::forEachMobNear(entities, PPlayer->loc.p, radius, consider);

    return best;
}

auto CPawnController::GetTopEnmity() const -> CBattleEntity*
{
    if (const auto* PMob = dynamic_cast<CMobEntity*>(POwner->GetBattleTarget()))
    {
        return PMob->PEnmityContainer->GetHighestEnmity();
    }
    return nullptr;
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
            PChar != nullptr && PChar->PSession != nullptr && PChar->loc.zone == POwner->loc.zone && !PChar->inMogHouse())
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
    if (!settings::get<bool>("pawn.FORMATION_DEBUG"))
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

auto CPawnController::FormationPoint(const Anchor& a, const float offset, const float angle, position_t& held, bool& hasHeld) -> position_t
{
    const position_t projected = nearPosition(a.anchor, offset, angle);

    // Remembered so Avoid() can re-seat the slot on the same ring
    m_HasSlot    = true;
    m_SlotAnchor = a.anchor;
    m_SlotOffset = offset;
    m_SlotAngle  = angle;

    // A moving player is re-aimed every tick; the deadband only absorbs
    // the coarse position/heading updates of a player standing still
    if (!hasHeld || a.moving || distance(projected, held) > settings::get<float>("pawn.FORMATION_DEADBAND"))
    {
        held    = projected;
        hasHeld = true;
    }
    return held;
}

auto CPawnController::IsShortHop(const position_t& point, const float followMax) const -> bool
{
    constexpr float plannerMinimumHop = 1.2f;
    const float     hop               = distance(POwner->loc.p, point);
    return hop > followMax && hop < plannerMinimumHop && POwner->PAI->PathFind->ValidPosition(point);
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

auto CPawnController::Avoid(position_t& point, float& followMax, float& followTarget, float& declumpDistance, const CBattleEntity* PIgnore, const bool fighting) -> AvoidAction
{
    using namespace cardian::formation;

    const auto* PPawn   = static_cast<const CCharEntity*>(POwner);
    // The party's own mob is never a danger to keep out of: a freshly
    // pulled aggressive mob is not fighting anyone yet, and its circle
    // would hold her at the rim of the very mob she is meant to hit, or
    // walk up to
    const auto  dangers = pawn::danger::around(pawn::entitiesAround(POwner), POwner->loc.p, settings::get<float>("pawn.AVOID_SCAN"), pawn::danger::Profile::of(PPawn), PIgnore);

    // The margins that keep the boundary from being slippery: every point
    // she walks to is planned against the circles padded by kClearance, so
    // a 400 ms step (about two yalms) and the circle's breathing as she
    // walks over bumps (the height slice) cannot land her inside. Only the
    // escape test uses the true circle, and it pushes her a little further
    // out than the padding. A hold stands anywhere within kHoldBand of the
    // padded ring.
    constexpr float kClearance   = 1.5f;
    constexpr float kEscapeExtra = 0.5f;
    constexpr float kHoldBand    = 1.0f;
    constexpr float kDetourArc   = 3.0f;

    AvoidAction action  = AvoidAction::None;
    bool        perched = false; // this tick used or took a perch
    if (!dangers.empty())
    {
        const auto&         circles = dangers; // the true circles: is she inside one
        std::vector<Circle> padded;            // the planning circles: every point she walks to
        padded.reserve(dangers.size());
        for (const auto& d : dangers)
        {
            padded.push_back(Circle{ d.x, d.z, d.radius + kClearance });
        }
        const position_t me = POwner->loc.p;

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

        if (insideAny(circles, me.x, me.z))
        {
            // Pushed away: the minimum proximity is never violated, whatever
            // the formation wanted. Straight out is the first choice; with a
            // wall at her back, other directions around the deepest circle
            // are tried for an on-mesh, clear point.
            auto [x, z] = pushOut(padded, me.x, me.z, kEscapeExtra);
            if (!POwner->PAI->PathFind->ValidPosition(position_t(x, me.y, z, 0, 0)))
            {
                const auto deepest = std::max_element(padded.begin(), padded.end(), [&](const auto& a, const auto& b)
                                                      {
                                                          return depthInside(a, me.x, me.z) < depthInside(b, me.x, me.z);
                                                      });
                const float base = std::atan2(me.z - deepest->z, me.x - deepest->x);
                for (const float turn : { 0.5f, -0.5f, 1.0f, -1.0f, 1.6f, -1.6f, 2.2f, -2.2f, 3.1f })
                {
                    const float cx = deepest->x + std::cos(base + turn) * (deepest->radius + kEscapeExtra);
                    const float cz = deepest->z + std::sin(base + turn) * (deepest->radius + kEscapeExtra);
                    if (!insideAny(padded, cx, cz) && POwner->PAI->PathFind->ValidPosition(position_t(cx, me.y, cz, 0, 0)))
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
                    // spot on offer
                    const position_t ideal  = point;
                    bool             seated = false;
                    if (m_HasSlot)
                    {
                        const auto onRing = [&](const float angle)
                        {
                            const position_t p = nearPosition(m_SlotAnchor, m_SlotOffset, angle);
                            return std::pair{ p.x, p.z };
                        };
                        if (const auto angle = safestAngleOnRing(padded, m_SlotAngle, onRing); angle.has_value())
                        {
                            point  = nearPosition(m_SlotAnchor, m_SlotOffset, *angle);
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

auto CPawnController::LeadPoint(const CCharEntity* PPlayer) -> position_t
{
    const auto a          = PlayerAnchor(PPlayer, 1.0f);
    m_PlayerMoving        = a.moving;
    m_LastPredictionAhead = a.ahead;

    float lead = settings::get<float>("pawn.FORMATION_LEAD_DISTANCE");
    if (a.moving)
    {
        lead += settings::get<float>("pawn.FORMATION_LEAD_MOVING_BONUS");
    }

    const auto point = FormationPoint(a, lead, 0.0f, m_LeadPoint, m_HasLeadPoint);
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

auto CPawnController::FightSeatOn(const uint32 mobId) const -> std::optional<pawn::Slot>
{
    if (mobId != 0 && m_FightSeat.mob == mobId)
    {
        return m_FightSeat.seat;
    }
    return std::nullopt;
}

auto CPawnController::FightRadius(const CBattleEntity* PTarget) const -> float
{
    // Inside its reach, always: a target it cannot reach is one it walks
    // onto again
    return std::min(RoamDistance, POwner->GetMeleeRange(PTarget) - settings::get<float>("pawn.MELEE_BACKOFF_MARGIN"));
}

auto CPawnController::SeatPoint(const CBattleEntity* PTarget, const pawn::Slot seat) const -> position_t
{
    constexpr float kPi = std::numbers::pi_v<float>;

    // The ring's frame: the mob facing its target; a mob with none faces
    // where it faces
    position_t frame = PTarget->loc.p;
    if (const auto* PFront = PTarget->GetBattleTarget(); PFront != nullptr)
    {
        frame.rotation = worldAngle(PTarget->loc.p, PFront->loc.p);
    }
    const float bearing = cardian::formation::seatBearing(seat, settings::get<float>("pawn.FIGHT_FLANK_DEG") * kPi / 180.0f,
                                                          settings::get<float>("pawn.FIGHT_REAR_DEG") * kPi / 180.0f);
    return nearPosition(frame, FightRadius(PTarget), bearing);
}

auto CPawnController::TakeFightSeat(const CBattleEntity* PTarget) -> std::optional<pawn::Slot>
{
    using cardian::formation::RingSeats;
    using cardian::formation::seatName;

    // The ring forms around a fight: a mob with no target yet (a pull on
    // the way in) is approached, not seated. Its target is the front, and
    // the front is wherever she stands
    const auto* PFront = PTarget->GetBattleTarget();
    if (PFront == nullptr || PFront == POwner)
    {
        if (m_FightSeat.mob == PTarget->id)
        {
            ShowInfoFmt("pawn: {} leaves {}'s {} ({})", POwner->getName(), PTarget->getName(), seatName(m_FightSeat.seat),
                        PFront == POwner ? "it turned on her" : "it has no one to face");
            m_FightSeat = {};
            m_SeatVia   = false;
        }
        return std::nullopt;
    }

    // Hers for the fight
    if (m_FightSeat.mob == PTarget->id)
    {
        return m_FightSeat.seat;
    }

    // The seats the party's other cardians hold on this mob
    cardian::formation::SeatsTaken taken{};
    if (const auto* PPawn = static_cast<const CCharEntity*>(POwner); PPawn->PParty != nullptr)
    {
        for (const auto* PMember : PPawn->PParty->members)
        {
            const auto* PChar = dynamic_cast<const CCharEntity*>(PMember);
            if (PChar == nullptr || PChar == POwner || !pawn::isPawn(PChar) || PChar->loc.zone != POwner->loc.zone)
            {
                continue;
            }
            const auto* PController = dynamic_cast<const CPawnController*>(PChar->PAI->GetController());
            if (PController == nullptr)
            {
                continue;
            }
            if (const auto held = PController->FightSeatOn(PTarget->id); held.has_value())
            {
                if (const auto it = std::ranges::find(RingSeats, *held); it != RingSeats.end())
                {
                    taken[static_cast<std::size_t>(it - RingSeats.begin())] = true;
                }
            }
        }
    }

    // A seat off the mesh (the mob against a wall) is no seat; none on
    // the mesh and she closes as the front does
    cardian::formation::SeatPoints points{};
    for (std::size_t i = 0; i < RingSeats.size(); ++i)
    {
        const auto p = SeatPoint(PTarget, RingSeats[i]);
        points[i]    = { p.x, p.z };
        if (!POwner->PAI->PathFind->ValidPosition(p))
        {
            taken[i] = true;
        }
    }
    if (std::ranges::all_of(taken, [](const bool t) { return t; }))
    {
        return std::nullopt;
    }
    const auto seat = RingSeats[cardian::formation::nearestSeat(points, taken, POwner->loc.p.x, POwner->loc.p.z)];
    m_FightSeat     = { PTarget->id, seat };
    m_SeatVia       = false;
    ShowInfoFmt("pawn: {} takes {}'s {}", POwner->getName(), PTarget->getName(), seatName(seat));
    return seat;
}

void CPawnController::WalkToSeat(const CBattleEntity* PTarget, const position_t& seat, const bool inReach)
{
    using cardian::formation::Circle;
    using cardian::formation::seatName;
    using cardian::formation::segmentCrosses;

    auto*       PPathFind = POwner->PAI->PathFind.get();
    const auto& me        = POwner->loc.p;
    const float off       = distance(me, seat);

    // On her seat, or near enough while in reach: she stands, and a path
    // still under her feet is dropped (it ends where the seat was)
    const float deadband = inReach ? settings::get<float>("pawn.FIGHT_SEAT_DEADBAND") : 0.5f;
    if (off <= deadband)
    {
        if (PPathFind->IsFollowingPath())
        {
            PPathFind->Clear();
        }
        m_SeatVia = false;
        return;
    }

    // A hop under the planner's floor (IsShortHop): straight at it, on
    // the mesh
    if (off < 1.2f)
    {
        if (PPathFind->ValidPosition(seat))
        {
            PPathFind->Clear();
            PPathFind->StepTo(seat);
        }
        else
        {
            // The seat has left the mesh (the mob against a wall): given
            // up, and another is picked next tick
            m_FightSeat = {};
        }
        return;
    }

    // A path already on the way stands until the seat has drifted a yalm
    // from where it was planned against
    if (PPathFind->IsFollowingPath() && distance(seat, m_SeatDestination) <= 1.0f)
    {
        return;
    }

    // A far seat is reached round the mob's side, never through it: a way
    // there crossing the mob goes by the flank on her side first
    position_t   goal = seat;
    const Circle body{ PTarget->loc.p.x, PTarget->loc.p.z, PTarget->modelHitboxSize + 0.8f };
    if (segmentCrosses(body, me.x, me.z, seat.x, seat.z))
    {
        const auto  right = SeatPoint(PTarget, pawn::Slot::FlankRight);
        const auto  left  = SeatPoint(PTarget, pawn::Slot::FlankLeft);
        const bool  byRight = distance(me, right) <= distance(me, left);
        const auto& via     = byRight ? right : left;
        if (distance(me, via) > 1.0f)
        {
            goal = via;
            if (!m_SeatVia)
            {
                ShowInfoFmt("pawn: {} goes round {} by the {}", POwner->getName(), PTarget->getName(), seatName(byRight ? pawn::Slot::FlankRight : pawn::Slot::FlankLeft));
            }
            m_SeatVia = true;
        }
    }

    m_SeatDestination = seat;
    if (!PathToward(goal, 0.3f))
    {
        m_FightSeat = {};
    }
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

auto CPawnController::StepBack(const CBattleEntity* PTarget) -> bool
{
    TracyZoneScoped;

    // The rest clock: the target is settled once it is off its path and on
    // the same spot as last tick; any move restarts the clock
    const bool pathing = PTarget->PAI->PathFind != nullptr && PTarget->PAI->PathFind->IsFollowingPath();
    const bool settled = !pathing && PTarget->id == m_TargetRestId && isWithinDistance(PTarget->loc.p, m_TargetRestPos, 0.1f);
    if (!settled)
    {
        m_TargetRestId    = PTarget->id;
        m_TargetRestPos   = PTarget->loc.p;
        m_TargetRestSince = m_Tick;
        m_TargetRestBeat.reset();
        return false;
    }
    if (!m_TargetRestBeat.has_value())
    {
        m_TargetRestBeat = ReactionBeat();
    }

    const auto seconds = [](const char* key)
    {
        return std::chrono::duration_cast<timer::duration>(std::chrono::duration<float>(settings::get<float>(key)));
    };
    if (m_Tick < m_TargetRestSince + seconds("pawn.MELEE_BACKOFF_DELAY") + *m_TargetRestBeat || m_Tick < m_LastStepBackAt + seconds("pawn.MELEE_BACKOFF_COOLDOWN"))
    {
        return false;
    }

    const float away = distance(POwner->loc.p, PTarget->loc.p);
    if (away >= settings::get<float>("pawn.MELEE_BACKOFF_TRIGGER"))
    {
        return false;
    }

    // Inside its reach, always (FightRadius): a target it cannot reach is
    // one it walks onto again, and that chase is what this exists to avoid
    const float radius = FightRadius(PTarget);
    if (radius <= away)
    {
        return false;
    }

    const position_t& me  = POwner->loc.p;
    const position_t& mob = PTarget->loc.p;

    // Standing on the mob there is no bearing to keep: she backs away the
    // way she faces it, turned round
    const float facingRadians = 2.0f * std::numbers::pi_v<float> - rotationToRadian(me.rotation);
    auto [x, z]               = cardian::formation::backOff(mob.x, mob.z, me.x, me.z, radius, facingRadians + std::numbers::pi_v<float>);

    // A wall at her back: round the mob a little, either side
    if (!POwner->PAI->PathFind->ValidPosition(position_t(x, me.y, z, 0, 0)))
    {
        const float base  = std::atan2(z - mob.z, x - mob.x);
        bool        found = false;
        for (const float turn : { 0.8f, -0.8f, 1.6f, -1.6f })
        {
            const float cx = mob.x + std::cos(base + turn) * radius;
            const float cz = mob.z + std::sin(base + turn) * radius;
            if (POwner->PAI->PathFind->ValidPosition(position_t(cx, me.y, cz, 0, 0)))
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
            ShowInfoFmt("pawn: {} has nowhere to step back from {} ({:.1f}y, off the mesh all round)", POwner->getName(), PTarget->getName(), away);
            return false;
        }
    }

    // A hop this short is under the planner's floor (IsShortHop): straight
    // at the point, and her face back on the mob -- the step turned her
    POwner->PAI->PathFind->Clear();
    POwner->PAI->PathFind->StepTo(position_t(x, me.y, z, 0, me.rotation));
    POwner->PAI->PathFind->LookAt(mob);
    m_LastStepBackAt = m_Tick;
    ShowInfoFmt("pawn: {} steps back from {} ({:.1f}y -> {:.1f}y)", POwner->getName(), PTarget->getName(), away, distance(POwner->loc.p, mob));
    return true;
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
