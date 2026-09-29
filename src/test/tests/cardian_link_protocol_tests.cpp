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

#include "map/pawn/action_keys.h"
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
    STATIC_REQUIRE(sizeof(cl_orders) == 28);
    STATIC_REQUIRE(sizeof(cl_set_strategy) == 20);
    STATIC_REQUIRE(sizeof(cl_set_hunt) == 20);
    STATIC_REQUIRE(sizeof(cl_retreat) == 20);
    STATIC_REQUIRE(sizeof(cl_stake) == 20);
    STATIC_REQUIRE(sizeof(cl_engage) == 20);
    STATIC_REQUIRE(sizeof(cl_wait) == 24);
    STATIC_REQUIRE(sizeof(cl_rescue) == 32);
    STATIC_REQUIRE(sizeof(cl_homepoint) == 20);
    STATIC_REQUIRE(sizeof(cl_cancel) == 20);
    STATIC_REQUIRE(sizeof(cl_pause) == sizeof(cl_header));
    STATIC_REQUIRE(sizeof(cl_action) == 4);
    STATIC_REQUIRE(sizeof(cl_do) == 28);
    STATIC_REQUIRE(sizeof(cl_queue) == 28);
    STATIC_REQUIRE(sizeof(cl_queues) == sizeof(cl_header));
    STATIC_REQUIRE(sizeof(cl_ah_listing) == 16);
    STATIC_REQUIRE(sizeof(cl_ah_shelf) == 1064);
    STATIC_REQUIRE(sizeof(cl_ah_sale) == 40);
    STATIC_REQUIRE(sizeof(cl_ah_history) == 428);
    STATIC_REQUIRE(sizeof(cl_ah_bid) == 40);
    STATIC_REQUIRE(sizeof(cl_legacy_cd) == sizeof(cl_header));
}

TEST_CASE("Cardian link: a cardian's order key and its action fields cross both ways", "[cardian][link]")
{
    // Every key the pawn code gives an order, and back unchanged
    for (const std::string key : { "1:0:0", "2:2:1", "3:2:35", "4:2:32", "item:4112", "attack", "disengage", "move", "movewait", "rest:80" })
    {
        const auto action = pawn::actionOfKey(key);
        CHECK(action.kind != CL_AK_NONE);
        CHECK(pawn::keyOfAction(action) == key);
    }

    const auto cure = pawn::actionOfKey("2:2:1");
    CHECK(cure.kind == CL_AK_MAGIC);
    CHECK(cure.mode == 2);
    CHECK(cure.id == 1);

    // Nothing a cardian is ordered: no key, and no fields that pass for one
    CHECK(pawn::actionOfKey("").kind == CL_AK_NONE);
    CHECK(pawn::actionOfKey("cmd:Heal").kind == CL_AK_NONE);
    CHECK(pawn::actionOfKey("5:2:4112").kind == CL_AK_NONE); // an item is item:<id> alone
    CHECK(pawn::actionOfKey("rest:0").kind == CL_AK_NONE);
    CHECK(pawn::keyOfAction(cl_action{ CL_AK_CLIENT, 0, 2 }).empty());
    CHECK(pawn::keyOfAction(cl_action{ CL_AK_HEAL, 0, 0 }).empty());
    CHECK(pawn::keyOfAction(cl_action{ CL_AK_REST, 0, 101 }).empty());
    CHECK(pawn::keyOfAction(cl_action{ 99, 0, 0 }).empty());
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
