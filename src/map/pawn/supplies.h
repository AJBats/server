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

class CCharEntity;
class CZone;

// A cardian's conquest supplies (the user, 2026-10-08): the Instant Warp she
// warps home by (warp_together.h) and the Instant Reraise he orders from her
// bags. Nothing hands them to her; she buys them at a conquest guard with her
// own conquest points, as a player does -- a body of the world, an alt, one of
// his own alike. The census gives a body of the world the points her career
// would have earned (tools/world/census.py, CP_RATE and CP_KEPT).
//   - At a guard: the player standing by a gate guard or a consulate's
//     (gate_guards.h, the conquest exchange's reach), each cardian of his party
//     beside him who lacks a scroll and has the points buys it, through the
//     guard's own sale (modules/cardian/lua/conquest_exchange.lua). A guard of
//     another nation sells her nothing unless her nation outranks his in the
//     conquest tally: she says so, that her own nation's consulate would sell,
//     once in kSpeakAgain at that guard.
//   - Arriving in a city with a conquest shop (any zone-in, a warp home
//     within the city included): once the party has caught up, one cardian
//     of his party in the city with him who lacks a scroll and could buy
//     one, picked at random, says she wants to visit the conquest shop. One
//     who has both, cannot pay for either or has no room in her bag says
//     nothing.
// What she does is told to the player's addon (the Link's SUPPLIES), which
// words it: the purchase as the conquest exchange's own line, the rest as her
// line in party chat.
namespace pawn::guards
{
    struct Guard;
} // namespace pawn::guards

namespace pawn::supplies
{
    // From each zone's module tick
    void tick(CZone* PZone);
    // The gear-up errand's stop at a guard (errands.h): she, standing at
    // this guard, buys the scrolls she lacks through the guard's own sale as
    // she would at his side; the player told (his addon's SUPPLIES) when
    // given. How many she bought; `refusal`, when given, the guard's reason
    // for turning her away in the exchange's terms (OUTRANKED: another
    // nation's guard, his nation outranking hers), empty when none
    auto buyAt(CCharEntity* PPawn, const guards::Guard& guard, const CCharEntity* PTell, std::string* refusal = nullptr) -> uint32;
    // A real player's zone-in (the pawn module's): an arrival, wherever from
    void zonedIn(const CCharEntity* PPlayer);
} // namespace pawn::supplies
