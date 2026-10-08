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
#include "pawn/cardian_link_messages.h"

#include <functional>
#include <optional>
#include <string>

class CBasicPacket;
class CCharEntity;

// The combat pause's input gate: what a player's own client may start while the
// simulation is held (pause.h).
//
// A command -- attack, disengage, a spell, an ability, a weapon skill, a shot, an
// item, /heal, anything else off the action menu -- starts nothing while held. It
// waits as that character's one queued command, the newest replacing the last, the
// way a cardian keeps one order. At the release each is handed to the game's own
// handler, which judges it as if it had just arrived: every refusal and message is
// the game's, as of the release.
//
// What is not a command passes as ever: talking to an NPC (shops and menus work
// through a pause on purpose), assist (it only moves the client's cursor), the
// zone-in sync and the blockaid setting.
//
// Nobody logs out of a held game: a request that would start the logout countdown
// (game time, it would never run down) is refused with a line of chat, not queued.
// Stopping a countdown already running passes.
//
// Nobody starts a synthesis or casts a fishing line in a held game either, refused the
// same way: the client plays those out by itself once told to, while the server's half
// counts game time and would only begin at the release. Nor is the game held by a
// player in the middle of one (pause.cpp, toggle). Harvesting, logging, mining and
// excavation are one trade with an NPC, done when answered: nothing to gate.
//
// His body stays where it is: a position packet while held is pinned to where the
// server has him before its handler reads it (pause P4).
//
// A feature that asks the player before a command of his starts -- the warp together
// (pawn/warp_together.h) -- is asked first, held or not, and may claim the command:
// it then holds it while he answers, and hands it on (handOn) as if it had just
// arrived, or drops it. Main thread only.
namespace cardian::pause::input
{

// A feature's ask: true when it has claimed the packet, which it copies to hand on
// later; the gate and the handler then leave it alone.
using Ask = std::function<bool(CCharEntity* PChar, CBasicPacket& packet)>;

// The one ask, set by its feature as it starts; none in the test server.
void setAsk(Ask ask);

// A claimed command handed on, unasked: queued as his one command while held,
// otherwise judged and run by the game's own handler now.
void handOn(CCharEntity* PChar, const CBasicPacket& packet);

struct Queued
{
    uint16 packetId    = 0;
    uint16 actionId    = 0; // 0x01A's ActionID; 0 for the other packets
    uint16 targetIndex = 0;
};

// Asked of every validated client packet, before its handler. True when the feature's
// ask has claimed it, or while held for a command, which has been queued: either way
// the handler must not run. A position packet is pinned in place and answers false:
// its handler still runs.
auto intercept(CCharEntity* PChar, CBasicPacket& packet) -> bool;

// The command that character has waiting, if any.
auto queued(uint32 charid) -> std::optional<Queued>;

// The same for his command window's queue line: the Link's QUEUE, the action and its
// target's index (CL_AK_NONE with none). The addon words it. It is told whenever this
// changes.
auto queueLine(uint32 charid) -> cl_queue;

// The player takes his queued command back. False with none.
auto cancel(CCharEntity* PChar) -> bool;

// Hands every queued command to its handler and empties the queue. The release's
// last step, once the clock runs again; a character who has left, or changed zone
// since (his target index meant something there), has his dropped.
void replay();

} // namespace cardian::pause::input
