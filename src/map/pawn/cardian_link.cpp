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
#include "formation_math.h"

#include "common/logging.h"
#include "common/scheduler.h"
#include "common/settings.h"
#include "common/timer.h"
#include "common/version.h"

#include "common/types/position.h"
#include "entities/char_entity.h"
#include "map_session.h"
#include "utils/zoneutils.h"
#include "zone.h"

#include <asio/ip/tcp.hpp>
#include <asio/read_until.hpp>
#include <asio/steady_timer.hpp>
#include <asio/write.hpp>

#include <charconv>
#include <chrono>
#include <cmath>
#include <deque>
#include <exception>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

using namespace std::chrono_literals;

namespace
{
    cardian::link::Stats g_stats;

    // The uplink side store: charid -> last streamed position, server
    // conventions. Written by pos lines, read by cardian AI, both on the
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

    constexpr auto FreshPositionMaxAge = std::chrono::seconds(1);

    auto splitWords(std::string_view line) -> std::vector<std::string_view>
    {
        std::vector<std::string_view> words;
        std::size_t                   pos = 0;
        while (pos < line.size())
        {
            const auto start = line.find_first_not_of(' ', pos);
            if (start == std::string_view::npos)
            {
                break;
            }
            const auto end = line.find(' ', start);
            words.emplace_back(line.substr(start, end == std::string_view::npos ? std::string_view::npos : end - start));
            if (end == std::string_view::npos)
            {
                break;
            }
            pos = end + 1;
        }
        return words;
    }

    // 0 = malformed (bind treats 0 itself as malformed too)
    auto parseCharID(std::string_view text) -> uint32
    {
        uint32     value    = 0;
        const auto [ptr, ec] = std::from_chars(text.data(), text.data() + text.size(), value);
        return (ec == std::errc() && ptr == text.data() + text.size()) ? value : 0;
    }

    auto parseFloat(std::string_view text, float& out) -> bool
    {
        const auto [ptr, ec] = std::from_chars(text.data(), text.data() + text.size(), out);
        return ec == std::errc() && ptr == text.data() + text.size() && std::isfinite(out);
    }

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

    // One addon connection: two coroutines on the main context sharing this
    // object.
    //   - the READER (run/serve) owns the lifecycle: it reads lines, handles
    //     them, pings a quiet peer, drops a silent one, and closes the socket
    //     when it is done.
    //   - the WRITER (writeLoop) is the only code that ever writes to the
    //     socket. Everyone else -- replies, pings, future pushes -- calls
    //     enqueue(), which never blocks: the outbox is bounded and a full
    //     one drops the newest line with a counter. A stalled peer can
    //     therefore never stall the reader, and the silence rule ends it.
    // Nothing thrown in either reaches the scheduler: an exception closes
    // this connection and nothing else.
    class Connection : public std::enable_shared_from_this<Connection>
    {
    public:
        Connection(Scheduler& scheduler, asio::ip::tcp::socket socket, const cardian::link::Config& config)
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

            // Let the writer deliver what the peer was told (an err, a last
            // pong) before the socket goes -- bounded, so a stalled peer
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
            wake_.cancel(); // release the writer if it is waiting for lines

            if (serverDrop_)
            {
                ++g_stats.dropped;
            }
            --g_stats.live;
            if (boundCharID_ != 0)
            {
                g_freshPositions.erase(boundCharID_); // no ghost freshness after the link is gone
            }
            ShowInfoFmt("link: {} disconnected ({})", peer_, closeReason);

            asio::error_code ec;
            socket_.shutdown(asio::ip::tcp::socket::shutdown_both, ec);
            socket_.close(ec);
        }

    private:
        // The only path to the wire. Never blocks, never writes.
        void enqueue(std::string line)
        {
            if (closing_)
            {
                return;
            }
            if (outbox_.size() >= config_.maxOutboxLines)
            {
                ++g_stats.outDropped;
                return;
            }
            line.push_back('\n');
            outbox_.push_back(std::move(line));
            wake_.cancel(); // a no-op unless the writer is waiting
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

                    const std::string line = std::move(outbox_.front());
                    outbox_.pop_front();

                    writeInFlight_           = true;
                    const auto [ec, written] = co_await asio::async_write(socket_, asio::buffer(line), asio::as_tuple(asio::use_awaitable));
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
                    ++g_stats.linesOut;
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
            lastRx_      = timer::now();
            windowStart_ = lastRx_;

            while (socket_.is_open() && !scheduler_.closeRequested())
            {
                auto result = co_await Scheduler::withTimeout(
                    asio::async_read_until(socket_, asio::dynamic_buffer(inbox_, config_.maxLine), '\n', asio::as_tuple(asio::use_awaitable)),
                    config_.pingInterval);

                if (!result.has_value())
                {
                    // Nothing arrived within a ping interval
                    if (timer::now() - lastRx_ > config_.deadAfter)
                    {
                        serverDrop_ = true;
                        co_return fmt::format("silent for {}ms", config_.deadAfter.count());
                    }
                    enqueue(fmt::format("ping {}", ++pingSeq_));
                    continue;
                }

                const auto [ec, length] = result.value();
                if (ec)
                {
                    if (ec == asio::error::eof)
                    {
                        co_return "closed by peer";
                    }
                    if (ec == asio::error::not_found)
                    {
                        serverDrop_ = true;
                        co_return "line too long";
                    }
                    co_return ec.message();
                }

                const auto now = timer::now();
                lastRx_        = now;

                if (now - windowStart_ >= 1s)
                {
                    windowStart_     = now;
                    linesThisSecond_ = 0;
                }
                if (++linesThisSecond_ > config_.maxLinesPerSecond)
                {
                    serverDrop_ = true;
                    co_return "flooding";
                }

                std::string line = inbox_.substr(0, length - 1);
                inbox_.erase(0, length);
                if (!line.empty() && line.back() == '\r')
                {
                    line.pop_back();
                }

                ++g_stats.linesIn;

                if (!handleLine(line))
                {
                    if (!greeted_)
                    {
                        serverDrop_ = true;
                        co_return "hello expected";
                    }
                    co_return "bye";
                }
            }

            co_return "server shutting down";
        }

        // The bound character, re-resolved and re-verified on EVERY use: it
        // must exist, be session-backed (a pawn is not), and its session's
        // client address must still be this socket's peer. Sessions die at
        // zone lines and possession re-homes identities, so nothing here may
        // cache a pointer; any failure clears the bind and the addon,
        // noticing its own identity, binds again.
        auto resolveBound() -> CCharEntity*
        {
            if (boundCharID_ == 0)
            {
                return nullptr;
            }
            auto* PChar = zoneutils::GetChar(boundCharID_);
            if (PChar == nullptr || PChar->PSession == nullptr || PChar->PSession->client_ipp.getIPString() != peerAddress_)
            {
                boundCharID_ = 0;
                return nullptr;
            }
            return PChar;
        }

        // false ends the connection
        auto handleLine(std::string_view line) -> bool
        {
            const auto words = splitWords(line);
            if (words.empty())
            {
                return true;
            }

            const auto verb = words[0];

            if (!greeted_)
            {
                if (verb != "hello")
                {
                    enqueue("err hello first");
                    return false;
                }
                greeted_ = true;
                ShowInfoFmt("link: {} hello (addon v{})", peer_, printable(words.size() > 1 ? words[1] : "?"));
                enqueue(fmt::format("welcome {} 0", version::GetGitSha()));
                return true;
            }

            if (verb == "ping")
            {
                enqueue(fmt::format("pong {}", printable(words.size() > 1 ? words[1] : "0")));
                return true;
            }
            if (verb == "pong" || verb == "hello")
            {
                return true;
            }
            if (verb == "stats")
            {
                const auto s = g_stats;
                enqueue(fmt::format("stats accepted={} live={} rejected={} dropped={} in={} out={} outdrop={} pos={}",
                                    s.accepted, s.live, s.rejected, s.dropped, s.linesIn, s.linesOut, s.outDropped, s.posIn));
                return true;
            }
            if (verb == "bind")
            {
                const auto id = words.size() > 1 ? parseCharID(words[1]) : 0;
                if (id == 0)
                {
                    enqueue("err bind malformed");
                    return true;
                }
                auto* PChar = zoneutils::GetChar(id);
                if (PChar == nullptr)
                {
                    enqueue("err bind no such character");
                    return true;
                }
                if (PChar->PSession == nullptr)
                {
                    enqueue("err bind not a played character");
                    return true;
                }
                if (PChar->PSession->client_ipp.getIPString() != peerAddress_)
                {
                    enqueue("err bind address mismatch");
                    return true;
                }
                if (boundCharID_ != 0 && boundCharID_ != id)
                {
                    g_freshPositions.erase(boundCharID_); // the old identity's stream dies with the bind
                }
                boundCharID_ = id;
                calibrated_  = false;
                ShowInfoFmt("link: {} bound to {} ({})", peer_, PChar->getName(), id);
                enqueue(fmt::format("bound {} {}", id, PChar->getName()));
                return true;
            }
            if (verb == "whoami")
            {
                if (boundCharID_ == 0)
                {
                    enqueue("err not bound");
                    return true;
                }
                auto* PChar = resolveBound();
                if (PChar == nullptr)
                {
                    enqueue("err bind stale");
                    return true;
                }
                enqueue(fmt::format("you {} {} {}", PChar->id, PChar->getName(),
                                    PChar->loc.zone != nullptr ? PChar->loc.zone->getName() : "nozone"));
                return true;
            }
            if (verb == "pos")
            {
                handlePos(words);
                return true;
            }
            if (verb == "bye")
            {
                return false;
            }

            ShowDebugFmt("link: {} unknown verb '{}'", peer_, printable(verb));
            enqueue(fmt::format("err unknown {}", printable(verb)));
            return true;
        }

        // pos <x> <y> <z> <yaw> <moving>: the client's raw values, filed in
        // the side store in server conventions with the motion derived from
        // the previous sample. No ack: twenty a second answer themselves in
        // aggregate through ping health.
        void handlePos(const std::vector<std::string_view>& words)
        {
            if (boundCharID_ == 0)
            {
                enqueue("err not bound");
                return;
            }
            auto* PChar = resolveBound();
            if (PChar == nullptr)
            {
                enqueue("err bind stale");
                return;
            }

            float x   = 0.0f;
            float y   = 0.0f;
            float z   = 0.0f;
            float yaw = 0.0f;
            if (words.size() < 6 ||
                !parseFloat(words[1], x) || !parseFloat(words[2], y) ||
                !parseFloat(words[3], z) || !parseFloat(words[4], yaw) ||
                (words[5] != "0" && words[5] != "1"))
            {
                enqueue("err pos malformed");
                return;
            }

            // The 0x015 handler's axis swap (its "not a typo" lines): the
            // client's y-slot is the server's z and vice versa; yaw radians
            // encode into the uint8 rotation
            StoredPosition stored;
            stored.x        = x;
            stored.y        = z;
            stored.z        = y;
            stored.rotation = radianToRotation(yaw);
            stored.moving   = words[5] == "1";
            stored.at       = timer::now();

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

        Scheduler&                  scheduler_;
        asio::ip::tcp::socket       socket_;
        asio::steady_timer          wake_; // the writer parks on this; enqueue() cancels it
        const cardian::link::Config config_;
        std::string                 peer_;
        std::string                 peerAddress_; // address only, for session-identity checks
        std::string                 inbox_;
        std::deque<std::string>     outbox_;
        std::string                 writeError_;
        timer::time_point           lastRx_{};
        timer::time_point           windowStart_{};
        uint32                      linesThisSecond_ = 0;
        uint32                      pingSeq_         = 0;
        uint32                      boundCharID_     = 0;
        bool                        calibrated_      = false;
        bool                        greeted_         = false;
        bool                        serverDrop_      = false;
        bool                        closing_         = false;
        bool                        writeInFlight_   = false;
    };

    class Listener
    {
    public:
        Listener(Scheduler& scheduler, const cardian::link::Config& config)
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
        Scheduler&                  scheduler_;
        asio::ip::tcp::acceptor     acceptor_;
        const cardian::link::Config config_;
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
        ShowInfoFmt("link: listening on port {} (ping {}ms, dead {}ms, {} connections max)", port, config.pingInterval.count(), config.deadAfter.count(), config.maxConnections);
    }

    auto stats() -> Stats
    {
        return g_stats;
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
