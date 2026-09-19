// Cardian: one lifecycle for Support Mage, Rest With Player and town kneeling.
#include "pawn_controller.h"
#include "pawn.h"
#include "role_support.h"
#include "tactics.h"

#include "ai/ai_container.h"
#include "ai/helpers/pathfind.h"
#include "common/logging.h"
#include "common/settings.h"
#include "entities/char_entity.h"
#include "entities/mob_entity.h"
#include "entities/pet_entity.h"
#include "status_effect_container.h"

namespace
{
    auto restSeconds(const timer::time_point time) -> double
    {
        return std::chrono::duration<double>(time.time_since_epoch()).count();
    }
}

auto CPawnController::RestAllowsAction() const -> bool
{
    return m_Rest.canAct(restSeconds(timer::now()), POwner->StatusEffectContainer->HasStatusEffect(xi::StatusEffect::Healing));
}

auto CPawnController::RestInterruptionCost() const -> double
{
    const auto* healing = POwner->StatusEffectContainer->GetStatusEffect(xi::StatusEffect::Healing);
    if (healing == nullptr)
    {
        return 0.0;
    }
    const double interval = std::chrono::duration<double>(healing->GetTickTime()).count();
    const int ticks = healing->GetElapsedTickCount();
    const double next = restSeconds(healing->GetStartTime()) + (ticks + 1) * interval - restSeconds(timer::now());
    return cardian::rest::interruptionCost(ticks, next, interval, POwner->getMod(xi::Mod::CLEAR_MIND), POwner->getMod(xi::Mod::MPHEAL));
}

auto CPawnController::RestReadyIn(const double now) const -> double
{
    return m_Rest.readyIn(now, POwner->StatusEffectContainer->HasStatusEffect(xi::StatusEffect::Healing));
}

void CPawnController::StandFromRest(const std::string_view why)
{
    m_RestDeferredPosition = false;
    m_Rest.wantsDown = false;
    if (POwner->StatusEffectContainer->HasStatusEffect(xi::StatusEffect::Healing))
    {
        const double now = restSeconds(timer::now());
        if (!m_Rest.requestStand(now))
        {
            return;
        }
        POwner->StatusEffectContainer->DelStatusEffectSilent(xi::StatusEffect::Healing);
        m_Rest.stood(now);
        m_RestTicks = 0;
        ShowInfoFmt("rest: {} stands ({})", POwner->getName(), why);
    }
}

auto CPawnController::PrepareRestAction(const bool ordered) -> bool
{
    if (ordered)
    {
        StandFromRest("the player's action order");
    }
    if (!RestAllowsAction())
    {
        return false;
    }
    return true;
}

auto CPawnController::RestTick(const bool stationary, const bool townKneel, const bool routinePosition) -> bool
{
    const double now = restSeconds(timer::now());
    auto* healing = POwner->StatusEffectContainer->GetStatusEffect(xi::StatusEffect::Healing);
    auto* leader = GetAnchor();
    const bool followEnabled = m_Gambits->MasterOn() && RestsWithPlayer() && leader != nullptr;
    const bool follow = m_RestFollow.request(followEnabled, leader != nullptr && leader->animation == xi::Animation::Healing,
                                           now, std::chrono::duration<double>(ReactionBeat()).count());
    const bool nearLeader = leader != nullptr && (Staked() || distance(POwner->loc.p, leader->loc.p) < 10.0f);
    const bool withPlayer = follow && (healing != nullptr || nearLeader);
    const auto advice = pawn::tactics::restAdvice(static_cast<CCharEntity*>(POwner));
    const bool support = advice.has_value() && pawn::tactics::supportMage(POwner) && m_Gambits->MasterOn();
    const bool supportRecovery = support && (advice->recover || POwner->health.hp < POwner->GetMaxHP());
    // Rest With Player stays an explicit input even when a support role owns
    // autonomous recovery. Do not overwrite it with the role's MP decision.
    const bool want = townKneel || (support && Staked() && (supportRecovery || healing != nullptr));
    const int ticks = healing != nullptr ? healing->GetElapsedTickCount() : 0;
    const bool landed = ticks >= 2 && ticks > m_RestTicks;
    m_RestTicks = ticks;
    const bool mpMissing = POwner->health.mp < POwner->GetMaxMP();
    // Use the same targets and flag-centered HUNT_LEASH as camp engagement.
    // A distant pull or untouched wildlife does not end a useful rest.
    const bool campClear = support && Staked() && healing != nullptr && mpMissing &&
        PartyEngageTarget(leader, m_Stake->at).target == nullptr;

    bool unsafe = false;
    if (want || withPlayer || healing != nullptr)
    {
        RefreshDangers(AttendedTarget());
        unsafe = InsideDanger();
        pawn::forEachMobNear(pawn::entitiesAround(POwner), POwner->loc.p, 30.0f, [&](CMobEntity* mob)
        {
            unsafe |= !mob->isDead() && mob->GetBattleTarget() == POwner;
        });
    }
    // Ordinary DoTs share REGEN_DOWN. Helix and nightmare Bio tick directly.
    const auto* pet = dynamic_cast<CPetEntity*>(POwner->PPet);
    const bool noRecovery = POwner->getMod(xi::Mod::REGEN_DOWN) > 0 ||
        (pet != nullptr && pet->getPetType() == PET_TYPE::AVATAR) ||
        POwner->StatusEffectContainer->HasPreventActionEffect() ||
        POwner->StatusEffectContainer->HasStatusEffect({xi::StatusEffect::Helix, xi::StatusEffect::Bio,
            xi::StatusEffect::Disease, xi::StatusEffect::Plague, xi::StatusEffect::CurseIi});
    const bool blocked = Acting() || unsafe || noRecovery || POwner->isDead() ||
        m_Retreat || m_Mode == Mode::Travel || HasQueuedOrder() || POwner->PAI->IsEngaged();
    const auto decision = m_Rest.decide({.now = now, .resting = healing != nullptr, .want = want,
        .withPlayer = withPlayer,
        .campClear = campClear, .mpMissing = mpMissing,
        .urgent = support && advice->wake, .blocked = blocked,
        .moving = !stationary, .routinePosition = routinePosition && support && Staked(),
        .recovered = support && Staked() && !supportRecovery, .tickLanded = landed});
    if (decision == cardian::rest::Decision::Stand)
    {
        StandFromRest(support && advice->wake ? advice->why : unsafe ? "danger" : noRecovery ? "recovery blocked" :
            HasQueuedOrder() ? "the player's action order" :
            support && Staked() && landed && !supportRecovery && !withPlayer && !campClear ? "recovery tick: pace and reserve ready" : "rest request ended or movement needed");
    }
    else if (decision == cardian::rest::Decision::Kneel)
    {
        const auto interval = std::chrono::seconds(settings::get<uint8>("map.HEALING_TICK_DELAY"));
        POwner->StatusEffectContainer->AddStatusEffect(xi::StatusEffect::Healing, 0, 0, interval, 0s);
        m_RestTicks = 0;
        ShowInfoFmt("rest: {} kneels ({}, hp {}%, mp {}%)", POwner->getName(),
                    townKneel ? "town" : withPlayer ? "with the player" : "support recovery", POwner->GetHPP(), POwner->GetMPP());
    }
    else if (decision == cardian::rest::Decision::StayDown && campClear && landed && !supportRecovery && !withPlayer)
    {
        ShowInfoFmt("rest: {} keeps resting after recovery tick: no party enemy within {:.0f} y of camp; MP {}/{} (pace and reserve ready)",
                    POwner->getName(), settings::get<float>("pawn.HUNT_LEASH"), POwner->health.mp, POwner->GetMaxMP());
    }
    if (support && Staked() && POwner->StatusEffectContainer->HasStatusEffect(xi::StatusEffect::Healing) && now >= m_RestChatAt)
    {
        m_RestChatAt = now + 60.0;
        if (advice->knownCost && POwner->health.mp < advice->readyMp)
        {
            const auto* current = POwner->StatusEffectContainer->GetStatusEffect(xi::StatusEffect::Healing);
            const double interval = std::chrono::duration<double>(current->GetTickTime()).count();
            const double next = restSeconds(current->GetStartTime()) + (current->GetElapsedTickCount() + 1) * interval - now;
            const double wait = cardian::rest::timeToReady(advice->readyMp - POwner->health.mp, current->GetElapsedTickCount(), next,
                                                         interval, POwner->getMod(xi::Mod::CLEAR_MIND), POwner->getMod(xi::Mod::MPHEAL));
            const auto line = advice->readyMp > POwner->GetMaxMP() ? std::string("A fight and link reserve here need more MP than I can hold.") :
                fmt::format("Recovering: ready in about {:.0f} seconds, with a link reserve.", std::ceil(wait));
            pawn::tactics::role::sayParty(static_cast<CCharEntity*>(POwner), line);
            ShowInfoFmt("rest: {}: {}", POwner->getName(), line);
        }
    }
    const bool down = POwner->StatusEffectContainer->HasStatusEffect(xi::StatusEffect::Healing);
    if (down && routinePosition && !stationary && !m_RestDeferredPosition)
    {
        m_RestDeferredPosition = true;
        ShowInfoFmt("rest: {} defers camp repositioning to preserve recovery", POwner->getName());
    }
    if (!down)
    {
        m_RestDeferredPosition = false;
    }
    return down;
}
