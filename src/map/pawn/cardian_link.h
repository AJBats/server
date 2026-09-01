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

class Scheduler;

// Cardian Link: the direct TCP channel between the companion addon and this
// map server (RESEARCH.md §7, which also carries the invariants this code is
// held to). Newline-delimited text, one connection per client. Everything
// runs on the main thread: the acceptor and every connection are coroutines
// on the scheduler's main context, so game state is never touched from
// another thread and the socket never has two readers or two writers.
//
// Wire (both directions are lines of space-separated words):
//   addon -> server   hello <addon version> | ping <n> | pong <n> | stats | bye
//   server -> addon   welcome <server build> <charid> | ping <n> | pong <n>
//                     | stats k=v ... | err <text>
// The first line must be hello. Each side pings after Config::pingInterval
// of silence and drops the peer after Config::deadAfter of it.
namespace cardian::link
{
    struct Config
    {
        std::chrono::milliseconds pingInterval      = std::chrono::seconds(5);
        std::chrono::milliseconds deadAfter         = std::chrono::seconds(15);
        std::size_t               maxLine           = 512; // bytes, newline included
        uint32                    maxConnections    = 32;
        uint32                    maxLinesPerSecond = 200;
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
        uint32 linesOut = 0;
    };

    auto stats() -> Stats;
} // namespace cardian::link
