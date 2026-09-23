// Cardian: one lifecycle for Support Mage, Rest With Player, town kneeling and the player's rest order.
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

    // Healing's own clock: ticks so far, seconds to the next, seconds between
    auto healingClock(const CStatusEffect* healing, const double now) -> CPawnController::RestClock
    {
        const double interval = std::chrono::duration<double>(healing->GetTickTime()).count();
        const int ticks = healing->GetElapsedTickCount();
        return {.down = true, .ticks = ticks,
                .next = restSeconds(healing->GetStartTime()) + (ticks + 1) * interval - now, .interval = interval};
    }
}

void CPawnController::SetRestOrder(const int percent, const std::string_view why)
{
    m_RestOrder = cardian::rest::Order{ .percent = std::clamp(percent, 1, 100) };
    // An order to rest leaves whatever fight she was in or walking to
    StandDown(fmt::format("rests until {}% on the player's order", m_RestOrder.percent));
    ShowInfoFmt("rest: {} is ordered to rest until {}% HP and MP ({})", POwner->getName(), m_RestOrder.percent, why);
}

void CPawnController::EndRestOrder(const std::string_view why)
{
    if (!m_RestOrder.active())
    {
        return;
    }
    ShowInfoFmt("rest: {}'s order to rest until {}% ends ({})", POwner->getName(), m_RestOrder.percent, why);
    m_RestOrder = {};
}

auto CPawnController::RestOrderPercent() const -> int
{
    return m_RestOrder.percent;
}

auto CPawnController::RestNow() const -> RestClock
{
    const auto* healing = POwner->StatusEffectContainer->GetStatusEffect(xi::StatusEffect::Healing);
    return healing != nullptr ? healingClock(healing, restSeconds(timer::now())) : RestClock{};
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
    const auto clock = healingClock(healing, restSeconds(timer::now()));
    return cardian::rest::interruptionCost(clock.ticks, clock.next, clock.interval, POwner->getMod(xi::Mod::CLEAR_MIND), POwner->getMod(xi::Mod::MPHEAL));
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
        EndRestOrder("the player's action order");
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
    if (m_RestOrder.metBy(POwner->health.hp, POwner->GetMaxHP(), POwner->health.mp, POwner->GetMaxMP()))
    {
        EndRestOrder(fmt::format("HP and MP at {}%, her gambits take over", m_RestOrder.percent));
    }
    bool ordered = m_RestOrder.active();
    auto* leader = GetAnchor();
    const auto* place = CurrentPlace(leader);
    const bool followEnabled = m_Gambits->MasterOn() && RestsWithPlayer() && leader != nullptr;
    const bool follow = m_RestFollow.request(followEnabled, leader != nullptr && leader->animation == xi::Animation::Healing,
                                           now, std::chrono::duration<double>(ReactionBeat()).count());
    const bool nearLeader = leader != nullptr && (Staked() || distance(POwner->loc.p, leader->loc.p) < 10.0f);
    const bool withPlayer = follow && (healing != nullptr || nearLeader);
    const auto advice = pawn::tactics::restAdvice(static_cast<CCharEntity*>(POwner));
    const bool support = advice.has_value() && pawn::tactics::supportMage(POwner) && m_Gambits->MasterOn();
    // MP alone decides a Support Mage's own rest: her missing HP is her
    // cures' to mend, as anyone else's is (the user, 2026-09-23)
    const bool supportRecovery = support && advice->recover;
    // Rest With Player stays an explicit input even when a support role owns
    // autonomous recovery. Do not overwrite it with the role's MP decision.
    const bool want = townKneel || (support && place != nullptr && (supportRecovery || (healing != nullptr && POwner->health.mp < POwner->GetMaxMP())));
    const int ticks = healing != nullptr ? healing->GetElapsedTickCount() : 0;
    const bool landed = ticks >= 2 && ticks > m_RestTicks;
    m_RestTicks = ticks;
    const bool mpMissing = POwner->health.mp < POwner->GetMaxMP();
    // Use the same targets and place-centered HUNT_LEASH as party engagement.
    // A distant pull or untouched wildlife does not end a useful rest.
    const bool campClear = support && place != nullptr && healing != nullptr && mpMissing &&
        PartyEngageTarget(leader, place->position()).target == nullptr;

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
    // An order she cannot carry out ends, and he is told: held down by
    // nothing but a reason she cannot recover, she would stand idle for good
    if (ordered && (noRecovery || POwner->isDead()))
    {
        const auto why = POwner->isDead() ? std::string("KO'd") : std::string("she cannot recover right now");
        Note(fmt::format("{}'s rest ends: {}", POwner->getName(), why));
        EndRestOrder(why);
        ordered = false;
    }
    // The player's rest order stands down for nothing but the emergency cure
    // (urgent, below): a maneuver walks into aggro by design. Only what makes
    // a kneel impossible blocks it; his other orders end it before they act
    const bool impossible = Acting() || noRecovery || POwner->isDead() || POwner->PAI->IsEngaged();
    const bool blocked = impossible || (!ordered && (unsafe || m_Retreat || m_Mode == Mode::Travel || HasQueuedOrder()));
    // An ongoing support rest, or one the player ordered, defers formation
    // and seat requests every tick, even while the player moves. The rest
    // policy decides when to stand.
    const bool deferPosition = routinePosition && ((support && place != nullptr) || ordered);
    const auto decision = m_Rest.decide({.now = now, .resting = healing != nullptr, .want = want,
        .withPlayer = withPlayer,
        .campClear = campClear, .mpMissing = mpMissing,
        .urgent = support && advice->wake, .blocked = blocked,
        .moving = !stationary, .routinePosition = deferPosition,
        .recovered = support && place != nullptr && !supportRecovery, .tickLanded = landed,
        .ordered = ordered});
    if (decision == cardian::rest::Decision::Stand)
    {
        StandFromRest(support && advice->wake ? advice->why : unsafe && !ordered ? "danger" : noRecovery ? "recovery blocked" :
            HasQueuedOrder() && !m_ManeuverResting ? "the player's action order" :
            support && place != nullptr && landed && !supportRecovery && !withPlayer && !campClear ? "recovery tick: pace and reserve ready" : "rest request ended or movement needed");
    }
    else if (decision == cardian::rest::Decision::Kneel)
    {
        const auto interval = std::chrono::seconds(settings::get<uint8>("map.HEALING_TICK_DELAY"));
        POwner->StatusEffectContainer->AddStatusEffect(xi::StatusEffect::Healing, 0, 0, interval, 0s);
        m_RestTicks = 0;
        ShowInfoFmt("rest: {} kneels ({}, hp {}%, mp {}%)", POwner->getName(),
                    ordered ? "the player's rest order" : townKneel ? "town" : withPlayer ? "with the player" : "support recovery",
                    POwner->GetHPP(), POwner->GetMPP());
    }
    else if (decision == cardian::rest::Decision::StayDown && campClear && landed && !supportRecovery && !withPlayer)
    {
        ShowInfoFmt("rest: {} keeps resting after recovery tick: no party enemy within {:.0f} y of {}; MP {}/{} (pace and reserve ready)",
                    POwner->getName(), settings::get<float>("pawn.HUNT_LEASH"), place->name(), POwner->health.mp, POwner->GetMaxMP());
    }
    if (support && place != nullptr && POwner->StatusEffectContainer->HasStatusEffect(xi::StatusEffect::Healing) && now >= m_RestChatAt)
    {
        m_RestChatAt = now + 60.0;
        if (advice->knownCost && POwner->health.mp < advice->readyMp)
        {
            const auto clock = healingClock(POwner->StatusEffectContainer->GetStatusEffect(xi::StatusEffect::Healing), now);
            const double wait = cardian::rest::timeToReady(advice->readyMp - POwner->health.mp, clock.ticks, clock.next,
                                                         clock.interval, POwner->getMod(xi::Mod::CLEAR_MIND), POwner->getMod(xi::Mod::MPHEAL));
            const auto line = advice->readyMp > POwner->GetMaxMP() ? std::string("A fight and link reserve here need more MP than I can hold.") :
                fmt::format("Recovering: ready in about {:.0f} seconds, with a link reserve.", std::ceil(wait));
            pawn::tactics::role::sayParty(static_cast<CCharEntity*>(POwner), line);
            ShowInfoFmt("rest: {}: {}", POwner->getName(), line);
        }
    }
    const bool down = POwner->StatusEffectContainer->HasStatusEffect(xi::StatusEffect::Healing);
    if (down && deferPosition && !stationary && !m_RestDeferredPosition)
    {
        m_RestDeferredPosition = true;
        ShowInfoFmt("rest: {} defers routine repositioning to preserve recovery", POwner->getName());
    }
    if (!down)
    {
        m_RestDeferredPosition = false;
    }
    return down;
}
