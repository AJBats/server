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
// decides how, through SafeWalk, which plans, holds, backs off and sneaks
// by, for every walk, a trek's included

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
#include "common/xirand.h"
#include "entities/char_entity.h"
#include "entities/mob_entity.h"
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

namespace
{
    // A short walk is planned whole, on the ground round her and the point,
    // every time the point moves; a long one -- a trek across a zone -- is
    // planned whole as she sets out and then a stretch at a time
    constexpr auto  kCheckEvery   = 1s;      // the route held against where the mobs stand now
    constexpr auto  kHeldRetry    = 3s;      // a choke looked at again
    constexpr auto  kRefreshEvery = 15s;     // a shorter way that opened since
    constexpr auto  kForgotten    = 5s;      // not asked for this long, the route is planned anew
    constexpr float kSameGoal     = 1.0f;    // a trek's line moved less than this is the same walk
    constexpr float kLocalSpare   = 30.0f;   // a walk's ground: half as long again as the way, and this much more
    constexpr float kWiderSpare   = 100.0f;  // ... and where the way round runs wider, three times the way and this much more
    constexpr float kOffRoute     = 2.0f;    // knocked this far off the leg she walks, her route is planned afresh from where she stands
    constexpr float kHopLate      = 2.0f;    // seconds past its planned end a sneak-by is given up: as far as its plan holds (WalkSneakBy's slack)
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

    // Two walks under the same rules: a change of them -- the vet waived for
    // a pull, a race begun, a range asked for -- is a walk planned afresh
    auto sameRules(const Rules& a, const Rules& b) -> bool
    {
        return a.danger == b.danger && a.forceAvoid == b.forceAvoid && a.sightHides == b.sightHides && a.holds == b.holds && a.sneaks == b.sneaks &&
               a.wholeZone == b.wholeZone && std::fabs(a.extra - b.extra) < 0.01f && std::fabs(a.closeTo - b.closeTo) < 0.01f;
    }

    // Two walks that hold for a choke alike: the same rings, the same rule
    // for holding. How near she means to stop is no part of it
    auto sameHold(const Rules& a, const Rules& b) -> bool
    {
        return a.danger == b.danger && a.forceAvoid == b.forceAvoid && a.sightHides == b.sightHides && a.holds == b.holds && a.wholeZone == b.wholeZone &&
               std::fabs(a.extra - b.extra) < 0.01f;
    }

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
    // Memoised per tick, to the yalm: a plan asks about many points, and the
    // mob's own line-of-sight cache is eight entries deep
    if (m_BallSightAt != m_Tick)
    {
        m_BallSight.clear();
        m_BallSightAt = m_Tick;
    }
    // The point to the yalm, its height too: a spot asked about on a slope
    // and the route's point beneath it are two questions
    const uint64_t mob = sphere.mob != nullptr ? sphere.mob->id : 0;
    const auto     qx  = static_cast<uint64_t>(std::lround(point.x)) & 0x7FF;
    const auto     qy  = static_cast<uint64_t>(std::lround(point.y)) & 0x3FF;
    const auto     qz  = static_cast<uint64_t>(std::lround(point.z)) & 0x7FF;
    const uint64_t key = (mob << 32) | (qx << 21) | (qy << 11) | qz;
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

    // The walk asked for is the same one while its point stays put -- within
    // a tenth of a yalm near it, a twentieth of the way far off, so a moving
    // player's spot is planned afresh as it moves and a far one is not
    // planned every tick -- under the same rules. A trek's line is the same
    // within a yalm
    const float within   = rules.wholeZone ? kSameGoal : std::clamp(distance(here, point) * 0.05f, 0.1f, 5.0f);
    const bool  sameGoal = plan.zone == zone && distance(plan.goal, point) <= within && sameRules(plan.rules, rules);

    // A sneak-by is never cut short mid-way: it ends through, late, out of
    // its zone, or once she races, and only then is her walk planned again,
    // however her point has moved meanwhile (the user, 2026-10-13).
    // The one exception is the player taking the wheel: his steering's rules
    // carry no sneak-by
    if (!plan.hop.empty() && !rules.sneaks)
    {
        ShowInfoFmt("pawn: {}: the sneak-by is let go: the player steers her", who);
        plan.hop.clear();
        plan.hopWalking = false;
        plan.known      = false;
        plan.heldSince.reset();
    }

    // On a sneak-by she walks it past the mobs, and nothing else has a say
    // until it is over -- unless she races: alone with a mob on her, she
    // runs for the party or her line instead. On the party's ground, a mob
    // on her or not, the party is there and the sneak-by goes on
    if (WalkHopTick(racing))
    {
        result.outcome = Outcome::Hopping;
        result.target  = plan.hop.empty() ? point : plan.hop.back();
        result.action  = AvoidAction::Detour;
        return result;
    }

    // No route to this point a moment ago: asked again in a while -- unless a
    // ring is on her, which is planned out of now
    if (plan.failed && sameGoal && caughtBy == nullptr && m_Tick - plan.plannedAt < kRefreshEvery)
    {
        result.outcome = Outcome::NoRoute;
        return result;
    }
    bool fresh = !plan.known || !sameGoal || m_Tick - plan.checkedAt > kForgotten;
    if (!fresh && plan.next < plan.route.size())
    {
        plan.next = cardian::walk::progress(plan.route, plan.next, here);
        // Knocked off the leg she walks (a knockback, a shove): planned
        // afresh from where she stands, never a straight line back to it
        if (plan.next > 0 && plan.next < plan.route.size() && cardian::walk::offLeg(plan.route[plan.next - 1], plan.route[plan.next], here) > kOffRoute)
        {
            fresh = true;
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

        // The mobs that matter to this walk: on a trek the zone's every one,
        // planned whole and then a stretch at a time; on any other walk those
        // whose balls reach the ground round her and the point, planned whole
        // every time the point moves
        const float crow     = distance(here, point);
        const bool  longWalk = rules.wholeZone;
        const auto  spheres  = WalkSpheres(rules, longWalk ? kTrekScan : crow * 1.5f + kLocalSpare);
        const auto  hazards  = toHazards(spheres);
        const auto  sight    = sightOf(spheres);
        const auto  nameOf   = [&](const int h) -> std::string
        {
            return h >= 0 && h < static_cast<int>(spheres.size()) && spheres[h].mob != nullptr ? std::string(spheres[h].mob->getName()) : std::string("a mob");
        };

        // The point inside a ring: the safe ground nearest it, and her way
        // there round the ring's edge (the user, 2026-10-12). A trek's line
        // is never moved: a ring on it is a choke, held short of and sneaked
        // by, or raced through
        position_t target = point;
        if (!hazards.empty() && !rules.wholeZone)
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
        const bool newTarget = !plan.known || distance(plan.target, target) > within;

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
                // A walk's ground is the ellipse round her and the target;
                // the way round a ring may run wider, and is then looked for
                // on ground three times the way -- never the whole mesh, which
                // a point no walk reaches would search every time it moves
                const float span = distance(here, target);
                route            = pawn::travel::routeAvoiding(zone, here, target, hazards, sight, longWalk ? 0.0f : span * 1.5f + kLocalSpare);
                searched += route.has_value() ? route->searched : 0;
                if (!longWalk && (!route.has_value() || !route->complete || route->points.empty()))
                {
                    route = pawn::travel::routeAvoiding(zone, here, target, hazards, sight, span * 3.0f + kWiderSpare);
                    searched += route.has_value() ? route->searched : 0;
                }
                plan.plannedAt = m_Tick;
                plan.failed    = !route.has_value() || !route->complete || route->points.empty();
                plan.wholeHeld = !plan.failed && route->firstDanger >= 0;
                plan.wholeAt   = m_Tick;
            }
            const auto spent = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - began);
            const auto took  = std::chrono::duration_cast<std::chrono::milliseconds>(spent).count();
            if (rules.wholeZone)
            {
                pawn::drill::notePlan(POwner->id, spent, searched, stretchTo.has_value());
            }
            if (!stretchTo.has_value() && plan.failed)
            {
                plan.goal    = point;
                plan.zone    = zone;
                plan.rules   = rules;
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
                // A new walk starts its own hold -- unless she holds for a
                // choke alike in the same zone and stands where she held: the
                // point moved round her (the player about his camp), and the
                // choke holds her still, its clock running on. A race begun,
                // or a zone crossed, is a new hold
                const bool holdsOn = plan.zone == zone && sameHold(plan.rules, rules) && distance(here, plan.heldAt) <= kOffRoute;
                if ((fresh || newTarget) && plan.heldSince.has_value() && !holdsOn)
                {
                    plan.heldSince.reset();
                }
                plan.route   = std::move(planned);
                plan.next    = 1;
                plan.danger  = danger;
                plan.goal    = point;
                plan.zone    = zone;
                plan.rules   = rules;
                plan.target  = target;
                plan.known   = true;
                plan.walking = false;
                adopted      = true;

                const cardian::route::DangerTest test(hazards, cardian::route::toDetour(here), sight);
                plan.detour = !hazards.empty() && test.holder(here, target) >= 0;

                // The walk to a range ends short of the point, never past it;
                // the safe ground beside a point in a ring is walked to itself
                if (rules.closeTo > 0.0f && distance(target, point) < 0.01f && plan.danger < 0 && !stretchTo.has_value())
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
                        why = fmt::format("in {}'s reach, {:.1f}y off it, she at ({:.1f}, {:.1f}, {:.1f})", caughtBy->getName(), distance(here, caughtBy->loc.p), here.x,
                                          here.y, here.z);
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
                                                 : !plan.backOff.empty() ? fmt::format(", with no spot short of it out of every reach: she goes {:.0f}y to the nearest ground that is",
                                                                                       cardian::walk::lengthOf(plan.backOff) + distance(here, plan.backOff.front()))
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

    // The action, the word her other code reads: out of a ring first, held,
    // at the safe ground beside her point, or round a ring
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
        plan.heldAt = here;
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
    if (rules.wholeZone)
    {
        pawn::drill::noteHop(POwner->id);
    }
    plan.hop        = std::move(hop.points);
    plan.hopZone    = POwner->getZone();
    plan.hopAt      = m_Tick;
    plan.hopSeconds = hop.seconds;
    plan.hopWalking = false;
    return true;
}

auto CPawnController::WalkHopTick(const bool racing) -> bool
{
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
    if (elapsed > plan.hopSeconds + kHopLate)
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
    const auto& plan = m_Plan;
    return !plan.hop.empty() && POwner->getZone() == plan.hopZone && std::chrono::duration<float>(m_Tick - plan.hopAt).count() <= plan.hopSeconds + kHopLate;
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
            m_TravelStuckSaid    = false;
            m_TravelProgressTime = m_Tick; // no stall: the line's no-progress crossing waits on it
            return;
        case Outcome::Held:
            m_TravelProgressTime = m_Tick;
            SayStuckIfSure(true);
            return;
        case Outcome::NoRoute:
            break;
    }

    // No walk on the mesh to the line: the mesh often ends short of the zone
    // mouth, and the server itself accepts crossings from up to ~40 yalms
    // out; farther, the order is let go, or she says she is stuck and looks
    // again in a while
    if (distToLine < CrossingSlack)
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
    const bool vetted     = intent.vet && !kneelsHere && !PlayersRestHolds() &&
                        (intent.trek || arriving || IsAvoiding() || (intent.keepsClear && proposes && KeepsClearOfAggro()));

    // No monster aggroes a rider (the game's own rule, CMobController): on
    // her chocobo she plans round no danger at all
    Rules rules;
    rules.danger = vetted && !POwner->isMounted();
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
                    PPathFind->StepTo(intent.point);
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
                    const auto goal = intent.trek ? intent.point : CourtesyStep(intent.point, settle);
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
