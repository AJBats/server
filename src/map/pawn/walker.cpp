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

// The walker (ROADMAP W, walker.h): one body's walk planned round the danger
// map's balls, held, backed off and sneaked by, and handed to its path finder

#include "walker.h"

#include "drill.h"
#include "pawn_doors.h"
#include "pawn_travel.h"
#include "sneak.h"

#include "ai/ai_container.h"
#include "ai/controllers/mob_controller.h"
#include "ai/helpers/pathfind.h"
#include "common/logging.h"
#include "common/utils.h"
#include "entities/battle_entity.h"
#include "entities/mob_entity.h"
#include "roam_region.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <limits>
#include <string>
#include <vector>

using namespace std::chrono_literals;
using cardian::walk::Outcome;
using cardian::walk::Rules;

namespace pawn
{
    using namespace walker;

    namespace
    {
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
    } // namespace

    auto walker::toHazards(const std::vector<danger::Sphere>& spheres) -> std::vector<cardian::route::Hazard>
    {
        std::vector<cardian::route::Hazard> out;
        out.reserve(spheres.size());
        for (const auto& s : spheres)
        {
            out.push_back({ .at = s.at, .radius = s.radius, .unseen = s.unseen });
        }
        return out;
    }

    Walker::Walker(CBattleEntity* owner, WalkHost& host)
    : owner_(owner)
    , host_(host)
    {
    }

    auto Walker::ballsFor(const Rules& rules, const float scan) -> std::vector<danger::Sphere>
    {
        if (!rules.danger)
        {
            return {};
        }
        return host_.walkBalls(rules, scan);
    }

    auto Walker::sneakingBy(const timer::time_point tick) const -> bool
    {
        return !plan.hop.empty() && owner_->getZone() == plan.hopZone && std::chrono::duration<float>(tick - plan.hopAt).count() <= plan.hopSeconds + kHopLate;
    }

    void Walker::forgetSight()
    {
        sight_.clear();
    }

    auto Walker::sees(const danger::Sphere& sphere, const position_t& point, const timer::time_point tick) const -> bool
    {
        // Memoised per tick, to the yalm: a plan asks about many points, and the
        // mob's own line-of-sight cache is eight entries deep
        if (sightAt_ != tick)
        {
            sight_.clear();
            sightAt_ = tick;
        }
        // The point to the yalm, its height too: a spot asked about on a slope
        // and the route's point beneath it are two questions
        const uint64_t mob = sphere.mob != nullptr ? sphere.mob->id : 0;
        const auto     qx  = static_cast<uint64_t>(std::lround(point.x)) & 0x7FF;
        const auto     qy  = static_cast<uint64_t>(std::lround(point.y)) & 0x3FF;
        const auto     qz  = static_cast<uint64_t>(std::lround(point.z)) & 0x7FF;
        const uint64_t key = (mob << 32) | (qx << 21) | (qy << 11) | qz;
        if (const auto it = sight_.find(key); it != sight_.end())
        {
            return it->second;
        }
        const bool seen = pawn::danger::sees(sphere, point);
        sight_.emplace(key, seen);
        return seen;
    }

    auto Walker::walk(const position_t& point, const Rules& rules, const timer::time_point tick) -> Result
    {
        tick_        = tick;
        plan.askedAt = tick;
        auto*      PPathFind = owner_->PAI->PathFind.get();
        const auto zone      = owner_->getZone();
        const auto here      = owner_->loc.p;
        const bool racing    = !rules.holds;
        // A trek's lines keep the trek's words, so its log reads as it always has
        const std::string who = rules.wholeZone ? fmt::format("travel {}", owner_->getName()) : std::string(owner_->getName());

        Result result;
        result.target = point;

        // Whether a ball's mob sees a point: the game's line of sight, a wall
        // hiding her; a trek keeps every ring whole and asks none
        const auto sightOf = [&](const std::vector<pawn::danger::Sphere>& balls) -> cardian::route::HazardSight
        {
            if (!rules.sightHides)
            {
                return {};
            }
            return [this, &balls](const size_t h, const position_t& p) { return sees(balls[h], p, tick_); };
        };

        // Every tick, standing in a ring whose mob sees her, she plans again from
        // where she and the mobs stand now, and the route leads out of the ring
        // first (route_search.h, kInsideCost). Nothing her last plan meant is
        // trusted: a mob that moves keeps no promise made against where it stood
        const auto       nearSpheres = ballsFor(rules, kNearRings);
        const auto       nearHazards = toHazards(nearSpheres);
        const CMobEntity* caughtBy   = nullptr;
        ringsNear.clear();
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
                ringsNear.push_back({ .mob = s.mob != nullptr ? s.mob->id : 0, .radius = s.radius, .off = distance(here, s.at) });
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
        if (hopTick(racing))
        {
            result.outcome = Outcome::Hopping;
            result.target  = plan.hop.empty() ? point : plan.hop.back();
            result.action  = WalkAction::Detour;
            return result;
        }

        // No route to this point a moment ago: asked again in a while -- unless a
        // ring is on her, which is planned out of now
        if (plan.failed && sameGoal && caughtBy == nullptr && tick_ - plan.plannedAt < kRefreshEvery)
        {
            result.outcome = Outcome::NoRoute;
            return result;
        }
        bool fresh = !plan.known || !sameGoal || tick_ - plan.checkedAt > kForgotten;
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
        // The door her walk ended short of, open now: the way through it is
        // planned afresh
        if (!fresh && plan.known && plan.door != 0 && !pawn::doors::blocks(owner_->loc.zone, plan.door))
        {
            fresh = true;
        }

        const bool chokeAhead  = plan.known && plan.danger >= 0 && plan.next <= static_cast<size_t>(plan.danger);
        const bool holding     = plan.heldSince.has_value();
        const bool holdUnknown = chokeAhead && !racing && plan.hold < 0 && plan.backOff.empty();
        bool       adopted     = false;
        bool       noGround    = false;

        if (fresh || caughtBy != nullptr || holdUnknown || tick_ - plan.checkedAt >= (holding ? kHeldRetry : kCheckEvery))
        {
            plan.checkedAt = tick_;

            // The mobs that matter to this walk: on a trek the zone's every one,
            // planned whole and then a stretch at a time; on any other walk those
            // whose balls reach the ground round her and the point, planned whole
            // every time the point moves
            const float crow     = distance(here, point);
            const bool  longWalk = rules.wholeZone;
            const auto  spheres  = ballsFor(rules, longWalk ? kTrekScan : crow * 1.5f + kLocalSpare);
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
                    // on the point's own floor: the search walks off a ledge's
                    // drop as readily as along the ground, and safe ground
                    // under the point is no place by it
                    const auto safe = pawn::travel::routeToSafeGround(zone, point, hazards, sight, kSpotSafe);
                    if (safe.has_value() && safe->complete && !safe->points.empty() && std::abs(safe->points.back().y - point.y) <= kSlotFloor)
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

            const bool refresh = tick_ - plan.plannedAt >= kRefreshEvery;
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
                        (route->firstDanger >= 0 && !holding && tick_ - plan.plannedAt >= kRefreshEvery))
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
                    plan.plannedAt = tick_;
                    plan.failed    = !route.has_value() || !route->complete || route->points.empty();
                    plan.wholeHeld = !plan.failed && route->firstDanger >= 0;
                    plan.wholeAt   = tick_;
                }
                const auto spent = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - began);
                const auto took  = std::chrono::duration_cast<std::chrono::milliseconds>(spent).count();
                if (rules.wholeZone)
                {
                    pawn::drill::notePlan(owner_->id, spent, searched, stretchTo.has_value());
                }
                if (!stretchTo.has_value() && plan.failed)
                {
                    plan.goal    = point;
                    plan.zone    = zone;
                    plan.rules   = rules;
                    plan.target  = target;
                    plan.known   = false;
                    plan.walking = false;
                    plan.door    = 0;
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

                // A closed door she may not open is a wall the mesh does not
                // know: her walk ends a yalm short of the first one it meets,
                // and her controller looks whether she crosses it as the game
                // would walk a player through (CPawnController::DoorTick)
                std::optional<pawn::doors::Ahead> doorAhead;
                uint32                            doorKept = 0; // a stretch's tail, the old route's, ends at the door it was cut at
                int                               sideKept = 0;
                if (settings::get<bool>("pawn.DOOR_CROSSING"))
                {
                    doorAhead = pawn::doors::firstOnRoute(owner_, planned);
                    if (!doorAhead.has_value() && stretchTo.has_value() && plan.door != 0 && !plan.route.empty() && distance(planned.back(), plan.route.back()) < 0.05f)
                    {
                        doorKept = plan.door;
                        sideKept = plan.doorSide;
                    }
                }
                if (doorAhead.has_value())
                {
                    planned.resize(doorAhead->leg);
                    if (distance(planned.back(), doorAhead->stop) > 0.05f)
                    {
                        planned.push_back(doorAhead->stop);
                    }
                    if (danger >= static_cast<int>(planned.size()))
                    {
                        danger = -1; // the trouble past the door is not hers yet
                    }
                    else if (danger == static_cast<int>(planned.size()) - 1 && danger > 0 && !hazards.empty() &&
                             cardian::route::DangerTest(hazards, cardian::route::toDetour(here), sight).holder(planned[danger - 1], planned.back()) < 0)
                    {
                        danger = -1; // the ring her cut leg met lay past the door
                    }
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
                    plan.route    = std::move(planned);
                    plan.next     = 1;
                    plan.danger   = danger;
                    plan.door     = doorAhead.has_value() ? doorAhead->npc : doorKept;
                    plan.doorSide = doorAhead.has_value() ? doorAhead->side : sideKept;
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
                    if (rules.closeTo > 0.0f && distance(target, point) < 0.01f && plan.danger < 0 && !stretchTo.has_value() && plan.door == 0)
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
                                    // never through a closed door she may not open
                                    std::vector<position_t> back(1, here);
                                    back.insert(back.end(), safe->points.begin(), safe->points.end());
                                    if (settings::get<bool>("pawn.DOOR_CROSSING"))
                                    {
                                        if (const auto door = pawn::doors::firstOnRoute(owner_, back); door.has_value())
                                        {
                                            back.resize(door->leg);
                                            back.push_back(door->stop);
                                        }
                                    }
                                    back.erase(back.begin());
                                    plan.backOff = std::move(back);
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
                    const bool caughtSaid = caughtBy != nullptr && tick_ - plan.caughtLogAt < 1s;
                    if (rules.wholeZone && (fresh || (caughtBy != nullptr && !caughtSaid) || crossedBy >= 0 || better || held != chokeAhead))
                    {
                        if (caughtBy != nullptr)
                        {
                            plan.caughtLogAt = tick_;
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
                    else if (!rules.wholeZone && (caughtBy != nullptr || held || plan.detour || distance(target, point) > 0.01f) && tick_ - plan.loggedAt >= 1s &&
                             settings::get<bool>("pawn.FORMATION_DEBUG"))
                    {
                        plan.loggedAt = tick_;
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
            result.action = WalkAction::Escape;
        }
        else if (distance(plan.target, point) > 0.01f)
        {
            result.action = WalkAction::Slot;
        }
        else if (plan.detour)
        {
            result.action = WalkAction::Detour;
        }

        if (plan.next >= plan.route.size())
        {
            plan.walking = false;
            result.outcome = caughtBy != nullptr                                       ? Outcome::Escaping
                             : distance(plan.target, point) > 0.01f || plan.door != 0 ? Outcome::Held
                                                                                       : Outcome::Walking;
            return result; // walked it all, or as far as a door
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
                    ShowInfoFmt("pawn: {}: the way opened after {} s", who, std::chrono::duration_cast<std::chrono::seconds>(tick_ - *plan.heldSince).count());
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
                result.action  = WalkAction::Escape;
                return result;
            }
            plan.backOff.clear();
            plan.hold = 1; // on safe ground: she holds here
        }
        // ... and never past the lip of a drop: the drop is her own
        const size_t end = cardian::walk::toNextLip(plan.route, plan.next, choke && !racing && plan.hold >= 0 ? static_cast<size_t>(plan.hold) : plan.route.size());
        if (plan.next >= end)
        {
            if (!plan.heldSince.has_value())
            {
                if (rules.wholeZone)
                {
                    ShowInfoFmt("pawn: {}: holds for the way ahead to open", who);
                    pawn::drill::noteHold(owner_->id);
                }
                plan.heldSince = tick_;
            }
            plan.heldAt = here;
            if (PPathFind->IsFollowingPath())
            {
                PPathFind->Clear();
            }
            plan.walking   = false;
            result.outcome = Outcome::Held;
            result.action  = WalkAction::Hold;

            // Holding, she looks for a sneak-by past the choke
            if (rules.sneaks && sneakBy(rules) && hopTick(racing))
            {
                result.outcome = Outcome::Hopping;
                result.target  = plan.hop.empty() ? plan.target : plan.hop.back();
                result.action  = WalkAction::Detour;
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
                plan.plannedAt = tick_;
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

    auto Walker::sneakBy(const Rules& rules) -> bool
    {
        constexpr auto  kLookEvery = 1s;   // a hold looks for a hop this often
        constexpr auto  kSayEvery  = 10s;  // ... and says why none fits at most this often
        constexpr float kSlack     = 2.0f; // every rest taken to end this many seconds early
        constexpr float kSettle    = 2.0f; // past the mobs, none may reach her for this long after
        constexpr float kSetOff    = 0.5f; // seconds before her first step
        if (tick_ - plan.sneakLookedAt < kLookEvery)
        {
            return false;
        }
        plan.sneakLookedAt = tick_;
        const auto        began = std::chrono::steady_clock::now();
        const std::string who   = rules.wholeZone ? fmt::format("travel {}", owner_->getName()) : std::string(owner_->getName());

        // Every mob that would come for her, as the hop reads it: its sight cone
        // and its reach all round, its walk, the seconds before its rest ends
        // (its last action plus its roam cool, as the game decides when to walk
        // again), and its territory
        const float                         scan = rules.wholeZone ? kTrekScan : distance(owner_->loc.p, plan.target) * 1.5f + kLocalSpare;
        std::vector<cardian::sneak::Mob>    mobs;
        std::vector<cardian::route::Hazard> hazards;
        std::vector<CMobEntity*>            who_;
        for (const auto& s : ballsFor(rules, scan))
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
                m.restLeft         = std::max(0.0f, std::chrono::duration<float>(resumes - tick_).count());
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
        const auto route = pawn::travel::routeAvoiding(owner_->getZone(), owner_->loc.p, plan.target, hazards, cone);
        // A danger later on the route, past where the hop ends, is the everyday
        // walk's: the timed walk meets any before its end on its own
        cardian::sneak::Hop hop;
        std::string         why = "no way to her point on the mesh";
        if (route.has_value() && route->complete)
        {
            std::vector<position_t> steps(1, owner_->loc.p);
            steps.insert(steps.end(), route->points.begin(), route->points.end());
            hop = cardian::sneak::plan(mobs, steps, static_cast<float>(owner_->GetSpeed()) / 17.0f, kSetOff, kSlack, kSettle, true);
            if (hop.caughtBy >= 0)
            {
                // Who would reach her, and why: its rest, its walk, its facing
                const auto& m = mobs[hop.caughtBy];
                why           = fmt::format("{} would reach her {:.0f} s in, {:.0f}y along ({:.0f}y from her now, {}, facing {} her)", who_[hop.caughtBy]->getName(),
                                            hop.caughtAt, hop.walked, distance(m.at, owner_->loc.p),
                                            m.restLeft > 0.0f ? fmt::format("resting {:.0f} s more", m.restLeft) : std::string("walking or about to"),
                                            facing(m.at, owner_->loc.p, 64) ? "toward" : "away from");
            }
            else
            {
                why = fmt::format("never past them on the way ({:.0f}y walked)", hop.walked);
            }
        }
        const auto took = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - began);
        if (!hop.fits)
        {
            if (tick_ - plan.sneakSaidAt >= kSayEvery)
            {
                plan.sneakSaidAt = tick_;
                ShowInfoFmt("pawn: {}: no sneak-by yet: {} ({} mobs, {:.1f} ms)", who, why, mobs.size(), took.count() / 1000.0);
            }
            return false;
        }

        // The shortest rest left among the resting mobs near her, for the log
        float  tightest = std::numeric_limits<float>::max();
        size_t tight    = 0;
        for (size_t m = 0; m < mobs.size(); ++m)
        {
            if (mobs[m].restLeft > 0.0f && mobs[m].restLeft < tightest && distance(mobs[m].at, owner_->loc.p) < mobs[m].reach() + 60.0f)
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
            pawn::drill::noteHop(owner_->id);
        }
        // A sneak-by never takes her through a closed door she may not open
        if (settings::get<bool>("pawn.DOOR_CROSSING"))
        {
            std::vector<position_t> way(1, owner_->loc.p);
            way.insert(way.end(), hop.points.begin(), hop.points.end());
            if (pawn::doors::firstOnRoute(owner_, way).has_value())
            {
                ShowInfoFmt("pawn: {}: no sneak-by: its way runs through a closed door", who);
                return false;
            }
        }
        plan.hop        = std::move(hop.points);
        plan.hopZone    = owner_->getZone();
        plan.hopAt      = tick_;
        plan.hopSeconds = hop.seconds;
        plan.hopWalking = false;
        return true;
    }

    auto Walker::hopTick(const bool racing) -> bool
    {
        constexpr float kThrough = 0.3f; // this near its end, she is past them: her path finder arrives within 0.1
        if (plan.hop.empty())
        {
            return false;
        }
        auto*             PPathFind = owner_->PAI->PathFind.get();
        const float       elapsed   = std::chrono::duration<float>(tick_ - plan.hopAt).count();
        const std::string who       = plan.rules.wholeZone ? fmt::format("travel {}", owner_->getName()) : std::string(owner_->getName());
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
        if (owner_->getZone() != plan.hopZone)
        {
            over("ends with her zone");
            return false;
        }
        if (distance(owner_->loc.p, plan.hop.back()) < kThrough)
        {
            over("is through");
            host_.sneakedThrough();
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
            const auto& here = owner_->loc.p;
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
        host_.sneakStepped();
        return true;
    }
} // namespace pawn
