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

#include "cardian_link_messages.h"
#include "common/cbasetypes.h"

#include <chrono>
#include <cstddef>
#include <functional>
#include <optional>
#include <string>
#include <string_view>

class CCharEntity;
class Scheduler;

// Cardian Link: the direct TCP channel between the companion addon and this
// map server (RESEARCH.md §7, which also carries the invariants this code is
// held to). Binary messages, one connection per client; every message and the
// rules they follow are in cardian_link_protocol.h, the one file both sides
// read. Everything runs on the main thread: the acceptor and every connection
// are coroutines on the scheduler's main context, so game state is never
// touched from another thread and the socket never has two readers or two
// writers.
//
// The transport answers the link's own messages (hello, bind, ping, bye, pos,
// stats, whoami). Everything else is answered by whoever registered its type
// with handle() -- the pawn module registers the cardian API at init -- so this
// file carries no game logic and links into xi_test without the pawn module.
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
        std::chrono::milliseconds pingInterval         = std::chrono::seconds(5);
        std::chrono::milliseconds deadAfter            = std::chrono::seconds(15);
        std::size_t               maxMessage           = 16384; // bytes, header included
        uint32                    maxConnections       = 32;
        uint32                    maxMessagesPerSecond = 200;
        std::size_t               maxOutboxMessages    = 256; // per connection; a full outbox drops the newest message
    };

    // Bind cardian.LINK_PORT and start accepting. No-op when
    // cardian.LINK_ENABLED is false. A port that cannot be bound is a critical
    // log line, not a fatal error: the game keeps serving and the addon
    // reports the link as down. Tests pass their own Config to run the
    // timeout paths in milliseconds.
    void start(Scheduler& scheduler, const Config& config = {});

    struct Stats
    {
        uint32 accepted    = 0; // connections accepted since boot
        uint32 live        = 0; // connections open now
        uint32 rejected    = 0; // refused at accept: connection cap reached
        uint32 dropped     = 0; // closed by the server: silence, flooding, oversize message, protocol, exception
        uint32 messagesIn  = 0;
        uint32 messagesOut = 0;
        uint32 outDropped  = 0; // messages dropped because a connection's outbox was full (a peer not reading)
        uint32 posIn       = 0; // pos messages accepted into the side store
    };

    auto stats() -> Stats;

    // The way back to the connection that sent a request, handed to its
    // handler: answers go to the asker, never to whichever link holds the
    // character by then. Made by the transport; valid only while the handler
    // runs. A request sent one-way (req 0) is answered by nothing.
    class Reply
    {
    public:
        Reply(const cl_header& asked, std::function<void(std::string)> send)
        : asked_(asked)
        , send_(std::move(send))
        {
        }

        // An answer that is not the last
        template <typename T>
        void more(T msg)
        {
            if (asked_.req != 0)
            {
                stamp(msg, asked_.req, CL_F_REPLY | CL_F_MORE, CL_S_OK);
                send_(bytesOf(msg));
            }
        }

        // The last answer: the request's own message, as asked or with its
        // answer fields filled, carrying the outcome
        template <typename T>
        void finish(T msg, const uint16 status)
        {
            if (asked_.req != 0)
            {
                stamp(msg, asked_.req, CL_F_REPLY, status);
                send_(bytesOf(msg));
            }
        }

    private:
        cl_header                        asked_;
        std::function<void(std::string)> send_;
    };

    // Who answers an addon message of type T. The transport checks the size and
    // the bind first: the handler gets the bound character, resolved and
    // verified, the message, and the way to answer it. A request of a type
    // nobody handles comes back CL_S_UNKNOWN_TYPE.
    void handleType(uint16 type, std::size_t size, std::function<void(CCharEntity* PChar, std::string_view frame, Reply& reply)> handler);

    template <typename T>
    void handle(std::function<void(CCharEntity* PChar, const T& msg, Reply& reply)> handler)
    {
        handleType(MessageType<T>::value, sizeof(T),
                   [handler = std::move(handler)](CCharEntity* PChar, const std::string_view frame, Reply& reply)
                   {
                       T msg;
                       std::memcpy(&msg, frame.data(), sizeof(T));
                       handler(PChar, msg, reply);
                   });
    }

    // A whole message to the connection bound to this character; false when
    // no link is bound to them
    auto sendBytes(uint32 charid, std::string bytes) -> bool;

    // A whole message to every bound connection
    void sendBytesToAll(const std::string& bytes);

    // A one-way message to this character's addon
    template <typename T>
    auto send(const uint32 charid, T msg) -> bool
    {
        stamp(msg, 0, 0, CL_S_OK);
        return sendBytes(charid, bytesOf(msg));
    }

    // A one-way message to every bound addon at once (the pause taken and let go)
    template <typename T>
    void sendToAll(T msg)
    {
        stamp(msg, 0, 0, CL_S_OK);
        sendBytesToAll(bytesOf(msg));
    }

    // Scaffolding while the text protocol is converted: a line of the old
    // protocol, "<tag> ...", carried to this character's addon as
    // CL_T_LEGACY_CD. Leaves with the conversion.
    auto sendLegacy(uint32 charid, std::string_view line) -> bool;

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
