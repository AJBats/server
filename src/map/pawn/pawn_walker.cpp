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

// The walker (ROADMAP W): every walk a cardian takes is a route the scored
// search plans round the danger map's balls (pawn/route_search.h) and walks
// as planned. A mover proposes where she wants to be (its Intent); Walk
// decides how, through her walker (walker.h, SafeWalk), which plans, holds,
// backs off and sneaks by, for every walk, a trek's included

#include "pawn_controller.h"

#include "drill.h"
#include "line_approach.h"
#include "pawn.h"
#include "pawn_danger.h"
#include "pawn_rules.h"
#include "pawn_travel.h"
#include "route_search.h"
#include "safe_walk.h"
#include "sneak.h"
#include "stake_math.h"

#include "ai/ai_container.h"
#include "ai/controllers/mob_controller.h"
#include "ai/helpers/pathfind.h"
#include "common/logging.h"
#include "common/settings.h"
#include "common/xirand.h"
#include "entities/char_entity.h"
#include "entities/mob_entity.h"
#include "map_constants.h"
#include "navmesh/detour_navmesh.h"
#include "roam_region.h"
#include "status_effect_container.h"
#include "utils/zoneutils.h"

#include <magic_enum/magic_enum.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <limits>
#include <optional>
#include <string>
#include <unordered_set>
#include <vector>

using namespace std::chrono_literals;
using cardian::walk::Outcome;
using cardian::walk::Rules;
using namespace pawn::walker;

namespace
{
    auto without(const std::vector<pawn::danger::Sphere>& balls, const CBaseEntity* exclude) -> std::vector<pawn::danger::Sphere>
    {
        std::vector<pawn::danger::Sphere> out;
        out.reserve(balls.size());
        for (const auto& b : balls)
        {
            if (exclude == nullptr || b.mob != exclude)
            {
                out.push_back(b);
            }
        }
        return out;
    }
} // namespace

auto CPawnController::BallHolding(const std::vector<pawn::danger::Sphere>& balls, const CBaseEntity* exclude, const position_t& point, const bool sight) const -> CMobEntity*
{
    const auto kept = without(balls, exclude);
    if (kept.empty())
    {
        return nullptr;
    }
    const auto                        hazards = toHazards(kept);
    const auto                        at      = cardian::route::toDetour(point);
    const cardian::route::HazardSight seen    = sight ? cardian::route::HazardSight([&](const size_t h, const position_t& p) { return SeesBall(kept[h], p); })
                                                      : cardian::route::HazardSight{};
    const int                         h       = cardian::route::DangerTest(hazards, at, seen).exposes(at);
    return h >= 0 ? kept[h].mob : nullptr;
}

auto CPawnController::BallCrossing(const std::vector<pawn::danger::Sphere>& balls, const CBaseEntity* exclude, const position_t& from, const position_t& to) const -> CMobEntity*
{
    const auto kept = without(balls, exclude);
    if (kept.empty())
    {
        return nullptr;
    }
    const auto                        hazards = toHazards(kept);
    const cardian::route::HazardSight seen    = [&](const size_t h, const position_t& p) { return SeesBall(kept[h], p); };
    const int                         h       = cardian::route::DangerTest(hazards, cardian::route::toDetour(from), seen).holder(from, to);
    return h >= 0 ? kept[h].mob : nullptr;
}

auto CPawnController::BallRouteHeld(const std::vector<pawn::danger::Sphere>& all, const CBaseEntity* exclude, const position_t& to) const -> CMobEntity*
{
    // The route she would walk there, planned as the walker plans it: held
    // when every way passes a ball, by the ball its first such step passes
    // -- or, should that step pass none now, the ball nearest its end
    const auto balls = without(all, exclude);
    if (balls.empty())
    {
        return nullptr;
    }
    const auto                        hazards = toHazards(balls);
    const cardian::route::HazardSight seen    = [&](const size_t h, const position_t& p) { return SeesBall(balls[h], p); };
    const auto                        here    = POwner->loc.p;
    const auto route = pawn::travel::routeAvoiding(POwner->getZone(), here, to, hazards, seen, distance(here, to) * 1.5f + kLocalSpare);
    if (!route.has_value() || !route->complete || route->firstDanger < 0)
    {
        return nullptr;
    }
    const auto& points = route->points;
    const auto  d      = static_cast<size_t>(route->firstDanger);
    const auto& a      = d == 0 ? here : points[d - 1];
    if (const int h = cardian::route::DangerTest(hazards, cardian::route::toDetour(here), seen).holder(a, points[d]); h >= 0)
    {
        return balls[h].mob;
    }
    size_t nearest = 0;
    for (size_t h = 1; h < balls.size(); ++h)
    {
        if (distance(balls[h].at, points[d]) - balls[h].radius < distance(balls[nearest].at, points[d]) - balls[nearest].radius)
        {
            nearest = h;
        }
    }
    return balls[nearest].mob;
}

auto CPawnController::DangerProfile(const bool forceAvoid) -> pawn::danger::Profile
{
    auto* PPawn   = static_cast<CCharEntity*>(POwner);
    auto  profile = pawn::danger::Profile::of(PPawn, forceAvoid || IsAvoidingAggro() || KeepsClearOfAggro(), forceAvoid || IsAvoidingLinks());
    // A spell held for magic aggro, or an attending mage's spot: the balls
    // count as though she were casting, so the walk takes her clear of them
    // before she does
    profile.casting = profile.casting || m_Tick < m_MagicHoldUntil || KeepsClearOfAggro();
    return profile;
}

auto CPawnController::WalkSpheres(const Rules& rules, const float scan) -> std::vector<pawn::danger::Sphere>
{
    if (!rules.danger)
    {
        return {};
    }
    auto profile        = DangerProfile(rules.forceAvoid);
    profile.extraBuffer = rules.extra;
    // The fight's own mob, as the movers named it this tick, is never a
    // danger: its ball would hold her off the very mob she is meant to hit
    const CBattleEntity* ignore = m_DangerIgnoreAt == m_Tick ? m_DangerIgnore : nullptr;
    return pawn::danger::spheres(pawn::entitiesAround(POwner), POwner->loc.p, scan, profile, ignore);
}

auto CPawnController::SeesBall(const pawn::danger::Sphere& sphere, const position_t& point) const -> bool
{
    return m_Walker.sees(sphere, point, m_Tick);
}

auto CPawnController::SafeWalk(const position_t& point, const Rules& rules) -> WalkResult
{
    return m_Walker.walk(point, rules, m_Tick);
}

auto CPawnController::walkBalls(const Rules& rules, const float scan) -> std::vector<pawn::danger::Sphere>
{
    return WalkSpheres(rules, scan);
}

void CPawnController::sneakStepped()
{
    m_LineApproach.held(m_Tick);
}

void CPawnController::sneakedThrough()
{
    if (m_TrekStuckSaid)
    {
        TrekGotBy("sneaked by");
    }
}

auto CPawnController::WalkExposed(const Rules& rules) -> bool
{
    const auto spheres = WalkSpheres(rules, kNearRings);
    if (spheres.empty())
    {
        return false;
    }
    const auto                        hazards = toHazards(spheres);
    const auto                        at      = cardian::route::toDetour(POwner->loc.p);
    const cardian::route::HazardSight sight   = rules.sightHides ? cardian::route::HazardSight([&](const size_t h, const position_t& p) { return SeesBall(spheres[h], p); })
                                                                 : cardian::route::HazardSight{};
    return cardian::route::DangerTest(hazards, at, sight).exposes(at) >= 0;
}

auto CPawnController::CampLineHolds(const std::vector<position_t>& route, const position_t& camp) const -> bool
{
    // Behind the camp's line: the end never in front of it, and at the camp
    // no step further forward than where she stands -- out in front, she
    // walks back; behind it, an AoE reposition never routes round a wall
    // through the front. Farther from the flag than her cast range (past
    // the deepest spot CampSpot looks at) she walks the mesh's own way
    // there: a camp set across a maze has no route to its back that never
    // crosses its front
    if (route.empty())
    {
        return true;
    }
    const auto forward = [&](const position_t& p) { return cardian::stake::forwardOf(camp.x, camp.z, camp.rotation, p.x, p.z); };
    if (forward(route.back()) > 0.05f)
    {
        return false;
    }
    const bool  atCamp = isWithinDistance(POwner->loc.p, camp, CastRange() + 8.0f);
    const float limit  = atCamp ? std::max(0.0f, forward(POwner->loc.p)) + 0.05f : std::numeric_limits<float>::max();
    return std::none_of(route.begin(), route.end(), [&](const position_t& p) { return forward(p) > limit; });
}

void CPawnController::SayStuckIfSure(const bool trek)
{
    // Stuck by aggro, said once she is sure: the way held this long, and the
    // plan of the whole way, made while she held, finds no way round
    const auto stuckAfter = std::chrono::duration_cast<timer::duration>(std::chrono::duration<float>(settings::get<float>("pawn.TREK_STUCK_AFTER")));
    if (m_TrekStuckSaid || !m_Plan.heldSince.has_value() || m_Tick - *m_Plan.heldSince < stuckAfter || !m_Plan.wholeHeld || m_Plan.wholeAt < *m_Plan.heldSince)
    {
        return;
    }
    static constexpr std::array kStuckLines = {
        "I'm stuck, too much aggro on the way.",
        "Can't get past these mobs. Waiting for an opening.",
        "The path is crawling with mobs. I'm holding back.",
        "No way through without pulling something. Waiting.",
        "Stuck behind some mobs here, they'd spot me if I moved.",
    };
    m_TrekStuckSaid = true;
    m_TrekStuckAt   = m_Plan.danger >= 0 && static_cast<size_t>(m_Plan.danger) < m_Plan.route.size() ? m_Plan.route[m_Plan.danger] : POwner->loc.p;
    ShowInfoFmt("pawn: {}{}: stuck by aggro, the way held {} s", trek ? "travel " : "", POwner->getName(),
                std::chrono::duration_cast<std::chrono::seconds>(m_Tick - *m_Plan.heldSince).count());
    if (trek)
    {
        pawn::drill::noteBlocked(POwner->id);
    }
    TrekSay(kStuckLines[xirand::GetRandomNumber(kStuckLines.size())]);
}

auto CPawnController::OrdersOwnerHere() const -> CCharEntity*
{
    const uint32 owner = pawn::ordersOwnerOf(static_cast<const CCharEntity*>(POwner));
    return owner != 0 && POwner->loc.zone != nullptr ? POwner->loc.zone->GetCharByID(owner) : nullptr;
}

auto CPawnController::FarFromParty() const -> bool
{
    const auto* PPlayer = OrdersOwnerHere();
    return PPlayer != nullptr && distance(POwner->loc.p, PPlayer->loc.p) > settings::get<float>("pawn.PARTY_GROUND_RADIUS");
}

// Arriving from a trek to meet him (m_TrekArriving), she keeps its rows --
// Avoid aggro, Avoid links -- and its walk across his zone until she is with
// him: within pawn.TREK_ARRIVE_YALMS of him, at her seat round him, or
// standing at her place on the party's ground (a camp's seat, a mage's
// attend spot) and not held at a choke (the user, 2026-10-14). The party's
// ground (PARTY_GROUND_RADIUS) keeps its own rules meanwhile, as for any
// follower. A task of her own (a hold, a fight, attending), his leaving her
// zone or another trek ends it too
void CPawnController::TrekArrivalTick()
{
    if (!m_TrekArriving)
    {
        return;
    }
    const auto* PPlayer = OrdersOwnerHere();
    std::string why;
    if (m_Mode == Mode::Travel)
    {
        why = "a trek again";
    }
    else if (PPlayer == nullptr)
    {
        why = "he is not in her zone";
    }
    else if (POwner->PAI->IsEngaged())
    {
        why = "a fight";
    }
    else if (m_Mode != Mode::Follow)
    {
        why = fmt::format("{} now", modeName(m_Mode));
    }
    else if (const float off = distance(POwner->loc.p, PPlayer->loc.p); off <= settings::get<float>("pawn.TREK_ARRIVE_YALMS"))
    {
        why = fmt::format("within {:.0f} y of {}", off, PPlayer->getName());
    }
    else if (m_SeatHeld)
    {
        why = "at her seat";
    }
    else if (!FarFromParty() && !POwner->PAI->PathFind->IsFollowingPath() && m_Plan.outcome != Outcome::Held)
    {
        why = "standing at her place on the party's ground";
    }
    if (why.empty())
    {
        return;
    }
    m_TrekArriving = false;
    ShowInfoFmt("pawn: travel {}: arrived ({}), her trek's rows go back", POwner->getName(), why);
}

auto CPawnController::SneakingBy() const -> bool
{
    return m_Walker.sneakingBy(m_Tick);
}

void CPawnController::TrekStep(const pawn::TravelHop& hop, const float distToLine, const bool ordered, const bool racing)
{
    constexpr float kGotByNear = 8.0f; // past a choke she was stuck at: this near where it held her

    // The trek is a mover like any other: it proposes its line, and the
    // walker plans the way there round every mob in her zone and walks it.
    // A trek walks outside the claim board, by design
    Intent toLine;
    toLine.kind      = Intent::Kind::Path;
    toLine.point     = hop.walkTo;
    toLine.arrive    = 0.0f;
    toLine.tolerance = 0.0f;
    toLine.trek      = true;
    toLine.racing    = racing;
    toLine.claimed   = true;
    Walk(std::move(toLine));
    if (m_Plan.outcomeAt != m_Tick)
    {
        return; // her rest put the step off
    }

    switch (m_Plan.outcome)
    {
        case Outcome::Walking:
        case Outcome::Escaping:
        {
            m_TravelStuckSaid = false;
            const bool chokeAhead = m_Plan.danger >= 0 && m_Plan.next <= static_cast<size_t>(m_Plan.danger);
            if (!chokeAhead && m_TrekStuckSaid && !racing && distance(POwner->loc.p, m_TrekStuckAt) < kGotByNear)
            {
                TrekGotBy("past the choke");
            }
            return;
        }
        case Outcome::BackingOff:
        case Outcome::Hopping:
            m_TravelStuckSaid = false;
            m_LineApproach.held(m_Tick); // no stall: the line's no-progress crossing waits on it
            return;
        case Outcome::Held:
            m_LineApproach.held(m_Tick);
            SayStuckIfSure(true);
            return;
        case Outcome::NoRoute:
            break;
    }

    // No walk on the mesh to the line from her floor: a timed lift to a
    // floor that has one (pawn_lift.cpp)
    if (LiftToward(hop.walkTo, 8.0f, fmt::format("her trek's line to zone {} is on another floor", static_cast<uint16>(hop.destinationZone)), true))
    {
        return;
    }

    // No walk on the mesh to the line: the mesh often ends short of the zone
    // mouth, and the server itself accepts crossings from up to ~40 yalms
    // out; farther, the order is let go, or she says she is stuck and looks
    // again in a while
    if (cardian::trek::withinSlack(distToLine))
    {
        m_TravelStuckSaid = false;
        // a stable is rented standing at it, or given up (RentAt)
        if (hop.rents && !RentAt(hop))
        {
            return;
        }
        m_TrekCrossedFrom = POwner->getZone();
        pawn::requestTransfer(POwner->id, hop);
        return;
    }
    if (ordered)
    {
        ShowInfoFmt("pawn: travel {}: cannot path to line ({:.1f}y away), order cancelled", POwner->getName(), distToLine);
        pawn::clearTravelOrder(POwner->id);
    }
    else
    {
        TravelStuck(fmt::format("cannot path to the line {:.1f}y away", distToLine));
    }
}

auto CPawnController::Walk(Intent intent) -> AvoidAction
{
    // The claim board first (claim_board.h): no two party members aim for
    // one spot, whichever mover proposed it
    ClaimSpot(intent);
    if (intent.kind != Intent::Kind::Keep)
    {
        m_MoveTrace.kind  = intent.kind;
        m_MoveTrace.point = intent.point;
        m_MoveTrace.at    = m_Tick;
    }

    auto*       PPathFind    = POwner->PAI->PathFind.get();
    position_t  point        = intent.point;
    const float followMax    = intent.tolerance;
    const float followTarget = intent.arrive;

    // The vet, the same for every mover: whether this walk keeps out of the
    // danger map's rings at all. A proposal the party waved through
    // (aggressive company allowed), the player's order and a hold are not
    // vetted; kneeling with nowhere to go, danger is her rest's to judge
    // (RestsInDanger), and on the player's rest order nothing stands her
    const bool proposes   = intent.kind != Intent::Kind::Stand && intent.kind != Intent::Kind::Keep;
    const bool kneelsHere = !proposes && POwner->StatusEffectContainer->HasStatusEffect(xi::StatusEffect::Healing);
    const bool arriving   = m_TrekArriving && !intent.trek; // the rest of a trek's way to him, in his zone (TrekArrivalTick)
    // The long way round he sent her on (ReachToPlayer): her place by him is
    // walked to as a trek walks to its line, to where he stood (moved on as
    // he goes far), round every mob of her zone (the user, 2026-10-24)
    const bool longWay = m_LongWayGo && intent.kind == Intent::Kind::Formation;
    if (longWay)
    {
        intent.point = m_LongWayGoal;
    }
    const bool vetted     = intent.vet && !kneelsHere && !PlayersRestHolds() &&
                        (intent.trek || arriving || IsAvoiding() || (intent.keepsClear && proposes && KeepsClearOfAggro()));

    // No monster aggroes a rider (the game's own rule, CMobController): on
    // her chocobo she plans round no danger at all
    Rules rules;
    rules.danger = vetted && !POwner->isMounted();
    if (intent.trek || longWay)
    {
        // A trek keeps every ring whole, walls or not (the game's sight ray,
        // cast from a flying mob hugging a cave wall, can start inside the
        // wall and see nothing), plans the zone at once, and with a mob on
        // her races for her line, holding for nothing
        rules.forceAvoid = true;
        rules.sightHides = false;
        rules.wholeZone  = true;
        rules.holds      = !intent.racing;
        rules.extra      = settings::get<float>("pawn.TREK_AVOID_EXTRA");
    }
    else
    {
        // The everyday walk plans against the rings padded by the pull
        // rule's clearance, so a mob that steps her way does not find her on
        // its edge. Arriving from a trek, the trek's own rings: every one
        // whole, walls or not, and its wider berth
        rules.extra = arriving ? settings::get<float>("pawn.TREK_AVOID_EXTRA") : cardian::rules::kClearance;
        if (arriving)
        {
            rules.forceAvoid = true;
            rules.sightHides = false;
        }
        // Alone with a mob on her -- off the party's ground, beyond
        // pawn.PARTY_GROUND_RADIUS of the player -- she runs on to the party
        // with it and holds for no choke: through it, round every other ring
        // she can. On the party's ground the party is there for her, and she
        // holds and sneaks by as ever, whatever is on her (the user,
        // 2026-10-13)
        rules.holds = !(FarFromParty() && HatedByAnyMob());
    }

    AvoidAction action  = AvoidAction::None;
    bool        walked  = false; // the walker set her path finder this tick
    bool        trying  = false; // for the auto rescue: a walk toward a spot she is not at, nothing holding her back
    const auto  walk    = [&](const position_t& to, const float closeTo) -> WalkResult
    {
        Rules r           = rules;
        r.closeTo         = closeTo;
        auto out          = SafeWalk(to, r);
        walked            = true;
        m_Plan.outcome    = out.outcome;
        m_Plan.outcomeAt  = m_Tick;
        if (out.action == AvoidAction::Slot && intent.fighting)
        {
            out.action = AvoidAction::Hold; // her fight's mob inside another's ring: she waits at the safe ground nearest it
        }
        action = out.action;
        point  = out.target;
        return out;
    };

    // Standing in a ring whose mob sees her, she walks out of it first,
    // whatever the mover proposed: escape wins over everything
    const bool exposed = vetted && WalkExposed(rules);
    if (exposed && !proposes)
    {
        walk(POwner->loc.p, 0.0f);
    }

    // A path produced by the walker for danger cannot be kept as though it
    // still led to the seat. Re-plan that seat when it resumes
    const auto settleSeat = [&]()
    {
        if (action != AvoidAction::None || intent.kind != Intent::Kind::Keep)
        {
            m_SeatPathActive = action == AvoidAction::None && intent.seat && intent.kind == Intent::Kind::Path;
        }
    };

    // Moving uses the same stand transition as spells and orders. A danger
    // escape can interrupt a rest; merely keeping an idle path cannot
    const bool takesStep = action != AvoidAction::None || (proposes && distance(POwner->loc.p, intent.point) > followMax) ||
                           (intent.kind == Intent::Kind::Keep && PPathFind->IsFollowingPath());
    if (takesStep)
    {
        StandFromRest(action == AvoidAction::Escape || exposed ? "escaping danger" : "moving");
        if (!RestAllowsAction())
        {
            PPathFind->Clear();
            m_Plan.walking = false;
            settleSeat();
            NoteWalk(false, point, followTarget);
            m_LastAvoidAction = action;
            return action;
        }
    }

    if (!walked)
    {
        switch (intent.kind)
        {
            case Intent::Kind::Stand:
                if (PPathFind->IsFollowingPath())
                {
                    PPathFind->Clear();
                }
                m_Plan.walking = false;
                break;
            case Intent::Kind::Keep:
                break;
            case Intent::Kind::Hop:
            {
                // A step under the planner's floor, straight at the point --
                // unless the point, or the step, meets a ring: then a walk
                const bool clear = !vetted || [&]()
                {
                    const auto spheres = WalkSpheres(rules, kNearRings);
                    if (spheres.empty())
                    {
                        return true;
                    }
                    const auto                        hazards = toHazards(spheres);
                    const cardian::route::HazardSight sight   = rules.sightHides ? cardian::route::HazardSight([&](const size_t h, const position_t& p) { return SeesBall(spheres[h], p); })
                                                                                 : cardian::route::HazardSight{};
                    const cardian::route::DangerTest test(hazards, cardian::route::toDetour(POwner->loc.p), sight);
                    return test.exposes(cardian::route::toDetour(intent.point)) < 0 && test.holder(POwner->loc.p, intent.point) < 0;
                }();
                if (clear && !exposed)
                {
                    PPathFind->Clear();
                    const auto before = POwner->loc.p;
                    PPathFind->StepTo(intent.point);
                    GroundStep(before);
                    CountMove(before, kLogicUpdateInterval);
                    m_Plan.walking = false;
                }
                else if (walk(intent.point, 0.0f).outcome == Outcome::NoRoute && exposed)
                {
                    walk(POwner->loc.p, 0.0f); // no way to the hop's point: out of the ring she stands in, at least
                }
                break;
            }
            case Intent::Kind::Path:
            case Intent::Kind::Formation:
            {
                const float away = distance(POwner->loc.p, intent.point);
                // The formation settles: any cell near her place will do, the
                // courtesy's cheapest, so a follower stops beside another,
                // and one standing on another's spot steps off it
                const float settle  = intent.kind == Intent::Kind::Formation && !intent.trek ? settings::get<float>("pawn.COURTESY_SETTLE") : 0.0f;
                const bool  crowded = settle > 0.0f && away <= followMax && !exposed && Crowded();
                if (away > followMax || exposed || crowded)
                {
                    // The player's personal space and the party's: her goal
                    // stepped clear of their bodies, his wake and where the
                    // others are going
                    const auto goal = intent.trek || longWay ? intent.point : CourtesyStep(intent.point, settle);
                    // Settled where she stands -- the cheapest cell near her
                    // place is her own -- the compass hears she has arrived
                    if (settle > 0.0f && distance(goal, POwner->loc.p, true) < 0.05f)
                    {
                        m_SettledAt   = POwner->loc.p;
                        m_SettledTick = m_Tick;
                    }
                    auto       out  = walk(goal, intent.kind == Intent::Kind::Path || away > followMax ? followTarget : 0.0f);
                    // No way to her point, a ring on her: she steps out of
                    // it all the same -- escape wins over everything, reachable
                    // point or not
                    if (out.outcome == Outcome::NoRoute && exposed)
                    {
                        out = walk(POwner->loc.p, 0.0f);
                    }
                    // The auto rescue watches everyday walks only: a trek
                    // caught on the mesh says so in its own lines, and a rescue
                    // to its line would carry her hundreds of yalms. A walk to
                    // a slot -- the safe ground nearest a point inside a ring
                    // -- counts while she is short of it, so a walk that swings
                    // between the slot and no route keeps one clock; caught,
                    // she is set down on the slot, never inside the ring
                    const bool shortOfSlot = out.action == AvoidAction::Slot && distance(POwner->loc.p, out.target) > followTarget + 1.0f;
                    trying                 = !intent.trek && out.outcome == Outcome::Walking && out.action != AvoidAction::Hold &&
                             (out.action != AvoidAction::Slot || shortOfSlot);

                    // The camp's line: normal positioning stays behind it; a
                    // walk shaped by danger overrides it. A courtesy step is a
                    // waypoint on her way, not its end: the line judges the
                    // way's end at her point, so one standing out in front
                    // walks back round the party
                    std::vector<position_t> ahead(m_Plan.route.begin() + static_cast<std::ptrdiff_t>(std::min(m_Plan.next, m_Plan.route.size())), m_Plan.route.end());
                    if (!ahead.empty() && distance(goal, intent.point, true) > 0.05f)
                    {
                        ahead.push_back(intent.point);
                    }
                    const bool campRefuses = out.outcome != Outcome::NoRoute && intent.rearBoundary.has_value() && action == AvoidAction::None &&
                                             !CampLineHolds(ahead, *intent.rearBoundary);
                    if (campRefuses)
                    {
                        PPathFind->Clear();
                        m_Plan.walking = false;
                    }
                    if (out.outcome == Outcome::NoRoute || campRefuses)
                    {
                        // The camp's line refusing her route keeps her where she
                        // is on purpose: she waits, she is not caught
                        trying = !intent.trek && !intent.rearBoundary.has_value();
                        // Said once a spot: where she cannot walk to, and from
                        // where (a trek says it in its own lines)
                        if (!intent.trek && (!m_PathFailedAt.has_value() || distance(*m_PathFailedAt, intent.point) > 1.0f))
                        {
                            m_PathFailedAt = intent.point;
                            ShowInfoFmt("pawn: {} cannot path to ({:.1f}, {:.1f}, {:.1f}) from ({:.1f}, {:.1f}, {:.1f}), {:.1f} y{}", POwner->getName(), intent.point.x,
                                        intent.point.y, intent.point.z, POwner->loc.p.x, POwner->loc.p.y, POwner->loc.p.z, away,
                                        campRefuses ? ", keeping behind the camp's flag" : "");
                        }
                        if (intent.kind == Intent::Kind::Path && intent.fallback.has_value())
                        {
                            // Retry toward the spell target through the same
                            // walker. Range and sight determine when to stop
                            intent.point     = *intent.fallback;
                            intent.arrive    = 0.0f;
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
                    else if (PPathFind->IsFollowingPath())
                    {
                        m_PathFailedAt.reset(); // walking: a later failure to the same spot is said again
                    }
                }
                else if (PPathFind->IsFollowingPath())
                {
                    PPathFind->Clear();
                    m_Plan.walking = false;
                }
                break;
            }
        }
    }
    settleSeat();

    // Off the party's ground, held at a choke on her way in, she says she is
    // stuck, as a trek does, and once past it that she got by; back with the
    // party some other way -- the player came to her -- there is nothing to
    // say (the user, 2026-10-13)
    if (!intent.trek && walked && m_Plan.outcomeAt == m_Tick)
    {
        const bool offGround = FarFromParty();
        if (offGround && m_Plan.outcome == Outcome::Held && m_Plan.heldSince.has_value())
        {
            SayStuckIfSure(false);
        }
        else if (m_TrekStuckSaid)
        {
            const bool chokeAhead = m_Plan.danger >= 0 && m_Plan.next <= static_cast<size_t>(m_Plan.danger);
            if (m_Plan.outcome == Outcome::Walking && !chokeAhead && distance(POwner->loc.p, m_TrekStuckAt) < 8.0f)
            {
                TrekGotBy("past the choke");
            }
            else if (!offGround && !m_Plan.heldSince.has_value())
            {
                m_TrekStuckSaid = false;
            }
        }
    }

    // Her claim on the board while she walks: the point the walker sent her
    // to, or the end of the path she keeps (SpotClaims)
    if (PPathFind->IsFollowingPath())
    {
        m_WalkTo   = action != AvoidAction::None || intent.kind != Intent::Kind::Keep ? point : PPathFind->GetDestination();
        m_WalkToAt = m_Tick;
    }
    else
    {
        m_WalkTo.reset();
    }
    NoteWalk(trying, action == AvoidAction::Slot ? point : intent.point, followTarget);

    const auto before = POwner->loc.p;
    if (!DropOnRoute())
    {
        PPathFind->FollowPath(m_Tick);
        GroundStep(before);
        CountMove(before, kLogicUpdateInterval);
    }

    // The walk trace (with the move trace, a development tool): each tick a
    // cardian of a real player's party steps, the step, the point her mover
    // asked for and the end of the path she walks, so a wobble in her walk
    // can be told apart -- a point that jumps, a path that ends elsewhere, a
    // step that turns on its own
    if (settings::get<bool>("pawn.MOVE_TRACE") && distance(before, POwner->loc.p, true) > 0.01f && GetLivePlayer() != nullptr)
    {
        const auto& end = PPathFind->IsFollowingPath() ? PPathFind->GetDestination() : POwner->loc.p;
        ShowInfoFmt("pawn: walk trace {} ({:.2f}, {:.2f}, {:.2f}) -> ({:.2f}, {:.2f}, {:.2f}) asked ({:.2f}, {:.2f}, {:.2f}) path end ({:.2f}, {:.2f}, {:.2f}) speed {} {}",
                    POwner->getName(), before.x, before.y, before.z, POwner->loc.p.x, POwner->loc.p.y, POwner->loc.p.z, intent.point.x, intent.point.y,
                    intent.point.z, end.x, end.y, end.z, POwner->GetSpeed(),
                    magic_enum::enum_name(intent.kind));
    }

    // Lock-on: within melee reach of the mob, her heading goes back onto it
    // after every step, the way a locked-on player strafes (Walk)
    if (intent.target != nullptr && KeepsSneakForBack(intent.target))
    {
        KeepSwingOff(intent.target);
    }
    else if (intent.target != nullptr && distance(POwner->loc.p, intent.target->loc.p) <= POwner->GetMeleeRange(intent.target) + LockOnSlack)
    {
        PPathFind->LookAt(intent.target->loc.p);
    }

    // A change of action is always worth a line; a trek's plans say it in
    // their own lines
    if (!intent.trek && m_LastAvoidAction != action && action != AvoidAction::None)
    {
        ShowInfoFmt("pawn: {} walks ({}): to ({:.1f}, {:.1f}, {:.1f}), {:.1f}y off the point asked for", POwner->getName(), magic_enum::enum_name(action), point.x, point.y,
                    point.z, distance(point, intent.point));
    }
    m_LastAvoidAction = action;
    return action;
}

void CPawnController::GroundStep(const position_t& before)
{
    auto& here = POwner->loc.p;
    if (here.x == before.x && here.z == before.z)
    {
        return;
    }
    if (const auto floor = pawn::travel::floorUnder(POwner->getZone(), here, pawn::travel::kFloorReach); floor.has_value())
    {
        here.y = *floor;
    }
}

void CPawnController::CountMove(const position_t& before, const std::chrono::microseconds took)
{
    auto& here = POwner->loc.p;
    if (here.x == before.x && here.y == before.y && here.z == before.z)
    {
        here.moving = before.moving;
        return;
    }
    m_MoveFrames += static_cast<float>(took.count()) * 60.0f / 1000000.0f;
    const auto whole = static_cast<uint16>(m_MoveFrames);
    m_MoveFrames -= static_cast<float>(whole);
    here.moving = static_cast<uint16>((before.moving + whole) % 0x2000);
}

// The move trace (pawn.MOVE_TRACE, a development tool): once a tick, for a
// cardian of a real player's party, whether the navmesh joins where she
// stands to where she stood the tick before -- the polygon under her now
// found among the polygons linked to her last one, within a few yalms of
// it, jump links included -- whether her height is the mesh's there, and
// whether she rose more than a slope the mesh walks. Any of them failing is
// said in the map log, with the last walk she was given and the leg of her
// path, so a move that left the navmesh names itself. A jump link used, where
// no ground joins the two, is said too, as one
void CPawnController::MoveTrace()
{
    const position_t here = POwner->loc.p;
    auto*            zone = POwner->loc.zone;
    if (!settings::get<bool>("pawn.MOVE_TRACE") || zone == nullptr || GetLivePlayer() == nullptr)
    {
        m_MoveTrace.has = false;
        return;
    }
    const position_t from   = m_MoveTrace.last;
    const auto       zoneId = static_cast<uint16>(zone->GetID());
    const bool       had    = m_MoveTrace.has && m_MoveTrace.zone == zoneId;
    m_MoveTrace.last        = here;
    m_MoveTrace.zone        = zoneId;
    m_MoveTrace.has         = true;
    const float across      = distance(from, here, true);
    if (!had || (across < 0.05f && std::abs(here.y - from.y) < 0.05f))
    {
        return;
    }
    const auto*      detour = dynamic_cast<const DetourNavMesh*>(zone->navMesh());
    const dtNavMesh* mesh   = detour != nullptr ? detour->detourMesh() : nullptr;
    // One query a thread, made again only for another mesh
    thread_local dtNavMeshQuery   query;
    thread_local const dtNavMesh* queryMesh = nullptr;
    if (mesh == nullptr)
    {
        return;
    }
    if (queryMesh != mesh)
    {
        queryMesh = nullptr;
        if (dtStatusFailed(query.init(mesh, 256)))
        {
            return;
        }
        queryMesh = mesh;
    }
    dtQueryFilter filter;
    filter.setIncludeFlags(0xFFFF);
    filter.setExcludeFlags(0); // switched-off polygons and links count as joined: the walker drops by them
    const float extents[3]{ 0.5f, 1.5f, 0.5f };
    const auto  a = cardian::route::toDetour(from);
    const auto  b = cardian::route::toDetour(here);
    dtPolyRef   fromRef{};
    dtPolyRef   hereRef{};
    float       fromAt[3]{};
    float       hereAt[3]{};
    query.findNearestPoly(a.data(), extents, &filter, &fromRef, fromAt);
    query.findNearestPoly(b.data(), extents, &filter, &hereRef, hereAt);

    std::vector<std::string> why;
    bool                     byLink = false;
    if (!hereRef)
    {
        why.emplace_back("no navmesh under her");
    }
    else if (fromRef && fromRef != hereRef)
    {
        // Out from her last polygon by its links, keeping to the polygons whose
        // box comes within reach of where she was: over the ground alone
        // first, then with the jump links
        const float reach   = across + 2.0f;
        const auto  joinsBy = [&](const bool links) -> bool
        {
            std::vector<dtPolyRef>        open{ fromRef };
            std::unordered_set<dtPolyRef> seen{ fromRef };
            for (size_t i = 0; i < open.size() && open.size() < 1024; ++i)
            {
                const dtMeshTile* tile = nullptr;
                const dtPoly*     poly = nullptr;
                mesh->getTileAndPolyByRefUnsafe(open[i], &tile, &poly);
                for (unsigned int l = poly->firstLink; l != DT_NULL_LINK; l = tile->links[l].next)
                {
                    const dtPolyRef next = tile->links[l].ref;
                    if (!next || !seen.insert(next).second)
                    {
                        continue;
                    }
                    const dtMeshTile* nextTile = nullptr;
                    const dtPoly*     nextPoly = nullptr;
                    mesh->getTileAndPolyByRefUnsafe(next, &nextTile, &nextPoly);
                    if (!links && nextPoly->getType() == DT_POLYTYPE_OFFMESH_CONNECTION)
                    {
                        continue;
                    }
                    if (next == hereRef)
                    {
                        return true;
                    }
                    float lo[2]{ std::numeric_limits<float>::max(), std::numeric_limits<float>::max() };
                    float hi[2]{ std::numeric_limits<float>::lowest(), std::numeric_limits<float>::lowest() };
                    for (int v = 0; v < nextPoly->vertCount; ++v)
                    {
                        const float* p = &nextTile->verts[nextPoly->verts[v] * 3];
                        lo[0]          = std::min(lo[0], p[0]);
                        hi[0]          = std::max(hi[0], p[0]);
                        lo[1]          = std::min(lo[1], p[2]);
                        hi[1]          = std::max(hi[1], p[2]);
                    }
                    const float dx = std::max({ lo[0] - a[0], 0.0f, a[0] - hi[0] });
                    const float dz = std::max({ lo[1] - a[2], 0.0f, a[2] - hi[1] });
                    if (std::hypot(dx, dz) <= reach)
                    {
                        open.push_back(next);
                    }
                }
            }
            return false;
        };
        if (!joinsBy(false))
        {
            if (joinsBy(true))
            {
                byLink = true;
            }
            else
            {
                why.emplace_back("the navmesh does not join it to where she was");
            }
        }
    }
    if (hereRef && std::abs(hereAt[1] - b[1]) > 1.0f)
    {
        why.emplace_back(fmt::format("the navmesh's ground there is {:.1f}y {}", std::abs(hereAt[1] - b[1]), hereAt[1] > b[1] ? "above her" : "below her"));
    }
    const float rise = from.y - here.y; // y is down
    if (rise > 1.0f + across)
    {
        why.emplace_back(fmt::format("she rose {:.1f}y over {:.1f}y", rise, across));
    }
    if (why.empty() && byLink)
    {
        why.emplace_back("by a jump link");
    }
    if (why.empty())
    {
        return;
    }

    auto*             PPathFind = POwner->PAI->PathFind.get();
    const std::string path      = PPathFind != nullptr && PPathFind->IsFollowingPath()
                                      ? fmt::format("her path finder walks to ({:.1f}, {:.1f}, {:.1f})", PPathFind->GetDestination().x, PPathFind->GetDestination().y, PPathFind->GetDestination().z)
                                      : std::string("no path under her feet");
    std::string       leg       = "no route";
    if (m_Plan.walking && m_Plan.next > 0 && m_Plan.next < m_Plan.route.size())
    {
        const auto& p = m_Plan.route[m_Plan.next - 1];
        const auto& q = m_Plan.route[m_Plan.next];
        leg           = fmt::format("route leg ({:.1f}, {:.1f}, {:.1f}) -> ({:.1f}, {:.1f}, {:.1f})", p.x, p.y, p.z, q.x, q.y, q.z);
    }
    ShowInfoFmt("move trace: {} ({:.1f}, {:.1f}, {:.1f}) -> ({:.1f}, {:.1f}, {:.1f}), {:.1f}y across, {:+.1f}y up: {}; last walk {} to ({:.1f}, {:.1f}, {:.1f}) {:.1f}s ago; {}; {}",
                POwner->getName(), from.x, from.y, from.z, here.x, here.y, here.z, across, rise, fmt::join(why, ", "),
                magic_enum::enum_name(m_MoveTrace.kind), m_MoveTrace.point.x, m_MoveTrace.point.y, m_MoveTrace.point.z,
                std::chrono::duration<float>(m_Tick - m_MoveTrace.at).count(), path, leg);
}
