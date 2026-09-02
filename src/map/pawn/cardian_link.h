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

#include <chrono>
#include <cstddef>
#include <optional>

class Scheduler;

// Cardian Link: the direct TCP channel between the companion addon and this
// map server (RESEARCH.md §7, which also carries the invariants this code is
// held to). Newline-delimited text, one connection per client. Everything
// runs on the main thread: the acceptor and every connection are coroutines
// on the scheduler's main context, so game state is never touched from
// another thread and the socket never has two readers or two writers.
//
// Wire (both directions are lines of space-separated words):
//   addon -> server   hello <addon version> | bind <charid> | whoami
//                     | pos <x> <y> <z> <yaw> <moving> | cd <cardian command...>
//                     | ping <n> | pong <n> | stats | bye
//   server -> addon   welcome <server build> <charid> | bound <charid> <name>
//                     | you <charid> <name> <zone> | cd <tag> ... | ping <n>
//                     | pong <n> | stats k=v ... | err <text>
//
// cd carries the cardian management API (scripts/commands/cardian.lua): the
// line after the verb runs as the bound character's `!cardian ...` command,
// and the command's replies come back as cd lines through sendToCharacter.
// This replaced the chat-channel transport (say packets in, channel-31
// lines out) on 2026-09-01.
// The first line must be hello. Each side pings after Config::pingInterval
// of silence and drops the peer after Config::deadAfter of it.
//
// bind attaches the connection to a live character: the charid must name a
// session-backed character whose session's client address is this socket's
// peer. The connection stores only the charid; every later use re-resolves
// and re-verifies it (see resolveBound), so a bind can never dangle across
// zone lines or possession -- it goes stale instead, and the addon binds
// again.
namespace cardian::link
{
    struct Config
    {
        std::chrono::milliseconds pingInterval      = std::chrono::seconds(5);
        std::chrono::milliseconds deadAfter         = std::chrono::seconds(15);
        std::size_t               maxLine           = 2048; // bytes, newline included (equipset manifests ride cd lines)
        uint32                    maxConnections    = 32;
        uint32                    maxLinesPerSecond = 200;
        std::size_t               maxOutboxLines    = 256; // per connection; a full outbox drops the newest line
    };

    // Bind cardian.LINK_PORT and start accepting. No-op when
    // cardian.LINK_ENABLED is false. A port that cannot be bound is a critical
    // log line, not a fatal error: the game keeps serving and the addon
    // reports the link as down. Tests pass their own Config to run the
    // timeout paths in milliseconds.
    void start(Scheduler& scheduler, const Config& config = {});

    struct Stats
    {
        uint32 accepted = 0; // connections accepted since boot
        uint32 live     = 0; // connections open now
        uint32 rejected = 0; // refused at accept: connection cap reached
        uint32 dropped  = 0; // closed by the server: silence, flooding, oversize line, protocol, exception
        uint32 linesIn  = 0;
        uint32 linesOut   = 0;
        uint32 outDropped = 0; // lines dropped because a connection's outbox was full (a peer not reading)
        uint32 posIn      = 0; // pos lines accepted into the side store
    };

    auto stats() -> Stats;

    // Push a line to the connection bound to this character (its cd replies).
    // false when no link is bound to them -- the caller decides what that
    // means (the cardian command falls back to chat for a human typing it).
    auto sendToCharacter(uint32 charid, std::string line) -> bool;

    // The uplink side store (RESEARCH.md par.7, option B): the freshest
    // client-reported position of a bound character, already converted to
    // server conventions. Cardian AI code is the only reader; loc.p and the
    // packet pipeline are never written. Main-thread only, like everything
    // else on the link.
    // TODO(cardian): this store lives inside the transport because the pawn
    // controller is its only consumer. The moment a second consumer appears,
    // move FreshPosition/freshPositionOf and the map behind them into their
    // own file so AI code stops including the socket.
    struct FreshPosition
    {
        float                     x        = 0.0f;
        float                     y        = 0.0f;
        float                     z        = 0.0f;
        uint8                     rotation = 0;
        bool                      moving   = false;
        float                     vx       = 0.0f; // yalms/s over the last samples (smoothed); 0 when standing
        float                     vz       = 0.0f;
        float                     yawRate  = 0.0f; // radians/s, signed; 0 when standing
        std::chrono::milliseconds age{};           // at the moment of the read
    };

    // The store entry for this character -- absent when nothing streamed or
    // the stream went stale (older than a second): consumers fall back to
    // loc.p and the world keeps working without the link.
    auto freshPositionOf(uint32 charid) -> std::optional<FreshPosition>;
} // namespace cardian::link
