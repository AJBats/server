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

// The Cardian Link's messages, for C++: the shared definitions (plain C that
// the addon reads too, cardian_link_protocol.h) and what C++ adds to them --
// each struct paired with its type number, and making, reading and text helpers.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <string_view>
#include <type_traits>

#include "cardian_link_protocol.h"

namespace cardian::link
{
    // Every message, as (CL_T_ suffix, struct): the one C++ list, checked
    // below against the struct rules and read by typeName for the logs
#define CARDIAN_LINK_MESSAGES(X) \
    X(HELLO, cl_hello)           \
    X(BIND, cl_bind)             \
    X(PING, cl_ping)             \
    X(BYE, cl_bye)               \
    X(POS, cl_pos)               \
    X(STATS, cl_stats)           \
    X(WHOAMI, cl_whoami)         \
    X(UNBOUND, cl_unbound)       \
    X(LEGACY_CD, cl_legacy_cd)   \
    X(INVENTORY, cl_inventory)   \
    X(GIVE, cl_give)             \
    X(WALK, cl_walk)             \
    X(VIEW, cl_view)             \
    X(MANEUVER, cl_maneuver)     \
    X(MANEUVERS, cl_maneuvers)   \
    X(ORDERS, cl_orders)         \
    X(SET_STRATEGY, cl_set_strategy) \
    X(SET_HUNT, cl_set_hunt)     \
    X(RETREAT, cl_retreat)       \
    X(STAKE, cl_stake)           \
    X(ENGAGE, cl_engage)         \
    X(WAIT, cl_wait)             \
    X(RESCUE, cl_rescue)         \
    X(HOMEPOINT, cl_homepoint)   \
    X(CANCEL, cl_cancel)         \
    X(DO, cl_do)                 \
    X(QUEUES, cl_queues)         \
    X(QUEUE, cl_queue)           \
    X(MANEUVER_STATE, cl_maneuver_state) \
    X(WALK_TAKEN, cl_walk_taken) \
    X(PAUSE, cl_pause)           \
    X(PAUSED, cl_paused)         \
    X(RESUMED, cl_resumed)       \
    X(CALENDAR, cl_calendar)     \
    X(AH_SHELF, cl_ah_shelf)     \
    X(AH_HISTORY, cl_ah_history) \
    X(AH_BID, cl_ah_bid)

    template <typename T>
    struct MessageType;

#define CARDIAN_LINK_PAIR(NAME, STRUCT)                                                                  \
    template <>                                                                                          \
    struct MessageType<STRUCT>                                                                           \
    {                                                                                                    \
        static constexpr uint16_t value = CL_T_##NAME;                                                   \
    };                                                                                                   \
    static_assert(std::is_trivially_copyable_v<STRUCT> && std::is_standard_layout_v<STRUCT>,             \
                  #STRUCT " crosses the wire as its bytes");                                             \
    static_assert(offsetof(STRUCT, h) == 0, #STRUCT " starts with its header");
    CARDIAN_LINK_MESSAGES(CARDIAN_LINK_PAIR)
#undef CARDIAN_LINK_PAIR

    static_assert(sizeof(cl_header) == 16, "the header is 16 bytes on both sides");

    // The name of a type number for the logs, "0x1234" when none is known
    inline auto typeName(const uint16_t type) -> std::string
    {
        switch (type)
        {
#define CARDIAN_LINK_CASE(NAME, STRUCT) \
    case CL_T_##NAME:                   \
        return #NAME;
            CARDIAN_LINK_MESSAGES(CARDIAN_LINK_CASE)
#undef CARDIAN_LINK_CASE
            default:
            {
                char hex[8];
                std::snprintf(hex, sizeof(hex), "0x%04X", type);
                return hex;
            }
        }
    }

    // A message of this type with its header's size and type filled in, the
    // rest zero
    template <typename T>
    auto make() -> T
    {
        T msg{};
        msg.h.size = sizeof(T);
        msg.h.type = MessageType<T>::value;
        return msg;
    }

    // The header fields a sender sets on a message it sends: the request it
    // belongs to (0 one-way), whether it answers (CL_F_*), and the outcome
    template <typename T>
    void stamp(T& msg, const uint32_t req, const uint16_t flags, const uint16_t status)
    {
        msg.h.req    = req;
        msg.h.flags  = flags;
        msg.h.status = status;
    }

    template <typename T>
    auto bytesOf(const T& msg) -> std::string
    {
        return std::string(reinterpret_cast<const char*>(&msg), sizeof(T));
    }

    // A whole message read back into its struct: false when the size is not
    // the struct's
    template <typename T>
    auto decode(std::string_view frame, T& out) -> bool
    {
        if (frame.size() != sizeof(T))
        {
            return false;
        }
        std::memcpy(&out, frame.data(), sizeof(T));
        return true;
    }

    // Text into a fixed field: cut to fit, the terminating zero kept
    template <std::size_t N>
    void setText(char (&field)[N], std::string_view text)
    {
        const auto length = std::min(text.size(), N - 1);
        std::memcpy(field, text.data(), length);
        std::memset(field + length, 0, N - length);
    }

    // A fixed text field as the sender wrote it, never reading past it
    template <std::size_t N>
    auto textOf(const char (&field)[N]) -> std::string_view
    {
        return { field, static_cast<std::size_t>(std::find(field, field + N, '\0') - field) };
    }
} // namespace cardian::link
