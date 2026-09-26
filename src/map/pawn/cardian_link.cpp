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

#include "cardian_link.h"
#include "view.h"
#include "formation_math.h"

#include "common/logging.h"
#include "common/scheduler.h"
#include "common/settings.h"
#include "common/timer.h"
#include "common/version.h"

#include "command_handler.h"
#include "common/types/position.h"
#include "entities/char_entity.h"
#include "map_session.h"
#include "pause/pause.h"
#include "utils/zoneutils.h"
#include "zone.h"

// The map's Lua state (luautils.h), taken by forward declaration so this
// transport never pays sol2's compile cost
namespace sol
{
class state;
}
extern sol::state lua;

#include <asio/ip/tcp.hpp>
#include <asio/read.hpp>
#include <asio/steady_timer.hpp>
#include <asio/write.hpp>

#include <chrono>
#include <cmath>
#include <deque>
#include <exception>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>

using namespace std::chrono_literals;

namespace
{
    using namespace cardian::link;

    Stats g_stats;

    // The uplink side store: charid -> last streamed position, server
    // conventions. Written by pos messages, read by cardian AI, both on the
    // main thread; entries go stale by age rather than needing cleanup,
    // but unbind and disconnect erase eagerly anyway.
    // TODO(cardian): the moment a second consumer of this store appears,
    // move it (and FreshPosition) out of the transport into its own file.
    struct StoredPosition
    {
        float                      x        = 0.0f;
        float                      y        = 0.0f;
        float                      z        = 0.0f;
        uint8                      rotation = 0;
        bool                       moving   = false;
        cardian::formation::Motion motion{};
        timer::time_point          at{};
    };
    std::unordered_map<uint32, StoredPosition> g_freshPositions;

    // StoredPosition::at is simulation time, unlike the connection's own liveness
    // fields, and that serves both of its jobs. Freshness asks how much game has
    // passed since the addon said where he stands. Velocity divides by the gap
    // between two samples, and a held simulation takes no simulation time: the last
    // sample before a pause and the first after it sit a normal step apart, so his
    // motion carries across the pause unbroken. That holds only while samples that
    // arrive DURING a hold are not ingested -- they would report a standing player
    // and a zero gap, and collapse the estimate. handlePos keeps them out.
    constexpr auto FreshPositionMaxAge = std::chrono::seconds(1);

    class Connection;
    // charid -> the connection bound to it, for messages addressed to a
    // character (sendBytes). Maintained by bind/unbind/disconnect on the main
    // thread; a connection erases itself before it can die.
    std::unordered_map<uint32, Connection*> g_boundConnections;

    // Who answers each addon message the transport does not answer itself
    struct Registered
    {
        std::size_t                                                                   size = 0;
        std::function<void(CCharEntity* PChar, std::string_view frame, Reply& reply)> handler;
    };
    std::unordered_map<uint16, Registered> g_handlers;

    // An addon of the text protocol opened with a line starting "hello ". The
    // first bytes are enough to tell it apart: as a binary size they are far
    // over any limit.
    constexpr std::string_view kTextHello = "hell";

    // Peer bytes never reach the log raw
    auto printable(std::string_view text) -> std::string
    {
        constexpr std::size_t maxShown = 64;

        std::string out;
        out.reserve(std::min(text.size(), maxShown) + 3);
        for (const char c : text.substr(0, maxShown))
        {
            out.push_back((c >= 0x20 && c <= 0x7e) ? c : '?');
        }
        if (text.size() > maxShown)
        {
            out += "...";
        }
        return out;
    }

    auto peerOf(const asio::ip::tcp::socket& socket) -> std::string
    {
        asio::error_code ec;
        const auto       endpoint = socket.remote_endpoint(ec);
        return ec ? "unknown" : fmt::format("{}:{}", endpoint.address().to_string(), endpoint.port());
    }

    auto headerOf(const std::string_view frame) -> cl_header
    {
        cl_header header{};
        std::memcpy(&header, frame.data(), sizeof(header));
        return header;
    }

    // One addon connection: two coroutines on the main context sharing this
    // object.
    //   - the READER (run/serve) owns the lifecycle: it reads messages,
    //     handles them, pings a quiet peer, drops a silent one, and closes the
    //     socket when it is done.
    //   - the WRITER (writeLoop) is the only code that ever writes to the
    //     socket. Everyone else -- answers, pings, pushes -- calls enqueue(),
    //     which never blocks: the outbox is bounded and a full one drops the
    //     newest message with a counter. A stalled peer can therefore never
    //     stall the reader, and the silence rule ends it.
    // Nothing thrown in either reaches the scheduler: an exception closes
    // this connection and nothing else.
    class Connection : public std::enable_shared_from_this<Connection>
    {
    public:
        Connection(Scheduler& scheduler, asio::ip::tcp::socket socket, const Config& config)
        : scheduler_(scheduler)
        , socket_(std::move(socket))
        , wake_(scheduler.mainContext())
        , config_(config)
        , peer_(peerOf(socket_))
        {
            asio::error_code ec;
            const auto       endpoint = socket_.remote_endpoint(ec);
            peerAddress_              = ec ? "" : endpoint.address().to_string();
            socket_.set_option(asio::ip::tcp::no_delay(true), ec);
        }

        auto run() -> Task<void>
        {
            auto self = shared_from_this();

            ++g_stats.accepted;
            ++g_stats.live;
            ShowInfoFmt("link: {} connected ({} open)", peer_, g_stats.live);

            scheduler_.postToMainThread(
                [self]() -> Task<void>
                {
                    co_await self->writeLoop();
                });

            std::string closeReason;
            try
            {
                closeReason = co_await serve();
            }
            catch (const std::exception& e)
            {
                closeReason = fmt::format("exception: {}", e.what());
                serverDrop_ = true;
            }
            catch (...)
            {
                closeReason = "unknown exception";
                serverDrop_ = true;
            }

            // Let the writer deliver what the peer was told (a refusal, a last
            // answer) before the socket goes -- bounded, so a stalled peer
            // cannot hold the close
            for (int i = 0; i < 100 && writeError_.empty() && (!outbox_.empty() || writeInFlight_); ++i)
            {
                co_await Scheduler::yieldFor(10ms);
            }

            if (!writeError_.empty())
            {
                closeReason = "write failed: " + writeError_;
            }

            closing_ = true;
            wake_.cancel(); // release the writer if it is waiting for messages

            if (serverDrop_)
            {
                ++g_stats.dropped;
            }
            --g_stats.live;
            unbind(); // no ghost freshness or messages after the link is gone
            ShowInfoFmt("link: {} disconnected ({})", peer_, closeReason);

            asio::error_code ec;
            socket_.shutdown(asio::ip::tcp::socket::shutdown_both, ec);
            socket_.close(ec);
        }

        // A whole message addressed to this connection's character
        void push(std::string bytes)
        {
            enqueue(std::move(bytes));
        }

    private:
        // What the character's link owns goes with the bind -- but only while this
        // connection is that link: a newer connection that took the character
        // over (an addon reloaded while this one waits out its silence) keeps
        // its stream and its view
        void unbind()
        {
            if (boundCharID_ == 0)
            {
                return;
            }
            if (const auto it = g_boundConnections.find(boundCharID_); it != g_boundConnections.end() && it->second == this)
            {
                g_boundConnections.erase(it);
                g_freshPositions.erase(boundCharID_);
                cardian::view::clearById(boundCharID_); // his camera came off her with the addon; her walk ends with the view (WalkOrderTick)
            }
            boundCharID_ = 0;
        }

        // The only path to the wire. Never blocks, never writes.
        void enqueue(std::string bytes)
        {
            if (closing_)
            {
                return;
            }
            if (outbox_.size() >= config_.maxOutboxMessages)
            {
                ++g_stats.outDropped;
                return;
            }
            outbox_.push_back(std::move(bytes));
            wake_.cancel(); // a no-op unless the writer is waiting
        }

        // The last answer to one of the link's own requests; a one-way message gets none
        template <typename T>
        void answer(T msg, const uint16 status)
        {
            if (msg.h.req == 0)
            {
                return;
            }
            stamp(msg, msg.h.req, CL_F_REPLY, status);
            enqueue(bytesOf(msg));
        }

        // A request the link refuses comes back as itself with the status; a
        // one-way message, or an answer, is not answered
        void refuse(const std::string_view frame, const uint16 status)
        {
            auto header = headerOf(frame);
            if (header.req == 0 || (header.flags & CL_F_REPLY) != 0)
            {
                return;
            }
            header.flags  = CL_F_REPLY;
            header.status = status;
            std::string bytes(frame);
            std::memcpy(bytes.data(), &header, sizeof(header));
            enqueue(std::move(bytes));
        }

        auto writeLoop() -> Task<void>
        {
            try
            {
                while (!closing_ && socket_.is_open() && !scheduler_.closeRequested())
                {
                    if (outbox_.empty())
                    {
                        // Parked until enqueue() cancels the timer
                        wake_.expires_after(std::chrono::hours(1));
                        co_await wake_.async_wait(asio::as_tuple(asio::use_awaitable));
                        continue;
                    }

                    const std::string bytes = std::move(outbox_.front());
                    outbox_.pop_front();

                    writeInFlight_           = true;
                    const auto [ec, written] = co_await asio::async_write(socket_, asio::buffer(bytes), asio::as_tuple(asio::use_awaitable));
                    writeInFlight_           = false;
                    if (ec)
                    {
                        if (!closing_)
                        {
                            writeError_ = ec.message();
                            asio::error_code ignored;
                            socket_.close(ignored); // the reader's pending read fails and ends the connection
                        }
                        co_return;
                    }
                    ++g_stats.messagesOut;
                }
            }
            catch (const std::exception& e)
            {
                writeInFlight_ = false;
                writeError_    = fmt::format("exception: {}", e.what());
                asio::error_code ignored;
                socket_.close(ignored);
            }
        }

        // Returns why the connection ended
        auto serve() -> Task<std::string>
        {
            lastRx_      = realtime::now();
            windowStart_ = lastRx_;

            // Room for a whole message and more behind it: every whole message
            // is taken out after each read, so what waits is under maxMessage
            const std::size_t inboxLimit = config_.maxMessage * 4;

            while (socket_.is_open() && !scheduler_.closeRequested())
            {
                // Read into the inbox itself, so bytes that arrive as the read
                // times out are kept rather than lost with the cancelled read
                const auto before = inbox_.size();
                auto       result = co_await Scheduler::withTimeout(
                    asio::async_read(socket_, asio::dynamic_buffer(inbox_, inboxLimit), asio::transfer_at_least(1), asio::as_tuple(asio::use_awaitable)),
                    config_.pingInterval);

                if (inbox_.size() != before)
                {
                    lastRx_ = realtime::now();
                }

                std::string endReason;
                if (!result.has_value())
                {
                    // Nothing arrived within a ping interval
                    if (realtime::now() - lastRx_ > config_.deadAfter)
                    {
                        serverDrop_ = true;
                        co_return fmt::format("silent for {}ms", config_.deadAfter.count());
                    }
                    if (inbox_.size() == before)
                    {
                        auto ping  = make<cl_ping>();
                        ping.h.req = ++pingSeq_;
                        enqueue(bytesOf(ping));
                    }
                }
                else if (const auto ec = std::get<0>(*result); ec)
                {
                    endReason = ec == asio::error::eof ? "closed by peer" : ec.message();
                }

                // Every whole message in the inbox, in order, even when the read
                // also ended the connection
                if (auto reason = drainInbox(); !reason.empty())
                {
                    co_return reason;
                }
                if (!endReason.empty())
                {
                    co_return endReason;
                }
            }

            co_return "server shutting down";
        }

        // Handles every whole message waiting in the inbox, then drops what was
        // handled; a non-empty reason ends the connection
        auto drainInbox() -> std::string
        {
            std::size_t handled = 0;
            auto        reason  = drainFrom(handled);
            inbox_.erase(0, handled);
            return reason;
        }

        // Each message is handled where it lies in the inbox, and `handled`
        // moves past it: the inbox is shifted once per drain, not once per
        // message. Nothing a handler does touches the inbox.
        auto drainFrom(std::size_t& handled) -> std::string
        {
            while (true)
            {
                const auto rest = std::string_view(inbox_).substr(handled);

                if (!greeted_ && rest.starts_with(kTextHello))
                {
                    // The text protocol's own welcome line, carrying this server's
                    // protocol number: an addon that old unloads itself with its
                    // own mismatch message
                    enqueue(fmt::format("welcome {} 0 {}\n", version::GetGitSha(), static_cast<int>(CL_PROTOCOL)));
                    return "an addon of the text protocol, told this server's protocol";
                }

                if (rest.size() < sizeof(uint32_t))
                {
                    return {};
                }
                uint32_t size = 0;
                std::memcpy(&size, rest.data(), sizeof(size));
                if (size < sizeof(cl_header) || size > config_.maxMessage)
                {
                    serverDrop_ = true;
                    return fmt::format("message size {} (limit {})", size, config_.maxMessage);
                }
                if (rest.size() < size)
                {
                    return {};
                }

                const auto frame = rest.substr(0, size);
                handled += size;

                const auto now = realtime::now();
                if (now - windowStart_ >= 1s)
                {
                    windowStart_        = now;
                    messagesThisSecond_ = 0;
                }
                if (++messagesThisSecond_ > config_.maxMessagesPerSecond)
                {
                    serverDrop_ = true;
                    return "flooding";
                }

                ++g_stats.messagesIn;
                if (auto reason = handleMessage(frame); !reason.empty())
                {
                    return reason;
                }
            }
        }

        // The bound character, re-resolved and re-verified on EVERY use: it
        // must exist, be session-backed (a pawn is not), and its session's
        // client address must still be this socket's peer. Sessions die at
        // zone lines and possession re-homes identities, so nothing here may
        // cache a pointer; any failure clears the bind and the addon,
        // told so, binds again.
        auto resolveBound() -> CCharEntity*
        {
            if (boundCharID_ == 0)
            {
                return nullptr;
            }
            auto* PChar = zoneutils::GetChar(boundCharID_);
            if (PChar == nullptr || PChar->PSession == nullptr || PChar->PSession->client_ipp.getIPString() != peerAddress_)
            {
                unbind();
                return nullptr;
            }
            return PChar;
        }

        // The bound character for a message that needs one. Without it, a
        // request comes back refused and a one-way message is answered with
        // an UNBOUND notice, so the addon binds again either way.
        auto requireBound(const std::string_view frame) -> CCharEntity*
        {
            const bool wasBound = boundCharID_ != 0;
            if (auto* PChar = resolveBound())
            {
                return PChar;
            }
            const uint16 why = wasBound ? CL_S_BIND_STALE : CL_S_NOT_BOUND;
            if (headerOf(frame).req != 0)
            {
                refuse(frame, why);
            }
            else
            {
                auto notice     = make<cl_unbound>();
                notice.h.status = why;
                enqueue(bytesOf(notice));
            }
            return nullptr;
        }

        // A non-empty reason ends the connection
        auto handleMessage(const std::string_view frame) -> std::string
        {
            const auto header = headerOf(frame);

            if (!greeted_)
            {
                if (header.type != CL_T_HELLO)
                {
                    refuse(frame, CL_S_HELLO_FIRST);
                    serverDrop_ = true;
                    return "hello expected";
                }
                return handleHello(frame);
            }

            switch (header.type)
            {
                case CL_T_HELLO:
                    return {}; // a second hello changes nothing
                case CL_T_BYE:
                    return "bye";
                case CL_T_PING:
                    handlePing(frame);
                    return {};
                case CL_T_STATS:
                    handleStats(frame);
                    return {};
                case CL_T_BIND:
                    handleBind(frame);
                    return {};
                case CL_T_WHOAMI:
                    handleWhoami(frame);
                    return {};
                case CL_T_POS:
                    handlePos(frame);
                    return {};
                case CL_T_LEGACY_CD:
                    handleLegacy(frame);
                    return {};
                default:
                    break;
            }

            const auto it = g_handlers.find(header.type);
            if (it == g_handlers.end())
            {
                ShowDebugFmt("link: {} sent a message nobody handles: {}", peer_, typeName(header.type));
                refuse(frame, CL_S_UNKNOWN_TYPE);
                return {};
            }
            if (frame.size() != it->second.size)
            {
                refuse(frame, CL_S_MALFORMED);
                return {};
            }
            if (auto* PChar = requireBound(frame))
            {
                Reply reply(header,
                            [this](std::string bytes)
                            {
                                enqueue(std::move(bytes));
                            });
                it->second.handler(PChar, frame, reply);
            }
            return {};
        }

        auto handleHello(const std::string_view frame) -> std::string
        {
            cl_hello hello{};
            if (!decode(frame, hello) || hello.magic != CL_MAGIC)
            {
                refuse(frame, CL_S_MALFORMED);
                serverDrop_ = true;
                return "hello malformed";
            }

            ShowInfoFmt("link: {} hello (addon v{}, protocol {})", peer_, printable(textOf(hello.version)), hello.protocol);

            auto welcome     = hello;
            welcome.protocol = CL_PROTOCOL;
            setText(welcome.version, version::GetGitSha());
            if (hello.protocol != CL_PROTOCOL)
            {
                answer(welcome, CL_S_PROTOCOL_MISMATCH);
                return fmt::format("protocol {}, this server's is {}", hello.protocol, static_cast<int>(CL_PROTOCOL));
            }

            greeted_ = true;
            answer(welcome, CL_S_OK);
            return {};
        }

        void handlePing(const std::string_view frame)
        {
            cl_ping ping{};
            if (!decode(frame, ping))
            {
                refuse(frame, CL_S_MALFORMED);
                return;
            }
            // An answer to our own ping needs nothing: its arrival already
            // counts as the peer being alive
            if ((ping.h.flags & CL_F_REPLY) == 0)
            {
                answer(ping, CL_S_OK);
            }
        }

        void handleStats(const std::string_view frame)
        {
            cl_stats ask{};
            if (!decode(frame, ask))
            {
                refuse(frame, CL_S_MALFORMED);
                return;
            }
            const auto s    = g_stats;
            ask.accepted    = s.accepted;
            ask.live        = s.live;
            ask.rejected    = s.rejected;
            ask.dropped     = s.dropped;
            ask.messagesIn  = s.messagesIn;
            ask.messagesOut = s.messagesOut;
            ask.outDropped  = s.outDropped;
            ask.posIn       = s.posIn;
            answer(ask, CL_S_OK);
        }

        void handleBind(const std::string_view frame)
        {
            cl_bind ask{};
            if (!decode(frame, ask) || ask.charid == 0)
            {
                refuse(frame, CL_S_MALFORMED);
                return;
            }
            auto* PChar = zoneutils::GetChar(ask.charid);
            if (PChar == nullptr)
            {
                answer(ask, CL_S_NO_SUCH_CHARACTER);
                return;
            }
            if (PChar->PSession == nullptr)
            {
                answer(ask, CL_S_NOT_PLAYED);
                return;
            }
            if (PChar->PSession->client_ipp.getIPString() != peerAddress_)
            {
                answer(ask, CL_S_ADDRESS_MISMATCH);
                return;
            }
            if (boundCharID_ != ask.charid)
            {
                unbind(); // the old identity's stream and messages die with the bind
            }
            boundCharID_                   = ask.charid;
            g_boundConnections[ask.charid] = this; // a later bind of the same character from another link takes over
            calibrated_                    = false;
            ShowInfoFmt("link: {} bound to {} ({})", peer_, PChar->getName(), ask.charid);

            auto bound = ask;
            setText(bound.name, PChar->getName());
            answer(bound, CL_S_OK);

            // What the addon must know from here: whether the simulation is
            // held, or else where the calendar stands (it runs behind real time
            // by every pause so far)
            if (const auto pause = cardian::pause::status(); pause.held)
            {
                auto paused     = make<cl_paused>();
                paused.holder   = pause.holder;
                paused.gametime = earth_time::vanadiel_timestamp();
                setText(paused.holderName, pause.holderName);
                enqueue(bytesOf(paused));
            }
            else
            {
                auto calendar     = make<cl_calendar>();
                calendar.gametime = earth_time::vanadiel_timestamp();
                enqueue(bytesOf(calendar));
            }
        }

        void handleWhoami(const std::string_view frame)
        {
            cl_whoami ask{};
            if (!decode(frame, ask))
            {
                refuse(frame, CL_S_MALFORMED);
                return;
            }
            auto* PChar = requireBound(frame);
            if (PChar == nullptr)
            {
                return;
            }
            ask.charid = PChar->id;
            ask.zone   = static_cast<uint16_t>(PChar->getZone());
            setText(ask.name, PChar->getName());
            answer(ask, CL_S_OK);
        }

        // The old protocol's cardian verbs, until each has its own message:
        // the text runs as the bound character's !cardian command, whose
        // replies come back as LEGACY_CD through sendLegacy
        void handleLegacy(const std::string_view frame)
        {
            auto* PChar = requireBound(frame);
            if (PChar == nullptr)
            {
                return;
            }
            auto text = frame.substr(sizeof(cl_legacy_cd));
            text.remove_prefix(std::min(text.find_first_not_of(' '), text.size()));
            if (text.empty())
            {
                return;
            }
            CCommandHandler::call(scheduler_, ::lua, PChar, fmt::format("cardian {}", text));
        }

        // The client's raw values, filed in the side store in server
        // conventions with the motion derived from the previous sample. No
        // answer: twenty a second answer themselves in aggregate through
        // ping health.
        void handlePos(const std::string_view frame)
        {
            cl_pos pos{};
            if (!decode(frame, pos) || !std::isfinite(pos.x) || !std::isfinite(pos.y) || !std::isfinite(pos.z) || !std::isfinite(pos.yaw) || pos.moving > 1)
            {
                ShowDebugFmt("link: {} sent a malformed pos", peer_);
                return;
            }
            auto* PChar = requireBound(frame);
            if (PChar == nullptr)
            {
                return;
            }

            // A held simulation (pause/pause.h) takes no samples: see StoredPosition::at
            if (cardian::pause::isHeld())
            {
                return;
            }

            // The 0x015 handler's axis swap (its "not a typo" lines): the
            // client's y-slot is the server's z and vice versa; yaw radians
            // encode into the uint8 rotation
            StoredPosition stored;
            stored.x        = pos.x;
            stored.y        = pos.z;
            stored.z        = pos.y;
            stored.rotation = radianToRotation(pos.yaw);
            stored.moving   = pos.moving == 1;
            stored.at       = timer::now();

            // Where the player is, in the map log every WORLD_WHERE_LOG
            // seconds: the slot tables are authored by walking (ROADMAP D3)
            // and the database only learns a position on a zone change
            if (const auto every = settings::get<uint32>("pawn.WORLD_WHERE_LOG"); every > 0)
            {
                static std::unordered_map<uint32, timer::time_point> lastWhere;
                auto&                                                 last = lastWhere[boundCharID_];
                if (stored.at - last >= std::chrono::seconds(every))
                {
                    last = stored.at;
                    ShowInfoFmt("link: {} is at ({:.1f}, {:.1f}, {:.1f}) rot {} in {}, {}", PChar->getName(), stored.x, stored.y, stored.z, stored.rotation,
                                PChar->loc.zone != nullptr ? PChar->loc.zone->getName() : "no zone", stored.moving ? "moving" : "standing");
                }
            }

            if (const auto previous = g_freshPositions.find(boundCharID_); previous != g_freshPositions.end())
            {
                const auto& p  = previous->second;
                const float dt = std::chrono::duration<float>(stored.at - p.at).count();
                stored.motion  = cardian::formation::deriveMotion(
                    cardian::formation::Sample{ p.x, p.z, p.rotation, p.moving }, p.motion,
                    cardian::formation::Sample{ stored.x, stored.z, stored.rotation, stored.moving }, dt);
            }

            // One line per bind comparing the stream to the packet-fed
            // position settles the axis/rotation conventions live
            if (!calibrated_)
            {
                calibrated_ = true;
                ShowInfoFmt("link: {} pos calibration for {}: stream ({:.1f} {:.1f} {:.1f} rot {}) vs loc.p ({:.1f} {:.1f} {:.1f} rot {})",
                            peer_, PChar->getName(), stored.x, stored.y, stored.z, stored.rotation,
                            PChar->loc.p.x, PChar->loc.p.y, PChar->loc.p.z, PChar->loc.p.rotation);
            }

            g_freshPositions[boundCharID_] = stored;
            ++g_stats.posIn;
        }

        Scheduler&              scheduler_;
        asio::ip::tcp::socket   socket_;
        asio::steady_timer      wake_; // the writer parks on this; enqueue() cancels it
        const Config            config_;
        std::string             peer_;
        std::string             peerAddress_; // address only, for session-identity checks
        std::string             inbox_;
        std::deque<std::string> outbox_;
        std::string             writeError_;
        realtime::time_point    lastRx_{};
        realtime::time_point    windowStart_{};
        uint32                  messagesThisSecond_ = 0;
        uint32                  pingSeq_            = 0;
        uint32                  boundCharID_        = 0;
        bool                    calibrated_         = false;
        bool                    greeted_            = false;
        bool                    serverDrop_         = false;
        bool                    closing_            = false;
        bool                    writeInFlight_      = false;
    };

    class Listener
    {
    public:
        Listener(Scheduler& scheduler, const Config& config)
        : scheduler_(scheduler)
        , acceptor_(scheduler.mainContext())
        , config_(config)
        {
        }

        auto open(uint16 port) -> bool
        {
            asio::error_code ec;
            const auto       endpoint = asio::ip::tcp::endpoint(asio::ip::tcp::v4(), port);

            acceptor_.open(endpoint.protocol(), ec);
            if (!ec)
            {
                acceptor_.set_option(asio::socket_base::reuse_address(true), ec);
            }
            if (!ec)
            {
                acceptor_.bind(endpoint, ec);
            }
            if (!ec)
            {
                acceptor_.listen(asio::socket_base::max_listen_connections, ec);
            }
            if (ec)
            {
                ShowCriticalFmt("link: cannot listen on port {}: {} -- the companion addon will report the link as down", port, ec.message());
                asio::error_code ignored;
                acceptor_.close(ignored);
                return false;
            }
            return true;
        }

        auto acceptLoop() -> Task<void>
        {
            while (!scheduler_.closeRequested())
            {
                auto [ec, socket] = co_await acceptor_.async_accept(asio::as_tuple(asio::use_awaitable));
                if (ec)
                {
                    if (ec == asio::error::operation_aborted)
                    {
                        break;
                    }
                    // Out of descriptors and the like: don't spin on it
                    ShowErrorFmt("link: accept failed: {}", ec.message());
                    co_await Scheduler::yieldFor(1s);
                    continue;
                }

                if (g_stats.live >= config_.maxConnections)
                {
                    ++g_stats.rejected;
                    ShowWarningFmt("link: {} rejected: {} connections already open", peerOf(socket), g_stats.live);
                    asio::error_code ignored;
                    socket.close(ignored);
                    continue;
                }

                // The connection owns itself through the coroutine's captured pointer
                auto connection = std::make_shared<Connection>(scheduler_, std::move(socket), config_);
                scheduler_.postToMainThread(
                    [connection]() -> Task<void>
                    {
                        co_await connection->run();
                    });
            }
        }

    private:
        Scheduler&              scheduler_;
        asio::ip::tcp::acceptor acceptor_;
        const Config            config_;
    };

    // Owned by the process: the scheduler's io_context is gone by the time
    // statics are destroyed, so the listener is never torn down (the same
    // arrangement as the pawn container).
    Listener* g_listener = nullptr;
} // namespace

namespace cardian::link
{
    void start(Scheduler& scheduler, const Config& config)
    {
        if (g_listener != nullptr)
        {
            return;
        }
        cardian::view::startTimers(scheduler); // the steer tick (pawn/view.h): the map hands the scheduler to nobody else

        if (!settings::get<bool>("cardian.LINK_ENABLED"))
        {
            ShowInfo("link: disabled (cardian.LINK_ENABLED)");
            return;
        }

        const auto port = settings::get<uint16>("cardian.LINK_PORT");

        auto* listener = new Listener(scheduler, config);
        if (!listener->open(port))
        {
            delete listener;
            return;
        }

        g_listener = listener;
        scheduler.postToMainThread(g_listener->acceptLoop());
        ShowInfoFmt("link: listening on port {} (protocol {}, ping {}ms, dead {}ms, {} connections max)", port, static_cast<int>(CL_PROTOCOL),
                    config.pingInterval.count(), config.deadAfter.count(), config.maxConnections);
    }

    auto stats() -> Stats
    {
        return g_stats;
    }

    void handleType(const uint16 type, const std::size_t size, std::function<void(CCharEntity* PChar, std::string_view frame, Reply& reply)> handler)
    {
        g_handlers[type] = Registered{ size, std::move(handler) };
    }

    auto sendBytes(const uint32 charid, std::string bytes) -> bool
    {
        const auto it = g_boundConnections.find(charid);
        if (it == g_boundConnections.end())
        {
            return false;
        }
        it->second->push(std::move(bytes));
        return true;
    }

    void sendBytesToAll(const std::string& bytes)
    {
        for (const auto& [charid, connection] : g_boundConnections)
        {
            connection->push(bytes);
        }
    }

    auto sendLegacy(const uint32 charid, const std::string_view line) -> bool
    {
        auto header   = make<cl_legacy_cd>();
        header.h.size = static_cast<uint32_t>(sizeof(cl_legacy_cd) + line.size());
        auto bytes    = bytesOf(header);
        bytes.append(line);
        return sendBytes(charid, std::move(bytes));
    }

    auto freshPositionOf(uint32 charid) -> std::optional<FreshPosition>
    {
        const auto it = g_freshPositions.find(charid);
        if (it == g_freshPositions.end())
        {
            return std::nullopt;
        }

        const auto age = timer::now() - it->second.at;
        if (age > FreshPositionMaxAge)
        {
            return std::nullopt;
        }

        return FreshPosition{ it->second.x, it->second.y, it->second.z, it->second.rotation, it->second.moving,
                              it->second.motion.vx, it->second.motion.vz, it->second.motion.yawRate,
                              std::chrono::duration_cast<std::chrono::milliseconds>(age) };
    }
} // namespace cardian::link
