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

#pragma once

#include "route_search.h"

#include "common/cbasetypes.h"
#include "common/types/position.h"
#include "data/enums/zone.h"

#include <chrono>
#include <optional>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

// World travel for pawns, over sub-zones (pawn/subzone_graph.h): every
// zone's navmesh split into the pieces no walk joins, each zone line from
// the piece its trigger stands on to the piece it lands on (the per-zone
// zoneline datasets: the trigger-box centre in its own zone, the arrival box
// in the destination; each placed on the ground under it, subzones.h
// lineEnd), and the drops between a zone's pieces; a trek takes the shortest
// walk across them, so she leaves by the right gate. Transport links
// (boats, airships) are not zonelines and are naturally absent; mog-house
// doors and intra-zone teleport pads are excluded as self-edges.
namespace pawn
{
    struct TravelHop
    {
        xi::ZoneId destinationZone{};
        position_t walkTo{};   // where she takes the line: the ground under its trigger-box centre (subzones.h lineEnd)
        position_t arriveAt{}; // where it sets her down: the ground under its arrival point, the same way
        bool       rents{};    // a city's stable, not a line: she rents at walkTo, and the rental sets her down at arriveAt, mounted
    };

    // How she travels (ROADMAP T, "Chocobos on the trek"): whether she may
    // rent a chocobo at a city's stable (she holds the license and is on
    // foot), whether she rides already, and a yalm ridden's cost in yalms
    // on foot (pawn.PAWN_SPEED over pawn.RIDE_SPEED)
    struct TravelRide
    {
        bool  mayRent     = false;
        bool  mounted     = false;
        float rideFactor  = 1.0f;
        bool  stayMounted = false; // never gets down on the way: no zone that allows no mount but the one bound for (the porter)
    };

    namespace travel
    {
        // The world's sub-zones cut and joined, once, as the map boots: after
        // the zones' meshes are loaded and before the watchdog watches, since
        // cutting every mesh takes a while
        void prepare();

        // First hop of the shortest walk from `at` in one zone toward another
        // -- to the piece `toPoint` stands on when given, which may be in her
        // own zone, cut off from hers -- or nullopt when no walk joins them,
        // or none is needed. Of the lines that land on the same piece of the
        // same next zone (West Ronfaure has more than one into East Ronfaure)
        // the one nearest `prefer` when given (the line her player just
        // walked through), else the walk's own. A rider (`ride`) plans the
        // quickest way instead, and her first hop may be a stable's rental
        auto nextHop(xi::ZoneId from, const position_t& at, xi::ZoneId to, std::optional<position_t> toPoint = std::nullopt,
                     std::optional<position_t> prefer = std::nullopt, const TravelRide& ride = {}) -> std::optional<TravelHop>;

        // Two bodies in one zone that no walk joins: each stands on the
        // mesh, on a piece of its own (North Gustaberg's lowlands and its
        // high ground), and a walk through other zones goes round from the
        // first's piece to the second's. False when either stands off the
        // mesh, where her piece cannot be told, or when no walk goes round
        auto cutOff(xi::ZoneId zone, const position_t& a, const position_t& b) -> bool;

        // The route from a body at `from` to `to` in `zone` kept out of the
        // hazards (pawn/route_search.h searchAvoiding): its `firstDanger`
        // names where the mobs hold every way, a choke known before she takes
        // a step. Not complete when no walk joins them -- within `reach`,
        // when given, the stretch's own ground; nullopt when the zone has no
        // mesh or she stands off it
        auto routeAvoiding(xi::ZoneId zone, const position_t& from, const position_t& to, const std::vector<cardian::route::Hazard>& hazards,
                           const cardian::route::HazardSight& sight, float reach = 0.0f) -> std::optional<cardian::route::Route>;

        // Whether a walk from `from` reaches `to` -- the route search the
        // walker plans on, round no danger: a complete route to the ground
        // nearest `to`, or one that stops within `within` of it. The ground
        // nearest `to` may lie yalms from it (a point off the mesh)
        auto walks(xi::ZoneId zone, const position_t& from, const position_t& to, float within) -> bool;

        // A town's navmesh by the client's own rule, with the town's props in it
        // (porter_mesh.h), which every route searched in a town keeps to (pawn.TOWN_CLIENT_NAVMESH):
        // a client steps half a yalm, stands on floors to 44.9 degrees and keeps
        // its body half a yalm off every wall, and that navmesh keeps to the same
        // by itself -- no faces, no seams, no step judged too steep: its stairs
        // are the client's -- with a berth off the walls where there is room.
        // Nullptr out of a town, or for a town without one; on trial, every
        // zone one was built for counts as a town here (porter_mesh.h walkedOnClient)
        auto townMesh(xi::ZoneId zone) -> const dtNavMesh*;

        // The nearest point of a town's navmesh by the client's rule to
        // `point` (pawn.TOWN_CLIENT_NAVMESH): a body set down in a town stands
        // where a client's own body could, half a yalm off every wall and on
        // nothing but floor. nullopt out of a town, for a town without one, or
        // with none of it near
        auto snapToTown(xi::ZoneId zone, const position_t& point) -> std::optional<position_t>;

        // Whether a straight walk on a town's navmesh by the client's rule
        // goes from `from` to `to` without leaving it: through no wall and no
        // prop, half a yalm off both. nullopt out of a town, or for a town
        // without one
        auto straightOnTown(xi::ZoneId zone, const position_t& from, const position_t& to) -> std::optional<bool>;

        // How far a straight walk from `from` toward `to` goes on the mesh
        // her walks keep to (townMesh, else the zone's): `to` itself, at
        // `from`'s height, when nothing stops it, else the point where a
        // wall, a ledge's rim or the mesh's own edge does. The ray crosses
        // only the edges two polygons share, never a link, so it stops at
        // every drop -- the ray a follower's point is clipped by. nullopt
        // when the zone has no mesh or `from` stands off it
        auto straightReach(xi::ZoneId zone, const position_t& from, const position_t& to) -> std::optional<position_t>;

        // Whether a walk from `from` ends within `within` yalms of `to`
        // (planar) without dropping off a ledge, on ground within `reach`
        // (the ellipse round the two): the route search on the mesh her
        // walks keep to, round no danger, no link taken that falls past the
        // mesh's climb. False when the zone has no mesh or either stands off it
        auto walksWithoutDrops(xi::ZoneId zone, const position_t& from, const position_t& to, float reach, float within) -> bool;

        // The height (FFXI's y) of the floor at `at`'s spot whose height is
        // nearest its own, within `reach` above or below it: the floor a
        // body walking there stands on, on the town's navmesh by the client's
        // rule in a town (townMesh) and the zone's navmesh elsewhere. nullopt
        // with no floor there that near -- in a town, also where only the
        // zone's navmesh has ground (the town's keeps off the walls): no
        // other navmesh is asked
        auto floorUnder(xi::ZoneId zone, const position_t& at, float reach) -> std::optional<float>;

        // yalms above or below the height her path gives her a body's floor
        // is looked for (floorUnder): her path's height strays from the
        // ground by under a yalm, and two floors in one column lie two
        // yalms or more apart
        constexpr float kFloorReach = 2.0f;

        // The same route over a navmesh of its own -- the porter's
        // (pawn/porter_mesh.h) -- with no faces and no seams: that navmesh
        // keeps to the client's rule by itself, so only its links cross its
        // borders
        auto routeOn(const dtNavMesh& mesh, const position_t& from, const position_t& to, const std::vector<cardian::route::Hazard>& hazards)
            -> std::optional<cardian::route::Route>;

        // The way from a body at `from` in `zone` to the nearest ground out
        // of every reach, `within` yalms of walk at most
        // (pawn/route_search.h searchSafeGround); nullopt when the zone has
        // no mesh or she stands off it
        auto routeToSafeGround(xi::ZoneId zone, const position_t& from, const std::vector<cardian::route::Hazard>& hazards,
                               const cardian::route::HazardSight& sight, float within) -> std::optional<cardian::route::Route>;

        // Whether a zone allows a mount, as the world's travel map was built
        // from the zones' own flags
        auto allowsMount(xi::ZoneId zone) -> bool;

        // The cities a rider reaches through a line from a zone that allows a
        // mount: every zone of type City such a line leads into (the porter's
        // cities, porter.h)
        auto rideableCities() -> std::vector<xi::ZoneId>;

        // A place a rider is timed to: a zone, and the point in it the ride
        // ends at, or none for a city, entered through its gate
        struct RideGoal
        {
            xi::ZoneId                zone{};
            std::optional<position_t> point;
        };

        // The chocobo porter's ride times (porter.h): from `at` in `from` to
        // every goal at once, mounted through the zones that allow a mount
        // (subzone_graph.h rideCosts), each in yalms ridden with a zone
        // line's crossing as kLineCost of them; none where no ride reaches it
        auto rideCosts(xi::ZoneId from, const position_t& at, const std::vector<RideGoal>& goals) -> std::vector<std::optional<float>>;

        // How many zone lines lie between each reachable zone and the nearest of these, a zone itself
        // at 0 (BFS from all of them at once); a zone no line reaches is absent
        auto hops(const std::vector<xi::ZoneId>& from) -> std::unordered_map<uint16, uint32>;

        // The zones a rider reaches from these without getting down: through
        // lines into zones that allow a mount, one after another. A zone of
        // these that allows none (a city) is a way in, never in the answer
        auto rideRegion(const std::vector<xi::ZoneId>& from) -> std::unordered_set<uint16>;

        // The map's budget for treks, pawn.TREK_PLAN_BUDGET_MS of each 400 ms
        // logic tick: every trek's tick is timed into it, and the moment one
        // logic tick's treks together pass it the map log shouts, a
        // [critical] line naming who spent it (dev-servers' map-log scan
        // reports it)
        void noteTrekTime(std::string_view who, std::chrono::steady_clock::duration took);

        // Times one trek's tick into the budget, from its making to its end
        class TrekClock
        {
        public:
            explicit TrekClock(const std::string_view who)
            : who_(who)
            , began_(std::chrono::steady_clock::now())
            {
            }

            ~TrekClock()
            {
                noteTrekTime(who_, std::chrono::steady_clock::now() - began_);
            }

            TrekClock(const TrekClock&)                    = delete;
            auto operator=(const TrekClock&) -> TrekClock& = delete;

        private:
            std::string_view                      who_;
            std::chrono::steady_clock::time_point began_;
        };
    } // namespace travel
} // namespace pawn
