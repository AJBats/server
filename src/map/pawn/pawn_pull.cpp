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

// Cardian: the camp's puller (ROADMAP, Pulling). At a camp, under the
// party's order Pull, the member in the Puller seat alone hunts. Once the
// party is ready -- nobody down, fighting, resting or walking in -- she picks
// a mob within kPullReach of the flag of a kind the player's Pull page has
// on, the first kind on his list first -- one aggro or kin hold too: she
// sets out for it and waits near it, for staying in camp is never the
// puller's call (the user, 2026-10-25) -- walks out through the walker to
// the farthest spot her ranged attack (a Bard: her song) reaches it from in
// sight, outside every ring when one is (ShotSpotFor), or in behind a mob's
// back on a timed visit, her walker's (walker.h visitTo), tags it,
// says the pull line in party chat and runs back to the camp's landing
// point. The tank's Provoke takes it off her as it comes in range
// (tank_calls.h, the pull); from then on she fights it as the Damage seat
// does. Her pulling kit -- a ranged weapon and its ammunition, or the map's
// own -- goes on as she takes the seat and stays on while she holds it. Her
// badge beside the party list (PULL_STATE) says what she is doing. The
// rules are pull_math.h's, the kit pull.h's.
#include "cardian_link.h"
#include "party_roster.h"
#include "pawn.h"
#include "pawn_controller.h"
#include "pawn_danger.h"
#include "pawn_rules.h"
#include "pawn_travel.h"
#include "pull.h"
#include "rest_policy.h"
#include "role_support.h"
#include "seats.h"
#include "spell_bank.h"
#include "tactics.h"
#include "sneak.h"
#include "walker.h"

#include "ai/ai_container.h"
#include "ai/helpers/pathfind.h"
#include "ai/states/magic_state.h"
#include "ai/states/range_state.h"
#include "common/logging.h"
#include "common/settings.h"
#include "common/xirand.h"
#include "entities/char_entity.h"
#include "entities/mob_entity.h"
#include "party.h"
#include "spell.h"
#include "status_effect_container.h"
#include "utils/charutils.h"
#include "utils/zoneutils.h"
#include "ximesh/ximesh.h"
#include "zone.h"

#include <fmt/format.h>
#include <magic_enum/magic_enum.hpp>

#include <algorithm>
#include <chrono>

namespace
{
    constexpr auto kKitLookEvery = std::chrono::seconds(2);  // her kit looked at this often while she holds the seat
    constexpr auto kTopUpEvery   = std::chrono::seconds(30); // her ammunition topped up at most this often

    auto seconds(const timer::duration d) -> double
    {
        return std::chrono::duration<double>(d).count();
    }

    // Seconds as the timer counts them, held to a sane range: an estimate
    // can come out huge
    auto after(const double secs) -> timer::duration
    {
        return std::chrono::duration_cast<timer::duration>(std::chrono::duration<double>(std::clamp(secs, 0.0, 1e6)));
    }

    // A member of her party, or one's pet
    auto ofParty(const CBattleEntity* POwner, const CBattleEntity* PWho) -> bool
    {
        if (PWho == nullptr)
        {
            return false;
        }
        if (PWho == POwner)
        {
            return true;
        }
        const auto* PParty = POwner->PParty;
        if (PParty == nullptr)
        {
            return false;
        }
        const auto* PMember = PWho->PMaster != nullptr ? PWho->PMaster : PWho;
        return std::ranges::find(PParty->members, PMember) != PParty->members.end();
    }
} // namespace

auto CPawnController::PullerSeat() const -> bool
{
    return pawn::roster::roleOf(static_cast<CCharEntity*>(POwner)) == cardian::party::Role::Puller;
}

auto CPawnController::BringsIn(const CBattleEntity* PChar, const CBattleEntity* PMob) -> bool
{
    if (PChar == nullptr || PMob == nullptr || PChar->objtype != TYPE_PC || PChar->PAI == nullptr)
    {
        return false;
    }
    const auto* PPuller = dynamic_cast<const CPawnController*>(PChar->PAI->GetController());
    return PPuller != nullptr && PPuller->m_Pull.has_value() && PPuller->m_Pull->step != PullRun::Step::Out && PPuller->m_Pull->mob == PMob;
}

void CPawnController::CallPull(const CMobEntity* PMob, const CCharEntity* PPlayer)
{
    const uint8 call  = pawn::pull::callOf(pawn::ordersOwnerOf(static_cast<const CCharEntity*>(POwner)));
    const uint8 level = PPlayer != nullptr ? pawn::pull::conLevel(PPlayer) : POwner->GetMLevel();
    const auto  check = static_cast<uint8>(charutils::CheckMob(level, const_cast<CMobEntity*>(PMob)));
    pawn::tactics::role::sayParty(static_cast<CCharEntity*>(POwner), cardian::pull::pullLine(PMob->getName(), check, call));
    m_Pull->called = PMob->id;
}

void CPawnController::SayPull(const std::string& line)
{
    if (line == m_PullSaid)
    {
        return;
    }
    m_PullSaid = line;
    if (!line.empty())
    {
        ShowInfoFmt("pull: {} {}", POwner->getName(), line);
    }
}

void CPawnController::SayHold(const std::string_view key, const std::string& line)
{
    if (CampFight() != nullptr)
    {
        m_PullHoldSaid.clear();
        return;
    }
    // each reason once between two fights, so reasons that take turns are
    // not said again at every turn
    const auto mark = fmt::format("|{}|", key);
    if (m_PullHoldSaid.find(mark) != std::string::npos)
    {
        return;
    }
    m_PullHoldSaid += mark;
    pawn::tactics::role::sayParty(static_cast<CCharEntity*>(POwner), line);
}

void CPawnController::EndPull(const std::string_view why)
{
    m_Pull.reset();
    m_PullSaid.clear();
    if (m_Mode == Mode::Pull)
    {
        Transition(IdleMode(), why);
    }
}

void CPawnController::GiveUpPull(const CMobEntity* PMob, const std::string_view why, const double passOverSeconds)
{
    const auto until = m_Tick + after(passOverSeconds);
    if (PMob != nullptr)
    {
        m_PullPassOver.emplace_back(PMob->id, until);
        ShowInfoFmt("pull: {} gives {} up ({}); passes it over for {:.0f} s", POwner->getName(), PMob->getName(), why, passOverSeconds);
    }
    EndPull(fmt::format("gives {} up ({})", PMob != nullptr ? PMob->getName() : std::string("the mob"), why));
}

void CPawnController::RetargetPull(const CMobEntity* PFrom, CMobEntity* PTo, const std::string_view why)
{
    // On to it from where she stands, the run kept: its pipeline's landing
    // time with it, which her shot waits for at the new spot. The one she
    // leaves is passed over a short while
    namespace pull = cardian::pull;
    ShowInfoFmt("pull: {} leaves {} for {}, which she can pull now ({})", POwner->getName(), PFrom->getName(), PTo->getName(), why);
    m_PullPassOver.emplace_back(PFrom->id, m_Tick + after(pull::kKinPassOverSeconds));
    m_Pull->mob          = EntityId(PTo);
    m_Pull->step         = PullRun::Step::Out;
    m_Pull->stepAt       = m_Tick;
    m_Pull->setOffAt     = m_Tick;
    m_Pull->outLimit     = pull::outSeconds(distance(POwner->loc.p, PTo->loc.p));
    m_Pull->spot.reset();
    m_Pull->cleanAt      = {};
    m_Pull->blockedSince = {};
    m_Pull->movingSince  = {};
    m_Pull->kinSince     = {};
    m_Pull->holdSince    = {};
    m_Pull->visitMay     = false;
    m_Pull->visited      = false;
    m_Pull->spotFailed   = false;
    m_Pull->respot       = false;
    m_Pull->atSpotSince  = {};
    m_Pull->called       = 0;
    m_PullCandidate.reset();
    m_PullSaid.clear();
    HeadLook(PTo);
}

void CPawnController::HoldForShot(const CMobEntity* PMob)
{
    // She stands through the walker, so a ring that comes onto her is
    // stepped out of -- but for a visit's spot, inside a ring by plan
    if (m_Pull->visited)
    {
        return;
    }
    RefreshDangers(PMob);
    Intent hold;
    hold.kind       = Intent::Kind::Stand;
    hold.keepsClear = true;
    Move(hold);
}

void CPawnController::KitTick()
{
    auto* PChar = static_cast<CCharEntity*>(POwner);
    if (POwner->isDead() || m_Tick < m_KitAt)
    {
        return;
    }
    m_KitAt = m_Tick + kKitLookEvery;
    // Out of a real player's party she holds no seat: a kit still on comes
    // off at the first free moment, as it does when she leaves the seat
    if (pawn::partyPlayer(PChar) == nullptr)
    {
        m_KitReady    = false;
        m_KitSeat     = false;
        m_KitPulling  = false;
        m_KitTellOwed = false;
        m_KitSaid.clear();
        m_PullItem  = 0;
        m_PullSpell = 0;
        if (!POwner->PAI->IsEngaged() && !Acting() && pawn::pull::wearsKit(PChar))
        {
            if (const auto said = pawn::pull::putBack(PChar); !said.empty())
            {
                ShowInfoFmt("pull: {} {} (she is out of the player's party)", POwner->getName(), said);
            }
        }
        return;
    }

    const bool puller = PullerSeat();
    // A body of the world in a real player's party: her planned ammunition,
    // as her food, never runs out, and in the seat the map's own kit too
    const bool world = pawn::seats::isWorlds(PChar->id) && pawn::withRealPlayer(PChar->id);
    if ((puller || world) && m_Tick >= m_KitTopUpAt)
    {
        m_KitTopUpAt = m_Tick + kTopUpEvery;
        pawn::pull::topUp(PChar, puller, world);
    }

    // Her kit goes on as she takes the seat and stays on while she holds it:
    // it is never swapped back between fights (the user, 2026-10-16).
    // Changing her ranged weapon ends her TP, and any change mid-action
    // breaks it: a ranged weapon waits for a free moment between fights; her
    // ammunition, which costs no TP, is refilled whenever she is not acting
    const bool quiet = !Acting() && !POwner->PAI->IsCurrentState<CRangeState>() && !POwner->PAI->IsCurrentState<CMagicState>();
    const bool free  = quiet && !POwner->PAI->IsEngaged() && !InManeuver() && PartyFightTarget() == nullptr && m_Mode != Mode::Pull;
    if (puller)
    {
        // The seat newly hers, or the party's pulling switched on with her in
        // it: whether she has anything to pull with is told at the next quiet look
        const bool pulling = IsHunting() && m_Stake.has_value();
        if (!m_KitSeat || (pulling && !m_KitPulling))
        {
            m_KitTellOwed = true;
        }
        m_KitSeat    = true;
        m_KitPulling = pulling;
        if (quiet)
        {
            const bool wasReady = m_KitReady;
            const auto worn     = pawn::pull::wearKit(PChar, free);
            auto       line = worn.said;
            if (line.empty() && !worn.ready)
            {
                line = "cannot make a ranged attack in what she wears";
            }
            // said once: a kit the game keeps refusing, or a Bard's song, is the same line every look
            if (line.empty())
            {
                m_KitSaid.clear();
            }
            else if (line != m_KitSaid)
            {
                m_KitSaid = line;
                ShowInfoFmt("pull: {} {}", POwner->getName(), line);
            }
            m_KitReady = worn.ready;
            // Nothing to pull with, said to the party as she takes the seat,
            // every time pulling is switched on, and as she runs out (the
            // user, 2026-10-18): no cooldown
            if (worn.nothing && (m_KitTellOwed || wasReady))
            {
                pawn::tactics::role::sayParty(PChar, std::string(xirand::GetRandomElement(cardian::pull::kNoKitLines)));
            }
            m_KitTellOwed = false;
        }
        const auto tool = pawn::pull::toolOf(PChar);
        m_PullItem      = tool.item;
        m_PullSpell     = tool.spell;
        return;
    }
    m_KitReady    = false;
    m_KitSeat     = false;
    m_KitPulling  = false;
    m_KitTellOwed = false;
    m_KitSaid.clear();
    m_PullItem  = 0;
    m_PullSpell = 0;
    if (free && pawn::pull::wearsKit(PChar))
    {
        if (const auto said = pawn::pull::putBack(PChar); !said.empty())
        {
            ShowInfoFmt("pull: {} {} (she no longer holds the Puller seat)", POwner->getName(), said);
        }
    }
}

auto CPawnController::PullBadge() const -> cardian::pull::Badge
{
    using cardian::pull::Badge;
    // the cheap tests first: this is asked of every cardian every tick
    if (POwner->isDead() || !IsHunting() || !Staked() || !PullerSeat())
    {
        return Badge::None;
    }
    if (m_Pull.has_value())
    {
        return m_Pull->step == PullRun::Step::Back ? Badge::Bringing : Badge::Out;
    }
    return Badge::Waiting;
}

auto CPawnController::PullStateLine() const -> cl_pull_state
{
    auto line    = cardian::link::make<cl_pull_state>();
    line.cardian = POwner->id;
    line.state   = static_cast<uint8_t>(PullBadge());
    if (line.state != CL_PULL_NONE)
    {
        line.item  = m_PullItem;
        line.spell = m_PullSpell;
    }
    return line;
}

void CPawnController::ShowPullState()
{
    // Told once at the least, so a cardian standing again clears a badge her
    // last body left on his screen; then as it changes
    const auto line = PullStateLine();
    if (m_PullShownOnce && line.state == m_PullShown.state && line.item == m_PullShown.item && line.spell == m_PullShown.spell)
    {
        return;
    }
    if (const auto owner = pawn::ordersOwnerOf(static_cast<const CCharEntity*>(POwner)); owner != 0)
    {
        m_PullShown     = line;
        m_PullShownOnce = true;
        cardian::link::send(owner, line);
    }
}

auto CPawnController::PullReach(CMobEntity* PMob) const -> float
{
    if (m_PullSpell != 0)
    {
        auto* PSpell = spell::GetSpell(static_cast<SpellID>(m_PullSpell));
        return PSpell != nullptr ? pawn::tactics::bank::castRange(POwner, PSpell, PMob) : 0.0f;
    }
    return POwner->GetRangedAttackRange();
}

auto CPawnController::PullBalls(const CMobEntity* PMob) const -> std::vector<pawn::danger::Sphere>
{
    auto profile        = pawn::danger::Profile::party(static_cast<CCharEntity*>(POwner));
    profile.extraBuffer = cardian::rules::kClearance;
    const float reach   = cardian::pull::shotReach(PullReach(const_cast<CMobEntity*>(PMob)));
    const float scan    = std::max(settings::get<float>("pawn.AVOID_SCAN"), distance(POwner->loc.p, PMob->loc.p) + reach + 10.0f);
    return pawn::danger::spheres(pawn::entitiesAround(POwner), POwner->loc.p, scan, profile, PMob);
}

namespace
{
    constexpr float kSpotFloor  = 8.0f; // a spot of the rings set on the floor found this far above or below the mob's height
    constexpr int   kSpotRoutes = 3;    // the routes asked for, one a ring, before the best clean spot is taken as it is
    constexpr float kVisitSlack = 2.0f; // a mob's rest taken to end this many seconds early, for the spot she waits beside (the sneak-by's slack)

    // Whether the game's sight line runs from `from` to `to`, as a body at
    // `from` looking at one at `to` asks it (CBaseEntity::CanSeeTarget's ray,
    // the shooter's way round: the ray is not the same both ways)
    auto seesFrom(const CBattleEntity* POwner, const position_t& from, const position_t& to) -> bool
    {
        constexpr float kEyes = 2.0f; // CanSeeTarget's ENTITY_HEIGHT
        const auto*     mesh  = POwner->loc.zone != nullptr ? POwner->loc.zone->xiMesh() : nullptr;
        return mesh == nullptr || !mesh->rayIntersect(Vector3{ from.x, from.y - kEyes, from.z }, Vector3{ to.x, to.y - kEyes, to.z });
    }

    // The spots of the rings round a mob (pull_math.h shotRings) she could
    // shoot it from: set on the floor, within her reach of it and in her
    // sight of it from there. Farthest ring first, each ring's nearest her
    // first
    auto spotsInSight(CBattleEntity* POwner, CMobEntity* PMob, const float reach) -> std::vector<std::vector<position_t>>
    {
        std::vector<std::vector<position_t>> out;
        for (auto& ring : cardian::pull::shotRings(PMob->loc.p, std::max(1.0f, reach - 0.5f), POwner->loc.p))
        {
            std::vector<position_t> kept;
            for (auto spot : ring)
            {
                const auto floor = pawn::travel::floorUnder(POwner->getZone(), spot, kSpotFloor);
                if (!floor.has_value())
                {
                    continue;
                }
                spot.y = *floor;
                if (distance(spot, PMob->loc.p) <= reach && seesFrom(POwner, spot, PMob->loc.p))
                {
                    kept.push_back(spot);
                }
            }
            if (!kept.empty())
            {
                out.push_back(std::move(kept));
            }
        }
        return out;
    }
} // namespace

auto CPawnController::ShotSpotFor(const CMobEntity* PMob, const std::vector<pawn::danger::Sphere>& all) const -> std::optional<ShotSpot>
{
    auto*       PTarget = const_cast<CMobEntity*>(PMob);
    const float reach   = cardian::pull::shotReach(PullReach(PTarget));
    const auto  spots   = spotsInSight(POwner, PTarget, reach);
    if (spots.empty())
    {
        return std::nullopt;
    }

    // The balls but the mob's own, as the route search reads them
    std::vector<pawn::danger::Sphere> balls;
    for (const auto& b : all)
    {
        if (b.mob != PMob)
        {
            balls.push_back(b);
        }
    }
    const auto                        hazards = pawn::walker::toHazards(balls);
    const cardian::route::HazardSight seen    = [&](const size_t h, const position_t& p) { return SeesBall(balls[h], p); };
    // The mobs as a sneak-by reads them, for the spot inside a ring she
    // waits beside: one no mob reaches as it rests and faces now is where a
    // visit in behind their backs can go (walker.h visitTo)
    std::vector<cardian::sneak::Mob> mobs;
    for (const auto& b : balls)
    {
        if (b.mob != nullptr)
        {
            mobs.push_back(pawn::walker::sneakMobOf(b, m_Tick));
        }
    }

    // The farthest spot outside every ring whose way in passes none, a route
    // asked a ring; else the farthest outside every ring, which she walks to
    // as far as the way allows and waits; else, inside a ring, the farthest
    // no mob reaches now, then the farthest in sight at all, which she waits
    // beside
    constexpr std::string_view kUnasked = "aggro may hold her way to it";
    std::optional<ShotSpot>    held;
    std::optional<ShotSpot>    behind;
    std::optional<ShotSpot>    inside;
    int                        asked = 0;
    for (const auto& ring : spots)
    {
        bool routed = false;
        for (const auto& spot : ring)
        {
            if (const auto* guard = BallHolding(balls, nullptr, spot, true); guard != nullptr)
            {
                const auto why = fmt::format("every spot within her reach of it is in {}'s circle", guard->getName());
                if (!behind.has_value() && std::none_of(mobs.begin(), mobs.end(), [&](const cardian::sneak::Mob& m) { return m.threatens(spot, 0.5f, kVisitSlack); }))
                {
                    behind = ShotSpot{ spot, false, false, why };
                }
                if (!inside.has_value())
                {
                    inside = ShotSpot{ spot, false, false, why };
                }
                continue;
            }
            if (balls.empty())
            {
                return ShotSpot{ spot, true, true, "" };
            }
            if (routed || asked >= kSpotRoutes)
            {
                if (!held.has_value())
                {
                    held = ShotSpot{ spot, true, false, std::string(kUnasked) };
                }
                continue;
            }
            routed = true;
            ++asked;
            const auto route = pawn::travel::routeAvoiding(POwner->getZone(), POwner->loc.p, spot, hazards, seen,
                                                           distance(POwner->loc.p, spot) * 1.5f + pawn::walker::kLocalSpare);
            if (!route.has_value() || !route->complete)
            {
                continue; // ground no walk of hers reaches
            }
            if (route->firstDanger < 0)
            {
                return ShotSpot{ spot, true, true, "" };
            }
            if (!held.has_value() || held->why == kUnasked)
            {
                const auto  d     = static_cast<size_t>(route->firstDanger);
                const auto& a     = d == 0 ? POwner->loc.p : route->points[d - 1];
                const int   h     = cardian::route::DangerTest(hazards, cardian::route::toDetour(POwner->loc.p), seen).holder(a, route->points[d]);
                const auto  guard = h >= 0 ? std::string(balls[h].mob->getName()) : std::string("a mob");
                held              = ShotSpot{ spot, true, false, fmt::format("the way in crosses {}'s circle", guard) };
            }
        }
    }
    if (held.has_value())
    {
        return held;
    }
    return behind.has_value() ? behind : inside;
}

auto CPawnController::VisitAllowed(CMobEntity* PMob, const position_t& spot) -> bool
{
    // Her shot goes the moment she stands in behind their backs: the
    // pipeline's time to tag it, read afresh, and no kin on its way to her
    // nor on her way back, are asked first
    const auto timing = PullTimingNow();
    if (!timing.ready)
    {
        return false;
    }
    m_Pull->timingReady = true;
    m_Pull->arriveAt    = m_Tick + after(timing.arriveIn);
    const auto   landing = nearPosition(m_Stake->at, cardian::stake::kMobAhead, 0.0f);
    const double pace    = POwner->GetSpeed() / 17.0;
    const double walkIn  = pace > 0.0 ? distance(POwner->loc.p, spot) * cardian::pull::kWayFactor / pace : 1e9;
    if (cardian::pull::shootIn(timing.arriveIn, distance(spot, landing), pace) > walkIn + 1.0)
    {
        return false;
    }
    return pawn::pull::wayBackKin(PMob, PMob->loc.p, spot) == nullptr && pawn::pull::wayBackKin(PMob, spot, landing) == nullptr;
}

auto CPawnController::SingPull(CMobEntity* PMob) -> bool
{
    // Straight to the cast: the party's rule that holds spells on a pull
    // still on its way to the camp (HoldsFireOn) is for the others, never
    // for the pull itself
    FaceTarget(EntityId(PMob));
    HeadLook(PMob);
    return CastAndStop(EntityId(PMob), static_cast<SpellID>(m_PullSpell));
}

auto CPawnController::PullTimingNow() -> PullTiming
{
    namespace pull = cardian::pull;
    PullTiming  out;
    auto*       PPawn = static_cast<CCharEntity*>(POwner);
    const auto* PRun  = m_Pull.has_value() ? m_Pull->mob.resolve<CMobEntity>() : nullptr;

    // One mob on the party at a time, her own pull aside, and none the
    // party leaves be
    const auto               owner = pawn::ordersOwnerOf(static_cast<const CCharEntity*>(POwner));
    std::vector<CMobEntity*> fights;
    pawn::forEachMobNear(pawn::entitiesAround(POwner), m_Stake->at, pull::kPullReach, [&](CMobEntity* PMob)
                         {
                             if (PMob != PRun && PMob->isAlive() && PMob->PAI->IsEngaged() && ofParty(POwner, PMob->GetBattleTarget()) &&
                                 !pawn::leftBehind(owner, PMob))
                             {
                                 fights.push_back(PMob);
                             }
                         });
    if (fights.size() > 1)
    {
        out.why = fmt::format("{} mobs are on the party", fights.size());
        return out;
    }
    // Leaving a fight early leaves its mob to a tank: with nobody in the
    // Tank seat she finishes the fight first (the user, 2026-10-16)
    if (!fights.empty() && PPawn->PParty != nullptr)
    {
        const bool tank = std::ranges::any_of(PPawn->PParty->members, [&](CBattleEntity* PMember)
                                              {
                                                  return PMember != POwner && PMember->objtype == TYPE_PC && PMember->isAlive() && PMember->loc.zone == POwner->loc.zone &&
                                                         pawn::roster::roleOf(static_cast<CCharEntity*>(PMember)) == cardian::party::Role::Tank;
                                              });
        if (!tank)
        {
            out.why = fmt::format("nobody holds the Tank seat: the fight on {} is finished first", fights.front()->getName());
            return out;
        }
    }

    // The party's healers: every member in the Healer seat with MP, the
    // player too, as one pool (pull_math.h poolReadyAt). A Damage seat's MP
    // never holds the pull (the user, 2026-10-27). Each is taken to kneel
    // between fights, kneeling now or once the fight ends. A healer the
    // tactician has measured is readied as her own rest readies her, for a
    // fight here and a link's reserve; else kUnknownCost of her pool
    std::vector<pull::Mage>  mages;
    std::vector<std::string> names;
    std::vector<std::size_t> curers;         // the healers who cure, by their place in `mages`
    double                   missingHp = 0; // the party's, herself and the player included
    const double             now       = pawn::tactics::restSeconds(timer::now());
    if (PPawn->PParty != nullptr)
    {
        for (auto* PMember : PPawn->PParty->members)
        {
            if (PMember->loc.zone != POwner->loc.zone || PMember->objtype != TYPE_PC)
            {
                continue;
            }
            if (PMember->isDead())
            {
                out.why = fmt::format("{} is down", PMember->getName());
                return out;
            }
            // what a Regen she wears will heal is no healer's to spend
            missingHp += std::max(0, PMember->GetMaxHP() - PMember->health.hp - pawn::tactics::role::regenLeft(PMember));
            if (PMember == POwner || PMember->GetMaxMP() <= 0 ||
                pawn::roster::roleOf(static_cast<CCharEntity*>(PMember)) != cardian::party::Role::Healer)
            {
                continue;
            }
            const auto  advice  = pawn::tactics::restAdvice(static_cast<CCharEntity*>(PMember));
            const bool  cures   = advice.has_value() && advice->cheapestCureMp > 0.0;
            const auto* healing = PMember->StatusEffectContainer->GetStatusEffect(xi::StatusEffect::Healing);
            pull::Mage m;
            m.mp              = PMember->health.mp;
            m.maxMp           = PMember->GetMaxMP();
            m.need            = pull::kUnknownCost * m.maxMp;
            m.interval        = settings::get<uint8>("map.HEALING_TICK_DELAY");
            m.clearMind       = PMember->getMod(xi::Mod::CLEAR_MIND);
            m.mpHeal          = PMember->getMod(xi::Mod::MPHEAL);
            if (advice.has_value())
            {
                m.need           = std::max(advice->knownCost ? advice->readyMp : m.need, advice->cheapestCureMp);
                m.spendPerSecond = advice->spentPerSecond;
            }
            if (cures)
            {
                curers.push_back(mages.size());
            }
            m.need = std::min(m.need, m.maxMp);
            if (healing != nullptr)
            {
                const auto clock = pawn::tactics::kneelClock(healing, now);
                m.kneeling       = true;
                m.ticks          = clock.ticks;
                m.nextTick       = clock.next;
                m.interval       = clock.interval;
            }
            mages.push_back(m);
            names.emplace_back(PMember->getName());
        }
    }

    double fightLeft = 0.0;
    if (!fights.empty())
    {
        fightLeft = pawn::tactics::lifeLeft(PPawn, fights.front());
        if (fightLeft < 0.0)
        {
            out.why = fmt::format("the fight on {} has no length yet", fights.front()->getName());
            return out;
        }
    }
    // Between fights, the party's missing HP is MP its healers have yet to
    // spend: each who cures owes an even share, counted against the pool
    // (the user, 2026-10-18), so a messy fight's wounds hold the next pull
    // back. With a fight on, its cures are already in their spending
    if (fights.empty())
    {
        const double each = pull::hpDebtEach(missingHp, pawn::tactics::cureExchange(PPawn).hpPerMp, curers.size());
        for (const auto i : curers)
        {
            mages[i].debt = each;
        }
        out.missingHp = curers.empty() ? 0.0 : missingHp;
        out.debtEach  = each;
    }
    // A healer keeps kneeling into the next fight only after one that went
    // as planned: after a link, she stands ready for it
    const auto arrival = pull::wantArrive(mages, fightLeft, pawn::tactics::lastFightLinked(PPawn) ? 0.0 : pull::kRestInto);
    out.ready          = true;
    out.fightLeft      = fightLeft;
    out.arriveIn       = arrival.in;
    if (arrival.paced)
    {
        out.pacer = names.size() == 1 ? names.front() + "'s MP" : "the healers' MP";
        for (std::size_t i = 0; i < mages.size(); ++i)
        {
            out.healers += fmt::format("{}{} {:.0f}/{:.0f}", i == 0 ? "" : ", ", names[i], mages[i].mp, mages[i].maxMp);
        }
    }
    return out;
}

auto CPawnController::MobOnHer() const -> CMobEntity*
{
    // A mob the party leaves be holds her back only while she stands within
    // its spells' reach (kLeftBehindReach)
    const auto* PRun  = m_Pull.has_value() ? m_Pull->mob.resolve<CMobEntity>() : nullptr;
    const auto  owner = pawn::ordersOwnerOf(static_cast<const CCharEntity*>(POwner));
    CMobEntity* PFound = nullptr;
    pawn::forEachMobNear(pawn::entitiesAround(POwner), POwner->loc.p, 30.0f, [&](CMobEntity* PMob)
                         {
                             if (PFound == nullptr && PMob != PRun && PMob->isAlive() && PMob->PAI->IsEngaged() && PMob->GetBattleTarget() == POwner &&
                                 !(distance(POwner->loc.p, PMob->loc.p) > cardian::pull::kLeftBehindReach && pawn::leftBehind(owner, PMob)))
                             {
                                 PFound = PMob;
                             }
                         });
    return PFound;
}

auto CPawnController::CampFight() const -> CMobEntity*
{
    if (!m_Stake.has_value())
    {
        return nullptr;
    }
    // A mob the party leaves be is no fight of the camp's
    const auto  owner  = pawn::ordersOwnerOf(static_cast<const CCharEntity*>(POwner));
    CMobEntity* PFound = nullptr;
    pawn::forEachMobNear(pawn::entitiesAround(POwner), m_Stake->at, cardian::pull::kPullReach, [&](CMobEntity* PMob)
                         {
                             if (PFound == nullptr && PMob->isAlive() && PMob->PAI->IsEngaged() && ofParty(POwner, PMob->GetBattleTarget()) &&
                                 !pawn::leftBehind(owner, PMob))
                             {
                                 PFound = PMob;
                             }
                         });
    return PFound;
}

auto CPawnController::PullDue(CCharEntity* PPlayer) -> CMobEntity*
{
    namespace pull = cardian::pull;
    // A look stands for a second: the fight's tick and the roam tick both ask
    if (m_Tick < m_PullLookAt)
    {
        auto* PGo = m_PullGo.has_value() ? m_PullGo->resolve<CMobEntity>() : nullptr;
        return PGo != nullptr && fairGame(PGo) ? PGo : nullptr;
    }
    // Not looked for a while -- pulling was off, or she was not the
    // puller: what she held for before is said again
    if (m_Tick - m_PullLookAt > std::chrono::seconds(5))
    {
        m_PullHoldSaid.clear();
    }
    m_PullLookAt = m_Tick + std::chrono::seconds(1);
    m_PullGo.reset();

    const auto  owner = pawn::ordersOwnerOf(static_cast<const CCharEntity*>(POwner));
    const auto  list  = pawn::pullListOf(owner);
    std::string blocker;
    if (!m_KitReady)
    {
        // her kit's own lines say this in party chat (kNoKitLines)
        blocker = m_KitSaid.empty() ? std::string("she has nothing to pull with yet") : m_KitSaid;
    }
    else if (!pull::anyOn(list))
    {
        blocker = "the Pull page has no kind of mob on";
        SayHold("list", "Nothing is on the pull list, so no pulling for now.");
    }
    else if (const auto* POnHer = MobOnHer(); POnHer != nullptr)
    {
        blocker = fmt::format("{} is on her", POnHer->getName());
    }
    else if (const auto* PFight = !pull::kLeavesFightsEarly ? CampFight() : nullptr; PFight != nullptr)
    {
        // With the early leave off she finishes the party's fight first --
        // the mob she just handed to the tank among them -- and helps kill it
        blocker = fmt::format("the fight on {} comes first", PFight->getName());
    }
    PullTiming timing;
    if (blocker.empty())
    {
        timing = PullTimingNow();
        if (!timing.ready)
        {
            blocker = timing.why;
            SayHold(timing.why, fmt::format("Holding the next pull: {}.", timing.why));
        }
    }
    if (!blocker.empty())
    {
        SayPull(fmt::format("waits to pull: {}", blocker));
        return nullptr;
    }

    // The next pull, picked again every few seconds or once it is no longer
    // fair game. The Pull page's list decides, never the Orders page's band,
    // and a pull never takes aggressive company or links along: the hunt
    // rules' defaults, both off
    auto* PMob = m_PullCandidate.has_value() ? m_PullCandidate->resolve<CMobEntity>() : nullptr;
    if (PMob == nullptr || !fairGame(PMob) || m_Tick - m_PullCandidateAt > std::chrono::seconds(5))
    {
        std::erase_if(m_PullPassOver, [&](const auto& passed) { return passed.second <= m_Tick; });
        std::vector<uint32> passOver;
        for (const auto& [id, until] : m_PullPassOver)
        {
            passOver.push_back(id);
        }
        pawn::HuntRules rules;
        rules.aggressive = false;
        rules.links      = false;
        std::string skipped;
        std::string waits;
        PMob               = PickHuntTarget(m_Stake->at, pawn::pull::conLevel(PPlayer), rules, &skipped, passOver, list, pull::kPullReach, false, &waits);
        m_PullCandidate    = PMob != nullptr ? std::optional<EntityId>(EntityId(PMob)) : std::nullopt;
        m_PullCandidateAt  = m_Tick;
        m_PullSkipped      = skipped;
        m_PullWaits        = waits;
        if (PMob == nullptr)
        {
            // a look that found nothing is made again a little later
            m_PullLookAt = m_Tick + after(pull::kQuietSeconds);
            SayPull("waits to pull: nothing on the list stands within reach");
            SayHold("nothing", "Nothing on the pull list is in reach right now. Keeping watch.");
            // said now and then: the kinds he has on may all be away, or fighting
            if (m_Tick - m_PullQuietAt > std::chrono::seconds(15))
            {
                m_PullQuietAt = m_Tick;
                std::string kinds;
                for (const auto& row : list)
                {
                    if (row.on)
                    {
                        kinds += (kinds.empty() ? "" : ", ") + row.name;
                    }
                }
                ShowInfoFmt("pull: {} finds nothing to pull within {:.0f} y of the camp (on the list: {}){}", POwner->getName(), pull::kPullReach, kinds,
                            skipped.empty() ? std::string() : "; skipped " + skipped);
            }
            return nullptr;
        }
    }

    // Backward from when it should land: the walk out to where she tags it,
    // the tag, and the run back to the landing point
    const double pace    = POwner->GetSpeed() / 17.0;
    const float  reach   = pull::shotReach(PullReach(PMob));
    const auto   landing = nearPosition(m_Stake->at, cardian::stake::kMobAhead, 0.0f);
    const float  out     = std::max(0.0f, distance(POwner->loc.p, PMob->loc.p) - reach);
    const float  back    = std::max(0.0f, distance(PMob->loc.p, landing) - reach);
    // A mob she will wait near (m_PullWaits) is set out for kWaitLead early:
    // how long aggro holds it is not known, and at its spot her shot waits
    // for its time to tag it all the same (the user, 2026-10-25)
    const double leave   = pull::leaveIn(timing.arriveIn, out, back, pace) - (m_PullWaits.empty() ? 0.0 : pull::kWaitLead);
    if (leave > 0.0)
    {
        SayPull(fmt::format("waits to pull: {} is next, timed to {}", PMob->getName(), timing.pacer.empty() ? std::string("the fight's end") : timing.pacer));
        // A wait of a few seconds is the walk's own timing, not a hold
        if (!timing.pacer.empty() && leave >= pull::kHoldSaidFrom)
        {
            SayHold("mp", fmt::format("Heading out for the next pull in about {:.0f} seconds, once the healers have the MP for it.", leave));
        }
        if (m_Tick - m_PullQuietAt > std::chrono::seconds(10))
        {
            m_PullQuietAt = m_Tick;
            ShowInfoFmt("pull: {} sets out after {} in {:.0f} s: it should land in {:.0f} s (the fight has {:.0f} s left{}{}); her way is {:.0f} y out and {:.0f} y back",
                        POwner->getName(), PMob->getName(), leave, timing.arriveIn, timing.fightLeft,
                        timing.pacer.empty() ? std::string() : fmt::format(", paced by {} ({})", timing.pacer, timing.healers),
                        timing.debtEach > 0.0 ? fmt::format(", the party's {:.0f} missing HP costing each curer {:.0f} MP", timing.missingHp, timing.debtEach) : std::string(),
                        out, back);
        }
        return nullptr;
    }
    m_PullGo       = EntityId(PMob);
    m_PullGoArrive = timing.arriveIn;
    m_PullHoldSaid.clear();
    return PMob;
}

auto CPawnController::PullInstead(CCharEntity* PPlayer, const CMobEntity* PHeld) -> CMobEntity*
{
    if (PPlayer == nullptr || !m_Stake.has_value())
    {
        return nullptr;
    }
    std::erase_if(m_PullPassOver, [&](const auto& passed) { return passed.second <= m_Tick; });
    std::vector<uint32> passOver{ PHeld->id };
    for (const auto& [id, until] : m_PullPassOver)
    {
        passOver.push_back(id);
    }
    pawn::HuntRules rules;
    rules.aggressive = false;
    rules.links      = false;
    std::string skipped;
    const auto  list  = pawn::pullListOf(pawn::ordersOwnerOf(static_cast<const CCharEntity*>(POwner)));
    auto*       PElse = PickHuntTarget(m_Stake->at, pawn::pull::conLevel(PPlayer), rules, &skipped, passOver, list, cardian::pull::kPullReach, true);
    if (PElse != nullptr)
    {
        // the next set-out takes it as its candidate
        m_PullCandidate   = EntityId(PElse);
        m_PullCandidateAt = m_Tick;
        m_PullSkipped     = skipped;
        m_PullWaits.clear();
    }
    return PElse;
}

auto CPawnController::PullCanGo() -> bool
{
    // One test for setting out, from a fight or from idle, so she never
    // leaves a fight she cannot set out from (and joins it again): free of
    // her own action and walk in, no rest of his or her Rest row's due, and
    // able to walk
    return !m_Approach.has_value() && !m_RestOrder.active() && !RestRowDue() && !Acting() && POwner->PAI->CanFollowPath() && POwner->GetSpeed() > 0;
}

auto CPawnController::PullLeavesFight() -> bool
{
    // Only the puller of a camp under Pull, on no run yet, free to go. His
    // own Attack order on the fight keeps her in it until it ends
    if (m_Pull.has_value() || POwner->isDead() || !IsHunting() || !Staked() || !PullerSeat() || m_Waiting || m_PlayersOrder.has_value() || !PullCanGo())
    {
        return false;
    }
    auto* PPlayer = GetLivePlayer();
    if (PPlayer == nullptr || CurrentPlace(PPlayer) == nullptr)
    {
        return false;
    }
    auto* PMob = PullDue(PPlayer);
    if (PMob == nullptr)
    {
        return false;
    }
    const auto* PFight = POwner->GetBattleTarget();
    ShowInfoFmt("pull: {} leaves the fight on {} to pull {} (it should land in {:.0f} s)", POwner->getName(),
                PFight != nullptr ? PFight->getName() : std::string("its mob"), PMob->getName(), m_PullGoArrive);
    StandDown(fmt::format("leaves the fight to pull {}", PMob->getName()));
    return true;
}

auto CPawnController::PullTick(const Place* place, CCharEntity* PPlayer, const bool hunting) -> bool
{
    namespace pull = cardian::pull;
    using Step     = PullRun::Step;

    if (!m_Pull.has_value())
    {
        // Set out: she alone hunts at a camp (pull_math.h hunts), nothing of
        // her own under way, when the pipeline says the next pull is due
        // (PullDue): something to pull with, a kind on the player's list
        // within reach, and the time come to go
        if (!hunting || !Staked() || place == nullptr || PPlayer == nullptr)
        {
            m_PullSaid.clear();
            m_PullGo.reset();
            return false;
        }
        if (!PullCanGo())
        {
            // A puller kept down by her Rest row -- a mage in the seat --
            // sets out only when she is full: a mage puller is not supported
            // yet, and the seat does not override her row as it does her own
            // kneel (the user, 2026-10-17; OPEN_ISSUES #518)
            if (m_RestOrder.active() || RestRowDue())
            {
                SayPull("waits to pull: her Rest row keeps her down until she is full (a mage puller is not supported yet, #518)");
            }
            return false;
        }
        auto* PMob = PullDue(PPlayer);
        if (PMob == nullptr)
        {
            return false;
        }
        // kneeling by her own choice: up, the pull is due
        if (POwner->StatusEffectContainer->HasStatusEffect(xi::StatusEffect::Healing))
        {
            StandFromRest("the next pull is due");
            return false;
        }
        const auto  level = pawn::pull::conLevel(PPlayer);
        const float out   = distance(m_Stake->at, PMob->loc.p);
        m_Pull            = PullRun{};
        m_Pull->mob       = EntityId(PMob);
        m_Pull->setOffAt  = m_Tick + ReactionBeat();
        m_Pull->stepAt    = m_Tick;
        m_Pull->outLimit  = pull::outSeconds(out);
        m_Pull->backLimit = pull::backSeconds(out);
        m_Pull->arriveAt  = m_Tick + after(m_PullGoArrive);
        // the camp's fights as she sets out, none the party leaves be
        const auto owner = pawn::ordersOwnerOf(static_cast<const CCharEntity*>(POwner));
        pawn::forEachMobNear(pawn::entitiesAround(POwner), m_Stake->at, pull::kPullReach, [&](CMobEntity* POther)
                             {
                                 if (POther->PAI->IsEngaged() && ofParty(POwner, POther->GetBattleTarget()) && !pawn::leftBehind(owner, POther))
                                 {
                                     m_Pull->camp.push_back(POther->id);
                                 }
                             });
        m_PullGo.reset();
        m_PullCandidate.reset();
        m_PullSaid.clear();
        const auto check = static_cast<uint8>(charutils::CheckMob(level, PMob));
        ShowInfoFmt("pull: {} sets out after {} ({} at level {}, {:.0f} y from the camp, {:.0f} y from her; to land in {:.0f} s){}{}", POwner->getName(),
                    PMob->getName(), pull::conWord(check), level, out, distance(POwner->loc.p, PMob->loc.p), m_PullGoArrive,
                    m_PullWaits.empty() ? std::string() : fmt::format("; she will wait near it ({})", m_PullWaits),
                    m_PullSkipped.empty() ? std::string() : "; passed over " + m_PullSkipped);
        Transition(Mode::Pull, fmt::format("sets out after {}", PMob->getName()));
        HeadLook(PMob);
        return true;
    }

    auto*      PMob    = m_Pull->mob.resolve<CMobEntity>();
    const auto onParty = [&](const CBattleEntity* PWho) { return ofParty(POwner, PWho); };
    if (PMob == nullptr || PMob->isDead())
    {
        EndPull(PMob == nullptr ? std::string("the mob is gone") : fmt::format("{} is dead", PMob->getName()));
        return false;
    }
    if (m_Retreat)
    {
        EndPull("the retreat is called");
        return false;
    }
    bool           fighting = PMob->PAI->IsEngaged();
    CBattleEntity* POn      = fighting ? PMob->GetBattleTarget() : nullptr;

    if (m_Pull->step != Step::Back)
    {
        // Before it is hers, the run ends with the camp, the order or the
        // seat -- but once she has fired at it, his hunt turned off stops
        // her next pull, not this one: the mob coming for her is brought in
        // (the user, 2026-10-17, after one she had shot at was fought where
        // she stood). It is never shot at again (below)
        if (!Staked() || (!hunting && m_Pull->shots == 0))
        {
            EndPull(!Staked() ? "the camp is gone" : "the party's hunt is off");
            return false;
        }
        // The camp's own mob turned on her after she set out (the tank lost
        // it): she drops the pull and fights it at camp, rather than lead it
        // into the field
        CMobEntity* PCampMob = nullptr;
        for (const auto id : m_Pull->camp)
        {
            if (auto* POther = dynamic_cast<CMobEntity*>(zoneutils::GetEntity(id, TYPE_MOB));
                POther != nullptr && POther->isAlive() && POther->PAI->IsEngaged() && POther->GetBattleTarget() == POwner)
            {
                PCampMob = POther;
                break;
            }
        }
        if (PCampMob != nullptr)
        {
            ShowInfoFmt("pull: {} drops the pull on {}: {} turned on her at the camp", POwner->getName(), PMob->getName(), PCampMob->getName());
            EndPull(fmt::format("{} turned on her", PCampMob->getName()));
            return false;
        }
        // Caught on the way by another mob: that one comes in instead, as
        // any pull would (P1: what joins her comes too). A mob the party
        // was already fighting as she set out is the camp's, not a catch
        // A mob the party already leaves be is no catch
        const auto  owner   = pawn::ordersOwnerOf(static_cast<const CCharEntity*>(POwner));
        CMobEntity* PCaught = nullptr;
        if (!onParty(POn))
        {
            pawn::forEachMobNear(pawn::entitiesAround(POwner), POwner->loc.p, 30.0f, [&](CMobEntity* POther)
                                 {
                                     if (PCaught == nullptr && POther != PMob && POther->isAlive() && POther->PAI->IsEngaged() &&
                                         POther->GetBattleTarget() == POwner && std::ranges::find(m_Pull->camp, POther->id) == m_Pull->camp.end() &&
                                         !pawn::leftBehind(owner, POther))
                                     {
                                         PCaught = POther;
                                     }
                                 });
        }
        // One that cannot chase her -- a worm -- caught her off the camp's
        // ground can never be brought in: the party leaves it be, and she
        // gives her pull up and comes back (the user, 2026-10-25)
        if (PCaught != nullptr && !pawn::chases(PCaught))
        {
            if (!pawn::onCampGround(owner, PCaught))
            {
                pawn::leaveBehind(owner, PCaught, fmt::format("it cannot chase, and caught {} off the camp's ground", POwner->getName()));
                GiveUpPull(PMob, fmt::format("{} caught her on the way and cannot be brought in", PCaught->getName()));
                return false;
            }
        }
        if (PCaught != nullptr)
        {
            ShowInfoFmt("pull: {} is caught by {} on her way to {}; brings it in instead", POwner->getName(), PCaught->getName(), PMob->getName());
            m_Pull->mob = EntityId(PCaught);
            PMob        = PCaught;
            fighting    = true;
            POn         = POwner;
        }
        else if (fighting && !onParty(POn))
        {
            GiveUpPull(PMob, "another got to it first");
            return false;
        }
        if (onParty(POn))
        {
            // Hers, or on the party: she runs it in. Her pull line went as her
            // first shot left; a mob that caught her on the way, coming in
            // instead, is called now
            const uint8 call  = pawn::pull::callOf(pawn::ordersOwnerOf(static_cast<const CCharEntity*>(POwner)));
            const uint8 level = PPlayer != nullptr ? pawn::pull::conLevel(PPlayer) : POwner->GetMLevel();
            const auto  check = static_cast<uint8>(charutils::CheckMob(level, PMob));
            if (m_Pull->called != PMob->id)
            {
                CallPull(PMob, PPlayer);
            }
            pawn::tactics::notePull(static_cast<CCharEntity*>(POwner), PMob);
            ShowInfoFmt("pull: {} brings {} in ({} at level {}, on {}, {:.0f} y out{})", POwner->getName(), PMob->getName(), pull::conWord(check), level,
                        POn != nullptr ? POn->getName() : std::string("nobody"), distance(m_Stake->at, PMob->loc.p),
                        call != cardian::chat::kNoCall ? fmt::format(", call {}", call) : std::string());
            m_Pull->step   = Step::Back;
            m_Pull->stepAt = m_Tick;
            m_Pull->backTo = pull::pastLanding(nearPosition(m_Stake->at, cardian::stake::kMobAhead, 0.0f), PMob->loc.p);
        }
    }

    if (m_Pull->step == Step::Out)
    {
        HeadLook(PMob);
        if (m_Tick < m_Pull->setOffAt)
        {
            return true;
        }
        const float reach   = pull::shotReach(PullReach(PMob));
        const bool  inReach = distance(POwner->loc.p, PMob->loc.p) <= reach && POwner->CanSeeTarget(PMob);
        const bool  waiting = m_Pull->blockedSince != timer::time_point{};

        // Her spot (ShotSpotFor): as far off as her shot reaches, outside
        // every ring when one is, chosen as she sets out and again once the
        // mob has roamed off where it stood then. Waiting, it is weighed again
        // every few seconds and taken only when it is better, so her walker's
        // goal stands still while the way stays shut; never in the middle of
        // a visit, whose goal it is
        if (m_Tick >= m_Pull->cleanAt && !Visiting())
        {
            m_Pull->cleanAt  = m_Tick + std::chrono::seconds(waiting ? 3 : 1);
            const bool roamed = !m_Pull->spot.has_value() || distance(m_Pull->spotFor, PMob->loc.p) > pull::kSpotRoam || m_Pull->respot;
            if (roamed || waiting)
            {
                const auto spot = ShotSpotFor(PMob, PullBalls(PMob));
                if (!spot.has_value())
                {
                    GiveUpPull(PMob, "no ground within her reach of it is in its sight");
                    return false;
                }
                const auto rank = [](const ShotSpot& s) { return s.open ? 2 : s.clean ? 1 : 0; };
                // a ring over her spot that gives her the same spot again:
                // she walks on toward the mob, as for a spot she cannot use
                if (m_Pull->respot && m_Pull->spot.has_value() && distance(spot->at, m_Pull->spot->at) < 1.0f)
                {
                    ShowInfoFmt("pull: {}'s spot for {} comes out the same again; she walks on toward it", POwner->getName(), PMob->getName());
                    m_Pull->spotFailed = true;
                }
                else if (roamed || rank(*spot) > rank(*m_Pull->spot))
                {
                    m_Pull->spot        = *spot;
                    m_Pull->spotFor     = PMob->loc.p;
                    m_Pull->spotFailed  = false;
                    m_Pull->atSpotSince = {};
                }
                m_Pull->respot = false;
            }
        }

        // In reach and in sight: the shot, from as far off as she first can
        // -- on her way there or held at its edge, on ground out of every
        // ring as her walker last read them, never part way through a
        // sneak-by past a mob, whose ring she is in; or at the end of a visit
        // in behind their backs, where she stands in one by plan
        const auto outcome   = m_Plan.outcome;
        const bool atVisit   = Visiting() && distance(POwner->loc.p, m_Pull->spot->at) < 0.5f;
        const auto* PRing    = BallHolding(m_DangerBalls, PMob, POwner->loc.p, true);
        const bool  clearHere = (outcome == cardian::walk::Outcome::Walking || outcome == cardian::walk::Outcome::Held) && PRing == nullptr;

        // At her spot a second and still no shot, or no walk of hers reaching
        // it, she says why, once. A ring come over her (its mob roamed or
        // turned there), her spot is chosen again round it; else -- her sight
        // from where she stands is not the spot's, she stands a little off it,
        // the spot is ground she cannot walk to -- she walks on toward the
        // mob itself, the walker keeping her out of every ring, until she can
        const bool atSpot  = distance(POwner->loc.p, m_Pull->spot->at) <= 1.0f;
        const bool noRoute = outcome == cardian::walk::Outcome::NoRoute;
        if (!m_Pull->spotFailed && !Visiting() && (atSpot || noRoute) && !(inReach && clearHere))
        {
            if (m_Pull->atSpotSince == timer::time_point{})
            {
                m_Pull->atSpotSince = m_Tick;
            }
            else if (m_Tick - m_Pull->atSpotSince > std::chrono::seconds(1))
            {
                const float off = distance(POwner->loc.p, PMob->loc.p);
                const auto  why = off > reach                       ? fmt::format("{:.1f} y off, past her reach of {:.1f}", off, reach)
                                  : !POwner->CanSeeTarget(PMob)     ? std::string("she cannot see it from there")
                                  : PRing != nullptr                ? fmt::format("{}'s circle is over her", PRing->getName())
                                                                    : fmt::format("her walk is {}", magic_enum::enum_name(outcome));
                const auto then = PRing != nullptr ? "chooses her spot again" : "walks on toward it";
                if (atSpot)
                {
                    ShowInfoFmt("pull: {} is at her spot for {} and cannot shoot it ({}); {}", POwner->getName(), PMob->getName(), why, then);
                }
                else
                {
                    ShowInfoFmt("pull: no walk of {}'s reaches her spot for {} ({}); {}", POwner->getName(), PMob->getName(), why, then);
                }
                if (PRing != nullptr)
                {
                    m_Pull->respot      = true;
                    m_Pull->cleanAt     = m_Tick;
                    m_Pull->atSpotSince = {};
                }
                else
                {
                    m_Pull->spotFailed = true;
                }
            }
        }
        else
        {
            m_Pull->atSpotSince = {};
        }

        if (inReach && (atVisit || clearHere))
        {
            m_Pull->visited = atVisit;
            m_Pull->step    = Step::Shoot;
            m_Pull->stepAt  = m_Tick;
        }
        else
        {
            // The walk there, through the walker: held short of a ring across
            // the way, at the safe ground beside a spot inside one, past a
            // choke by a sneak-by when one fits, and in to a spot inside one
            // behind its mobs' backs when a visit fits and may go (walker.h
            // visitTo, VisitAllowed)
            if (POwner->PAI->CanFollowPath() && POwner->GetSpeed() > 0)
            {
                RefreshDangers(PMob);
                Intent intent;
                intent.kind       = Intent::Kind::Path;
                intent.point      = m_Pull->spotFailed ? PMob->loc.p : m_Pull->spot->at;
                intent.arrive     = 0.2f;
                intent.tolerance  = 0.0f;
                intent.keepsClear = true;
                intent.fallback   = PMob->loc.p;
                intent.slideRound = PMob->loc.p;
                intent.visitStay  = !m_Pull->spot->clean && (m_Pull->visitMay || Visiting()) ? static_cast<float>(pull::kAimSeconds) + 1.0f : 0.0f;
                Move(intent);
            }
            // Waiting (the user, 2026-10-18 and 2026-10-25): held, she looks
            // every few seconds for one she can pull now, which she goes on to
            // from where she stands, and, her spot inside a ring, whether a
            // visit may go; she goes on once her walker has had her moving a
            // couple of seconds, and gives the mob up past kHoldSeconds of
            // waiting. The wait is no part of the walk out's time
            const bool held = m_Plan.outcomeAt == m_Tick && (m_Plan.outcome == cardian::walk::Outcome::Held || m_Plan.outcome == cardian::walk::Outcome::BackingOff);
            if (held)
            {
                m_Pull->movingSince = {};
                const auto why      = !m_Pull->spot->why.empty() ? m_Pull->spot->why : std::string("aggro holds her way to it");
                if (!waiting)
                {
                    m_Pull->blockedSince = m_Tick;
                    ShowInfoFmt("pull: {} waits near {} ({}); she looks for another meanwhile", POwner->getName(), PMob->getName(), why);
                }
                if (m_Tick >= m_PullLookAt)
                {
                    m_PullLookAt = m_Tick + std::chrono::seconds(3); // each look weighs a few mobs' spots: not every second
                    if (auto* PElse = PullInstead(PPlayer, PMob); PElse != nullptr)
                    {
                        RetargetPull(PMob, PElse, why);
                        return true;
                    }
                }
                if (!m_Pull->spot->clean && m_Tick >= m_Pull->visitLookAt)
                {
                    m_Pull->visitLookAt = m_Tick + std::chrono::seconds(2);
                    m_Pull->visitMay    = VisitAllowed(PMob, m_Pull->spot->at);
                }
                if (seconds(m_Tick - m_Pull->blockedSince) > pull::kHoldSeconds)
                {
                    GiveUpPull(PMob, fmt::format("{} for {:.0f} s", why, pull::kHoldSeconds));
                    return false;
                }
                return true;
            }
            if (waiting)
            {
                if (m_Pull->movingSince == timer::time_point{})
                {
                    m_Pull->movingSince = m_Tick;
                }
                if (m_Tick - m_Pull->movingSince < std::chrono::seconds(2))
                {
                    return true;
                }
                ShowInfoFmt("pull: {} goes on after {}: its way is clear again", POwner->getName(), PMob->getName());
                m_Pull->setOffAt += m_Pull->movingSince - m_Pull->blockedSince;
                m_Pull->blockedSince = {};
                m_Pull->movingSince  = {};
            }
            if (seconds(m_Tick - m_Pull->setOffAt) > m_Pull->outLimit)
            {
                GiveUpPull(PMob, "no shot at it in time");
                return false;
            }
            return true;
        }
    }

    if (m_Pull->step == Step::Shoot)
    {
        HeadLook(PMob);
        if (POwner->PAI->IsCurrentState<CRangeState>() || POwner->PAI->IsCurrentState<CMagicState>())
        {
            return true; // aiming, or singing
        }
        // out of reach or out of sight again (it roamed): back to the walk,
        // a walk of its own length from here
        if (distance(POwner->loc.p, PMob->loc.p) > PullReach(PMob) || !POwner->CanSeeTarget(PMob))
        {
            m_Pull->called   = 0; // nothing came of the call: the shot that brings it is called again
            m_Pull->step     = Step::Out;
            m_Pull->stepAt   = m_Tick;
            m_Pull->visited  = false;
            m_Pull->setOffAt = m_Tick;
            m_Pull->outLimit = pull::outSeconds(distance(POwner->loc.p, PMob->loc.p));
            return true;
        }
        if (POwner->PAI->PathFind != nullptr && POwner->PAI->PathFind->IsFollowingPath())
        {
            POwner->PAI->PathFind->Clear();
        }
        // The way back judged again before the shot: kin that roamed onto it
        // since the pick hold the shot, and waiting for them to move off is
        // worth it only for the last mob worth pulling (the user,
        // 2026-10-16) -- each second she looks for one she can pull now
        // instead, and goes after it from where she stands. Come in behind a
        // mob's back (a visit), nothing holds her shot: she stands inside its
        // ring, and the visit went only once these same tests allowed it
        // (VisitAllowed)
        if (m_Pull->shots == 0 && !m_Pull->visited)
        {
            const auto landing = nearPosition(m_Stake->at, cardian::stake::kMobAhead, 0.0f);
            // the chase: from the mob to her, then on with her to the landing point
            const auto* PKin = pawn::pull::wayBackKin(PMob, PMob->loc.p, POwner->loc.p);
            if (PKin == nullptr)
            {
                PKin = pawn::pull::wayBackKin(PMob, POwner->loc.p, landing);
            }
            if (PKin != nullptr)
            {
                if (m_Pull->kinSince == timer::time_point{})
                {
                    m_Pull->kinSince = m_Tick;
                    ShowInfoFmt("pull: {} holds her shot at {}: {} stands on the way back; she looks for another meanwhile", POwner->getName(), PMob->getName(),
                                PKin->getName());
                }
                if (m_Tick >= m_PullLookAt)
                {
                    m_PullLookAt = m_Tick + std::chrono::seconds(3); // each look weighs a few mobs' spots: not every second
                    if (auto* PElse = PullInstead(PPlayer, PMob); PElse != nullptr)
                    {
                        RetargetPull(PMob, PElse, fmt::format("{} stands on its way back", PKin->getName()));
                        return true;
                    }
                }
                if (seconds(m_Tick - m_Pull->kinSince) > pull::kHoldSeconds)
                {
                    GiveUpPull(PMob, fmt::format("{} stayed on the way back", PKin->getName()));
                    return false;
                }
                m_Pull->stepAt = m_Tick;
                HoldForShot(PMob);
                return true;
            }
            m_Pull->kinSince = {};
        }
        // Ready at the spot, her shot waits until tagging it lands it on
        // time (pull P2): the timing read again each second, held while there
        // is no plan (a member down, another mob on the party), given up past
        // kHoldSeconds
        if (m_Pull->shots == 0 && !m_Pull->visited)
        {
            if (m_Tick >= m_PullLookAt)
            {
                m_PullLookAt          = m_Tick + std::chrono::seconds(1);
                const auto timing     = PullTimingNow();
                m_Pull->timingReady   = timing.ready;
                m_Pull->timingWhy     = timing.why;
                if (timing.ready)
                {
                    m_Pull->arriveAt = m_Tick + after(timing.arriveIn);
                }
            }
            const auto   landing = nearPosition(m_Stake->at, cardian::stake::kMobAhead, 0.0f);
            const double wait    = !m_Pull->timingReady ? 1.0 : pull::shootIn(seconds(m_Pull->arriveAt - m_Tick), distance(POwner->loc.p, landing), POwner->GetSpeed() / 17.0);
            if (wait > 0.0)
            {
                if (m_Pull->holdSince == timer::time_point{})
                {
                    m_Pull->holdSince = m_Tick;
                    ShowInfoFmt("pull: {} is ready at {} and holds her shot ({})", POwner->getName(), PMob->getName(),
                                m_Pull->timingReady ? fmt::format("about {:.0f} s, to land it on time", wait) : m_Pull->timingWhy);
                }
                if (seconds(m_Tick - m_Pull->holdSince) > pull::kHoldSeconds)
                {
                    GiveUpPull(PMob, fmt::format("held her shot {:.0f} s", pull::kHoldSeconds));
                    return false;
                }
                m_Pull->stepAt = m_Tick; // the shot's own clock starts once it may go
                HoldForShot(PMob);
                return true;
            }
        }
        if (m_Pull->shots == 0 && seconds(m_Tick - m_Pull->stepAt) > pull::kShootSeconds)
        {
            GiveUpPull(PMob, fmt::format("could not {} it ({})", m_PullSpell != 0 ? "sing to" : "shoot", m_Pull->refused.empty() ? std::string("refused") : m_Pull->refused));
            return false;
        }
        // In behind their backs she stands no longer than her visit's stay:
        // a shot refused or that brought nothing is given up, and she walks
        // out of the ring (the walker's, as her next walk asks)
        if (m_Pull->visited && seconds(m_Tick - (m_Pull->shots > 0 ? m_Pull->shotAt : m_Pull->stepAt)) > pull::kAimSeconds + 1.0)
        {
            GiveUpPull(PMob, "her shot from in behind their backs brought nothing in time");
            return false;
        }
        // The mob on the party is the Back step's, above, so never Coming here
        switch (pull::shot(m_Pull->shots, seconds(m_Tick - m_Pull->shotAt), false))
        {
            case pull::Shot::Wait:
            case pull::Shot::Coming:
                return true;
            case pull::Shot::GiveUp:
                GiveUpPull(PMob, fmt::format("{} tries brought nothing", m_Pull->shots));
                return false;
            case pull::Shot::Fire:
                if (!hunting)
                {
                    EndPull("the party's hunt is off");
                    return false;
                }
                break;
        }
        const bool fired = m_PullSpell != 0 ? SingPull(PMob) : RangedAttack(EntityId(PMob));
        if (!fired)
        {
            const bool kit = m_PullSpell != 0 || pawn::pull::canShoot(static_cast<CCharEntity*>(POwner));
            const auto why = kit ? std::string("not yet") : std::string("nothing to shoot with");
            if (why != m_Pull->refused)
            {
                m_Pull->refused = why;
                ShowInfoFmt("pull: {} cannot pull {} ({})", POwner->getName(), PMob->getName(), why);
            }
            return true;
        }
        ++m_Pull->shots;
        m_Pull->shotAt = m_Tick;
        // The pull called as her first shot leaves, as a player calls it
        if (m_Pull->called != PMob->id)
        {
            CallPull(PMob, PPlayer);
        }
        ShowInfoFmt("pull: {} tags {} ({} {}, {:.1f} y)", POwner->getName(), PMob->getName(),
                    m_PullSpell != 0 ? fmt::format("song {}", m_PullSpell) : std::string("shot"), m_Pull->shots, distance(POwner->loc.p, PMob->loc.p));
        return true;
    }

    // Back: she runs it in to the landing point, fighting nothing; the
    // tank's Provoke, or her arrival, hands it to the camp
    if (!Staked())
    {
        EndPull("the camp is gone: the party takes it where it is");
        return false;
    }
    // A worm -- a mob that cannot chase -- that catches her on the way back
    // off the camp's ground is never brought in either: the party leaves it
    // be, and her run goes on with the mob she pulled
    {
        const auto owner = pawn::ordersOwnerOf(static_cast<const CCharEntity*>(POwner));
        pawn::forEachMobNear(pawn::entitiesAround(POwner), POwner->loc.p, 30.0f, [&](CMobEntity* POther)
                             {
                                 if (POther != PMob && POther->isAlive() && POther->PAI->IsEngaged() && POther->GetBattleTarget() == POwner &&
                                     !pawn::chases(POther) && std::ranges::find(m_Pull->camp, POther->id) == m_Pull->camp.end() &&
                                     !pawn::onCampGround(owner, POther) && !pawn::leftBehind(owner, POther))
                                 {
                                     pawn::leaveBehind(owner, POther, fmt::format("it cannot chase, and caught {} off the camp's ground on her way back", POwner->getName()));
                                 }
                             });
    }
    // She runs past the landing point, so the mob chasing her stops on it
    const auto landing = nearPosition(m_Stake->at, cardian::stake::kMobAhead, 0.0f);
    const auto runTo   = m_Pull->backTo.value_or(landing);
    switch (pull::back(fighting, POn == POwner, onParty(POn), distance(POwner->loc.p, runTo, true), seconds(m_Tick - m_Pull->stepAt), m_Pull->backLimit))
    {
        case pull::Back::Run:
        {
            if (POwner->PAI->CanFollowPath() && POwner->GetSpeed() > 0)
            {
                RefreshDangers(PMob);
                Intent intent;
                intent.kind      = Intent::Kind::Path;
                intent.point     = runTo;
                intent.arrive    = 0.5f;
                intent.tolerance = 0.5f;
                // Shot from in behind a mob's back, she stands in its ring:
                // her run home is vetted, out of it first, whatever her rows
                intent.keepsClear = m_Pull->visited;
                // With her pull on her she holds for no ring across her way:
                // through it, round every other she can. The camp waits for
                // the mob to come in, and a hold leaves her taking its hits
                // where nobody reaches her (the user, 2026-10-26)
                intent.racing = POn == POwner;
                Move(intent);
            }
            return true;
        }
        case pull::Back::Handed:
        {
            const bool tank = POn->objtype == TYPE_PC && pawn::roster::roleOf(static_cast<CCharEntity*>(POn)) == cardian::party::Role::Tank;
            ShowInfoFmt("pull: {} handed to {} ({}, {:.0f} y from the landing point)", PMob->getName(), tank ? "the tank" : "another member", POn->getName(),
                        distance(PMob->loc.p, landing, true));
            EndPull(fmt::format("hands {} to {}", PMob->getName(), POn->getName()));
            return false;
        }
        case pull::Back::Home:
            ShowInfoFmt("pull: {} is back at the camp with {} still on her", POwner->getName(), PMob->getName());
            EndPull(fmt::format("back at the camp with {}", PMob->getName()));
            return false;
        case pull::Back::Lost:
            // passed over a while, or the idle mob is the next pick at once
            ShowInfoFmt("pull: {} lost {} on the way back", POwner->getName(), PMob->getName());
            GiveUpPull(PMob, "it gave up the chase");
            return false;
        case pull::Back::Late:
            ShowInfoFmt("pull: {} has run {} in for {:.0f} s; the camp takes it where it is", POwner->getName(), PMob->getName(), m_Pull->backLimit);
            EndPull(fmt::format("the run with {} took too long", PMob->getName()));
            return false;
    }
    return true;
}
