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
// server has him before its handler reads it (pause P4). Main thread only.
namespace cardian::pause::input
{

struct Queued
{
    uint16 packetId    = 0;
    uint16 actionId    = 0; // 0x01A's ActionID; 0 for the other packets
    uint16 targetIndex = 0;
};

// Asked of every validated client packet, before its handler. True while held for a
// command: it has been queued and the handler must not run. A position packet is
// pinned in place and answers false: its handler still runs.
auto intercept(CCharEntity* PChar, CBasicPacket& packet) -> bool;

// The command that character has waiting, if any.
auto queued(uint32 charid) -> std::optional<Queued>;

// The same for his command window's queue line: its key and its target's index, "2:2:1
// 1024", "" with none. The addon words it. It is told whenever this changes (`cd q
// <his name> <key> <target index>`).
auto queuedLine(uint32 charid) -> std::string;

// The player takes his queued command back. False with none.
auto cancel(CCharEntity* PChar) -> bool;

// Hands every queued command to its handler and empties the queue. The release's
// last step, once the clock runs again; a character who has left, or changed zone
// since (his target index meant something there), has his dropped.
void replay();

} // namespace cardian::pause::input
