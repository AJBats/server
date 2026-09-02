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
#include "pawn_gambits.h"

#include "common/settings.h"
#include "common/utils.h"
#include "common/xirand.h"

#include <magic_enum/magic_enum.hpp>

#include <algorithm>
#include <cmath>
#include <tuple>
#include <vector>

#include "ai/ai_container.h"
#include "ai/helpers/pathfind.h"
#include "ai/states/magic_state.h"
#include "ai/states/range_state.h"
#include "enmity_container.h"
#include "entities/char_entity.h"
#include "status_effect_container.h"
#include "entities/mob_entity.h"
#include "items/item_weapon.h"
#include "navmesh/navmesh.h"
#include "party.h"
#include "spell.h"
#include "utils/charutils.h"
#include "utils/zoneutils.h"
#include "zone.h"

CPawnController::CPawnController(CCharEntity* PPawn)
: CPlayerController(PPawn)
, m_Gambits(std::make_unique<pawn::CGambits>(PPawn, this))
, m_AvoidAggroBase(settings::get<bool>("pawn.AVOID_AGGRO"))
{
}

void CPawnController::SetAvoidAggro(const bool on)
{
    if (m_AvoidAggroBase != on)
    {
        ShowInfoFmt("pawn: {} aggro avoidance {}{}", POwner->getName(), on ? "on" : "off",
                    m_AvoidAggroGambit.has_value() ? " (a gambit row currently overrides it)" : "");
    }
    m_AvoidAggroBase = on;
}

auto CPawnController::IsAvoidingAggro() const -> bool
{
    return m_AvoidAggroGambit.value_or(m_AvoidAggroBase);
}

void CPawnController::ClearGambitBehaviors()
{
    m_AvoidAggroGambit.reset();
}

void CPawnController::SetGambitBehavior(const uint16 behavior, const bool on)
{
    if (static_cast<pawn::Behavior>(behavior) == pawn::Behavior::AvoidAggro)
    {
        m_AvoidAggroGambit = on;
    }
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

    // The post-fight breather is timed from whenever combat actually ended,
    // however it ended (mob death, leash break, disengage)
    const bool engaged = POwner->PAI->IsEngaged();
    if (m_WasEngaged && !engaged)
    {
        m_CombatEndTime = tick;
    }
    m_WasEngaged = engaged;

    if (!POwner->isDead())
    {
        CheckBrain();
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

void CPawnController::SetHunting(const bool on)
{
    m_Hunting = on;
    if (!on)
    {
        RestoreNormalSpeed();
    }
    ShowInfoFmt("pawn: {} hunt mode {}", POwner->getName(), on ? "on" : "off");
}

auto CPawnController::IsHunting() const -> bool
{
    return m_Hunting;
}

void CPawnController::CheckBrain()
{
    auto*      PPawn = static_cast<CCharEntity*>(POwner);
    const auto mjob  = static_cast<uint8>(PPawn->GetMJob());
    const auto sjob  = static_cast<uint8>(PPawn->GetSJob());

    if (mjob != m_BrainMainJob || sjob != m_BrainSubJob)
    {
        m_BrainMainJob = mjob;
        m_BrainSubJob  = sjob;
        pawn::loadBrain(PPawn);
    }
}

auto CPawnController::DoCombatTick(const timer::time_point tick) -> Task<void>
{
    TracyZoneScoped;

    RestoreNormalSpeed();
    m_Gambits->TickBehaviors();

    CCharEntity* PPlayer = GetLivePlayer();

    // The player is the party's anchor: gone from the zone means stand down.
    // Their weapon going down does NOT call the party off any more -- a
    // fight runs until the mob dies or drifts past the leash.
    if (PPlayer == nullptr)
    {
        ShowInfoFmt("pawn: {} disengaging (player gone)", POwner->getName());
        POwner->PAI->Internal_Disengage();
        co_return;
    }

    // The player steers: fight what they fight, once they hold enmity on it
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
    if (PTarget == nullptr || PTarget->isDead())
    {
        POwner->PAI->Internal_Disengage();
        co_return;
    }

    // Runaway-train guard: a fight that drags too far from the player is abandoned
    if (distance(POwner->loc.p, PPlayer->loc.p) > settings::get<float>("pawn.HUNT_LEASH"))
    {
        ShowInfoFmt("pawn: {} breaking off {} (past the leash)", POwner->getName(), PTarget->getName());
        POwner->PAI->Internal_Disengage();
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

    m_Gambits->Tick(tick, true);

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
        TravelTick();
        co_return;
    }

    if (auto* PTarget = PartyEngageTarget(PPlayer); PTarget != nullptr)
    {
        POwner->StatusEffectContainer->DelStatusEffectSilent(xi::StatusEffect::Healing);
        POwner->PAI->Internal_Engage(EntityId(PTarget));
        co_return;
    }

    // Rest with the player: the Healing status is the real thing -- kneel
    // animation and resting regen ticks -- so the party sits down together
    const bool playerResting = PPlayer->animation == xi::Animation::Healing;
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

    // A hunter picks the party's next fight itself
    if (m_Hunting && m_Tick - m_LastHuntCheckTime > 3s)
    {
        m_LastHuntCheckTime = m_Tick;
        if (HuntReady(PPlayer))
        {
            if (auto* PMob = PickHuntTarget(PPlayer); PMob != nullptr)
            {
                ShowInfoFmt("pawn: {} pulls {} ({})", POwner->getName(), PMob->getName(),
                            magic_enum::enum_name(charutils::CheckMob(PPlayer->GetMLevel(), PMob)));
                POwner->PAI->Internal_Engage(EntityId(PMob));
                co_return;
            }
        }
    }

    // Where this pawn belongs: the lead holds a point ahead of the player,
    // everyone else follows the chain. A slot exists only if this tick's
    // decision comes from FormationPoint (chain followers have none).
    m_Gambits->TickBehaviors();
    m_HasSlot = false;
    position_t followPoint{};
    float      declumpDistance = 0.0f;
    float      followMax       = 2.0f;
    float      followTarget    = 1.0f;

    if (m_Hunting)
    {
        followPoint = LeadPoint(PPlayer);
        RampCatchUp(m_PlayerMoving, followPoint);
    }
    else
    {
        const CBattleEntity* PFollowTarget = GetFollowTarget();
        if (PFollowTarget == nullptr)
        {
            co_return;
        }

        const bool isFirstPawn = GetPawnPartyPosition() == 0;
        followPoint     = PFollowTarget->loc.p;
        declumpDistance = isFirstPawn ? 1.0f : 1.5f;
        followMax       = isFirstPawn ? 2.0f : 3.5f;
        followTarget    = isFirstPawn ? 1.5f : 3.0f;

        // Following the live player: the same fresh position the lead uses,
        // with a gentle prediction, and a slot of its own -- a rear quarter
        // FORMATION_FOLLOW_DISTANCE behind the player -- parked and held the
        // way the lead holds its point. Without a slot the follower's
        // destination is the player themself, and a fresh position puts it
        // right on top of them. Pawns following pawns keep the plain chain:
        // server positions have no lag.
        if (PFollowTarget == static_cast<const CBattleEntity*>(PPlayer))
        {
            const auto anchor = PlayerAnchor(PPlayer, settings::get<float>("pawn.FORMATION_FOLLOW_PREDICT_SCALE"));
            const auto angle  = static_cast<float>(M_PI) - settings::get<float>("pawn.FORMATION_FOLLOW_ANGLE_DEG") * static_cast<float>(M_PI) / 180.0f;
            followPoint       = FormationPoint(anchor, settings::get<float>("pawn.FORMATION_FOLLOW_DISTANCE"), angle, m_FollowPoint, m_HasFollowPoint);
            declumpDistance   = 0.0f;
            followMax         = 2.0f;
            followTarget      = 1.0f;
            RampCatchUp(anchor.moving, followPoint);
            FormationDebug("follow", PPlayer, anchor, followPoint);
        }
    }

    auto avoidAction = AvoidAction::None;
    if (IsAvoidingAggro())
    {
        avoidAction = Avoid(followPoint, followMax, followTarget, declumpDistance);
    }

    const float currentDistance = distance(POwner->loc.p, followPoint);

    // Avoidance moves are short and local. The navmesh planner refuses a hop
    // under a yalm (isNear) and plans nothing, so a required avoidance move
    // that short steps straight at its point when the point is on the mesh;
    // anything longer goes through the planner like every other move.
    constexpr float plannerMinimumHop = 1.2f;
    const bool      shortAvoidStep    = avoidAction != AvoidAction::None && currentDistance > followMax && currentDistance < plannerMinimumHop &&
                                        POwner->PAI->PathFind->ValidPosition(followPoint);
    if (shortAvoidStep)
    {
        POwner->PAI->PathFind->Clear();
        POwner->PAI->PathFind->StepTo(followPoint);
    }
    else if (currentDistance > followMax)
    {
        // Warp only when pathing genuinely fails; a pawn arriving at a zone
        // gate runs to its player like anyone else would
        if (!PathToward(followPoint, followTarget) && currentDistance > WarpDistance)
        {
            POwner->PAI->PathFind->WarpTo(followPoint);
            co_return;
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

    if (POwner->PAI->PathFind->IsFollowingPath())
    {
        POwner->PAI->PathFind->FollowPath(m_Tick);
    }
    else if (!POwner->PAI->IsCurrentState<CMagicState>() && avoidAction != AvoidAction::Escape && avoidAction != AvoidAction::Detour)
    {
        // Between fights, standing still: cures, raises, buffs -- but never
        // in a tick spent stepping to safety, since a cast would root the
        // pawn inside the circle
        m_Gambits->Tick(tick, false);
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
    return CPlayerController::Cast(castTarget, spellid);
}

auto CPawnController::WeaponSkill(const EntityId target, const uint16 wsid) -> bool
{
    FaceTarget(target);
    return CPlayerController::WeaponSkill(target, wsid);
}

auto CPawnController::Ability(const EntityId target, const uint16 abilityid) -> bool
{
    FaceTarget(target);
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
    // The player's engagement comes first and keeps the retail trust
    // convention: a melee swing (or the TrustEngageType charvar) signals
    // the intent to commit the party
    if (PPlayer != nullptr && PPlayer->PAI->IsEngaged() && PPlayer->GetBattleTarget() != nullptr)
    {
        auto*      playerController = dynamic_cast<CPlayerController*>(PPlayer->PAI->GetController());
        const bool playerMeleeSwing = playerController != nullptr && playerController->getLastAttackTime() > timer::now() - 1s;

        if (charutils::GetCharVar(PPlayer, "TrustEngageType") == 1 || playerMeleeSwing)
        {
            return PPlayer->GetBattleTarget();
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
    return nullptr;
}

auto CPawnController::HuntReady(const CCharEntity* PPlayer) const -> bool
{
    if (m_Tick - m_CombatEndTime < std::chrono::milliseconds(settings::get<uint32>("pawn.HUNT_DOWNTIME_MS")))
    {
        return false;
    }

    // Pull only from the player's side: the player drives, the hunter scouts
    if (distance(POwner->loc.p, PPlayer->loc.p) > 10.0f || PPlayer->animation == xi::Animation::Healing)
    {
        return false;
    }

    const auto readyHPP = settings::get<uint8>("pawn.HUNT_READY_HPP");
    const auto readyMPP = settings::get<uint8>("pawn.HUNT_READY_MPP");

    const auto* PPawn = static_cast<const CCharEntity*>(POwner);
    if (PPawn->PParty == nullptr)
    {
        return POwner->GetHPP() >= readyHPP;
    }

    for (auto* PMember : PPawn->PParty->members)
    {
        if (PMember->loc.zone != POwner->loc.zone)
        {
            continue;
        }
        if (PMember->isDead() || PMember->PAI->IsEngaged() || PMember->GetHPP() < readyHPP ||
            (PMember->health.maxmp > 0 && PMember->GetMPP() < readyMPP))
        {
            return false;
        }
    }
    return true;
}

auto CPawnController::PickHuntTarget(const CCharEntity* PPlayer) const -> CMobEntity*
{
    const auto  minCheck    = settings::get<uint8>("pawn.HUNT_CHECK_MIN");
    const auto  maxCheck    = settings::get<uint8>("pawn.HUNT_CHECK_MAX");
    const auto  radius      = settings::get<float>("pawn.HUNT_RADIUS");
    const auto  cleanRadius = settings::get<float>("pawn.HUNT_CLEAN_RADIUS");

    // One danger scan per hunt check, wide enough to cover every candidate's
    // clean radius and every approach from the hunter; candidates filter it
    const bool                        cleanPulls = settings::get<bool>("pawn.HUNT_CLEAN_PULLS");
    std::vector<pawn::danger::Danger> dangers;
    if (cleanPulls)
    {
        // Judged for the whole party that will fight beside the target, not
        // for the hunter's own buffs and health
        dangers = pawn::danger::around(POwner->loc.zone, PPlayer->loc.p, radius + std::max(cleanRadius, distance(POwner->loc.p, PPlayer->loc.p)),
                                       pawn::danger::Profile::worstCase());
    }

    CMobEntity* best     = nullptr;
    float       bestDist = radius; // the hunter's own walk is capped too

    POwner->loc.zone->ForEachMob([&](CMobEntity* PMob)
                                 {
                                     // idle, ordinary field mobs only
                                     const bool special = (PMob->m_Type & xi::MobType::Event) != xi::MobType::Normal ||
                                                          (PMob->m_Type & xi::MobType::Fished) != xi::MobType::Normal ||
                                                          (PMob->m_Type & xi::MobType::Battlefield) != xi::MobType::Normal ||
                                                          (PMob->m_Type & xi::MobType::Notorious) != xi::MobType::Normal;
                                     if (special || PMob->PMaster != nullptr || !PMob->isAlive() ||
                                         PMob->PAI->IsEngaged() || PMob->allegiance != xi::Allegiance::Mob)
                                     {
                                         return;
                                     }
                                     if (!PMob->PEnmityContainer->GetEnmityList()->empty())
                                     {
                                         return;
                                     }
                                     if (distance(PPlayer->loc.p, PMob->loc.p) > radius)
                                     {
                                         return;
                                     }

                                     const auto check = static_cast<uint8>(charutils::CheckMob(PPlayer->GetMLevel(), PMob));
                                     if (check < minCheck || check > maxCheck)
                                     {
                                         return;
                                     }

                                     const float toHunter = distance(POwner->loc.p, PMob->loc.p);
                                     if (toHunter >= bestDist)
                                     {
                                         return;
                                     }

                                     // Clean pulls: no other aggressive mob near the target, no
                                     // danger circle across the approach, and no linking family
                                     // member (aggressive or not) within the clean radius
                                     for (const auto& d : dangers)
                                     {
                                         if (d.mob == PMob)
                                         {
                                             continue;
                                         }
                                         if (isWithinDistance(d.mob->loc.p, PMob->loc.p, cleanRadius) ||
                                             cardian::formation::segmentCrosses(d, POwner->loc.p.x, POwner->loc.p.z, PMob->loc.p.x, PMob->loc.p.z))
                                         {
                                             return;
                                         }
                                     }
                                     if (cleanPulls && PMob->m_Link != 0)
                                     {
                                         bool linked = false;
                                         POwner->loc.zone->ForEachMob([&](CMobEntity* POther)
                                                                      {
                                                                          if (!linked && POther != PMob && POther->m_Link != 0 && POther->m_Family == PMob->m_Family &&
                                                                              POther->isAlive() && POther->PMaster == nullptr &&
                                                                              isWithinDistance(POther->loc.p, PMob->loc.p, cleanRadius))
                                                                          {
                                                                              linked = true;
                                                                          }
                                                                      });
                                         if (linked)
                                         {
                                             return;
                                         }
                                     }

                                     best     = PMob;
                                     bestDist = toHunter;
                                 });

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
        if (auto* PChar = dynamic_cast<CCharEntity*>(PMember);
            PChar != nullptr && PChar->PSession != nullptr && PChar->loc.zone == POwner->loc.zone)
        {
            return PChar;
        }
    }
    return nullptr;
}

namespace
{
    // A hunting pawn leads from the front and is left out of the follow chain
    auto isLead(const CCharEntity* PChar) -> bool
    {
        const auto* PController = dynamic_cast<const CPawnController*>(PChar->PAI->GetController());
        return PController != nullptr && PController->IsHunting();
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
    const float wanted      = cardian::formation::catchUpSpeed(distance(POwner->loc.p, point), playerMoving, normalSpeed,
                                                               settings::get<float>("pawn.FORMATION_CATCHUP_SPEED"),
                                                               settings::get<float>("pawn.FORMATION_CATCHUP_DISTANCE"));

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

auto CPawnController::Avoid(position_t& point, float& followMax, float& followTarget, float& declumpDistance) -> AvoidAction
{
    using namespace cardian::formation;

    const auto* PPawn   = static_cast<const CCharEntity*>(POwner);
    const auto  dangers = pawn::danger::around(POwner->loc.zone, POwner->loc.p, settings::get<float>("pawn.AVOID_SCAN"), pawn::danger::Profile::of(PPawn));

    AvoidAction action = AvoidAction::None;
    if (!dangers.empty())
    {
        const auto&      circles = dangers; // a Danger is its circle
        const position_t me      = POwner->loc.p;

        if (insideAny(circles, me.x, me.z))
        {
            // Pushed away: the minimum proximity is never violated, whatever
            // the formation wanted. A full yalm past the rim, so the next
            // tick finds her clearly outside. Straight out is the first
            // choice; with a wall at her back, other directions around the
            // deepest circle are tried for an on-mesh, clear point.
            auto [x, z] = pushOut(circles, me.x, me.z, 1.0f);
            if (!POwner->PAI->PathFind->ValidPosition(position_t(x, me.y, z, 0, 0)))
            {
                const auto deepest = std::max_element(circles.begin(), circles.end(), [&](const auto& a, const auto& b)
                                                      {
                                                          return depthInside(a, me.x, me.z) < depthInside(b, me.x, me.z);
                                                      });
                const float base = std::atan2(me.z - deepest->z, me.x - deepest->x);
                for (const float turn : { 0.5f, -0.5f, 1.0f, -1.0f, 1.6f, -1.6f, 2.2f, -2.2f, 3.1f })
                {
                    const float cx = deepest->x + std::cos(base + turn) * (deepest->radius + 1.0f);
                    const float cz = deepest->z + std::sin(base + turn) * (deepest->radius + 1.0f);
                    if (!insideAny(circles, cx, cz) && POwner->PAI->PathFind->ValidPosition(position_t(cx, me.y, cz, 0, 0)))
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
        else if (insideAny(circles, point.x, point.z))
        {
            // The slot is in danger: the nearest clear angle on its own ring
            // (the sandwich -- as close to the ideal spot as safety allows),
            // else the point pushed straight out
            bool seated = false;
            if (m_HasSlot)
            {
                const auto onRing = [&](const float angle)
                {
                    const position_t p = nearPosition(m_SlotAnchor, m_SlotOffset, angle);
                    return std::pair{ p.x, p.z };
                };
                if (const auto angle = safestAngleOnRing(circles, m_SlotAngle, onRing); angle.has_value())
                {
                    point  = nearPosition(m_SlotAnchor, m_SlotOffset, *angle);
                    seated = true;
                }
            }
            if (!seated)
            {
                const auto [x, z] = pushOut(circles, point.x, point.z, 0.5f);
                point.x           = x;
                point.z           = z;
            }
            action = seated ? AvoidAction::Slot : AvoidAction::PushedSlot;
        }
        else
        {
            // The straight way there crosses a circle: go around the NEAREST
            // crossing one first (the list is unordered), with the waypoint
            // itself pushed clear of any other circle; next tick re-plans
            const Circle* nearestCrossing = nullptr;
            float         nearestDist     = 0.0f;
            for (const auto& c : circles)
            {
                if (!segmentCrosses(c, me.x, me.z, point.x, point.z))
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
                auto [x, z]       = detourAround(*nearestCrossing, me.x, me.z, point.x, point.z, 0.5f);
                std::tie(x, z)    = pushOut(circles, x, z, 0.5f);
                point             = position_t(x, me.y, z, 0, me.rotation);
                followMax         = std::min(followMax, 1.0f);
                followTarget      = 0.5f;
                declumpDistance   = 0.0f;
                action            = AvoidAction::Detour;
            }
        }
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
            ShowInfoFmt("pawn: {} avoids {} ({}: {:.1f}y of a {:.1f}y circle)", POwner->getName(),
                        nearest->mob->getName(), magic_enum::enum_name(action), nearest->distance, nearest->radius);
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
                ShowInfoFmt("pawn: avoid {}: dangers={} nearest={} d={:.1f}y r={:.1f}y action={}", POwner->getName(), dangers.size(),
                            nearest->mob->getName(), nearest->distance, nearest->radius, magic_enum::enum_name(action));
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
            PChar != nullptr && pawn::isPawn(PChar) && !isLead(PChar))
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
