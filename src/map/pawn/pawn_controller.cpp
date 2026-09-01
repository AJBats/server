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
#include "pawn_gambits.h"

#include "common/settings.h"
#include "common/utils.h"
#include "common/xirand.h"

#include <magic_enum/magic_enum.hpp>

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
{
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
    // everyone else follows the chain
    position_t followPoint{};
    float      declumpDistance = 0.0f;
    float      followMax       = 2.0f;
    float      followTarget    = 1.0f;

    if (m_Hunting)
    {
        followPoint = LeadPoint(PPlayer);
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
    }

    const float currentDistance = distance(POwner->loc.p, followPoint);

    if (currentDistance > followMax)
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
    else if (!POwner->PAI->IsCurrentState<CMagicState>())
    {
        // Between fights, standing still: cures, raises, buffs
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
    const auto minCheck = settings::get<uint8>("pawn.HUNT_CHECK_MIN");
    const auto maxCheck = settings::get<uint8>("pawn.HUNT_CHECK_MAX");
    const auto radius   = settings::get<float>("pawn.HUNT_RADIUS");

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
                                     if (toHunter < bestDist)
                                     {
                                         best     = PMob;
                                         bestDist = toHunter;
                                     }
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

auto CPawnController::LeadPoint(const CCharEntity* PPlayer) -> position_t
{
    // The position packet's MoveFlame counter plus real displacement since the last packet
    const bool moving = PPlayer->loc.p.moving != 0 || PPlayer->m_lastMoveDistance > 0.05f;

    float lead = settings::get<float>("pawn.FORMATION_LEAD_DISTANCE");
    if (moving)
    {
        lead += settings::get<float>("pawn.FORMATION_LEAD_MOVING_BONUS");
    }
    const position_t fresh = nearPosition(PPlayer->loc.p, lead, 0.0f);

    // A moving player is re-aimed every tick; the deadband only absorbs
    // the coarse position/heading updates of a player standing still
    if (!m_HasLeadPoint || moving || distance(fresh, m_LeadPoint) > settings::get<float>("pawn.FORMATION_DEADBAND"))
    {
        m_LeadPoint    = fresh;
        m_HasLeadPoint = true;
    }
    return m_LeadPoint;
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
