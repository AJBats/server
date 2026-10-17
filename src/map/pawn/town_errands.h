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

class CZone;

// A cardian's own errands in town (the user, 2026-10-16; ROADMAP D, "Town
// errands"; the rules in town_errand_math.h). A cardian in a city zone, free
// of fights and of the player's orders -- a member of his party wherever he
// is, or one of the world's at her own hangout (the town seat she holds) out
// of any party, with a real player there to see her -- looks at what she
// needs as she arrives there, as she becomes free there and as she joins his
// party, and sees to it herself, each one on her own -- a party member at
// once, one of the world's a while later, so a town never empties its seats
// at once:
//   - one of the world's risen past the level she was last dressed for: the
//     auction counter of her zone, where the census is asked to dress her
//     (redress.h) and she waits a while for its answer;
//   - an Instant Warp or an Instant Reraise she lacks and can pay for: a gate
//     guard of her zone who sells to her, else, in his party, her own
//     nation's consulate in another zone of her city -- bought through the
//     guard's own sale (supplies.h);
//   - one of the world's at her seat whose career wants another main or
//     support job: a Mog House door or a Nomad Moogle of her zone, her jobs
//     changed there (redress.h takeCareerJobs, which asks the census for her
//     gear for them at once), and then the counter, to be dressed for them.
//     In his party she changes no job: the role he took her on in stays.
// One of his own -- an alt, an owned cardian (a recruit he owns among them)
// -- buys scrolls alone: her gear and her jobs are his to manage (one of the
// world's wearing his pearl, or a linkshell recruit, is still the world's). Her
// stops are visited nearest next, each walked as an errand's walk order (the
// one walker). In his party she says one line as she sets out and one once
// every stop is done, nothing at a stop -- and invited with nothing she can
// see to, her ready line at once; one of the world's says nothing, and walks
// back to her seat. A fight or an order of his suspends her errands, and she
// looks afresh once she is free in the city again; a pause holds her as it
// holds everyone. His going on ends nothing: she keeps going, says she will
// join him when she is ready, and then follows him by the usual rule -- on a
// trek through the zone line he walked, or holding where she is after a warp
// (warp_hold.h). Her errands are done before she sets out to meet him: while
// they run she treks after nobody (CPawnController::Treks), and a trek to
// meet him -- his Follow included -- waits. Every step is in the map log,
// `errand: ...`.
namespace pawn::town
{
    // From each zone's module tick: the errands under way stepped, and a city
    // zone's cardians looked at, each once a second
    void tick(CZone* PZone);

    // Her errands are under way, or she was invited into his party in a city
    // a moment ago and has not looked at her needs yet: she follows the player
    // through no zone line until they are done
    auto busy(uint32 charid) -> bool;

    // She has accepted the player's invite (pawn.cpp): her first look in a
    // city answers it, with her ready line when she has nothing to see to
    void noteJoined(uint32 charid);
} // namespace pawn::town
