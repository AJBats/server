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

#include "common/cbasetypes.h"
#include "common/types/position.h"
#include "errand_math.h"

#include <optional>
#include <string>

class CBaseEntity;
class CCharEntity;
class CZone;

// The linkshell's errands (RESEARCH §11.13): the player sends a member of his
// club (club.h) off to do something, and she comes back with it done. One
// errand per member, a row of cardian_errands each (made at boot, the rows
// read into memory): who, whose club, the kind and its own parameters (args),
// where it stands, the game clock as she was sent, left the world and is due
// back, and the kind's target and progress -- the shape a kind still to be
// built plugs into (errands.cpp's ensureTable has the life of a row).
//
// The rabbit hole: away, she is gone from the world -- the seat ladder keeps
// her online with no body (seat_ladder.h's away lookup), so search still
// finds her -- and back when it is done or he calls her back, where she left
// the world. The clock is the game clock (earth_time::game_timestamp), which
// a pause holds and which runs on, or not, while the map is off as
// cardian.CLOCK_RUNS_OFFLINE says.
//
// The kinds built (errand_math.h says whom each is for):
//   - Gear up: one of the world's wearing his pearl, standing in a town with
//     an auction house. It plays like a maneuver: she walks to the auction
//     counter, asks the census for her level as at the counter with him
//     (redress.h) and waits for its answer to be put on her, walks to her
//     nation's gate guard and buys the scrolls she lacks (supplies.h), and
//     walks back -- or, in his party with him there, rejoins him.
//   - A quest he has done and she has not, from the errand table
//     (modules/cardian/lua/errand_quests.lua): she walks to the zone line
//     toward it and leaves the world there, crosses the quest's zones over
//     its time, and comes back with it done in her own log -- and the job it
//     unlocks, unlocked.
//   - A rank catch-up: her nation's missions in order, each on the table's
//     route and time (its rank ladder), to the rank he picks -- above hers,
//     at most his own -- set out on the same way; back, the missions are
//     done in her log and her rank and rank bar are what the game holds after
//     them, so a battlefield's or a city's gate reads her as anyone. Called
//     back early she keeps the missions her time away covered.
//   - A money venture at a profession of hers (professions.h): fishing at
//     one of her waters (venture_spots.h), set out on the same way. Away, her
//     saved place is moved to the water, so search finds her there faded,
//     and the map fishes it on the game clock with her body in no zone
//     (angler.h). Due or called back, the fishing stops, her kit comes off
//     and she stands again where she left; what she caught sells on
//     (venture_sales.h).
//   - A level venture: one of her unlocked jobs to a level he picks -- above
//     hers, at most his highest on any of his jobs -- for any member of his
//     club. She walks to the nearest zone line and leaves the world there, off
//     the clock. Away, the live census levels her (census.py LevelVentures)
//     at his best pace on any of his jobs times the census's most generous
//     multiplier, with no breaks, through dings it plans and, with no body of
//     hers held, writes in her rows -- one ding landing every whole level its
//     count holds; its row of cardian_venture keeps that count. She is back
//     once her rows reach the level. Called back, the census counts no more,
//     but what it banked lands first -- a ding being written, the time it had
//     not counted yet when he called, whole levels in its count -- and then
//     the share of her next level is kept as experience on the job.
namespace pawn::errands
{
    void ensureTable();
    void load();
    void registerHandlers();

    // From the zone tick: each errand moved on, at most once a second
    void tick();

    // Gone from the world on an errand: the ladder keeps her online with no body
    auto isAway(uint32 charid) -> bool;

    // An errand as the Linkshell page shows it
    struct View
    {
        cardian::errand::Kind  kind        = cardian::errand::Kind::None;
        cardian::errand::State state       = cardian::errand::State::Going;
        uint32                 secondsLeft  = 0;
        uint32                 secondsTotal = 0; // away on the clock: the whole of it
        uint16                 zone         = 0; // away: the zone of her route she is crossing now
        uint8                  missionsDone = 0; // a rank catch-up away: the missions her time away has covered
        uint8                  missions     = 0; // a rank catch-up: the missions it takes her through
        uint8                  levelJob     = 0; // a level venture: the job she levels
        uint8                  levelFrom    = 0; // ... her level on it as she was sent
        uint8                  levelNow     = 0; // ... her level on it now
        uint8                  levelTarget  = 0; // ... the level she is back at
        uint8                  levelShare   = 0; // ... the share of her next level earned, in hundredths
        std::string            title;            // a quest's or a mission's
    };
    auto viewOf(uint32 charid) -> std::optional<View>;

    // A level venture's ceiling: the player's highest level on any of his
    // jobs. A target is at most this, and above her level on the job
    auto levelCap(const CCharEntity* PPlayer) -> uint8;

    // A level venture's Venture Report (professions.cpp report): away, her
    // levels and her pace as they stand; back, the venture as it ended. The
    // conquest points it earned her, and each skill it raised
    struct LevelReport
    {
        uint8                                    phase      = 0; // CL_VENTURE_WORKING away, CL_VENTURE_DONE back
        uint8                                    job        = 0;
        uint8                                    from       = 0; // her level on the job as she set out
        uint8                                    now        = 0; // ... now, or as she came back
        uint8                                    target     = 0;
        uint8                                    share      = 0; // away: hundredths of her next level earned; back: kept as experience
        uint32                                   minutes    = 0; // away so far, or in all, on the game clock
        uint32                                   expPerHour = 0; // away: her pace in experience an hour at her level now
        uint16                                   nextMinutes = 0; // away: the minutes her next level is still off at that pace
        uint32                                   expKept    = 0; // called back: kept toward her next level
        uint32                                   points     = 0;
        std::vector<cardian::errand::SkillRise> skills;
    };
    // Her level venture's report: away on one, or back from one her player
    // has not read since; none else. Reading it marks it read at its phase
    auto levelReportOf(uint32 charid) -> std::optional<LevelReport>;
    // She has a level venture's report to show
    auto hasLevelReport(uint32 charid) -> bool;
    // She is back from a level venture whose report her player has not read since
    auto levelVentureDone(uint32 charid) -> bool;

    // One of the world's by the census: a row of cardian_census, not
    // recruited, whether the map holds a body of hers or not (away on a
    // venture she has none). The test of who is paid the census's points for
    // a level venture's levels alone, and of who holds the Chocobo License by
    // her own jobs (riding.h licensedFor)
    auto ofTheWorld(uint32 charid) -> bool;

    // Her jobs as a level venture reads them: char_jobs.unlocked's mask and
    // her level on each job, by job id -- her body's when she has one, else
    // her rows'. Nothing when her rows cannot be read
    struct Jobs
    {
        uint32 unlocked = 0;
        uint8  levels[32]{};
    };
    auto jobsOf(uint32 charid) -> std::optional<Jobs>;

    // Why gear up cannot go now for a member of his (CL_S_*), CL_S_OK when it
    // can: standing, up, out of a fight, in a zone with an auction counter
    auto gearRefusal(uint32 charid) -> uint16;

    // Where an errand walks in town (here, and town_errands.h): the nearest
    // auction counter of her zone, nullptr for none; and the zone's floor a
    // step short of an NPC, on the side `from` stands -- a counter or a guard
    // often stands behind a desk the mesh leaves out -- nothing when the mesh
    // has no floor near it
    auto counterIn(const CCharEntity* PPawn) -> const CBaseEntity*;
    auto shortOf(CZone* PZone, const position_t& npc, const position_t& from) -> std::optional<position_t>;

    // Her nation and her rank in it, as the game holds them
    struct Rank
    {
        uint8 nation = 0;
        uint8 rank   = 1;
    };
    auto rankOf(uint32 charid) -> Rank;
    // The highest rank a catch-up takes one of this nation to: the player's
    // own rank, as far as the nation's ladder on the errand table goes
    auto rankCap(uint8 nation, const CCharEntity* PPlayer) -> uint8;

    // His call, and the club's: an errand ends for good, nothing kept, as she
    // leaves his club (a pearl broken, she released). She is back where she
    // left the world
    void drop(uint32 charid, const char* why);

    // Her player logging in as her: her errand ends as his call back ends it,
    // before the map loads her, so she comes in where she left the world
    void endForLogin(uint32 charid);
    // ... and once she is in, he is told it ended
    void zonedIn(CCharEntity* PChar);
} // namespace pawn::errands
