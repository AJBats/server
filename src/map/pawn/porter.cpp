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

#include "porter.h"

#include "cardian_link.h"
#include "link_api.h"
#include "pawn.h"
#include "pawn_danger.h"
#include "pawn_travel.h"
#include "players.h"
#include "porter_math.h"
#include "safe_walk.h"

#include "common/database.h"
#include "common/logging.h"
#include "common/lua.h"
#include "common/settings.h"
#include "common/timer.h"
#include "common/utils.h"
#include "entities/char_entity.h"
#include "lua/lua_base_entity.h"
#include "packets/s2c/0x05b_wpos.h"
#include "pause/pause.h"
#include "status_effect.h"
#include "status_effect_container.h"
#include "utils/moduleutils.h"
#include "utils/zoneutils.h"
#include "zone.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <limits>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

using namespace std::chrono_literals;

namespace pawn::porter
{
    namespace
    {
        using namespace cardian::link;
        using cardian::porter::End;
        using cardian::porter::Ends;
        using cardian::porter::Kind;
        using cardian::porter::Place;
        using cardian::porter::Point;

        constexpr auto  kLandedFor    = 1500ms; // after his zone-in, and a position from his client since: he is in
        constexpr auto  kAskFor       = 60s;    // the question after a rental waits this long for him to land and his addon to hear it
        constexpr auto  kGetDownBeat  = 1500ms; // got down at a city's gate: the dismount played before he goes on on foot
        constexpr float kGateGetDown  = 10.0f;  // a city's gate: his route stops this far short of its line, short of its box, which the game refuses a rider
        constexpr float kSpotCloseTo  = 2.0f;   // a place's spot: his route ends this far short of it
        constexpr float kMobOnReach   = 30.0f;  // a mob on him this near ends the ride
        constexpr float kMeshEndYalms = 10.0f;  // a place's spot off the mesh: his route's end within this of it is there
        constexpr float kLineSlack    = 40.0f;  // a line's mouth off the mesh: from the mesh's end within this of it, he runs straight on to it
        constexpr float kLineReach    = 3.0f;   // within this of a line's spot, his client runs on through it ...
        constexpr auto  kRunOnGrace   = 3s;     // ... and crosses it within this, or the server crosses it for him
        constexpr auto  kReplanEvery  = 2s;     // strayed, his route is planned again at most this often
        constexpr auto  kSettle       = 2s;     // after a set-down, the positions his client sent from before it are let pass
        constexpr float kStuckFar     = 50.0f;  // a snag told of this far from where the server has him is no snag of his route's
        constexpr auto  kLandingLost  = 60s;    // a zone change that has not landed in this long never will

        // A ride under way
        struct Journey
        {
            Place  place;
            uint16 ride    = 0;     // its number, which its routes carry
            bool   landing = false; // between zones, or zoned in and his client not yet in
            realtime::time_point landedAt{};

            // What it knows of his zone, forgotten at every zone change
            uint16                         zone = 0; // the zone it is of; 0 before any
            std::optional<pawn::TravelHop> hop;      // the trek's line out of it, chosen as he enters it and kept
            bool                           gotDown = false; // at the city's gate, off his chocobo
            timer::time_point              gotDownAt{};

            // His route across it, his spot as it was planned first
            std::vector<Point>               route;
            Ends                             ends    = Ends::Stop;
            bool                             planned = false;
            std::size_t                      next    = 0; // the waypoint he rides to next
            std::size_t                      sentTo  = 0; // the waypoints sent: up to here
            uint16                           seq     = 0; // the last route sent, which a snag names
            bool                             resend  = false;
            timer::time_point                plannedAt{};
            timer::time_point                settleUntil{};
            std::optional<timer::time_point> atLineSince; // running on through the line since

            cardian::porter::Snags snags;
            std::optional<Point>   stuck; // a snag his addon told of, answered on his next tick

            // His way left at the last plan, the least at any plan in the zone,
            // and the replans after straying that gained nothing (porter_math.h stalls)
            float left   = 0.0f;
            float best   = std::numeric_limits<float>::max();
            int   stalls = 0;
        };
        std::unordered_map<uint32, Journey> journeys; // by his charid
        uint16                              lastRide = 0;

        // The question after his rental: put once he has landed and his
        // client is in
        struct Question
        {
            realtime::time_point askedAt{};
            bool                 landed = false;
            realtime::time_point landedAt{};
        };
        std::unordered_map<uint32, Question> questions;

        void forgetZone(Journey& j)
        {
            j.zone    = 0;
            j.hop.reset();
            j.gotDown = false;
            j.route.clear();
            j.planned = false;
            j.next    = 0;
            j.sentTo  = 0;
            j.resend  = false;
            j.settleUntil = {};
            j.atLineSince.reset();
            j.snags = {};
            j.stuck.reset();
            j.left   = 0.0f;
            j.best   = std::numeric_limits<float>::max();
            j.stalls = 0;
        }

        // Where riders snagged, learned on every ride and kept
        // (cardian_porter_snags): every route in a zone keeps clear of its
        // spots, where the mesh goes and a client does not -- a tangle of
        // little cliffs gathers several. A soak fills it ahead of play
        struct Learned
        {
            uint32 id = 0;
            Point  at{};
        };
        std::unordered_map<uint16, std::vector<Learned>> learned;

        void loadSnags(const uint16 zone)
        {
            auto& spots = learned[zone];
            spots.clear();
            if (const auto rset = db::preparedStmt("SELECT id, x, y, z FROM cardian_porter_snags WHERE zone = ?", zone); rset)
            {
                while (rset->next())
                {
                    spots.push_back(Learned{ .id = rset->get<uint32>("id"), .at = Point{ rset->get<float>("x"), rset->get<float>("y"), rset->get<float>("z") } });
                }
            }
        }

        void loadAllSnags()
        {
            learned.clear();
            db::preparedStmt("CREATE TABLE IF NOT EXISTS `cardian_porter_snags` ("
                             "`id` int(10) unsigned NOT NULL AUTO_INCREMENT, "
                             "`zone` smallint(5) unsigned NOT NULL, "
                             "`x` float NOT NULL, `y` float NOT NULL, `z` float NOT NULL, "
                             "`hits` int(10) unsigned NOT NULL DEFAULT 1, "
                             "`last_hit` datetime NOT NULL DEFAULT CURRENT_TIMESTAMP, "
                             "PRIMARY KEY (`id`), KEY `zone` (`zone`)) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_general_ci");
            std::size_t count = 0;
            if (const auto rset = db::preparedStmt("SELECT DISTINCT zone FROM cardian_porter_snags"); rset)
            {
                std::vector<uint16> zones;
                while (rset->next())
                {
                    zones.push_back(rset->get<uint16>("zone"));
                }
                for (const auto zone : zones)
                {
                    loadSnags(zone);
                    count += learned[zone].size();
                }
            }
            ShowInfoFmt("porter: {} snag spot(s) learned in {} zone(s)", count, learned.size());
        }

        // A snag where he stands: its spot counted again, or learned
        void learnSnag(const uint16 zone, const Point& at)
        {
            auto& spots = learned[zone];
            for (const auto& l : spots)
            {
                if (cardian::porter::flat(l.at, at) <= cardian::porter::kSnagSameSpot)
                {
                    db::preparedStmt("UPDATE cardian_porter_snags SET hits = hits + 1, last_hit = NOW() WHERE id = ?", l.id);
                    return;
                }
            }
            db::preparedStmt("INSERT INTO cardian_porter_snags (zone, x, y, z) VALUES (?, ?, ?, ?)", zone, at.x, at.y, at.z);
            loadSnags(zone);
            ShowInfoFmt("porter: snag spot learned in zone {} at ({:.1f}, {:.1f}, {:.1f}), {} there now", zone, at.x, at.y, at.z, spots.size());
        }

        auto kindName(const Kind kind) -> const char*
        {
            switch (kind)
            {
                case Kind::City:
                    return "city";
                case Kind::Outpost:
                    return "outpost";
                case Kind::Crag:
                    return "crag";
            }
            return "?";
        }

        auto spotOf(const Place& place) -> position_t
        {
            position_t p{};
            p.x = place.x;
            p.y = place.y;
            p.z = place.z;
            return p;
        }

        auto pointOf(const position_t& p) -> Point
        {
            return Point{ p.x, p.y, p.z };
        }

        auto positionOf(const Point& p) -> position_t
        {
            position_t out{};
            out.x = p.x;
            out.y = p.y;
            out.z = p.z;
            return out;
        }

        auto zoneName(const CCharEntity* PChar) -> std::string
        {
            return PChar->loc.zone != nullptr ? std::string(PChar->loc.zone->getName()) : std::string("?");
        }

        // A ride's time, as Ride to shows it
        auto clockOf(const float seconds) -> std::string
        {
            const auto whole = static_cast<int>(std::max(0.0f, seconds));
            return fmt::format("{}:{:02}", whole / 60, whole % 60);
        }

        // His chocobo's pace on the server's step, yalms a second
        auto ridePace() -> float
        {
            return static_cast<float>(settings::get<uint8>("pawn.RIDE_SPEED")) / 17.0f;
        }

        // The time left on his rental, in seconds, off its MOUNTED effect;
        // none for a mount with no end, nothing with none
        auto rentalLeft(CCharEntity* PChar) -> std::optional<float>
        {
            const auto* effect = PChar->StatusEffectContainer->GetStatusEffect(xi::StatusEffect::Mounted);
            if (effect == nullptr)
            {
                return 0.0f;
            }
            if (effect->GetDuration() <= timer::duration::zero())
            {
                return std::nullopt;
            }
            const auto left = effect->GetStartTime() + effect->GetDuration() - timer::now();
            return std::max(0.0f, std::chrono::duration<float>(left).count());
        }

        // The places' library is loaded (the pawn module's init, which may come
        // after the map's first time-server tick)
        auto libraryReady() -> bool
        {
            const sol::object cardian = ::lua["xi"]["cardian"];
            if (cardian.get_type() != sol::type::table)
            {
                return false;
            }
            const sol::object porter = cardian.as<sol::table>()["porter"];
            return porter.get_type() == sol::type::table && porter.as<sol::table>()["places"].get_type() == sol::type::function;
        }

        // Every place the porter rides to, built once its library is loaded:
        // the cities from the trek's world graph (travel::rideableCities), the
        // outposts and crags from the game's own scripts
        // (modules/cardian/lua/porter_places.lua). None before
        auto places() -> const std::vector<Place>&
        {
            static std::vector<Place> built;
            static bool               done = false;
            if (done || !libraryReady())
            {
                return built;
            }
            done = true;
            for (const auto zone : pawn::travel::rideableCities())
            {
                built.push_back(Place{ .kind = Kind::City, .zone = static_cast<uint16>(zone) });
            }
            if (auto call = pawn::linkapi::libraryCall("porter", "places"))
            {
                if (const auto list = pawn::linkapi::libraryTable("porter.places", (*call)()))
                {
                    for (const auto& [_, value] : *list)
                    {
                        if (value.get_type() != sol::type::table)
                        {
                            continue;
                        }
                        const auto row  = value.as<sol::table>();
                        const auto kind = row.get<sol::optional<std::string>>("kind").value_or(std::string{});
                        Place      place{};
                        place.kind = kind == "outpost" ? Kind::Outpost : Kind::Crag;
                        place.key  = row.get<sol::optional<uint16>>("key").value_or(uint16{ 0 });
                        place.zone = row.get<sol::optional<uint16>>("zone").value_or(uint16{ 0 });
                        place.x    = row.get<sol::optional<float>>("x").value_or(0.0f);
                        place.y    = row.get<sol::optional<float>>("y").value_or(0.0f);
                        place.z    = row.get<sol::optional<float>>("z").value_or(0.0f);
                        if ((kind == "outpost" || kind == "crag") && place.zone != 0)
                        {
                            built.push_back(place);
                        }
                    }
                }
            }
            std::array<std::size_t, 4> byKind{};
            for (std::size_t i = 0; i < built.size(); ++i)
            {
                built[i].number = static_cast<uint16>(i);
                ++byKind[static_cast<uint8>(built[i].kind)];
            }
            ShowInfoFmt("porter: the chocobo porter rides to {} places: {} cities, {} outposts, {} crags", built.size(), byKind[1], byKind[2], byKind[3]);
            return built;
        }

        // The places offered from where he stands
        auto offersFor(const CCharEntity* PChar) -> std::vector<cardian::porter::Offer>
        {
            const auto zone = PChar->getZone();
            return cardian::porter::offered(places(), static_cast<uint16>(zone), pawn::travel::hops({ zone }), [](const uint16 z)
            {
                return pawn::travel::allowsMount(static_cast<xi::ZoneId>(z));
            });
        }

        auto goalOf(const Place& place) -> pawn::travel::RideGoal
        {
            return { .zone = static_cast<xi::ZoneId>(place.zone), .point = place.kind == Kind::City ? std::nullopt : std::optional<position_t>(spotOf(place)) };
        }

        // Ride to's list from where he stands: each place a ride reaches,
        // the quickest first, with its time and whether his rental covers
        // it, as of now
        auto placesMessage(CCharEntity* PChar) -> cl_porter_places
        {
            auto       msg    = make<cl_porter_places>();
            const auto offers = offersFor(PChar);
            std::vector<pawn::travel::RideGoal> goals;
            goals.reserve(offers.size());
            for (const auto& offer : offers)
            {
                goals.push_back(goalOf(offer.place));
            }
            const auto  costs = pawn::travel::rideCosts(PChar->getZone(), PChar->loc.p, goals);
            const float pace  = ridePace();
            const auto  left  = rentalLeft(PChar);

            struct Timed
            {
                const cardian::porter::Offer* offer   = nullptr;
                uint16                        seconds = 0;
                bool                          covered = false;
            };
            std::vector<Timed> timed;
            for (std::size_t i = 0; i < offers.size(); ++i)
            {
                if (!costs[i].has_value())
                {
                    continue; // no ride reaches it: its way runs through a zone that allows no mount
                }
                const auto seconds = cardian::porter::rideSeconds(*costs[i], pace);
                timed.push_back({ &offers[i], seconds, cardian::porter::rentalCovers(seconds, left) });
            }
            std::stable_sort(timed.begin(), timed.end(), [](const Timed& a, const Timed& b) { return a.seconds < b.seconds; });
            for (const auto& t : timed)
            {
                if (msg.count >= std::size(msg.places))
                {
                    break;
                }
                auto& out   = msg.places[msg.count++];
                out.place   = t.offer->place.number;
                out.kind    = static_cast<uint8_t>(t.offer->place.kind);
                out.hops    = t.offer->hops;
                out.zone    = t.offer->place.zone;
                out.key     = t.offer->place.key;
                out.seconds = t.seconds;
                out.flags   = t.covered ? 0 : CL_PORTER_PLACE_SHORT;
            }
            return msg;
        }

        auto stateMessage(const Place* place, const uint16 ride, const bool riding, const uint8 why) -> cl_porter_state
        {
            auto msg   = make<cl_porter_state>();
            msg.riding = riding ? 1 : 0;
            msg.why    = why;
            msg.ride   = ride;
            if (place != nullptr)
            {
                msg.kind = static_cast<uint8_t>(place->kind);
                msg.zone = place->zone;
                msg.key  = place->key;
            }
            return msg;
        }

        // His route to his addon: a stretch of it, the leg he rides from its
        // start and on. Sent again on his next tick when the Link cannot take it
        void sendRoute(const uint32 charid, Journey& j)
        {
            const std::size_t legStart = j.next > 0 ? j.next - 1 : 0;
            const auto        seg      = cardian::porter::segment(j.route.size(), std::min(legStart, j.route.empty() ? std::size_t{ 0 } : j.route.size() - 1), j.ends);
            auto       msg = make<cl_porter_route>();
            msg.ride       = j.ride;
            msg.seq        = ++j.seq;
            msg.zone       = j.zone;
            msg.ends       = static_cast<uint8_t>(seg.ends);
            for (std::size_t i = seg.from; i < seg.to && msg.count < std::size(msg.points); ++i)
            {
                auto& p = msg.points[msg.count++];
                p.x     = j.route[i].x;
                p.y     = j.route[i].y;
                p.z     = j.route[i].z;
            }
            j.sentTo = seg.from + msg.count;
            j.resend = !send(charid, msg);
        }

        void finish(CCharEntity* PChar, const uint32 charid, const End why, const std::string_view how)
        {
            const auto it = journeys.find(charid);
            if (it == journeys.end())
            {
                return;
            }
            const Place  place = it->second.place;
            const uint16 ride  = it->second.ride;
            journeys.erase(it);
            ShowInfoFmt("porter: {}'s ride to the {} in zone {} ends ({})", PChar != nullptr ? PChar->getName() : std::to_string(charid), kindName(place.kind), place.zone, how);
            send(charid, stateMessage(&place, ride, false, static_cast<uint8>(why)));
        }

        // A zone line crossed by the server, as his client's run into it
        // would be: noted as a walk, so his party follows through where he
        // crossed (pawn.h noteZoneLine), and the zone change asked for as the
        // game's own setPos asks for one
        auto cross(CCharEntity* PChar, const pawn::TravelHop& hop, const std::string_view why) -> bool
        {
            if (zoneutils::GetZoneIPP(hop.destinationZone) == 0)
            {
                return false;
            }
            ShowInfoFmt("porter: {} at ({:.1f}, {:.1f}, {:.1f}) in {} {}: the server crosses the line to zone {} for him{}", PChar->getName(), PChar->loc.p.x,
                        PChar->loc.p.y, PChar->loc.p.z, zoneName(PChar), why, static_cast<uint16>(hop.destinationZone), PChar->isMounted() ? ", riding" : ", on foot");
            pawn::noteZoneLine(PChar);
            PChar->loc.destination     = hop.destinationZone;
            PChar->loc.p.x             = hop.arriveAt.x;
            PChar->loc.p.y             = hop.arriveAt.y;
            PChar->loc.p.z             = hop.arriveAt.z;
            PChar->status              = xi::Status::Disappear;
            PChar->loc.boundary        = 0;
            PChar->m_moghouseID        = 0;
            PChar->requestedZoneChange = true;
            if (PChar->shouldPetPersistThroughZoning())
            {
                PChar->setPetZoningInfo();
            }
            return true;
        }

        // The line taken by the server for him; his body is loaded afresh in
        // the next zone
        void take(CCharEntity* PChar, const uint32 charid, Journey& j, const std::string_view why)
        {
            if (!cross(PChar, *j.hop, why))
            {
                finish(PChar, charid, End::NoWay, fmt::format("zone {} is closed", static_cast<uint16>(j.hop->destinationZone)));
                return;
            }
            j.landing  = true;
            j.landedAt = realtime::now();
            forgetZone(j);
        }

        // The game's own same-zone position set (setPos within a zone): his
        // body put there, the zone told it moved, his client told where he
        // stands (0x05B)
        void setDownAt(CCharEntity* PChar, const position_t& to)
        {
            PChar->loc.p.x = to.x;
            PChar->loc.p.y = to.y;
            PChar->loc.p.z = to.z;
            if (PChar->loc.zone != nullptr)
            {
                PChar->loc.zone->onEntityMoved(PChar);
            }
            PChar->pushPacket<GP_SERV_COMMAND_WPOS>(PChar, PChar->loc.p);
            PChar->updatemask |= UPDATE_POS;
        }

        // A rented chocobo goes into no city: at its gate he gets down, short
        // of the line, and once the dismount has played he goes on and
        // through on foot
        void getDown(CCharEntity* PChar, Journey& j, const timer::time_point now)
        {
            PChar->StatusEffectContainer->DelStatusEffect(xi::StatusEffect::Mounted);
            j.gotDown   = true;
            j.gotDownAt = now;
            j.planned   = false;
            ShowInfoFmt("porter: {} gets down at the gate of zone {}, {:.0f}y from its line", PChar->getName(), j.place.zone, distance(PChar->loc.p, j.hop->walkTo));
        }

        // His route across this zone from `from`: to the place's spot in its
        // zone; else to the trek's line, stopping short of it at the gate of
        // the city he is bound for until he is got down. Sent at once. False
        // when the ride ended for want of one
        auto plan(CCharEntity* PChar, const uint32 charid, Journey& j, const position_t& from, const timer::time_point now, const std::string_view why) -> bool
        {
            const auto  zone   = PChar->getZone();
            const bool  inZone = static_cast<uint16>(zone) == j.place.zone;
            const bool  line   = !inZone;
            const bool  gate   = line && !j.gotDown &&
                              cardian::porter::atLine(pawn::travel::allowsMount(j.hop->destinationZone),
                                                      j.place.kind == Kind::City && static_cast<uint16>(j.hop->destinationZone) == j.place.zone) ==
                                  cardian::porter::AtLine::GetDown;
            const auto  goal   = inZone ? spotOf(j.place) : j.hop->walkTo;
            const float slack  = line ? kLineSlack : kMeshEndYalms;

            // The trek's route search over the zone's mesh: no monster aggroes
            // a rider, so it goes round nothing but the spots he snagged at
            // in this zone, where the mesh goes and his client does not
            std::vector<cardian::route::Hazard> snagged;
            if (const auto it = learned.find(static_cast<uint16>(zone)); it != learned.end())
            {
                std::vector<Point> spots;
                for (const auto& l : it->second)
                {
                    spots.push_back(l.at);
                }
                for (const auto& c : cardian::porter::avoidAreas(spots))
                {
                    snagged.push_back(cardian::route::Hazard{ .at = positionOf(c.at), .radius = c.radius });
                }
            }
            std::vector<position_t> way(1, from);
            const auto              route = pawn::travel::routeAvoiding(zone, from, goal, snagged, {}, 0.0f);
            if (route.has_value() && !route->points.empty() && (route->complete || distance(route->points.back(), goal) <= slack))
            {
                way.insert(way.end(), route->points.begin(), route->points.end());
                if (!route->complete && line)
                {
                    way.push_back(goal); // the mesh ends short of the line's mouth: straight on from its end
                }
            }
            else if (distance(from, goal) <= slack)
            {
                if (line)
                {
                    way.push_back(goal);
                }
            }
            else
            {
                finish(PChar, charid, End::NoWay, fmt::format("no way on the mesh to {} {:.0f}y away", line ? "the line" : "its spot", distance(from, goal)));
                return false;
            }
            cardian::walk::cutShort(way, gate ? kGateGetDown : inZone ? kSpotCloseTo : 0.0f);

            j.route.clear();
            j.route.reserve(way.size());
            for (const auto& p : way)
            {
                j.route.push_back(pointOf(p));
            }
            j.ends      = line && !gate ? Ends::Line : Ends::Stop;
            j.next      = cardian::porter::progress(j.route, 1, pointOf(from));
            j.sentTo    = 0;
            j.planned   = true;
            j.plannedAt = now;
            j.left      = cardian::walk::lengthOf(way);
            j.atLineSince.reset();
            ShowInfoFmt("porter: {}'s route in {} ({}): {:.0f}y in {} waypoints to {}", PChar->getName(), zoneName(PChar), why, cardian::walk::lengthOf(way), way.size() - 1,
                        inZone ? std::string("the place") : gate ? std::string("the city's gate") : fmt::format("the line to zone {}", static_cast<uint16>(j.hop->destinationZone)));
            sendRoute(charid, j);
            return true;
        }

        // A snag his addon told of, at `at`: planned round from there the
        // first time at a spot, set down on along his way the second, and the
        // ride ended after a few set-downs without getting on (porter_math.h)
        void snag(CCharEntity* PChar, const uint32 charid, Journey& j, const Point& at, const timer::time_point now)
        {
            j.next             = cardian::porter::progress(j.route, j.next, at);
            const auto  answer = cardian::porter::snagged(j.snags, at);
            learnSnag(static_cast<uint16>(PChar->getZone()), at);
            const Point toward = j.next < j.route.size() ? j.route[j.next] : (j.route.empty() ? at : j.route.back());
            const auto  where  = fmt::format("({:.1f}, {:.1f}, {:.1f}) in {}, riding for ({:.1f}, {:.1f}, {:.1f})", at.x, at.y, at.z, zoneName(PChar), toward.x, toward.y,
                                             toward.z);
            switch (answer)
            {
                case cardian::porter::SnagAnswer::Replan:
                {
                    ShowInfoFmt("porter: {} snagged at {}: his route planned again from there", PChar->getName(), where);
                    plan(PChar, charid, j, positionOf(at), now, "snagged");
                    return;
                }
                case cardian::porter::SnagAnswer::SetDown:
                {
                    const auto on = cardian::porter::along(j.route, j.next, at, cardian::porter::setDownAhead(j.snags.setDowns));
                    if (on.pastEnd && j.ends == Ends::Line)
                    {
                        take(PChar, charid, j, fmt::format("snagged again at {}, the line {:.1f}y on", where, cardian::porter::flat(at, j.route.back())));
                        return;
                    }
                    position_t to = positionOf(on.at);
                    if (auto* navMesh = PChar->loc.zone != nullptr ? PChar->loc.zone->navMesh() : nullptr; navMesh != nullptr)
                    {
                        if (const auto snapped = navMesh->findClosestValidPoint(to); snapped.has_value())
                        {
                            to = *snapped;
                        }
                    }
                    setDownAt(PChar, to);
                    cardian::porter::setDown(j.snags, pointOf(to));
                    j.settleUntil = now + kSettle;
                    ShowInfoFmt("porter: {} snagged at {} again: set down at ({:.1f}, {:.1f}, {:.1f}), {:.1f}y on (set-down {} of {})", PChar->getName(), where, to.x, to.y,
                                to.z, cardian::porter::flat(at, pointOf(to)), j.snags.setDowns, cardian::porter::kSetDownsMax);
                    plan(PChar, charid, j, to, now, "set down past a snag");
                    return;
                }
                case cardian::porter::SnagAnswer::GiveUp:
                {
                    ShowInfoFmt("porter: {} snagged at {}: {} snags and {} set-downs without getting on", PChar->getName(), where, j.snags.total, j.snags.setDowns);
                    finish(PChar, charid, End::Snagged, "snagged, and no set-down got him on");
                    return;
                }
            }
        }

        // His client is in after a zone change: a position from it since his
        // zone-in, and a moment past it
        auto landed(const uint32 charid, const realtime::time_point landedAt) -> bool
        {
            const auto since = realtime::now() - landedAt;
            const auto age   = pawn::positionPacketAge(charid);
            return since >= kLandedFor && age.has_value() && *age < since;
        }

        // One tick of his ride, on his zone's tick
        void ride(CCharEntity* PChar, const uint32 charid)
        {
            auto& j = journeys.at(charid);
            if (PChar->status == xi::Status::Disappear || PChar->requestedZoneChange)
            {
                return; // on his way out of the zone
            }
            if (j.landing)
            {
                if (!landed(charid, j.landedAt))
                {
                    return;
                }
                j.landing = false;
            }

            const auto zone  = static_cast<uint16>(PChar->getZone());
            const auto spot  = spotOf(j.place);
            const bool there = cardian::porter::arrived(j.place, zone, distance(PChar->loc.p, spot, true));
            cardian::porter::State state;
            state.arrived    = there;
            state.mounted    = PChar->isMounted();
            state.atCityGate = j.gotDown;
            state.mobOn      = pawn::mobOn(PChar, kMobOnReach);
            state.dead       = PChar->isDead();
            if (const auto why = cardian::porter::ends(state))
            {
                static constexpr const char* kHow[] = { "", "there", "he took the reins", "off his chocobo", "a monster is on him", "no way on", "snagged" };
                finish(PChar, charid, *why, kHow[static_cast<uint8>(*why)]);
                return;
            }

            // In an event (a cutscene as he lands, a trigger area) his client
            // is the event's, and the ride waits
            if (PChar->isInEvent())
            {
                return;
            }
            const auto now = timer::now();

            // A zone of the ride's: the trek's line out of it chosen as he
            // enters it, and kept
            if (j.zone != zone)
            {
                forgetZone(j);
                j.zone = zone;
                if (zone != j.place.zone)
                {
                    const std::optional<position_t> toPoint = j.place.kind == Kind::City ? std::nullopt : std::optional<position_t>(spot);
                    j.hop = pawn::travel::nextHop(PChar->getZone(), PChar->loc.p, static_cast<xi::ZoneId>(j.place.zone), toPoint, std::nullopt,
                                                  TravelRide{ .mayRent = false, .mounted = PChar->isMounted(), .rideFactor = 1.0f });
                    if (!j.hop.has_value())
                    {
                        finish(PChar, charid, End::NoWay, "the trek's map knows no way from here");
                        return;
                    }
                    const bool intoCity = j.place.kind == Kind::City && static_cast<uint16>(j.hop->destinationZone) == j.place.zone;
                    if (cardian::porter::atLine(pawn::travel::allowsMount(j.hop->destinationZone), intoCity) == cardian::porter::AtLine::NoWayOn)
                    {
                        finish(PChar, charid, End::NoWay, fmt::format("the way goes on foot through zone {}", static_cast<uint16>(j.hop->destinationZone)));
                        return;
                    }
                }
            }
            if (j.gotDown && now - j.gotDownAt < kGetDownBeat)
            {
                return; // the dismount plays
            }
            if (!j.planned)
            {
                plan(PChar, charid, j, PChar->loc.p, now, j.gotDown ? "on foot through the gate" : "set out");
                return;
            }
            if (j.stuck.has_value())
            {
                const Point at = *j.stuck;
                j.stuck.reset();
                snag(PChar, charid, j, at, now);
                return;
            }
            if (j.resend)
            {
                sendRoute(charid, j);
            }
            if (now < j.settleUntil)
            {
                return;
            }

            const Point here = pointOf(PChar->loc.p);
            cardian::porter::rode(j.snags, here);
            j.next           = cardian::porter::progress(j.route, j.next, here);
            const bool atEnd = cardian::porter::atEnd(j.route, j.next, here);

            if (j.ends == Ends::Line)
            {
                // His client runs on through the line and crosses it itself;
                // one that has not in a while, the server crosses for him.
                // Run on past it, he has not strayed
                if (atEnd || cardian::porter::beyondEnd(j.route, here, kLineSlack) || distance(PChar->loc.p, j.hop->walkTo, true) <= kLineReach)
                {
                    if (!j.atLineSince.has_value())
                    {
                        j.atLineSince = now;
                    }
                    else if (now - *j.atLineSince >= kRunOnGrace)
                    {
                        take(PChar, charid, j, "ran on through the line and his client never crossed it");
                    }
                    return;
                }
                j.atLineSince.reset();
            }
            else if (atEnd)
            {
                if (zone == j.place.zone)
                {
                    const float off = distance(PChar->loc.p, spot, true);
                    if (off <= kMeshEndYalms)
                    {
                        finish(PChar, charid, End::Arrived, "there, as near as the ground goes");
                    }
                    else
                    {
                        finish(PChar, charid, End::NoWay, fmt::format("the ground ends {:.0f}y short of its spot", off));
                    }
                }
                else
                {
                    getDown(PChar, j, now);
                }
                return;
            }

            if (cardian::porter::strayed(j.route, j.next, here) && now - j.plannedAt >= kReplanEvery)
            {
                if (!plan(PChar, charid, j, PChar->loc.p, now, fmt::format("strayed {:.1f}y off it", cardian::porter::offRoute(j.route, j.next, here))))
                {
                    return;
                }
                j.stalls = cardian::porter::stalls(j.stalls, j.best, j.left);
                j.best   = std::min(j.best, j.left);
                if (j.stalls >= cardian::porter::kStallsToSnag)
                {
                    j.stalls = 0;
                    ShowInfoFmt("porter: {} has strayed {} times with his way no shorter: a snag", PChar->getName(), cardian::porter::kStallsToSnag);
                    snag(PChar, charid, j, here, now);
                }
                return;
            }
            if (cardian::porter::refill(j.route.size(), j.next, j.sentTo))
            {
                sendRoute(charid, j);
            }
        }

        // The question after his rental, once he has landed and his client is in
        void ask(CCharEntity* PChar, const uint32 charid)
        {
            auto& q = questions.at(charid);
            if (!q.landed || !landed(charid, q.landedAt) || PChar->isInEvent())
            {
                return;
            }
            const auto msg = placesMessage(PChar);
            if (send(charid, msg))
            {
                ShowInfoFmt("porter: {} is asked where to, {} places", PChar->getName(), msg.count);
                questions.erase(charid);
            }
        }

        // The Link's porter messages ------------------------------------------

        void askPlaces(CCharEntity* PChar, const cl_porter_places& /*ask*/, Reply& reply)
        {
            if (!PChar->isMounted())
            {
                reply.finish(make<cl_porter_places>(), CL_S_NOT_RIDING);
                return;
            }
            reply.finish(placesMessage(PChar), CL_S_OK);
        }

        void go(CCharEntity* PChar, const cl_porter_go& ask, Reply& reply)
        {
            const auto& all = places();
            if (ask.place >= all.size())
            {
                reply.finish(ask, CL_S_NO_SUCH_PLACE);
                return;
            }
            if (!PChar->isMounted())
            {
                reply.finish(ask, CL_S_NOT_RIDING);
                return;
            }
            if (PChar->isInEvent())
            {
                reply.finish(ask, CL_S_IN_EVENT);
                return;
            }
            if (pawn::mobOn(PChar, kMobOnReach))
            {
                reply.finish(ask, CL_S_UNDER_ATTACK);
                return;
            }
            const Place& place   = all[ask.place];
            const auto   offers  = offersFor(PChar);
            const bool   offered = std::any_of(offers.begin(), offers.end(), [&](const auto& o) { return o.place.number == place.number; });
            const auto   cost    = offered ? pawn::travel::rideCosts(PChar->getZone(), PChar->loc.p, { goalOf(place) }).front() : std::nullopt;
            if (!cost.has_value())
            {
                reply.finish(ask, CL_S_NO_WAY);
                return;
            }
            const auto seconds = cardian::porter::rideSeconds(*cost, ridePace());
            const auto left    = rentalLeft(PChar);
            if (!cardian::porter::rentalCovers(seconds, left))
            {
                reply.finish(ask, CL_S_RENTAL_SHORT);
                return;
            }
            auto& j = journeys[PChar->id];
            j       = Journey{};
            j.place = place;
            if (++lastRide == 0)
            {
                ++lastRide;
            }
            j.ride = lastRide;
            questions.erase(PChar->id);
            ShowInfoFmt("porter: {} rides to the {} in zone {} (place {}, ride {}), some {} away, his chocobo's time left {}", PChar->getName(), kindName(place.kind), place.zone,
                        place.number, j.ride, clockOf(seconds), left.has_value() ? clockOf(*left) : std::string("no end"));
            reply.finish(ask, CL_S_OK);
            send(PChar->id, stateMessage(&place, j.ride, true, 0));
        }

        void stop(CCharEntity* PChar, const cl_porter_stop& ask, Reply& reply)
        {
            if (!journeys.contains(PChar->id))
            {
                reply.finish(ask, CL_S_NO_JOURNEY);
                return;
            }
            reply.finish(ask, CL_S_OK);
            finish(PChar, PChar->id, End::TakenBack, "he took the reins");
        }

        // A fresh bind asks the ride as it stands; its route goes again after
        void state(CCharEntity* PChar, const cl_porter_state& /*ask*/, Reply& reply)
        {
            const auto it = journeys.find(PChar->id);
            if (it == journeys.end())
            {
                reply.finish(stateMessage(nullptr, 0, false, 0), CL_S_OK);
                return;
            }
            it->second.resend = it->second.planned;
            reply.finish(stateMessage(&it->second.place, it->second.ride, true, 0), CL_S_OK);
        }

        // His run snagged: answered on his next tick, if it is of the route
        // sent last
        void stuck(CCharEntity* PChar, const cl_porter_stuck& msg, Reply& /*reply*/)
        {
            const auto it = journeys.find(PChar->id);
            if (it == journeys.end() || !it->second.planned || msg.ride != it->second.ride || msg.seq != it->second.seq || !std::isfinite(msg.x) ||
                !std::isfinite(msg.y) || !std::isfinite(msg.z))
            {
                return;
            }
            const Point at{ msg.x, msg.y, msg.z };
            if (cardian::porter::flat(at, pointOf(PChar->loc.p)) > kStuckFar)
            {
                return;
            }
            it->second.stuck = at;
        }
    } // namespace

    void askAfterRental(CCharEntity* PChar)
    {
        if (PChar == nullptr || pawn::isPawn(PChar) || PChar->PSession == nullptr)
        {
            return;
        }
        questions[PChar->id] = Question{ .askedAt = realtime::now() };
        ShowInfoFmt("porter: {} rents a chocobo: asked where to once he lands", PChar->getName());
    }
} // namespace pawn::porter

class PorterModule : public CPPModule
{
    void OnInit() override
    {
        using namespace pawn::porter;
        cardian::link::handle<cl_porter_places>(askPlaces);
        cardian::link::handle<cl_porter_go>(go);
        cardian::link::handle<cl_porter_stop>(stop);
        cardian::link::handle<cl_porter_state>(state);
        cardian::link::handle<cl_porter_stuck>(stuck);
        loadAllSnags();

        // modules/cardian/lua/chocobo_ride.lua: his rental at a city's stable
        // has set him down in its field, mounted
        ::lua["CBaseEntity"]["cardianPorterAsk"] = [](CLuaBaseEntity* PLuaBaseEntity)
        {
            pawn::porter::askAfterRental(dynamic_cast<CCharEntity*>(PLuaBaseEntity->GetBaseEntity()));
        };
    }

    // Landed in a zone: his ride, or the question after his rental, waits
    // for his client to be in
    void OnCharZoneIn(CCharEntity* PChar) override
    {
        using namespace pawn::porter;
        if (PChar == nullptr)
        {
            return;
        }
        if (const auto it = journeys.find(PChar->id); it != journeys.end())
        {
            it->second.landing  = true;
            it->second.landedAt = realtime::now();
            forgetZone(it->second);
        }
        if (const auto it = questions.find(PChar->id); it != questions.end())
        {
            it->second.landed   = true;
            it->second.landedAt = realtime::now();
        }
    }

    // The rides and the questions of the players in this zone, on its tick,
    // outside every entity's own tick. A held pause holds them
    void OnZoneTick(CZone* PZone) override
    {
        using namespace pawn::porter;
        if (cardian::pause::isHeld() || (journeys.empty() && questions.empty()))
        {
            return;
        }
        std::vector<uint32> here;
        for (const auto& [charid, _] : journeys)
        {
            if (const auto* PChar = zoneutils::GetChar(charid); PChar != nullptr && PChar->loc.zone == PZone)
            {
                here.push_back(charid);
            }
        }
        for (const auto charid : here)
        {
            if (journeys.contains(charid))
            {
                ride(zoneutils::GetChar(charid), charid);
            }
        }
        here.clear();
        for (const auto& [charid, _] : questions)
        {
            if (const auto* PChar = zoneutils::GetChar(charid); PChar != nullptr && PChar->loc.zone == PZone)
            {
                here.push_back(charid);
            }
        }
        for (const auto charid : here)
        {
            if (questions.contains(charid))
            {
                ask(zoneutils::GetChar(charid), charid);
            }
        }
    }

    // A ride whose player has signed out ends with him, and one whose zone
    // change never landed ends where he is; a question nobody heard in time
    // is let go
    void OnTimeServerTick() override
    {
        using namespace pawn::porter;
        // The places, read as soon as their library is loaded, so the map log
        // says what the porter offers from the start
        static bool read = false;
        if (!read && libraryReady())
        {
            read = true;
            places();
        }
        std::vector<uint32> lost;
        for (auto it = journeys.begin(); it != journeys.end();)
        {
            if (!pawn::players::online(it->first))
            {
                ShowInfoFmt("porter: the ride of {} ends: he has signed out", it->first);
                it = journeys.erase(it);
                continue;
            }
            if (it->second.landing && realtime::now() - it->second.landedAt > kLandingLost)
            {
                lost.push_back(it->first);
            }
            ++it;
        }
        for (const auto charid : lost)
        {
            finish(zoneutils::GetChar(charid), charid, cardian::porter::End::NoWay, "his zone change never landed");
        }
        for (auto it = questions.begin(); it != questions.end();)
        {
            if (!pawn::players::online(it->first) || realtime::now() - it->second.askedAt > kAskFor)
            {
                it = questions.erase(it);
                continue;
            }
            ++it;
        }
    }
};

REGISTER_CPP_MODULE(PorterModule);
