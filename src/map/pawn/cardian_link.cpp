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

#include "common/logging.h"
#include "common/scheduler.h"
#include "common/settings.h"
#include "common/timer.h"
#include "common/version.h"

#include <asio/ip/tcp.hpp>
#include <asio/read_until.hpp>
#include <asio/write.hpp>

#include <chrono>
#include <exception>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

using namespace std::chrono_literals;

namespace
{
    cardian::link::Stats g_stats;

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

    // One addon connection. Reads run on the main context; a read that sees
    // nothing for pingInterval sends a ping instead, and deadAfter of silence
    // closes the socket. Replies are written inline by the same coroutine,
    // so the socket never has two writers. Nothing thrown in here reaches
    // the scheduler: an exception closes this connection and nothing else.
    class Connection : public std::enable_shared_from_this<Connection>
    {
    public:
        Connection(Scheduler& scheduler, asio::ip::tcp::socket socket, const cardian::link::Config& config)
        : scheduler_(scheduler)
        , socket_(std::move(socket))
        , config_(config)
        , peer_(peerOf(socket_))
        {
            asio::error_code ec;
            socket_.set_option(asio::ip::tcp::no_delay(true), ec);
        }

        auto run() -> Task<void>
        {
            auto self = shared_from_this();

            ++g_stats.accepted;
            ++g_stats.live;
            ShowInfoFmt("link: {} connected ({} open)", peer_, g_stats.live);

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

            if (serverDrop_)
            {
                ++g_stats.dropped;
            }
            --g_stats.live;
            ShowInfoFmt("link: {} disconnected ({})", peer_, closeReason);

            asio::error_code ec;
            socket_.shutdown(asio::ip::tcp::socket::shutdown_both, ec);
            socket_.close(ec);
        }

    private:
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
                    if (!co_await sendLine(fmt::format("ping {}", ++pingSeq_)))
                    {
                        co_return "write failed";
                    }
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

                if (!co_await handleLine(line))
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

        auto sendLine(std::string line) -> Task<bool>
        {
            line.push_back('\n');
            const auto [ec, written] = co_await asio::async_write(socket_, asio::buffer(line), asio::as_tuple(asio::use_awaitable));
            if (ec)
            {
                co_return false;
            }
            ++g_stats.linesOut;
            co_return true;
        }

        // false ends the connection
        auto handleLine(std::string_view line) -> Task<bool>
        {
            const auto words = splitWords(line);
            if (words.empty())
            {
                co_return true;
            }

            const auto verb = words[0];

            if (!greeted_)
            {
                if (verb != "hello")
                {
                    co_await sendLine("err hello first");
                    co_return false;
                }
                greeted_ = true;
                ShowInfoFmt("link: {} hello (addon v{})", peer_, printable(words.size() > 1 ? words[1] : "?"));
                co_return co_await sendLine(fmt::format("welcome {} 0", version::GetGitSha()));
            }

            if (verb == "ping")
            {
                co_return co_await sendLine(fmt::format("pong {}", printable(words.size() > 1 ? words[1] : "0")));
            }
            if (verb == "pong" || verb == "hello")
            {
                co_return true;
            }
            if (verb == "stats")
            {
                const auto s = g_stats;
                co_return co_await sendLine(fmt::format("stats accepted={} live={} rejected={} dropped={} in={} out={}",
                                                        s.accepted, s.live, s.rejected, s.dropped, s.linesIn, s.linesOut));
            }
            if (verb == "bye")
            {
                co_return false;
            }

            ShowDebugFmt("link: {} unknown verb '{}'", peer_, printable(verb));
            co_return co_await sendLine(fmt::format("err unknown {}", printable(verb)));
        }

        Scheduler&                  scheduler_;
        asio::ip::tcp::socket       socket_;
        const cardian::link::Config config_;
        std::string                 peer_;
        std::string                 inbox_;
        timer::time_point           lastRx_{};
        timer::time_point           windowStart_{};
        uint32                      linesThisSecond_ = 0;
        uint32                      pingSeq_         = 0;
        bool                        greeted_         = false;
        bool                        serverDrop_      = false;
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
} // namespace cardian::link
