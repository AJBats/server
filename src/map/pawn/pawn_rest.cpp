// Cardian: one lifecycle for the tactician's MP pacing (her marked Rest row), a plain Rest row, Rest With Player, town kneeling and the player's rest order.
#include "pawn_controller.h"
#include "pawn.h"
#include "rest_policy.h"
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

#include <optional>

namespace
{
    using pawn::tactics::restSeconds;
}

void CPawnController::SetRestOrder(const int percent, const std::string_view why, const bool byRow)
{
    m_RestOrder = cardian::rest::Order{ .percent = std::clamp(percent, 1, 100), .byRow = byRow };
    // An order to rest leaves whatever fight she was in or walking to
    StandDown(fmt::format("rests until {}% on {}", m_RestOrder.percent, byRow ? "her Rest row" : "the player's order"));
    ShowInfoFmt("rest: {} {} rest until {}% HP and MP ({})", POwner->getName(), byRow ? "takes her Rest row's" : "is ordered to", m_RestOrder.percent, why);
}

void CPawnController::EndRestOrder(const std::string_view why)
{
    if (!m_RestOrder.active())
    {
        return;
    }
    ShowInfoFmt("rest: {}'s {} to rest until {}% ends ({})", POwner->getName(), m_RestOrder.byRow ? "Rest row" : "order", m_RestOrder.percent, why);
    m_RestOrder = {};
}

auto CPawnController::RestAllowsAction() const -> bool
{
    return pawn::tactics::kneelAllowsAction(m_Rest, POwner);
}

auto CPawnController::RestInterruptionCost() const -> double
{
    return pawn::tactics::restInterruptionCost(POwner);
}

auto CPawnController::RestReadyIn(const double now) const -> double
{
    return pawn::tactics::kneelReadyIn(m_Rest, POwner, now);
}

void CPawnController::StandFromRest(const std::string_view why)
{
    m_RestDeferredPosition = false;
    m_Rest.wantsDown = false;
    if (pawn::tactics::standFromKneel(m_Rest, POwner, why))
    {
        m_RestTicks = 0;
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
    // Her own row's order goes with her gambits: switched off, it ends
    if (m_RestOrder.active() && m_RestOrder.byRow && !m_Gambits->MasterOn())
    {
        EndRestOrder("her gambits are off");
    }
    auto* leader = GetAnchor();
    const auto* place = CurrentPlace(leader);
    // The party's fight as the engage door scans it, whatever her rows say:
    // the same foes and place-centered HUNT_LEASH, and the same foes counted
    // absent (below ground, held off). A distant pull or untouched wildlife
    // is no fight to her. A body with no place -- a solo farmer in the wild
    // -- scans round herself. Asked only when something turns on it, once
    std::optional<bool> fightSeen;
    const auto fightOn = [&]
    {
        if (!fightSeen.has_value())
        {
            fightSeen = PartyFightScan(leader, place != nullptr ? place->position() : POwner->loc.p).target != nullptr;
        }
        return *fightSeen;
    };
    const bool followEnabled = m_Gambits->MasterOn() && RestsWithPlayer() && leader != nullptr;
    const bool follow = m_RestFollow.request(followEnabled, leader != nullptr && leader->animation == xi::Animation::Healing,
                                           now, std::chrono::duration<double>(ReactionBeat()).count());
    const bool nearLeader = leader != nullptr && (Staked() || distance(POwner->loc.p, leader->loc.p) < 10.0f);
    const bool withPlayer = follow && (healing != nullptr || nearLeader);
    const auto advice = pawn::tactics::restAdvice(static_cast<CCharEntity*>(POwner));
    // The emergency cure stands any kneeling caster the party picks for it
    // (the bank advises everyone who offers it spells), whatever her Rest
    // row says: the wake is about the cure, not her MP pacing
    const bool urgent = advice.has_value() && advice->wake && m_Gambits->MasterOn();
    // Her marked Rest row, its conditions holding, is the tactician's MP
    // pacing (RESEARCH §17.13): the bank's advice on when to kneel and
    // when she is ready
    const bool support = advice.has_value() && pawn::tactics::offersRest(POwner) && m_Gambits->MasterOn();
    // MP alone decides a casting mage's own rest: her missing HP is her
    // cures' to mend, as anyone else's is (the user, 2026-09-23). Rest
    // With Player stays an explicit input beside it; neither overwrites
    // the other with the MP decision
    const bool supportRecovery = support && advice->recover;
    const bool want            = townKneel || (support && place != nullptr && (supportRecovery || (healing != nullptr && POwner->health.mp < POwner->GetMaxMP())));
    const int ticks = healing != nullptr ? healing->GetElapsedTickCount() : 0;
    const bool landed = ticks >= 2 && ticks > m_RestTicks;
    m_RestTicks = ticks;
    const bool mpMissing = POwner->health.mp < POwner->GetMaxMP();
    // No fight on while she is down with MP missing: a useful rest goes on
    const bool campClear = support && place != nullptr && healing != nullptr && mpMissing && !fightOn();

    // Ordinary DoTs share REGEN_DOWN. Helix and nightmare Bio tick directly.
    const auto* pet = dynamic_cast<CPetEntity*>(POwner->PPet);
    const bool noRecovery = POwner->getMod(xi::Mod::REGEN_DOWN) > 0 ||
        (pet != nullptr && pet->getPetType() == PET_TYPE::AVATAR) ||
        POwner->StatusEffectContainer->HasPreventActionEffect() ||
        POwner->StatusEffectContainer->HasStatusEffect({xi::StatusEffect::Helix, xi::StatusEffect::Bio,
            xi::StatusEffect::Disease, xi::StatusEffect::Plague, xi::StatusEffect::CurseIi});
    // A plain Rest row is an order she gives herself: when its conditions
    // hold and no fight is on, down until full -- predictable, no
    // judgement, the player's command the way out (RESEARCH §17.13). It is
    // taken only where she could kneel on it now: anything that would stand
    // her straight up again (below) keeps it from starting, and so does an
    // order of his still to fire, which the order's StandDown would drop.
    // The world's layer carries one for every wild body (brains.yaml): the
    // solo farmer's rest, half HP or low MP
    const bool mayKneel = !Acting() && !noRecovery && !POwner->isDead() && !POwner->PAI->IsEngaged() &&
                          !m_Retreat && m_Mode != Mode::Travel && !HasQueuedOrder() && !HasPlayersOrder();
    const bool rowDue   = RestRowDue() && mayKneel;
    bool unsafe = false;
    if (want || withPlayer || healing != nullptr || rowDue || (m_RestOrder.active() && m_RestOrder.byRow))
    {
        RefreshDangers(AttendedTarget());
        unsafe = InsideDanger();
        pawn::forEachMobNear(pawn::entitiesAround(POwner), POwner->loc.p, 30.0f, [&](CMobEntity* mob)
        {
            unsafe |= !mob->isDead() && mob->GetBattleTarget() == POwner;
        });
    }
    if (rowDue && !unsafe && !fightOn())
    {
        SetRestOrder(100, "her Rest row's conditions hold", true);
    }
    bool ordered = m_RestOrder.active();
    // An order she cannot carry out ends, and he is told when it was his:
    // held down by nothing but a reason she cannot recover, she would stand
    // idle for good. Her own row's order ends the same way, said to nobody
    if (ordered && (noRecovery || POwner->isDead()))
    {
        if (!m_RestOrder.byRow)
        {
            auto note   = cardian::link::make<cl_note>();
            note.kind   = CL_NOTE_REST_ENDS;
            note.action = cl_action{ CL_AK_REST, 0, static_cast<uint16_t>(m_RestOrder.percent) };
            Note(note, POwner->isDead() ? CL_S_KNOCKED_OUT : CL_S_CANNOT_RECOVER);
        }
        EndRestOrder(POwner->isDead() ? "KO'd" : "she cannot recover right now");
        ordered = false;
    }
    // The player's rest order stands down for nothing but the emergency cure
    // (urgent, below): a maneuver walks into aggro by design. Only what makes
    // a kneel impossible blocks it; his other orders end it before they act.
    // Her own Rest row's order is no maneuver: danger and the party's fight
    // stand her up, as the policy's own rests are, and she kneels again
    // after -- the order holds until full, or his command. A routine move
    // waits on it as on his own (DoRoamTick): the player's follow is the
    // way out
    const bool impossible = Acting() || noRecovery || POwner->isDead() || POwner->PAI->IsEngaged();
    const bool deliberate = ordered && !m_RestOrder.byRow;
    const bool rowRest    = ordered && m_RestOrder.byRow;
    // The player's own Attack keeps her up until the fight it named is over
    const bool blocked = impossible || (!deliberate && (unsafe || m_Retreat || m_Mode == Mode::Travel || HasQueuedOrder() || HasPlayersOrder() || (rowRest && fightOn())));
    // An ongoing support rest, or one the player ordered, defers formation
    // and seat requests every tick, even while the player moves. The rest
    // policy decides when to stand.
    const bool deferPosition = routinePosition && ((support && place != nullptr) || ordered);
    const auto decision = m_Rest.decide({.now = now, .resting = healing != nullptr, .want = want,
        .withPlayer = withPlayer,
        .campClear = campClear, .mpMissing = mpMissing,
        .urgent = urgent, .blocked = blocked,
        .moving = !stationary, .routinePosition = deferPosition,
        .recovered = support && place != nullptr && !supportRecovery, .tickLanded = landed,
        .ordered = ordered});
    if (decision == cardian::rest::Decision::Stand)
    {
        StandFromRest(urgent ? advice->why : unsafe && !deliberate ? "danger" : noRecovery ? "recovery blocked" :
            rowRest && fightOn() ? "the party's fight" :
            HasQueuedOrder() && !m_ManeuverResting ? "the player's action order" :
            support && place != nullptr && landed && !supportRecovery && !withPlayer && !campClear ? "recovery tick: pace and reserve ready" : "rest request ended or movement needed");
    }
    else if (decision == cardian::rest::Decision::Kneel)
    {
        const auto interval = std::chrono::seconds(settings::get<uint8>("map.HEALING_TICK_DELAY"));
        POwner->StatusEffectContainer->AddStatusEffect(xi::StatusEffect::Healing, 0, 0, interval, 0s);
        m_RestTicks = 0;
        ShowInfoFmt("rest: {} kneels ({}, hp {}%, mp {}%)", POwner->getName(),
                    ordered ? (m_RestOrder.byRow ? "her Rest row" : "the player's rest order") : townKneel ? "town" : withPlayer ? "with the player" : "the tactician's recovery",
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
            const auto clock = pawn::tactics::kneelClock(POwner->StatusEffectContainer->GetStatusEffect(xi::StatusEffect::Healing), now);
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
