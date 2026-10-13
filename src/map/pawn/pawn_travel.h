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
#include <vector>

// World travel for pawns, over sub-zones (pawn/subzone_graph.h): every
// zone's navmesh split into the pieces no walk joins, each zone line from
// the piece its trigger stands on to the piece it lands on (the per-zone
// zoneline datasets: the trigger-box centre in its own zone, the arrival box
// in the destination), and the drops between a zone's pieces; a trek takes
// the shortest walk across them, so she leaves by the right gate. Transport
// links (boats, airships) are not zonelines and are naturally absent;
// mog-house doors and intra-zone teleport pads are excluded as self-edges.
namespace pawn
{
    struct TravelHop
    {
        xi::ZoneId destinationZone{};
        position_t walkTo{};   // zone-line trigger centre in the current zone
        position_t arriveAt{}; // arrival box centre in the destination zone
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
        // walked through), else the walk's own
        auto nextHop(xi::ZoneId from, const position_t& at, xi::ZoneId to, std::optional<position_t> toPoint = std::nullopt,
                     std::optional<position_t> prefer = std::nullopt) -> std::optional<TravelHop>;

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

        // The way from a body at `from` in `zone` to the nearest ground out
        // of every reach, `within` yalms of walk at most
        // (pawn/route_search.h searchSafeGround); nullopt when the zone has
        // no mesh or she stands off it
        auto routeToSafeGround(xi::ZoneId zone, const position_t& from, const std::vector<cardian::route::Hazard>& hazards,
                               const cardian::route::HazardSight& sight, float within) -> std::optional<cardian::route::Route>;

        // How many zone lines lie between each reachable zone and the nearest of these, a zone itself
        // at 0 (BFS from all of them at once); a zone no line reaches is absent
        auto hops(const std::vector<xi::ZoneId>& from) -> std::unordered_map<uint16, uint32>;

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
