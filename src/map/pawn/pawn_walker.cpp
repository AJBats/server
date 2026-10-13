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

// The one walker (ROADMAP W): every walk a cardian takes is a route the
// scored search plans round the danger map's balls (pawn/route_search.h)
// and walks as planned. A mover proposes where she wants to be (its
// Intent); WalkPlanned decides how, through SafeWalk, which plans, holds
// and backs off the way the trek's walk always has, for every walk. On
// while pawn.SAFE_WALK is; off, Walk keeps the first walker (Avoid and
// the game's own path finder)

#include "pawn_controller.h"

#include "drill.h"
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
#include "entities/char_entity.h"
#include "entities/mob_entity.h"
#include "roam_region.h"
#include "status_effect_container.h"

#include <magic_enum/magic_enum.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <limits>
#include <optional>
#include <string>
#include <vector>

using namespace std::chrono_literals;
using cardian::walk::Outcome;
using cardian::walk::Rules;

namespace
{
    // A short walk is planned whole, on the ground round her and the point,
    // every time the point moves; a long one -- a trek across a zone -- is
    // planned whole as she sets out and then a stretch at a time
    constexpr auto  kCheckEvery   = 1s;      // the route held against where the mobs stand now
    constexpr auto  kHeldRetry    = 3s;      // a choke looked at again
    constexpr auto  kRefreshEvery = 15s;     // a shorter way that opened since
    constexpr auto  kForgotten    = 5s;      // not asked for this long, the route is planned anew
    constexpr float kSameGoal     = 1.0f;    // a point moved less than this is the same walk
    constexpr float kLongWalk     = 150.0f;  // a walk this long or longer is planned as a trek's
    constexpr float kLocalSpare   = 30.0f;   // a short walk's ground: half as long again as the way, and this much more
    constexpr float kStretchPast  = 60.0f;   // a stretch planned again runs this many yalms of her route past the trouble
    constexpr float kStretchSpare = 60.0f;   // ... on ground half as long again as the stretch, and this many yalms more
    constexpr float kCrossingNear = 100.0f;  // a mob crossing her route this near ahead is planned round now; one farther waits until she is nearer
    constexpr float kSafeGround   = 150.0f;  // with no spot short of a choke out of every ring: the nearest ground that is, within this walk
    constexpr float kSpotSafe     = 40.0f;   // a point inside a ring: the safe ground nearest it, within this walk of it
    constexpr float kNearRings    = 20.0f;   // the rings looked at every tick for one on her
    constexpr float kTrekScan     = 2000.0f; // a long walk's mobs: the zone's every one, a choke known however far ahead

    auto toHazards(const std::vector<pawn::danger::Sphere>& spheres) -> std::vector<cardian::route::Hazard>
    {
        std::vector<cardian::route::Hazard> out;
        out.reserve(spheres.size());
        for (const auto& s : spheres)
        {
            out.push_back({ .at = s.at, .radius = s.radius, .unseen = s.unseen });
        }
        return out;
    }
} // namespace

auto CPawnController::SafeWalkOn() const -> bool
{
    return settings::get<bool>("pawn.SAFE_WALK");
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
    // Memoised per tick, to the yalm: a plan asks about many points, and the
    // mob's own line-of-sight cache is eight entries deep
    if (m_BallSightAt != m_Tick)
    {
        m_BallSight.clear();
        m_BallSightAt = m_Tick;
    }
    const uint64_t mob = sphere.mob != nullptr ? sphere.mob->id : 0;
    const auto     qx  = static_cast<uint16_t>(static_cast<int16_t>(std::lround(point.x)));
    const auto     qz  = static_cast<uint16_t>(static_cast<int16_t>(std::lround(point.z)));
    const uint64_t key = (mob << 32) | (static_cast<uint64_t>(qx) << 16) | qz;
    if (const auto it = m_BallSight.find(key); it != m_BallSight.end())
    {
        return it->second;
    }
    const bool seen = pawn::danger::sees(sphere, point);
    m_BallSight.emplace(key, seen);
    return seen;
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
    // Behind the camp's line, as RearCampRoute holds the first walker's
    // paths: the end never in front of it, and at the camp no step further
    // forward than where she stands -- an AoE reposition never routes round
    // a wall through the front. Farther from the flag than her cast range
    // she walks the mesh's own way there
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

auto CPawnController::SafeWalk(const position_t& point, const Rules& rules) -> WalkResult
{
    auto&      plan      = m_Plan;
    auto*      PPathFind = POwner->PAI->PathFind.get();
    const auto zone      = POwner->getZone();
    const auto here      = POwner->loc.p;
    const bool racing    = !rules.holds;
    // A trek's lines keep the trek's words, so its log reads as it always has
    const std::string who = rules.wholeZone ? fmt::format("travel {}", POwner->getName()) : std::string(POwner->getName());

    WalkResult result;
    result.target = point;

    // On a sneak-by she walks it past the mobs, and nothing else has a say
    // until it is over
    if (WalkHopTick(racing))
    {
        result.outcome = Outcome::Hopping;
        result.target  = plan.hop.empty() ? point : plan.hop.back();
        result.action  = AvoidAction::Detour;
        return result;
    }

    const bool sameGoal = plan.zone == zone && distance(plan.goal, point) <= kSameGoal && plan.rules.wholeZone == rules.wholeZone;
    if (plan.failed && sameGoal && m_Tick - plan.plannedAt < kRefreshEvery)
    {
        result.outcome = Outcome::NoRoute;
        return result;
    }
    const bool fresh = !plan.known || !sameGoal || m_Tick - plan.checkedAt > kForgotten;
    if (!fresh && plan.next < plan.route.size())
    {
        plan.next = cardian::walk::progress(plan.route, plan.next, here);
    }

    // Whether a ball's mob sees a point: the game's line of sight, a wall
    // hiding her; a trek keeps every ring whole and asks none
    const auto sightOf = [&](const std::vector<pawn::danger::Sphere>& balls) -> cardian::route::HazardSight
    {
        if (!rules.sightHides)
        {
            return {};
        }
        return [this, &balls](const size_t h, const position_t& p) { return SeesBall(balls[h], p); };
    };

    // Every tick, standing in a ring whose mob sees her, she plans again from
    // where she and the mobs stand now, and the route leads out of the ring
    // first (route_search.h, kInsideCost). Nothing her last plan meant is
    // trusted: a mob that moves keeps no promise made against where it stood
    const auto       nearSpheres = WalkSpheres(rules, kNearRings);
    const auto       nearHazards = toHazards(nearSpheres);
    const CMobEntity* caughtBy   = nullptr;
    m_TrekRingsNear.clear();
    if (!nearHazards.empty())
    {
        const auto                       at = cardian::route::toDetour(here);
        const cardian::route::DangerTest test(nearHazards, at, sightOf(nearSpheres));
        if (const int h = test.exposes(at); h >= 0)
        {
            caughtBy = nearSpheres[h].mob;
        }
        for (const auto& s : nearSpheres)
        {
            m_TrekRingsNear.push_back({ .mob = s.mob != nullptr ? s.mob->id : 0, .radius = s.radius, .off = distance(here, s.at) });
        }
    }

    const bool chokeAhead  = plan.known && plan.danger >= 0 && plan.next <= static_cast<size_t>(plan.danger);
    const bool holding     = plan.heldSince.has_value();
    const bool holdUnknown = chokeAhead && !racing && plan.hold < 0 && plan.backOff.empty();
    bool       adopted     = false;
    bool       noGround    = false;

    if (fresh || caughtBy != nullptr || holdUnknown || m_Tick - plan.checkedAt >= (holding ? kHeldRetry : kCheckEvery))
    {
        plan.checkedAt = m_Tick;

        // The mobs that matter to this walk: on a long one the zone's every
        // one, on a short one those whose balls reach the ground round her
        // and the point
        const float crow     = distance(here, point);
        const bool  longWalk = rules.wholeZone || crow >= kLongWalk;
        const auto  spheres  = WalkSpheres(rules, longWalk ? kTrekScan : crow * 1.5f + kLocalSpare);
        const auto  hazards  = toHazards(spheres);
        const auto  sight    = sightOf(spheres);
        const auto  nameOf   = [&](const int h) -> std::string
        {
            return h >= 0 && h < static_cast<int>(spheres.size()) && spheres[h].mob != nullptr ? std::string(spheres[h].mob->getName()) : std::string("a mob");
        };

        // The point inside a ring: the safe ground nearest it, and her way
        // there round the ring's edge (the user, 2026-10-12)
        position_t target = point;
        if (!hazards.empty())
        {
            const auto at = cardian::route::toDetour(point);
            if (cardian::route::DangerTest(hazards, at, sight).exposes(at) >= 0)
            {
                const auto safe = pawn::travel::routeToSafeGround(zone, point, hazards, sight, kSpotSafe);
                if (safe.has_value() && safe->complete && !safe->points.empty())
                {
                    target = safe->points.back();
                }
            }
        }
        const bool newTarget = !plan.known || distance(plan.target, target) > kSameGoal;

        // The rest of her route, against where the mobs stand now: a
        // crossing within kCrossingNear ahead of her. One farther off is let
        // be: a mob that far ahead moves again before she gets there
        int    crossedBy = -1;
        size_t crossedAt = 0;
        float  remaining = 0.0f;
        if (!fresh && !newTarget)
        {
            const cardian::route::DangerTest test(hazards, cardian::route::toDetour(here), sight);
            position_t                       from = here;
            for (size_t i = plan.next; i < plan.route.size(); ++i)
            {
                if (crossedBy < 0 && remaining < kCrossingNear && (!chokeAhead || i < static_cast<size_t>(plan.danger)))
                {
                    crossedBy = test.holder(from, plan.route[i]);
                    crossedAt = i;
                }
                remaining += distance(from, plan.route[i]);
                from = plan.route[i];
            }
        }

        const bool refresh = m_Tick - plan.plannedAt >= kRefreshEvery;
        if (fresh || newTarget || caughtBy != nullptr || holdUnknown || holding || crossedBy >= 0 || refresh)
        {
            // On a long walk, a stretch is planned again, not the whole way,
            // when a mob comes onto her way or onto her, or while she holds
            // for a choke: from her to the waypoint kStretchPast yalms past
            // the trouble, joined to the rest of her route
            std::optional<size_t> stretchTo;
            float                 stretchLength = 0.0f;
            if (longWalk && !fresh && !newTarget && !refresh && plan.next < plan.route.size())
            {
                size_t from = plan.next;
                if (crossedBy >= 0)
                {
                    from = crossedAt;
                }
                else if (holding && plan.danger >= 0)
                {
                    from = std::max(from, static_cast<size_t>(plan.danger));
                }
                size_t to   = std::min(from, plan.route.size() - 1);
                float  past = 0.0f;
                while (to + 1 < plan.route.size() && past < kStretchPast)
                {
                    past += distance(plan.route[to], plan.route[to + 1]);
                    ++to;
                }
                position_t at = here;
                for (size_t i = plan.next; i <= to; ++i)
                {
                    stretchLength += distance(at, plan.route[i]);
                    at = plan.route[i];
                }
                stretchTo = to;
            }

            const auto                           began    = std::chrono::steady_clock::now();
            int                                  searched = 0;
            std::optional<cardian::route::Route> route;
            if (stretchTo.has_value())
            {
                const float reach = std::max(stretchLength * 1.5f, distance(here, plan.route[*stretchTo])) + kStretchSpare;
                route             = pawn::travel::routeAvoiding(zone, here, plan.route[*stretchTo], hazards, sight, reach);
                searched += route.has_value() ? route->searched : 0;
                if (!route.has_value() || !route->complete || route->points.empty() ||
                    (route->firstDanger >= 0 && !holding && m_Tick - plan.plannedAt >= kRefreshEvery))
                {
                    stretchTo.reset();
                    route.reset();
                }
            }
            if (!route.has_value())
            {
                // A short walk's ground is the ellipse round her and the
                // target; the way round a ring may run wider, and is then
                // looked for on the whole mesh
                const float span = distance(here, target);
                route            = pawn::travel::routeAvoiding(zone, here, target, hazards, sight, longWalk ? 0.0f : span * 1.5f + kLocalSpare);
                searched += route.has_value() ? route->searched : 0;
                if (!longWalk && (!route.has_value() || !route->complete || route->points.empty()))
                {
                    route = pawn::travel::routeAvoiding(zone, here, target, hazards, sight);
                    searched += route.has_value() ? route->searched : 0;
                }
                plan.plannedAt = m_Tick;
                plan.failed    = !route.has_value() || !route->complete || route->points.empty();
                plan.wholeHeld = !plan.failed && route->firstDanger >= 0;
                plan.wholeAt   = m_Tick;
            }
            const auto spent = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - began);
            const auto took  = std::chrono::duration_cast<std::chrono::milliseconds>(spent).count();
            pawn::drill::notePlan(POwner->id, spent, searched, stretchTo.has_value());
            plan.goal  = point;
            plan.zone  = zone;
            plan.rules = rules;
            if (!stretchTo.has_value() && plan.failed)
            {
                plan.target  = target;
                plan.known   = false;
                plan.walking = false;
                plan.route.clear();
                plan.heldSince.reset();
                if (rules.wholeZone)
                {
                    ShowInfoFmt("pawn: {}: no route on the mesh to the line ({} ms)", who, took);
                }
                result.outcome = Outcome::NoRoute;
                result.target  = target;
                return result;
            }

            // The route she would walk: the stretch joined to the rest of
            // hers, or the whole one, her own spot first
            std::vector<position_t> planned(1, here);
            planned.insert(planned.end(), route->points.begin(), route->points.end());
            int danger = route->firstDanger >= 0 ? route->firstDanger + 1 : -1;
            if (stretchTo.has_value())
            {
                const size_t joined = planned.size();
                if (danger < 0 && plan.danger > static_cast<int>(*stretchTo))
                {
                    danger = static_cast<int>(joined + (plan.danger - (*stretchTo + 1)));
                }
                planned.insert(planned.end(), plan.route.begin() + static_cast<std::ptrdiff_t>(*stretchTo + 1), plan.route.end());
            }
            const float length = cardian::walk::lengthOf(planned);

            // Looked at again while her route keeps clear, the new one only
            // when it is shorter by a tenth -- no swapping sides round a mob
            // -- or when the choke ahead has opened
            const bool held   = danger >= 0;
            const bool better = length < remaining * 0.9f;
            if (stretchTo.has_value() || fresh || newTarget || caughtBy != nullptr || holdUnknown || holding || crossedBy >= 0 || better || (chokeAhead && !held))
            {
                if (fresh || newTarget)
                {
                    plan.heldSince.reset(); // a new walk: a hold of the last one is not this one's
                }
                plan.route   = std::move(planned);
                plan.next    = 1;
                plan.danger  = danger;
                plan.target  = target;
                plan.known   = true;
                plan.walking = false;
                adopted      = true;

                const cardian::route::DangerTest test(hazards, cardian::route::toDetour(here), sight);
                plan.detour = !hazards.empty() && test.holder(here, target) >= 0;

                // The walk to a range ends short of the point, never past it;
                // the safe ground beside a point in a ring is walked to itself
                if (rules.closeTo > 0.0f && distance(target, point) < 0.01f && plan.danger < 0)
                {
                    cardian::walk::cutShort(plan.route, rules.closeTo);
                    plan.next = std::min<size_t>(1, plan.route.size());
                }

                // Where she holds for a choke: the last point short of it out
                // of every ring. With none ahead of her, the nearest ground
                // out of every ring, any way within kSafeGround of walk -- back
                // the way she came, an alcove, a side passage; with none, she
                // holds where she stands. Racing, she holds for nothing
                plan.hold = -1;
                plan.backOff.clear();
                if (held && !racing)
                {
                    const auto exposed = [&](const position_t& p) { return test.exposes(cardian::route::toDetour(p)) >= 0; };
                    for (int i = plan.danger - 1; i >= 0; --i)
                    {
                        if (!exposed(plan.route[i]))
                        {
                            plan.hold = i + 1;
                            break;
                        }
                    }
                    if (plan.hold < 0)
                    {
                        const auto safe = pawn::travel::routeToSafeGround(zone, here, hazards, sight, kSafeGround);
                        if (safe.has_value() && safe->complete)
                        {
                            if (safe->points.empty())
                            {
                                plan.hold = 1; // her spot is out of every ring already: she holds here
                            }
                            else
                            {
                                plan.backOff = safe->points;
                            }
                        }
                        else
                        {
                            plan.hold = 1; // no ground out of every ring within her walk: where she stands
                            noGround  = true;
                        }
                    }
                }

                // The plan, in the log: a trek's every one, as it always was;
                // an everyday walk's when danger shaped it, once a second
                const bool caughtSaid = caughtBy != nullptr && m_Tick - plan.caughtLogAt < 1s;
                if (rules.wholeZone && (fresh || (caughtBy != nullptr && !caughtSaid) || crossedBy >= 0 || better || held != chokeAhead))
                {
                    if (caughtBy != nullptr)
                    {
                        plan.caughtLogAt = m_Tick;
                    }
                    std::string why = "the way opened";
                    if (fresh)
                    {
                        why = "set out";
                    }
                    else if (caughtBy != nullptr)
                    {
                        why = fmt::format("in {}'s reach", caughtBy->getName());
                    }
                    else if (crossedBy >= 0)
                    {
                        why = fmt::format("{} crossed her route", nameOf(crossedBy));
                    }
                    else if (held)
                    {
                        why = "held";
                    }
                    else if (better)
                    {
                        why = "a shorter way opened";
                    }
                    std::string choke;
                    if (held)
                    {
                        const auto d     = static_cast<size_t>(plan.danger);
                        float      ahead = 0.0f;
                        for (size_t i = 1; i < d; ++i)
                        {
                            ahead += distance(plan.route[i - 1], plan.route[i]);
                        }
                        // The choke a stretch kept from her last route was found
                        // against where the mobs stood then: with none holding
                        // that step now, the mob whose ring comes nearest it
                        int holder = d < plan.route.size() ? test.holder(plan.route[d - 1], plan.route[d]) : -1;
                        if (holder < 0 && d < plan.route.size())
                        {
                            float nearest = std::numeric_limits<float>::max();
                            for (size_t h = 0; h < hazards.size(); ++h)
                            {
                                const float gap = cardian::walk::offLeg(plan.route[d - 1], plan.route[d], hazards[h].at) - hazards[h].radius;
                                if (gap < nearest)
                                {
                                    nearest = gap;
                                    holder  = static_cast<int>(h);
                                }
                            }
                        }
                        choke = fmt::format("; the way is held by {} {:.0f}y ahead{}", nameOf(holder), ahead,
                                                 racing                 ? std::string(", racing: she does not hold for it")
                                                 : !plan.backOff.empty() ? std::string(", with no spot short of it out of every reach: she goes to the nearest ground that is")
                                                 : noGround              ? std::string(", with no ground out of every reach near her: she holds where she stands")
                                                                         : std::string());
                    }
                    ShowInfoFmt("pawn: {}: route to the line round {} mobs ({}): {:.0f}y in {} waypoints{}, {} points searched, {} ms{}", who, hazards.size(), why,
                                length, plan.route.size() - 1,
                                stretchTo.has_value() ? fmt::format(", the next {:.0f}y planned again", route->length) : std::string(", planned whole"), searched,
                                took, choke);
                }
                else if (!rules.wholeZone && (caughtBy != nullptr || held || plan.detour || distance(target, point) > 0.01f) && m_Tick - plan.loggedAt >= 1s &&
                         settings::get<bool>("pawn.FORMATION_DEBUG"))
                {
                    plan.loggedAt = m_Tick;
                    ShowInfoFmt("pawn: {} walks round {} rings: {:.0f}y{}{}{}{}, {} points searched, {} ms", who, hazards.size(), length,
                                caughtBy != nullptr ? fmt::format(", out of {}'s ring first", caughtBy->getName()) : std::string(),
                                distance(target, point) > 0.01f ? fmt::format(", to the safe ground {:.1f}y from her point", distance(target, point)) : std::string(),
                                plan.detour ? std::string(", round the rings the straight way crosses") : std::string(),
                                held && static_cast<size_t>(plan.danger) < plan.route.size()
                                    ? fmt::format(", held short of {}", nameOf(test.holder(plan.route[plan.danger - 1], plan.route[plan.danger])))
                                    : std::string(),
                                searched,
                                took);
                }
            }
        }
    }
    if (!plan.known)
    {
        result.outcome = Outcome::NoRoute;
        return result;
    }
    result.target = plan.target;

    // The action, in the first walker's words her other code reads: out of a
    // ring first, held, at the safe ground beside her point, or round a ring
    if (caughtBy != nullptr)
    {
        result.action = AvoidAction::Escape;
    }
    else if (distance(plan.target, point) > 0.01f)
    {
        result.action = AvoidAction::Slot;
    }
    else if (plan.detour)
    {
        result.action = AvoidAction::Detour;
    }

    if (plan.next >= plan.route.size())
    {
        plan.walking = false;
        result.outcome = caughtBy != nullptr ? Outcome::Escaping : distance(plan.target, point) > 0.01f ? Outcome::Held : Outcome::Walking;
        return result; // walked it all
    }

    // The part she walks: all of it, or up to the choke while she holds for
    // it to open -- or, with no spot short of it out of every ring, to the
    // nearest ground that is. Racing, she never holds. A choke behind her
    // holds nothing
    const bool choke = plan.danger >= 0 && plan.next <= static_cast<size_t>(plan.danger);
    if (!choke)
    {
        plan.backOff.clear();
        if (plan.heldSince.has_value())
        {
            if (rules.wholeZone)
            {
                ShowInfoFmt("pawn: {}: the way opened after {} s", who, std::chrono::duration_cast<std::chrono::seconds>(m_Tick - *plan.heldSince).count());
            }
            plan.heldSince.reset();
        }
    }
    if (choke && !racing && !plan.backOff.empty())
    {
        // However short the way, she walks it: safe ground is at a ring's
        // edge, and her path finder arrives within 0.1 yalms
        if (distance(here, plan.backOff.back()) > 0.2f)
        {
            if (adopted || !plan.walking || plan.end != 0 || !PPathFind->IsFollowingPath())
            {
                std::vector<pathpoint_t> back;
                back.reserve(plan.backOff.size());
                for (const auto& p : plan.backOff)
                {
                    back.push_back(pathpoint_t{ p, 0s, false });
                }
                if (PPathFind->PathThrough(std::move(back), PATHFLAG_RUN))
                {
                    plan.walking = true;
                    plan.end     = 0; // walking to safe ground, none of her route
                }
            }
            result.outcome = Outcome::BackingOff;
            result.action  = AvoidAction::Escape;
            return result;
        }
        plan.backOff.clear();
        plan.hold = 1; // on safe ground: she holds here
    }
    const size_t end = choke && !racing && plan.hold >= 0 ? static_cast<size_t>(plan.hold) : plan.route.size();
    if (plan.next >= end)
    {
        if (!plan.heldSince.has_value())
        {
            if (rules.wholeZone)
            {
                ShowInfoFmt("pawn: {}: holds for the way ahead to open", who);
                pawn::drill::noteHold(POwner->id);
            }
            plan.heldSince = m_Tick;
        }
        if (PPathFind->IsFollowingPath())
        {
            PPathFind->Clear();
        }
        plan.walking   = false;
        result.outcome = Outcome::Held;
        result.action  = AvoidAction::Hold;

        // Holding, she looks for a sneak-by past the choke
        if (rules.sneaks && WalkSneakBy(rules) && WalkHopTick(racing))
        {
            result.outcome = Outcome::Hopping;
            result.target  = plan.hop.empty() ? plan.target : plan.hop.back();
            result.action  = AvoidAction::Detour;
        }
        return result;
    }

    // Handed to her path finder: a plan adopted, a walk let go, or a path
    // another hand set in between
    const bool elsewhere = PPathFind->IsFollowingPath() && distance(PPathFind->GetDestination(), plan.route[end - 1]) > 0.1f;
    if (adopted || !plan.walking || plan.end != end || !PPathFind->IsFollowingPath() || elsewhere)
    {
        std::vector<pathpoint_t> points;
        points.reserve(end - plan.next);
        for (size_t i = plan.next; i < end; ++i)
        {
            points.push_back(pathpoint_t{ plan.route[i], 0s, false });
        }
        if (!PPathFind->PathThrough(std::move(points), PATHFLAG_RUN))
        {
            ShowInfoFmt("pawn: {}: her path finder refused the route", who);
            plan.known     = false;
            plan.failed    = true;
            plan.plannedAt = m_Tick;
            plan.walking   = false;
            result.outcome = Outcome::NoRoute;
            return result;
        }
        plan.walking = true;
        plan.end     = end;
    }
    result.outcome = caughtBy != nullptr ? Outcome::Escaping : Outcome::Walking;
    return result;
}

auto CPawnController::WalkSneakBy(const Rules& rules) -> bool
{
    constexpr auto  kLookEvery = 1s;   // a hold looks for a hop this often
    constexpr auto  kSayEvery  = 10s;  // ... and says why none fits at most this often
    constexpr float kSlack     = 2.0f; // every rest taken to end this many seconds early
    constexpr float kSettle    = 2.0f; // past the mobs, none may reach her for this long after
    constexpr float kSetOff    = 0.5f; // seconds before her first step
    auto&           plan       = m_Plan;
    if (m_Tick - plan.sneakLookedAt < kLookEvery)
    {
        return false;
    }
    plan.sneakLookedAt = m_Tick;
    const auto        began = std::chrono::steady_clock::now();
    const std::string who   = rules.wholeZone ? fmt::format("travel {}", POwner->getName()) : std::string(POwner->getName());

    // Every mob that would come for her, as the hop reads it: its sight cone
    // and its reach all round, its walk, the seconds before its rest ends
    // (its last action plus its roam cool, as the game decides when to walk
    // again), and its territory
    const float                         scan = rules.wholeZone ? kTrekScan : distance(POwner->loc.p, plan.target) * 1.5f + kLocalSpare;
    std::vector<cardian::sneak::Mob>    mobs;
    std::vector<cardian::route::Hazard> hazards;
    std::vector<CMobEntity*>            who_;
    for (const auto& s : WalkSpheres(rules, scan))
    {
        if (s.mob == nullptr)
        {
            continue;
        }
        cardian::sneak::Mob m;
        m.at       = s.mob->loc.p;
        m.cone     = s.cone;
        m.allRound = s.allRound;
        m.speed    = static_cast<float>(std::max(s.mob->GetSpeed(), s.mob->baseSpeed)) / 17.0f;

        const auto* PPath      = s.mob->PAI->PathFind.get();
        const bool  patrolling = PPath != nullptr && PPath->IsPatrolling();
        const bool  walking    = patrolling || (PPath != nullptr && PPath->IsFollowingPath());
        if (const auto* PController = dynamic_cast<const CMobController*>(s.mob->PAI->GetController()); PController != nullptr && !walking)
        {
            const auto resumes = PController->LastActionTime() + std::chrono::seconds(s.mob->getMobMod(xi::MobMod::RoamCool));
            m.restLeft         = std::max(0.0f, std::chrono::duration<float>(resumes - m_Tick).count());
        }
        if (patrolling)
        {
            m.unbounded = true;
        }
        else if (const RoamRegion* region = s.mob->roamRegion(); region != nullptr)
        {
            m.outside = [region](const position_t& p) { return region->distanceOutside(p); };
        }
        else
        {
            m.home = s.mob->m_SpawnPoint;
            m.roam = s.mob->GetRoamDistance();
        }
        // Away from its territory -- on its way home -- it is not where the
        // model says it can be
        if (!m.unbounded)
        {
            const float away = m.outside ? m.outside(m.at) : std::max(0.0f, distance(m.at, m.home, true) - m.roam);
            m.unbounded      = away > 5.0f;
        }
        mobs.push_back(std::move(m));
        hazards.push_back({ .at = s.at, .radius = s.radius, .unseen = s.allRound });
        who_.push_back(s.mob);
    }

    // Her way past them as they stand now, round each one's cone and its
    // reach all round, then her walk along it played forward to the second
    const cardian::route::HazardSight cone = [&](const size_t h, const position_t& p) { return facing(mobs[h].at, p, 64); };
    const auto route = pawn::travel::routeAvoiding(POwner->getZone(), POwner->loc.p, plan.target, hazards, cone);
    // A danger later on the route, past where the hop ends, is the everyday
    // walk's: the timed walk meets any before its end on its own
    cardian::sneak::Hop hop;
    std::string         why = "no way to her point on the mesh";
    if (route.has_value() && route->complete)
    {
        std::vector<position_t> walk(1, POwner->loc.p);
        walk.insert(walk.end(), route->points.begin(), route->points.end());
        hop = cardian::sneak::plan(mobs, walk, static_cast<float>(POwner->GetSpeed()) / 17.0f, kSetOff, kSlack, kSettle, true);
        if (hop.caughtBy >= 0)
        {
            // Who would reach her, and why: its rest, its walk, its facing
            const auto& m = mobs[hop.caughtBy];
            why           = fmt::format("{} would reach her {:.0f} s in, {:.0f}y along ({:.0f}y from her now, {}, facing {} her)", who_[hop.caughtBy]->getName(),
                                        hop.caughtAt, hop.walked, distance(m.at, POwner->loc.p),
                                        m.restLeft > 0.0f ? fmt::format("resting {:.0f} s more", m.restLeft) : std::string("walking or about to"),
                                        facing(m.at, POwner->loc.p, 64) ? "toward" : "away from");
        }
        else
        {
            why = fmt::format("never past them on the way ({:.0f}y walked)", hop.walked);
        }
    }
    const auto took = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - began);
    if (!hop.fits)
    {
        if (m_Tick - plan.sneakSaidAt >= kSayEvery)
        {
            plan.sneakSaidAt = m_Tick;
            ShowInfoFmt("pawn: {}: no sneak-by yet: {} ({} mobs, {:.1f} ms)", who, why, mobs.size(), took.count() / 1000.0);
        }
        return false;
    }

    // The shortest rest left among the resting mobs near her, for the log
    float  tightest = std::numeric_limits<float>::max();
    size_t tight    = 0;
    for (size_t m = 0; m < mobs.size(); ++m)
    {
        if (mobs[m].restLeft > 0.0f && mobs[m].restLeft < tightest && distance(mobs[m].at, POwner->loc.p) < mobs[m].reach() + 60.0f)
        {
            tightest = mobs[m].restLeft;
            tight    = m;
        }
    }
    ShowInfoFmt("pawn: {}: sneaks by: {:.0f}y to ground past them, {:.0f} s{}{} ({} mobs, {:.1f} ms)", who, hop.walked, hop.seconds,
                hop.island ? ", ground none can ever reach" : "",
                tightest < std::numeric_limits<float>::max() ? fmt::format("; {} rests {:.0f} s more", who_[tight]->getName(), tightest) : std::string(),
                mobs.size(), took.count() / 1000.0);
    pawn::drill::noteHop(POwner->id);
    plan.hop        = std::move(hop.points);
    plan.hopZone    = POwner->getZone();
    plan.hopAt      = m_Tick;
    plan.hopSeconds = hop.seconds;
    plan.hopWalking = false;
    return true;
}

auto CPawnController::WalkHopTick(const bool racing) -> bool
{
    constexpr float kLate    = 2.0f; // seconds past its planned end a hop is given up: as far as its plan holds (WalkSneakBy's slack)
    constexpr float kThrough = 0.3f; // this near its end, she is past them: her path finder arrives within 0.1
    auto&           plan     = m_Plan;
    if (plan.hop.empty())
    {
        return false;
    }
    auto*             PPathFind = POwner->PAI->PathFind.get();
    const float       elapsed   = std::chrono::duration<float>(m_Tick - plan.hopAt).count();
    const std::string who       = plan.rules.wholeZone ? fmt::format("travel {}", POwner->getName()) : std::string(POwner->getName());
    const auto        over      = [&](const std::string_view how)
    {
        ShowInfoFmt("pawn: {}: the sneak-by {} after {:.0f} s", who, how, elapsed);
        plan.hop.clear();
        plan.hopWalking = false;
        plan.known      = false; // her route is planned afresh from where the hop left her
        plan.heldSince.reset();
        if (PPathFind->IsFollowingPath())
        {
            PPathFind->Clear();
        }
    };

    // Caught on the way, the chase has her; at its end, past her time to
    // reach it, or out of the zone it was planned in, the everyday rules take
    // over again
    if (racing)
    {
        over("is caught");
        return false;
    }
    if (POwner->getZone() != plan.hopZone)
    {
        over("ends with her zone");
        return false;
    }
    if (distance(POwner->loc.p, plan.hop.back()) < kThrough)
    {
        over("is through");
        if (m_TrekStuckSaid)
        {
            TrekGotBy("sneaked by");
        }
        return false;
    }
    if (elapsed > plan.hopSeconds + kLate)
    {
        over("runs late, and is given up");
        return false;
    }

    if (!plan.hopWalking || !PPathFind->IsFollowingPath())
    {
        // From the end of the leg she stands nearest: her path finder,
        // cleared on the way, never takes her back toward the mobs, nor cuts
        // a corner
        const auto& here = POwner->loc.p;
        size_t      from = plan.hop.size() - 1;
        float       best = std::numeric_limits<float>::max();
        for (size_t i = 1; i < plan.hop.size(); ++i)
        {
            if (const float d = cardian::walk::offLeg(plan.hop[i - 1], plan.hop[i], here); d < best)
            {
                best = d;
                from = i;
            }
        }
        std::vector<pathpoint_t> points;
        points.reserve(plan.hop.size() - from);
        for (size_t i = from; i < plan.hop.size(); ++i)
        {
            points.push_back(pathpoint_t{ plan.hop[i], 0s, false });
        }
        PPathFind->PathThrough(std::move(points), PATHFLAG_RUN);
        plan.hopWalking = true;
        plan.walking    = false; // her route's walk is handed to her path finder again after the hop
    }
    m_TravelProgressTime = m_Tick;
    return true;
}

auto CPawnController::WalkPlanned(Intent intent) -> std::optional<AvoidAction>
{
    // The claim board first (claim_board.h): no two party members aim for
    // one spot, whichever mover proposed it
    ClaimSpot(intent);

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
    const bool vetted     = intent.vet && !kneelsHere && !PlayersRestHolds() &&
                        (intent.trek || IsAvoiding() || (intent.keepsClear && proposes && KeepsClearOfAggro()));

    Rules rules;
    rules.danger = vetted;
    if (intent.trek)
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
        // The everyday walk plans against the rings padded as the first
        // walker's were, so a mob that steps her way does not find her on
        // its edge
        rules.extra = cardian::rules::kClearance;
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
                    PPathFind->StepTo(intent.point);
                    m_Plan.walking = false;
                }
                else
                {
                    walk(intent.point, 0.0f);
                }
                break;
            }
            case Intent::Kind::Path:
            case Intent::Kind::Formation:
            {
                const float away = distance(POwner->loc.p, intent.point);
                if (away > followMax || exposed)
                {
                    // The player's personal space: her goal stepped clear of
                    // his body and his wake, as the first walker's every path
                    const auto goal = intent.trek ? intent.point : CourtesyStep(intent.point);
                    const auto out  = walk(goal, intent.kind == Intent::Kind::Path || away > followMax ? followTarget : 0.0f);
                    trying         = out.outcome == Outcome::Walking && out.action != AvoidAction::Slot && out.action != AvoidAction::Hold;

                    // The camp's line: normal positioning stays behind it, as the
                    // first walker's did; a walk shaped by danger overrides it
                    const bool campRefuses = out.outcome != Outcome::NoRoute && intent.rearBoundary.has_value() && action == AvoidAction::None &&
                                             !CampLineHolds(std::vector<position_t>(m_Plan.route.begin() + static_cast<std::ptrdiff_t>(std::min(m_Plan.next, m_Plan.route.size())), m_Plan.route.end()),
                                                            *intent.rearBoundary);
                    if (campRefuses)
                    {
                        PPathFind->Clear();
                        m_Plan.walking = false;
                    }
                    if (out.outcome == Outcome::NoRoute || campRefuses)
                    {
                        // The camp's line refusing her route keeps her where she
                        // is on purpose: she waits, she is not caught
                        trying = !intent.rearBoundary.has_value();
                        // Said once a spot: where she cannot walk to, and from where
                        if (!m_PathFailedAt.has_value() || distance(*m_PathFailedAt, intent.point) > 1.0f)
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
                            return WalkPlanned(std::move(intent));
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
    NoteWalk(trying, intent.point, followTarget);

    PPathFind->FollowPath(m_Tick);

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

    // A change of action is always worth a line, as the first walker's was
    if (m_LastAvoidAction != action && action != AvoidAction::None)
    {
        ShowInfoFmt("pawn: {} walks ({}): to ({:.1f}, {:.1f}, {:.1f}), {:.1f}y off the point asked for", POwner->getName(), magic_enum::enum_name(action), point.x, point.y,
                    point.z, distance(point, intent.point));
    }
    m_LastAvoidAction = action;
    return action;
}
