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
#include "common/timer.h"

#include <string>
#include <string_view>

class CCharEntity;

// The combat pause: one hold over the whole map process, every zone at once.
//
// A hold freezes the simulation and nothing else. The simulation clock stands still
// (common/timer.h), so every cast, recast, effect and respawn keeps the time it had
// left, and the zone's entity ticks stand down (zone_entities.cpp), so nothing takes a
// step. A command a player's client sends waits for the release (input_gate.h).
// Networking, the Cardian Link, menus, shopping and chat run on: the point is to
// give one player time to think and to order a party, not to stop the world.
//
// There is one hold and it belongs to whoever took it. When that player goes
// offline the hold lets go by itself, and the server carries on as it would have.
//
// Taking and letting go tell the players: each real player's client gets his status
// packet again, which carries speed 0 while held (packets/char_status.cpp) -- that is
// the movement lock, derived from the hold, so there is nothing to restore -- and
// every bound addon gets `cd paused <holder>` or `cd resumed` for its banner. The
// release also tells each client again what is left of his ability recasts and buff
// timers, which it counted down on its own clock while the server stood still.
// Everything here is for the main thread except isHeld(), which is safe from anywhere.
namespace cardian::pause
{

enum class Result : uint8
{
    Ok,
    AlreadyHeld,
    NotHeld,
};

struct Status
{
    bool               held   = false;
    uint32             holder = 0; // charid; 0 is nobody in particular (the system, a test)
    std::string        holderName;
    realtime::duration heldFor{};   // this hold so far, in real time
    uint32             holds = 0;   // taken since boot
    realtime::duration heldTotal{}; // finished holds since boot, in real time
};

auto hold(uint32 holderCharId, std::string_view holderName) -> Result;
auto release(std::string_view why) -> Result;

// The pause button: takes the hold, or lets go of the asker's own. Anyone may pause
// and only the holder may resume (co-op's real rules are a later decision); a player
// on his way out of the game may not pause. Answers with why not, or with nothing
// when it did.
auto toggle(CCharEntity* PChar) -> std::string;

auto isHeld() -> bool;
auto status() -> Status;

// Lets go of a hold whose holder is no longer online, and says whether it did.
// Asked from a tick that keeps running while held.
auto letGoIfHolderLeft() -> bool;

} // namespace cardian::pause
