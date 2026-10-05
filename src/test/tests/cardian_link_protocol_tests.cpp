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
#include "map/pawn/gambit_text.h"
#include "map/pawn/gambit_wire.h"

#include <string>
#include <vector>

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
    STATIC_REQUIRE(sizeof(cl_stake) == 32);
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
    STATIC_REQUIRE(sizeof(cl_offer) == 24);
    STATIC_REQUIRE(sizeof(cl_offer_answer) == 24);
    STATIC_REQUIRE(sizeof(cl_party_role) == 120);
    STATIC_REQUIRE(sizeof(cl_party_roles) == 20);
    STATIC_REQUIRE(sizeof(cl_set_party_role) == 24);
    STATIC_REQUIRE(sizeof(cl_ah_listing) == 16);
    STATIC_REQUIRE(sizeof(cl_ah_shelf) == 1064);
    STATIC_REQUIRE(sizeof(cl_ah_sale) == 40);
    STATIC_REQUIRE(sizeof(cl_ah_history) == 428);
    STATIC_REQUIRE(sizeof(cl_ah_bid) == 40);
    STATIC_REQUIRE(sizeof(cl_roster) == 20);
    STATIC_REQUIRE(sizeof(cl_member) == 96);
    STATIC_REQUIRE(sizeof(cl_sync) == 20);
    STATIC_REQUIRE(sizeof(cl_member_stats) == 56);
    STATIC_REQUIRE(sizeof(cl_worn) == 4);
    STATIC_REQUIRE(sizeof(cl_gear) == 84);
    STATIC_REQUIRE(sizeof(cl_bag) == 4);
    STATIC_REQUIRE(sizeof(cl_bags) == 88);
    STATIC_REQUIRE(sizeof(cl_recast) == 8);
    STATIC_REQUIRE(sizeof(cl_recasts) == 536);
    STATIC_REQUIRE(sizeof(cl_profile) == 64);
    STATIC_REQUIRE(sizeof(cl_jobs) == 48);
    STATIC_REQUIRE(sizeof(cl_skill) == 8);
    STATIC_REQUIRE(sizeof(cl_skills) == 280);
    STATIC_REQUIRE(sizeof(cl_take) == 28);
    STATIC_REQUIRE(sizeof(cl_gil) == 28);
    STATIC_REQUIRE(sizeof(cl_equip_slot) == 4);
    STATIC_REQUIRE(sizeof(cl_equip) == 120);
    STATIC_REQUIRE(sizeof(cl_use) == 24);
    STATIC_REQUIRE(sizeof(cl_drop) == 28);
    STATIC_REQUIRE(sizeof(cl_sort) == 24);
    STATIC_REQUIRE(sizeof(cl_move) == 28);
    STATIC_REQUIRE(sizeof(cl_give_use) == 28);
    STATIC_REQUIRE(sizeof(cl_gambit_condition) == 8);
    STATIC_REQUIRE(sizeof(cl_gambit_action) == 8);
    STATIC_REQUIRE(sizeof(cl_gambit) == 200);
    STATIC_REQUIRE(sizeof(cl_gambit_row) == 354);
    STATIC_REQUIRE(sizeof(cl_gambits) == 24);
    STATIC_REQUIRE(sizeof(cl_gambit_toggle) == 24);
    STATIC_REQUIRE(sizeof(cl_gambit_move) == 24);
    STATIC_REQUIRE(sizeof(cl_gambit_delete) == 24);
    STATIC_REQUIRE(sizeof(cl_gambit_insert) == 224);
    STATIC_REQUIRE(sizeof(cl_gambit_replace) == 224);
    STATIC_REQUIRE(sizeof(cl_gambit_master) == 24);
    STATIC_REQUIRE(sizeof(cl_vocab_condition) == 64);
    STATIC_REQUIRE(sizeof(cl_vocab_conditions) == 3096);
    STATIC_REQUIRE(sizeof(cl_vocab_status) == 32);
    STATIC_REQUIRE(sizeof(cl_vocab_statuses) == 1560);
    STATIC_REQUIRE(sizeof(cl_vocab_action) == 64);
    STATIC_REQUIRE(sizeof(cl_vocab_actions) == 3096);
    STATIC_REQUIRE(sizeof(cl_gambit_vocab) == 24);
    STATIC_REQUIRE(sizeof(cl_owned_cardian) == 24);
    STATIC_REQUIRE(sizeof(cl_owned) == 788);
    STATIC_REQUIRE(sizeof(cl_spawn) == 20);
    STATIC_REQUIRE(sizeof(cl_despawn) == 20);
    STATIC_REQUIRE(sizeof(cl_shout_responder) == 192);
    STATIC_REQUIRE(sizeof(cl_shout) == 28);
    STATIC_REQUIRE(sizeof(cl_peek) == 104);
    STATIC_REQUIRE(sizeof(cl_invite) == 120);
    STATIC_REQUIRE(sizeof(cl_contract) == 28);
    STATIC_REQUIRE(sizeof(cl_contracts) == 468);
    STATIC_REQUIRE(sizeof(cl_end_contract) == 20);
    STATIC_REQUIRE(sizeof(cl_note) == 68);
    STATIC_REQUIRE(sizeof(cl_goal) == 68);
    STATIC_REQUIRE(sizeof(cl_goals) == 52);
    STATIC_REQUIRE(sizeof(cl_cp_item) == 28);
    STATIC_REQUIRE(sizeof(cl_cp_shop) == 56);
    STATIC_REQUIRE(sizeof(cl_cp_buy) == 36);
    STATIC_REQUIRE(sizeof(cl_job_change) == 24);
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
    CHECK(pawn::keyOfAction(cl_action{ CL_AK_OWN_REST, 0, 0 }).empty()); // her own rest is shown, never ordered
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

    // A character of several bytes is kept whole or left out, never cut
    setText(bind.name, "Abcdefghijklmn\xE2\x89\xA5"); // fourteen letters and a three-byte sign
    CHECK(textOf(bind.name) == "Abcdefghijklmn");
}

TEST_CASE("Cardian link: type numbers name their messages in the logs", "[cardian][link]")
{
    CHECK(typeName(CL_T_GIVE) == "GIVE");
    CHECK(typeName(CL_T_CP_BUY) == "CP_BUY");
    CHECK(typeName(0x00FF) == "0x00FF"); // LEGACY_CD's, never reused
    CHECK(typeName(0x7FFE) == "0x7FFE");
}

TEST_CASE("Cardian link: a gambit row crosses as its fields and back", "[cardian][link]")
{
    // The default rows, rows of several groups, an any-of group and two
    // actions, and the Cardian-only ids: -na (best), the Enfeeble action
    // (select 100) and the Enfeeble status (10000)
    for (const std::string row : { "100|0:0|0:0:0|0", "2|2:50|4:0:0|0", "0|0:0|100:4:2|0", "1|101:0|2:0:1|0", "1|1:45&101:0|2:0:1|0",
                                   "1|3:40&?12:3,12:4|2:2:1+2:2:2|5", "0|?12:3,12:4&13:6|3:2:35|0",
                                   "1|101:0|2:0:4|0", "2|101:0|2:100:0|0", "1|9:10000|2:0:4|0" })
    {
        const auto gambit = pawn::text::parseRow(row);
        REQUIRE(gambit.has_value());
        cl_gambit fields{};
        REQUIRE(pawn::wire::toWire(*gambit, fields));
        const auto back = pawn::wire::fromWire(fields);
        REQUIRE(back.has_value());
        CHECK(pawn::text::formatRow(*back) == row);
    }
}

TEST_CASE("Cardian link: a gambit row's fields that no row makes are refused", "[cardian][link]")
{
    const auto valid = [](const std::string& row)
    {
        const auto gambit = pawn::text::parseRow(row);
        REQUIRE(gambit.has_value());
        cl_gambit fields{};
        REQUIRE(pawn::wire::toWire(*gambit, fields));
        return fields;
    };

    // No conditions, no actions
    CHECK_FALSE(pawn::wire::fromWire(cl_gambit{}).has_value());

    // A group listed out of order
    auto skipped                = valid("1|3:40&12:3|2:2:1|0");
    skipped.conditions[1].group = 2;
    CHECK_FALSE(pawn::wire::fromWire(skipped).has_value());

    // An any-of bit on a group that is not there
    auto phantom     = valid("1|3:40|2:2:1|0");
    phantom.orGroups = 0x02;
    CHECK_FALSE(pawn::wire::fromWire(phantom).has_value());

    // A retired behaviour, which the row grammar refuses too
    auto retired       = valid("0|0:0|100:6:1|0");
    retired.actions[0] = cl_gambit_action{ 100, 8, 1 };
    CHECK_FALSE(pawn::wire::fromWire(retired).has_value());
    // And the retired Role row (11), whatever it named; a formation row crosses
    retired.actions[0] = cl_gambit_action{ 100, 11, 2 };
    CHECK_FALSE(pawn::wire::fromWire(retired).has_value());
    retired.actions[0] = cl_gambit_action{ 100, 4, 2 };
    CHECK(pawn::wire::fromWire(retired).has_value());

    // A group with no condition, which the fields cannot name
    gambits::Gambit_t gap;
    gap.predicate_groups.emplace_back(gambits::G_LOGIC::AND, std::vector<gambits::Predicate_t>{ gambits::Predicate_t(gambits::G_CONDITION::ALWAYS, 0) });
    gap.predicate_groups.emplace_back(gambits::G_LOGIC::AND, std::vector<gambits::Predicate_t>{});
    gap.actions.emplace_back(gambits::G_REACTION::MA, gambits::G_SELECT::SPECIFIC, 1);
    cl_gambit gapFields{};
    CHECK_FALSE(pawn::wire::toWire(gap, gapFields));

    // More conditions than the fields carry: not sent at all, the fields left empty
    gambits::Gambit_t big;
    big.predicate_groups.emplace_back(gambits::G_LOGIC::AND, std::vector<gambits::Predicate_t>(pawn::wire::kConditions + 1, gambits::Predicate_t(gambits::G_CONDITION::ALWAYS, 0)));
    big.actions.emplace_back(gambits::G_REACTION::MA, gambits::G_SELECT::SPECIFIC, 1);
    cl_gambit fields{};
    CHECK_FALSE(pawn::wire::toWire(big, fields));
    CHECK(fields.conditionCount == 0);
    CHECK(fields.actionCount == 0);
}
