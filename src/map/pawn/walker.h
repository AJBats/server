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

#include "pawn_danger.h"
#include "route_search.h"
#include "safe_walk.h"

#include "common/cbasetypes.h"
#include "common/timer.h"
#include "common/types/position.h"
#include "data/enums/zone.h"

#include <chrono>
#include <optional>
#include <unordered_map>
#include <vector>

class CBattleEntity;

// The walker (ROADMAP W): one body's walk, planned round the danger map's
// balls by the scored search (pawn/route_search.h) and walked as planned --
// held short of a choke, backed off to safe ground, a sneak-by past the mobs
// (pawn/sneak.h) -- and handed to the body's path finder, which steps it.
// Every body the server walks walks through one: each cardian's (her
// controller's Walk and its movers, pawn_walker.cpp). What the walker asks
// of the body -- the danger map's balls as her rows see them, what a trek
// makes of a sneak-by -- it asks of its host
namespace pawn
{
    // What the danger map made of a walk, the word the rest of a cardian's
    // code reads: a spell is not cast while she steps out of a ring, a mover
    // knows its point was moved
    enum class WalkAction : uint8
    {
        None,
        Escape, // she stands in a ring: walking out of it first
        Slot,   // her point is in a ring: walking to the safe ground nearest it
        Hold,   // held short of a choke, or her fight's mob is in another's ring: waiting at the safe ground nearest it
        Detour, // the way there goes round a ring, or sneaks by
    };

    // What a walker asks of the body it walks
    class WalkHost
    {
    public:
        virtual ~WalkHost() = default;

        // The danger map's balls for a walk under these rules, `scan` yalms
        // round the body; asked only of a walk that keeps out of danger
        virtual auto walkBalls(const cardian::walk::Rules& rules, float scan) -> std::vector<danger::Sphere> = 0;

        // A sneak-by stepped this tick: a trek's line waits on it before it
        // calls the walk stalled
        virtual void sneakStepped()
        {
        }

        // A sneak-by ended past the mobs
        virtual void sneakedThrough()
        {
        }
    };

    namespace walker
    {
        // A short walk is planned whole, on the ground round her and the
        // point, every time the point moves; a long one -- a trek across a
        // zone -- is planned whole as she sets out and then a stretch at a time
        constexpr auto  kCheckEvery   = std::chrono::seconds(1);  // the route held against where the mobs stand now
        constexpr auto  kHeldRetry    = std::chrono::seconds(3);  // a choke looked at again
        constexpr auto  kRefreshEvery = std::chrono::seconds(15); // a shorter way that opened since
        constexpr auto  kForgotten    = std::chrono::seconds(5);  // not asked for this long, the route is planned anew
        constexpr float kSameGoal     = 1.0f;    // a trek's line moved less than this is the same walk
        constexpr float kLocalSpare   = 30.0f;   // a walk's ground: half as long again as the way, and this much more
        constexpr float kWiderSpare   = 100.0f;  // ... and where the way round runs wider, three times the way and this much more
        constexpr float kOffRoute     = 2.0f;    // knocked this far off the leg she walks, her route is planned afresh from where she stands
        constexpr float kHopLate      = 2.0f;    // seconds past its planned end a sneak-by is given up: as far as its plan holds (the sneak-by's slack)
        constexpr float kStretchPast  = 60.0f;   // a stretch planned again runs this many yalms of her route past the trouble
        constexpr float kStretchSpare = 60.0f;   // ... on ground half as long again as the stretch, and this many yalms more
        constexpr float kCrossingNear = 100.0f;  // a mob crossing her route this near ahead is planned round now; one farther waits until she is nearer
        constexpr float kSafeGround   = 150.0f;  // with no spot short of a choke out of every ring: the nearest ground that is, within this walk
        constexpr float kSpotSafe     = 40.0f;   // a point inside a ring: the safe ground nearest it, within this walk of it
        constexpr float kNearRings    = 20.0f;   // the rings looked at every tick for one on her
        constexpr float kTrekScan     = 2000.0f; // a long walk's mobs: the zone's every one, a choke known however far ahead

        // The balls as the route search reads them
        auto toHazards(const std::vector<danger::Sphere>& spheres) -> std::vector<cardian::route::Hazard>;
    } // namespace walker

    class Walker
    {
    public:
        Walker(CBattleEntity* owner, WalkHost& host);

        // The walk: its route, kept while it holds and planned again as a
        // trek's is
        struct Plan
        {
            std::vector<position_t>          route;          // her spot as it was planned, then its waypoints
            size_t                           next   = 0;     // the waypoint she walks to next
            int                              danger = -1;    // the first waypoint reached through a ring, -1 for none
            int                              hold   = -1;    // the end of her walk while she holds short of it, -1 for no spot out of every ring
            bool                             known  = false;
            bool                             failed = false; // no route could be planned to `goal`: asked again in a while
            bool                             detour = false; // the route goes round a ring the straight way would cross
            uint32                           door     = 0;   // a closed door she may not open: the route ends a yalm short of it (pawn_doors.h), 0 for none
            int                              doorSide = 0;   // ... and her side of it
            timer::time_point                askedAt;        // the walk last asked for: a walk no mover asks for any longer is over
            position_t                       goal{};         // the point asked for
            position_t                       target{};       // where the route goes: the point, or the safe ground nearest it
            xi::ZoneId                       zone{};
            cardian::walk::Rules             rules{};
            timer::time_point                plannedAt;
            timer::time_point                checkedAt;      // last held against where the mobs stand
            timer::time_point                caughtLogAt;    // a plan for a ring on her, as logged: once a second
            timer::time_point                loggedAt;       // the everyday walk's line, once a second
            bool                             walking = false; // handed to her path finder
            size_t                           end     = 0;     // ... up to here
            std::optional<timer::time_point> heldSince;
            position_t                       heldAt{};        // where she stood holding, last tick she held: a new plan holding her there holds on, its clock kept
            std::vector<position_t>          backOff;        // the way to the nearest ground out of every ring, with no spot short of a choke that is
            bool                             wholeHeld = false; // the last plan of the whole way found it held
            timer::time_point                wholeAt;
            std::vector<position_t>          hop;             // a sneak-by under way: her walk past the mobs, its end last
            xi::ZoneId                       hopZone{};
            timer::time_point                hopAt;
            float                            hopSeconds = 0.0f;
            bool                             hopWalking = false;
            timer::time_point                sneakLookedAt;
            timer::time_point                sneakSaidAt;
            cardian::walk::Outcome           outcome = cardian::walk::Outcome::Walking; // what the walk came to at its last tick, for the mover that asked
            timer::time_point                outcomeAt;
        };

        // One walk's tick: planned, or planned again when the point moves, a
        // mob comes onto her route or she stands in a ring, and handed to her
        // path finder. `target` is where the walk goes now: the point, or with
        // the point inside a ring the safe ground nearest it. Her path finder
        // is set; whoever moves her steps it
        struct Result
        {
            cardian::walk::Outcome outcome = cardian::walk::Outcome::Walking;
            position_t             target{};
            WalkAction             action = WalkAction::None; // what the danger map made of the walk
        };
        auto walk(const position_t& point, const cardian::walk::Rules& rules, timer::time_point tick) -> Result;

        // A sneak-by under way, within its time
        auto sneakingBy(timer::time_point tick) const -> bool;

        // Whether a ball's mob sees a point (no wall between), memoised per tick
        auto sees(const danger::Sphere& sphere, const position_t& point, timer::time_point tick) const -> bool;

        // Forget the tick's sight answers: a mob may have walked out from
        // behind a wall since
        void forgetSight();

        // The rings near her at the last walk, and how far she stood from
        // each mob: the trek's evidence when one turns on her
        struct RingNear
        {
            uint32 mob    = 0;
            float  radius = 0.0f;
            float  off    = 0.0f; // how far the mob stood from her
        };

        Plan                  plan;
        std::vector<RingNear> ringsNear;

    private:
        // The balls of a walk that keeps out of danger, from the host; none
        // for a walk that does not
        auto ballsFor(const cardian::walk::Rules& rules, float scan) -> std::vector<danger::Sphere>;

        // Holding short of a choke, once a second: a sneak-by past the mobs
        // that hold it, toward the walk's target. True when she sets out on one
        auto sneakBy(const cardian::walk::Rules& rules) -> bool;

        // A sneak-by under way: walked past the mobs; false once it is over
        auto hopTick(bool racing) -> bool;

        CBattleEntity*    owner_;
        WalkHost&         host_;
        timer::time_point tick_{};

        mutable std::unordered_map<uint64_t, bool> sight_;
        mutable timer::time_point                  sightAt_{};
    };
} // namespace pawn
