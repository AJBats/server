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
#include "errand_math.h"

#include <optional>
#include <string>

class CCharEntity;

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
// Level and money are kept in the row's shape and on the Link, not built.
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
        uint32                 secondsLeft = 0;
        uint16                 zone        = 0; // away: the zone of her route she is crossing now
        std::string            title;           // a quest's or a mission's
    };
    auto viewOf(uint32 charid) -> std::optional<View>;

    // Why gear up cannot go now for a member of his (CL_S_*), CL_S_OK when it
    // can: standing, up, out of a fight, in a zone with an auction counter
    auto gearRefusal(uint32 charid) -> uint16;

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
} // namespace pawn::errands
