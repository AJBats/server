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

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <optional>
#include <tuple>
#include <unordered_map>
#include <vector>

// The chocobo porter's rules (ROADMAP T, "The chocobo porter"; porter.h):
// which places he is offered from where he stands and how long each ride
// takes, when a ride is over, what the next zone line asks of a rider, and
// how the route his addon steers him along is sent, followed and answered
// when his client snags on it. Pure, so the porter's tests need no world.
namespace cardian::porter
{
    // What a place is (the Link's CL_PORTER_*)
    enum class Kind : uint8_t
    {
        City    = 1, // ridden to its gate, and on foot through it
        Outpost = 2, // a region's conquest outpost
        Crag    = 3, // a teleport crag
    };

    // A place the porter rides to: a city by its zone alone, any other by
    // its spot in its zone
    struct Place
    {
        uint16_t number = 0; // its place in the porter's list, which the Link names
        Kind     kind   = Kind::City;
        uint16_t zone   = 0;
        uint16_t key    = 0; // an outpost's region, a crag's teleport
        float    x = 0.0f, y = 0.0f, z = 0.0f;
    };

    // How far within a place's spot a ride is over
    constexpr float kArriveYalms = 4.0f;

    // The places offered from his zone, nearest first, each with the zone
    // lines between: a city a line from a zone that allows a mount leads
    // into, any other place in a zone that allows one -- the porter rides
    // him there, and walks him nowhere but through a city's gate -- and
    // every one in a zone the zone lines reach from his (`hops`, by zone).
    // His own zone's city, were he in one, is no ride
    struct Offer
    {
        Place   place;
        uint8_t hops = 0;
    };
    inline auto offered(const std::vector<Place>& places, const uint16_t here, const std::unordered_map<uint16_t, uint32_t>& hops,
                        const std::function<bool(uint16_t)>& allowsMount) -> std::vector<Offer>
    {
        std::vector<Offer> out;
        for (const auto& place : places)
        {
            const auto it = hops.find(place.zone);
            if (it == hops.end() || (place.kind == Kind::City && place.zone == here) || (place.kind != Kind::City && !allowsMount(place.zone)))
            {
                continue;
            }
            out.push_back({ place, static_cast<uint8_t>(std::min<uint32_t>(it->second, UINT8_MAX)) });
        }
        std::stable_sort(out.begin(), out.end(), [](const Offer& a, const Offer& b)
        {
            return std::tie(a.hops, a.place.kind, a.place.zone, a.place.key) < std::tie(b.hops, b.place.kind, b.place.zone, b.place.key);
        });
        return out;
    }

    // Whether the ride is there: in a city's zone, or within kArriveYalms of
    // any other place's spot in its zone
    inline auto arrived(const Place& place, const uint16_t zone, const float fromSpot) -> bool
    {
        if (zone != place.zone)
        {
            return false;
        }
        return place.kind == Kind::City || fromSpot <= kArriveYalms;
    }

    // What the next zone line asks of a rider. A zone that allows a mount is
    // ridden into. One that allows none (a city, a dungeon) takes no rented
    // chocobo (the game refuses it at the line): he gets down short of the
    // line and goes through on foot when it is the city he is bound for; any
    // other is no way on, since the porter walks him through no zone on foot
    enum class AtLine : uint8_t
    {
        RideOn,
        GetDown,
        NoWayOn,
    };
    inline auto atLine(const bool nextAllowsMount, const bool nextIsTheCity) -> AtLine
    {
        if (nextAllowsMount)
        {
            return AtLine::RideOn;
        }
        return nextIsTheCity ? AtLine::GetDown : AtLine::NoWayOn;
    }

    // Why a ride ends (the Link's CL_PORTER_*). Off his chocobo -- the
    // rental ran out, or anything else got him down -- ends it, but for the
    // gate of the city he is bound for, where the porter itself gets him
    // down
    enum class End : uint8_t
    {
        Arrived   = 1,
        TakenBack = 2,
        OnFoot    = 3,
        Attacked  = 4,
        NoWay     = 5,
        Snagged   = 6, // his client caught on the ground again and again, set down past it and caught again
    };
    struct State
    {
        bool arrived    = false;
        bool mounted    = true;
        bool atCityGate = false; // got down at the gate of the city he is bound for
        bool mobOn      = false;
        bool dead       = false;
    };
    // Why the ride ends now, the first that holds; none while it goes on
    inline auto ends(const State& s) -> std::optional<End>
    {
        if (s.arrived)
        {
            return End::Arrived;
        }
        if (s.dead || (!s.mounted && !s.atCityGate))
        {
            return End::OnFoot;
        }
        if (s.mobOn)
        {
            return End::Attacked;
        }
        return std::nullopt;
    }

    // ---- Ride to's times ---------------------------------------------------
    //
    // Each place's ride from where he is, in seconds: the trek's planner's
    // yalms ridden (travel::rideCosts, a zone line's crossing counted in
    // them) at his chocobo's pace. A place is in reach when the time left on
    // his rental -- what its MOUNTED effect has left -- covers the ride with a
    // tenth to spare, so a ride never ends a few yalms short of it; any other
    // is greyed on Ride to and refused

    constexpr float    kRentalMargin = 0.10f;
    constexpr uint16_t kNoTime       = UINT16_MAX; // a ride too long to count in seconds

    // The seconds a ride of `yalms` takes at `pace` yalms a second, rounded up
    inline auto rideSeconds(const float yalms, const float pace) -> uint16_t
    {
        if (pace <= 0.0f)
        {
            return kNoTime;
        }
        const float seconds = std::ceil(std::max(0.0f, yalms) / pace);
        return seconds >= static_cast<float>(kNoTime) ? kNoTime : static_cast<uint16_t>(seconds);
    }

    // Whether the time left on his rental covers a ride, with kRentalMargin
    // to spare; a rental with no end (`left` none) covers any
    inline auto rentalCovers(const uint16_t seconds, const std::optional<float> left) -> bool
    {
        if (!left.has_value())
        {
            return true;
        }
        return seconds != kNoTime && static_cast<float>(seconds) * (1.0f + kRentalMargin) <= *left;
    }

    // ---- The route his addon steers him along --------------------------------
    //
    // The server plans his way across a zone, to its next line or to the
    // place, and sends it a stretch at a time (PORTER_ROUTE); his addon runs
    // his client along it with the client's own auto-run. A route's points
    // are in the server's axes, y the height; every rule here judges them
    // on the ground, the height aside, since where his client stands and
    // where the mesh lies differ in height by a yalm or so

    struct Point
    {
        float x = 0.0f;
        float y = 0.0f; // the height
        float z = 0.0f;
    };

    // How far apart two points stand on the ground
    inline auto flat(const Point& a, const Point& b) -> float
    {
        const float dx = a.x - b.x;
        const float dz = a.z - b.z;
        return std::sqrt(dx * dx + dz * dz);
    }

    // How far `here` stands off the leg a-b on the ground, and how far along
    // it his nearest point lies (0 at a, 1 at b)
    inline auto offLeg(const Point& a, const Point& b, const Point& here, float* along = nullptr) -> float
    {
        const float dx  = b.x - a.x;
        const float dz  = b.z - a.z;
        const float len = dx * dx + dz * dz;
        const float t   = len > 0.0f ? std::clamp(((here.x - a.x) * dx + (here.z - a.z) * dz) / len, 0.0f, 1.0f) : 0.0f;
        if (along != nullptr)
        {
            *along = t;
        }
        const float px = a.x + dx * t - here.x;
        const float pz = a.z + dz * t - here.z;
        return std::sqrt(px * px + pz * pz);
    }

    // What a route's last point is (the Link's CL_PORTER_ENDS_*)
    enum class Ends : uint8_t
    {
        More = 0, // a stretch of a longer route: the rest follows as he nears its end
        Line = 1, // a zone line: run on through it, straight on past its last point
        Stop = 2, // stop at its last point: the place, or short of a city's gate, where he is got down
    };

    constexpr std::size_t kWindow       = 32;   // the waypoints a PORTER_ROUTE carries
    constexpr std::size_t kRefillWithin = 8;    // ... and the next stretch is sent once he rides to one this near its last
    constexpr float       kReachYalms   = 2.0f; // a waypoint this near on the ground is reached
    constexpr float       kSameLevel    = 2.0f; // yalms: a leg or a waypoint further above or below him is on another level
    constexpr float       kGroundStep   = 2.0f; // yalms between the ground's heights kept along a leg (Ground)
    constexpr float       kStrayYalms   = 5.0f; // this far off the leg he rides, his route is planned again from where he is
    constexpr float       kEndReach     = 4.5f; // run past a route's last point this near it, he is at its end: his addon stops so within 4 (porter.lua kPassReach)
    constexpr float       kStallGain    = 3.0f; // a replan after straying that leaves his way no shorter than this has gained nothing
    constexpr int         kStallsToSnag = 3;    // ... and this many such in a row are a snag, though he never stood still

    // Strayed and planned again, and again, with his way no shorter: caught on
    // something he slides along or turns back and forth at, moving all the
    // while, so his addon's stand-still test never fires. The stalls so far,
    // given the best way left at any replan in the zone and the way left now;
    // a snag once they reach kStallsToSnag
    inline auto stalls(const int before, const float best, const float left) -> int
    {
        return left > best - kStallGain ? before + 1 : 0;
    }

    // The stretch of a route sent: from `from` -- the start of the leg he
    // rides -- at most `window` points; its last point says what it is --
    // the route's own end, or a stretch with more to follow
    struct Segment
    {
        std::size_t from = 0;
        std::size_t to   = 0; // one past its last
        Ends        ends = Ends::More;
    };
    inline auto segment(const std::size_t routeSize, const std::size_t from, const Ends routeEnds, const std::size_t window = kWindow) -> Segment
    {
        Segment s;
        s.from = std::min(from, routeSize);
        s.to   = std::min(routeSize, s.from + window);
        s.ends = s.to == routeSize ? routeEnds : Ends::More;
        return s;
    }

    // The next stretch is due: more of the route remains past the last point
    // sent, and he rides to one within kRefillWithin of it
    inline auto refill(const std::size_t routeSize, const std::size_t next, const std::size_t sentTo) -> bool
    {
        return sentTo < routeSize && next + kRefillWithin >= sentTo;
    }

    // The ground's height under a route's legs, read off the mesh as the
    // route is planned: [i] holds leg i's, from point i-1 to point i, evenly
    // spaced from its start to its end. A leg with none keeps the straight
    // line between its two points, which a long leg over a rise or a hollow
    // leaves yalms off the ground
    using Ground = std::vector<std::vector<float>>;

    // The height of leg i `along` its length (0 at its start, 1 at its end)
    inline auto groundAt(const std::vector<Point>& route, const Ground& ground, const std::size_t i, const float along) -> float
    {
        if (i < ground.size() && ground[i].size() >= 2)
        {
            const auto&       h = ground[i];
            const float       f = std::clamp(along, 0.0f, 1.0f) * static_cast<float>(h.size() - 1);
            const std::size_t k = std::min(static_cast<std::size_t>(f), h.size() - 2);
            return h[k] + (h[k + 1] - h[k]) * (f - static_cast<float>(k));
        }
        return route[i - 1].y + (route[i].y - route[i - 1].y) * along;
    }

    // The waypoint he rides to next on a route whose first point is where he
    // set out: of the next few legs from `next`, the one he stands nearest,
    // and on past every waypoint he stands within kReachYalms of
    inline auto progress(const std::vector<Point>& route, std::size_t next, const Point& here, const Ground& ground = {}) -> std::size_t
    {
        if (next == 0)
        {
            next = 1;
        }
        if (next >= route.size())
        {
            return route.size();
        }
        // The nearest of the next few legs on the ground -- of those on his
        // level: a route that climbs round a cliff and comes back along its
        // top passes near him in plan, a cliff's height away -- and within
        // kStrayYalms of him: a leg further off is never his, however its
        // level, so where none is near he keeps the one he rides
        std::size_t best   = next;
        float       bestAt = std::numeric_limits<float>::max();
        for (std::size_t i = next; i < std::min(route.size(), next + 4); ++i)
        {
            float       along = 0.0f;
            const float d     = offLeg(route[i - 1], route[i], here, &along);
            const float y     = groundAt(route, ground, i, along);
            if (d <= kStrayYalms && std::abs(y - here.y) <= kSameLevel && d < bestAt - 0.01f)
            {
                bestAt = d;
                best   = i;
            }
        }
        while (best < route.size() && flat(here, route[best]) <= kReachYalms && std::abs(route[best].y - here.y) <= kSameLevel)
        {
            ++best;
        }
        return best;
    }

    // How far off the leg he rides he stands, on the ground; nothing past the
    // route's end
    inline auto offRoute(const std::vector<Point>& route, const std::size_t next, const Point& here) -> float
    {
        if (next == 0 || next >= route.size())
        {
            return 0.0f;
        }
        return offLeg(route[next - 1], route[next], here);
    }

    // Whether he stands beyond the end of his route -- past the line across
    // the end of its last leg -- within `within` of its last point
    inline auto beyondEnd(const std::vector<Point>& route, const Point& here, const float within) -> bool
    {
        if (route.empty() || flat(here, route.back()) > within)
        {
            return false;
        }
        if (route.size() < 2)
        {
            return true;
        }
        float t = 0.0f;
        offLeg(route[route.size() - 2], route.back(), here, &t);
        return t >= 1.0f;
    }

    // He is at his route's end, where his addon stops the run or runs on
    // through a zone line: past its last waypoint (within kReachYalms of
    // it), or run past it within kEndReach
    inline auto atEnd(const std::vector<Point>& route, const std::size_t next, const Point& here) -> bool
    {
        return route.empty() || next >= route.size() || beyondEnd(route, here, kEndReach);
    }

    // He has strayed from his route: it is planned again from where he is
    inline auto strayed(const std::vector<Point>& route, const std::size_t next, const Point& here) -> bool
    {
        return offRoute(route, next, here) > kStrayYalms;
    }

    // The point `yalms` on along the route from where he stands on the leg he
    // rides, and whether the route ran out first (its last point then)
    struct Along
    {
        Point at{};
        bool  pastEnd = false;
    };
    inline auto along(const std::vector<Point>& route, const std::size_t next, const Point& here, const float yalms) -> Along
    {
        if (route.empty())
        {
            return { here, true };
        }
        if (next == 0 || next >= route.size())
        {
            return { route.back(), true };
        }
        float       t = 0.0f;
        const auto& a = route[next - 1];
        const auto& b = route[next];
        offLeg(a, b, here, &t);
        Point from{ a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t };
        float left = yalms;
        for (std::size_t i = next; i < route.size(); ++i)
        {
            const float leg = flat(from, route[i]);
            if (leg >= left && leg > 0.0f)
            {
                const float f = left / leg;
                return { Point{ from.x + (route[i].x - from.x) * f, from.y + (route[i].y - from.y) * f, from.z + (route[i].z - from.z) * f }, false };
            }
            left -= leg;
            from = route[i];
        }
        return { route.back(), true };
    }

    // ---- Snags ----------------------------------------------------------------
    //
    // His client can catch on ground the navmesh would have let a body the
    // server walks clip past: his addon tells of a run that makes no headway
    // (PORTER_STUCK). The first snag at a spot is planned round again from
    // where he stands; a second at the same spot sets him down a few yalms on
    // along his route, the clip-past the mesh would have given; and after a
    // few set-downs, or many snags, without getting on, the ride ends

    constexpr float   kSnagSameSpot = 4.0f;  // a snag this near the last one is at the same spot
    constexpr float   kSetDownAhead = 5.0f;  // a set-down puts him this far on along his route
    constexpr float   kSnagProgress = 20.0f; // this far from where the last snag or set-down left him, he has got on: they are forgotten
    constexpr uint8_t kSetDownsMax  = 3;     // set-downs without getting on before the ride ends
    constexpr uint8_t kSnagsMax     = 8;     // snags without getting on before the ride ends, wherever they are
    constexpr float   kSnagAvoid    = 4.0f;  // a snag's spot is kept this far clear of by every later route in its zone
    constexpr float   kSnagLink     = 30.0f; // learned spots this near each other are one stretch of bad ground
    constexpr float   kAreaRadius   = 8.0f;  // ... kept this far clear of along its whole length
    constexpr float   kAreaStep     = 6.0f;  // ... by a chain of circles this far apart

    // The ground every route in a zone keeps clear of, from the spots learned
    // there: a lone spot a small circle; spots near each other one stretch,
    // a chain of wider circles along the links joining them (the shortest
    // links that join them all), so a route goes round the whole of a
    // tangle of little cliffs rather than threading the gaps between its spots
    struct Circle
    {
        Point at{};
        float radius = 0.0f;
    };
    inline auto avoidAreas(const std::vector<Point>& spots) -> std::vector<Circle>
    {
        std::vector<Circle> out;
        const std::size_t   n = spots.size();
        std::vector<bool>   linked(n, false);
        // The shortest links joining the spots, Prim's way, a link no longer than kSnagLink
        std::vector<bool>  inTree(n, false);
        std::vector<float> best(n, std::numeric_limits<float>::max());
        std::vector<int>   from(n, -1);
        for (std::size_t start = 0; start < n; ++start)
        {
            if (inTree[start])
            {
                continue;
            }
            best[start] = 0.0f;
            from[start] = -1; // a stretch of its own: no link back to one too far
            while (true)
            {
                int pick = -1;
                for (std::size_t i = 0; i < n; ++i)
                {
                    if (!inTree[i] && best[i] <= kSnagLink && (pick < 0 || best[i] < best[static_cast<std::size_t>(pick)]))
                    {
                        pick = static_cast<int>(i);
                    }
                }
                if (pick < 0)
                {
                    break;
                }
                const auto p = static_cast<std::size_t>(pick);
                inTree[p]    = true;
                if (from[p] >= 0)
                {
                    const auto& a = spots[static_cast<std::size_t>(from[p])];
                    const auto& b = spots[p];
                    linked[p] = linked[static_cast<std::size_t>(from[p])] = true;
                    const float len   = flat(a, b);
                    const int   steps = std::max(1, static_cast<int>(std::ceil(len / kAreaStep)));
                    for (int s = 0; s <= steps; ++s)
                    {
                        const float f = static_cast<float>(s) / static_cast<float>(steps);
                        out.push_back(Circle{ Point{ a.x + (b.x - a.x) * f, a.y + (b.y - a.y) * f, a.z + (b.z - a.z) * f }, kAreaRadius });
                    }
                }
                for (std::size_t i = 0; i < n; ++i)
                {
                    const float d = flat(spots[p], spots[i]);
                    if (!inTree[i] && d < best[i])
                    {
                        best[i] = d;
                        from[i] = pick;
                    }
                }
            }
        }
        for (std::size_t i = 0; i < n; ++i)
        {
            if (!linked[i])
            {
                out.push_back(Circle{ spots[i], kSnagAvoid });
            }
        }
        return out;
    }

    // How far on a set-down puts him: further each time on one ride without
    // getting on, so a set-down that left him short of what holds him is not
    // the next one's distance too (5, 15, 25 yalms)
    inline auto setDownAhead(const uint8_t setDowns) -> float
    {
        return kSetDownAhead + 10.0f * static_cast<float>(setDowns > 0 ? setDowns - 1 : 0);
    }

    enum class SnagAnswer : uint8_t
    {
        Replan,  // planned again from where he stands
        SetDown, // set down kSetDownAhead on along his route
        GiveUp,  // the ride ends
    };

    // A ride's snags since he last got on
    struct Snags
    {
        bool    any      = false;
        Point   spot{};       // the last snag's spot
        uint8_t atSpot   = 0; // snags at that spot in a row
        uint8_t total    = 0;
        uint8_t setDowns = 0;
        Point   last{}; // where the last snag or set-down left him
    };

    // A snag where he stands: what is done about it
    inline auto snagged(Snags& s, const Point& here) -> SnagAnswer
    {
        if (s.any && flat(s.spot, here) <= kSnagSameSpot)
        {
            ++s.atSpot;
        }
        else
        {
            s.spot   = here;
            s.atSpot = 1;
        }
        s.any  = true;
        s.last = here;
        ++s.total;
        if (s.total > kSnagsMax)
        {
            return SnagAnswer::GiveUp;
        }
        if (s.atSpot < 2)
        {
            return SnagAnswer::Replan;
        }
        if (s.setDowns >= kSetDownsMax)
        {
            return SnagAnswer::GiveUp;
        }
        ++s.setDowns;
        s.atSpot = 0; // a snag where the set-down leaves him is a first one
        return SnagAnswer::SetDown;
    }

    // Where a set-down put him
    inline void setDown(Snags& s, const Point& at)
    {
        s.last = at;
    }

    // Where he stands now: far enough from where the last snag or set-down
    // left him, he has got on, and they are forgotten
    inline void rode(Snags& s, const Point& here)
    {
        if (s.any && flat(s.last, here) >= kSnagProgress)
        {
            s = {};
        }
    }
} // namespace cardian::porter
