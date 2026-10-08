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
#include "entities/base_entity.h"

#include <optional>

class CCharEntity;

// Warping together (OPEN_ISSUES #230). The player reads an Instant Warp or
// casts Warp with cardians of his party beside him -- in his zone, alive --
// and it does not start: the input gate's ask (pause/input_gate.h) holds it,
// and his addon asks him how the party follows (offers.h,
// CL_OFFER_WARP_TOGETHER), listing each cardian's way home: her own Warp, an
// Instant Warp from her bag, or nothing, and she stays behind. Her Warp is
// picked before her scroll; he may pick another way she has, or leave her.
//   - The party: his warp goes ahead, and each cardian with a way home uses
//     the one picked as an order ahead of everything else in her line. Each
//     lands at his home point, with him, whoever's home point her own is.
//   - Alone: his warp goes ahead, and they hold where they are, as any warp
//     of his leaves them (warp_hold.h).
//   - Cancel, no answer in time, or his leaving the zone: nothing goes, and
//     his scroll or MP is kept.
// His warp that comes to nothing after all -- interrupted, refused -- calls
// theirs off with it: a cardian still reading or casting hers is stopped, one
// still waiting to is let go of it, and one whose warp has already taken lands
// at his home point all the same. With nobody beside him, no addon to ask, a
// warp of his already under way, or a Warp he cannot cast now, his warp is the
// game's, as ever.
//
// The kit that makes it work: every body of the world in a real player's
// party carries one Instant Warp and one Instant Reraise (the user,
// 2026-10-07; both are rare, so one each is all she can hold), topped up
// with her food. The Reraise is only carried: he orders its use from her bags.
namespace pawn::together
{
    // The gate's ask and the question's resolver, set once at start
    void init();

    // Where a cardian's own warp lands: his home point, while she warps with
    // him on his answer; nothing for any other warp of hers
    auto landingFor(const CCharEntity* PPawn) -> std::optional<location_t>;

    // His warp watched to its end: gone (theirs go on), or come to nothing
    // (theirs are called off). From the zone tick
    void tick();

    // A body of the world's kit topped up in her bag. How many items she was
    // given
    auto topUpKit(CCharEntity* PPawn) -> uint32;
} // namespace pawn::together
