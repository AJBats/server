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

// The gambit row grammar (M3.85): the same text on the wire and in the
// database. Round trips and rejections, so a change to the grammar fails
// here before it corrupts a saved set.

#include <catch2/catch_test_macros.hpp>

#include "map/pawn/gambit_text.h"

using namespace gambits;
using pawn::text::formatRow;
using pawn::text::parseRow;

TEST_CASE("row grammar: a one-condition, one-action row round-trips", "[cardian][gambits]")
{
    Gambit_t g;
    g.target_selector = G_TARGET::PARTY;
    g.predicate_groups.emplace_back(G_LOGIC::AND, std::vector<Predicate_t>{ Predicate_t(G_CONDITION::HPP_LT, 50) });
    g.actions.emplace_back(G_REACTION::MA, G_SELECT::HIGHEST, 1);
    g.retry_delay = 0;

    const auto text = formatRow(g);
    REQUIRE(text == "1|1:50|2:0:1|0");

    const auto back = parseRow(text);
    REQUIRE(back.has_value());
    REQUIRE(back->target_selector == G_TARGET::PARTY);
    REQUIRE(back->predicate_groups.size() == 1);
    REQUIRE(back->predicate_groups[0].predicates[0].condition == G_CONDITION::HPP_LT);
    REQUIRE(back->predicate_groups[0].predicates[0].condition_arg == 50);
    REQUIRE(back->actions.size() == 1);
    REQUIRE(back->actions[0].select_arg == 1);
    REQUIRE(formatRow(*back) == text);
}

TEST_CASE("row grammar: OR groups, several groups, several actions and a retry survive the trip", "[cardian][gambits]")
{
    Gambit_t g;
    g.target_selector = G_TARGET::SELF;
    g.predicate_groups.emplace_back(G_LOGIC::OR, std::vector<Predicate_t>{ Predicate_t(G_CONDITION::STATUS, 2), Predicate_t(G_CONDITION::STATUS, 3) });
    g.predicate_groups.emplace_back(G_LOGIC::AND, std::vector<Predicate_t>{ Predicate_t(G_CONDITION::NOT_PT_HAS_TANK, 0) });
    g.actions.emplace_back(G_REACTION::JA, G_SELECT::SPECIFIC, 5);
    g.actions.emplace_back(static_cast<G_REACTION>(100), static_cast<G_SELECT>(2), 1);
    g.retry_delay = 60;

    const auto text = formatRow(g);
    REQUIRE(text == "0|?9:2,9:3&26:0|3:2:5+100:2:1|60");

    const auto back = parseRow(text);
    REQUIRE(back.has_value());
    REQUIRE(back->predicate_groups.size() == 2);
    REQUIRE(back->predicate_groups[0].logic == G_LOGIC::OR);
    REQUIRE(back->predicate_groups[0].predicates.size() == 2);
    REQUIRE(back->predicate_groups[1].logic == G_LOGIC::AND);
    REQUIRE(back->actions.size() == 2);
    REQUIRE(static_cast<uint16>(back->actions[1].reaction) == 100);
    REQUIRE(back->retry_delay == 60);
    REQUIRE(formatRow(*back) == text);
}

TEST_CASE("row grammar: malformed rows are refused", "[cardian][gambits]")
{
    REQUIRE_FALSE(parseRow("").has_value());
    REQUIRE_FALSE(parseRow("1|1:50|2:0:1").has_value());      // a field short
    REQUIRE_FALSE(parseRow("1|1:50|2:0:1|0|x").has_value());  // a field over
    REQUIRE_FALSE(parseRow("x|1:50|2:0:1|0").has_value());    // not a number
    REQUIRE_FALSE(parseRow("1||2:0:1|0").has_value());        // no conditions
    REQUIRE_FALSE(parseRow("1|1:50||0").has_value());         // no actions
    REQUIRE_FALSE(parseRow("1|1|2:0:1|0").has_value());       // a predicate without its argument
    REQUIRE_FALSE(parseRow("1|1:50|2:0|0").has_value());      // an action without its argument
    REQUIRE_FALSE(parseRow("70000|1:50|2:0:1|0").has_value()); // target outside 16 bits
}
