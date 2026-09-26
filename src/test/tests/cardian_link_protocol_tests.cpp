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

// The Cardian Link's messages as C++ makes and reads them
// (map/pawn/cardian_link_messages.h over cardian_link_protocol.h).
//
// The addon reads the same structs through LuaJIT's ffi, so their sizes are the
// contract between the two builds: these pin the ones the addon and
// tools/linktest.py compute from the same file. A size that moves here moves on
// both sides at once and needs a protocol bump, never a quiet change.

#include <catch2/catch_test_macros.hpp>

#include "map/pawn/cardian_link_messages.h"

#include <string>

using namespace cardian::link;

TEST_CASE("Cardian link: the structs are the sizes both sides read", "[cardian][link]")
{
    STATIC_REQUIRE(sizeof(cl_header) == 16);
    STATIC_REQUIRE(sizeof(cl_hello) == 72);
    STATIC_REQUIRE(sizeof(cl_bind) == 36);
    STATIC_REQUIRE(sizeof(cl_pos) == 36);
    STATIC_REQUIRE(sizeof(cl_item) == 8);
    STATIC_REQUIRE(sizeof(cl_inventory) == 664);
    STATIC_REQUIRE(sizeof(cl_give) == 28);
    STATIC_REQUIRE(sizeof(cl_paused) == 40);
    STATIC_REQUIRE(sizeof(cl_walk) == 36);
    STATIC_REQUIRE(sizeof(cl_walk_taken) == 40);
    STATIC_REQUIRE(sizeof(cl_view) == 20);
    STATIC_REQUIRE(sizeof(cl_maneuver) == 28);
    STATIC_REQUIRE(sizeof(cl_maneuvers) == 20);
    STATIC_REQUIRE(sizeof(cl_maneuver_state) == 24);
    STATIC_REQUIRE(sizeof(cl_legacy_cd) == sizeof(cl_header));
}

TEST_CASE("Cardian link: make fills the header and zeroes the rest", "[cardian][link]")
{
    const auto give = make<cl_give>();
    CHECK(give.h.size == sizeof(cl_give));
    CHECK(give.h.type == CL_T_GIVE);
    CHECK(give.h.req == 0);
    CHECK(give.h.flags == 0);
    CHECK(give.h.status == CL_S_OK);
    CHECK(give.cardian == 0);
    CHECK(give.qty == 0);
}

TEST_CASE("Cardian link: a message reads back only at its own size", "[cardian][link]")
{
    auto give    = make<cl_give>();
    give.h.req   = 7;
    give.cardian = 123456;
    give.slot    = 4;
    give.qty     = 12;

    const auto bytes = bytesOf(give);
    REQUIRE(bytes.size() == sizeof(cl_give));

    cl_give back{};
    REQUIRE(decode(bytes, back));
    CHECK(back.h.req == 7);
    CHECK(back.cardian == 123456);
    CHECK(back.slot == 4);
    CHECK(back.qty == 12);

    CHECK_FALSE(decode(std::string_view(bytes).substr(0, bytes.size() - 1), back));
    CHECK_FALSE(decode(bytes + "x", back));
}

TEST_CASE("Cardian link: text is cut to fit and always terminated", "[cardian][link]")
{
    auto bind = make<cl_bind>();

    setText(bind.name, "Zapp");
    CHECK(textOf(bind.name) == "Zapp");

    // Sixteen bytes hold fifteen letters and the terminating zero
    setText(bind.name, "Abcdefghijklmnopqrstuvwxyz");
    CHECK(textOf(bind.name) == "Abcdefghijklmno");
    CHECK(bind.name[15] == '\0');

    // A shorter name leaves nothing of the longer one behind it
    setText(bind.name, "Mi");
    CHECK(textOf(bind.name) == "Mi");
    CHECK(bind.name[5] == '\0');
}

TEST_CASE("Cardian link: type numbers name their messages in the logs", "[cardian][link]")
{
    CHECK(typeName(CL_T_GIVE) == "GIVE");
    CHECK(typeName(CL_T_LEGACY_CD) == "LEGACY_CD");
    CHECK(typeName(0x7FFE) == "0x7FFE");
}
