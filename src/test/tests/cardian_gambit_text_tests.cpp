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

#include "map/pawn/engage_math.h"
#include "map/pawn/gambit_ids.h"
#include "map/pawn/gambit_text.h"

#include <array>
#include <string>

using namespace gambits;
using cardian::engage::isEngageRow;
using cardian::engage::isFoeTarget;
using cardian::engage::pairingError;
using pawn::text::formatRow;
using pawn::text::parseRow;

namespace
{
    // The editor's verdict on a row in the grammar: "" when it may stand
    auto pairing(const std::string& spec) -> std::string
    {
        const auto g = parseRow(spec);
        REQUIRE(g.has_value());
        return std::string(pairingError(*g));
    }

    // An Attack row on the party leader's target under one condition
    auto attackWhen(const G_CONDITION condition, const uint32 arg) -> Gambit_t
    {
        Gambit_t g;
        g.target_selector = pawn::G_TARGET_LEADERS_TARGET;
        g.predicate_groups.emplace_back(G_LOGIC::AND, std::vector<Predicate_t>{ Predicate_t(condition, arg) });
        g.actions.emplace_back(G_REACTION::ATTACK, static_cast<G_SELECT>(0), 0);
        return g;
    }
} // namespace

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

TEST_CASE("row grammar: retired rest actions cannot return through numeric imports", "[cardian][gambits][rest]")
{
    REQUIRE_FALSE(parseRow("0|0:0|100:8:1|0").has_value());
    REQUIRE_FALSE(parseRow("0|0:0|100:10:0|0").has_value());
    REQUIRE_FALSE(parseRow("0|0:0|100:6:1+100:8:1|0").has_value());
    REQUIRE(parseRow("0|0:0|100:6:1|0").has_value());
    REQUIRE(parseRow("0|0:0|100:11:1|0").has_value());
    REQUIRE(parseRow("1|1:50|2:2:8|0").has_value());
    REQUIRE(parseRow("1|1:50|2:2:10|0").has_value());
}

TEST_CASE("row grammar: the retired melee mage switch cannot return", "[cardian][gambits]")
{
    REQUIRE_FALSE(parseRow("0|0:0|100:12:1|0").has_value());
    REQUIRE_FALSE(parseRow("0|0:0|100:12:0|0").has_value());
    REQUIRE_FALSE(parseRow("0|0:0|100:6:1+100:12:1|0").has_value());
    REQUIRE(parseRow("1|1:50|2:2:12|0").has_value()); // spell 12 is a spell, not the behaviour
}

TEST_CASE("row grammar: Avoid links is its own switch, beside Avoid aggro", "[cardian][gambits][avoid]")
{
    for (const auto* spec : { "0|0:0|100:13:1|0", "0|0:0|100:1:1|0" })
    {
        INFO("row " << spec);
        const auto g = parseRow(spec);
        REQUIRE(g.has_value());
        CHECK(formatRow(*g) == spec);
    }
    const auto links = parseRow("0|0:0|100:13:1|0");
    REQUIRE(links.has_value());
    CHECK(static_cast<uint16>(links->actions[0].select) == static_cast<uint16>(pawn::Behavior::AvoidLinks));
    CHECK(pawn::isSwitch(pawn::Behavior::AvoidLinks));
    CHECK_FALSE(pawn::isRetiredBehavior(static_cast<uint16>(pawn::Behavior::AvoidLinks)));
}

TEST_CASE("row grammar: the foe targets with Attack round-trip", "[cardian][gambits]")
{
    constexpr std::array targets{
        pawn::G_TARGET_LEADERS_TARGET,
        pawn::G_TARGET_TARGETED_BY_ALLY,
        pawn::G_TARGET_TARGETING_ALLY,
        pawn::G_TARGET_TARGETING_SELF,
    };
    for (const auto target : targets)
    {
        Gambit_t g;
        g.target_selector = target;
        g.predicate_groups.emplace_back(G_LOGIC::AND, std::vector<Predicate_t>{ Predicate_t(G_CONDITION::ALWAYS, 0) });
        g.actions.emplace_back(G_REACTION::ATTACK, G_SELECT::HIGHEST, 0);

        const auto text = formatRow(g);
        INFO("row " << text);
        REQUIRE(text == std::to_string(static_cast<uint16>(target)) + "|0:0|0:0:0|0");

        const auto back = parseRow(text);
        REQUIRE(back.has_value());
        REQUIRE(back->target_selector == target);
        REQUIRE(back->actions.size() == 1);
        REQUIRE(back->actions[0].reaction == G_REACTION::ATTACK);
        REQUIRE(static_cast<uint16>(back->actions[0].select) == 0);
        REQUIRE(back->actions[0].select_arg == 0);
        REQUIRE(formatRow(*back) == text);
    }
    REQUIRE(parseRow("100|0:0|0:0:0|0").has_value());
    REQUIRE(parseRow("103|0:0|0:0:0|0").has_value());
}

TEST_CASE("row grammar: the tactician's choice condition round-trips", "[cardian][gambits]")
{
    Gambit_t g;
    g.target_selector = G_TARGET::PARTY;
    g.predicate_groups.emplace_back(G_LOGIC::AND, std::vector<Predicate_t>{ Predicate_t(pawn::G_CONDITION_TACTICIANS_CHOICE, 0) });
    g.actions.emplace_back(G_REACTION::MA, G_SELECT::HIGHEST, 1);

    const auto text = formatRow(g);
    REQUIRE(text == "1|101:0|2:0:1|0");

    const auto back = parseRow(text);
    REQUIRE(back.has_value());
    REQUIRE(back->predicate_groups.size() == 1);
    REQUIRE(back->predicate_groups[0].predicates.size() == 1);
    REQUIRE(back->predicate_groups[0].predicates[0].condition == pawn::G_CONDITION_TACTICIANS_CHOICE);
    REQUIRE(back->predicate_groups[0].predicates[0].condition_arg == 0);
    REQUIRE(formatRow(*back) == text);
}

TEST_CASE("row pairing: the foe targets are 100 to 103, and an engage row is one with Attack", "[cardian][gambits][engage]")
{
    CHECK_FALSE(isFoeTarget(static_cast<G_TARGET>(99)));
    CHECK(isFoeTarget(pawn::G_TARGET_LEADERS_TARGET));
    CHECK(isFoeTarget(pawn::G_TARGET_TARGETED_BY_ALLY));
    CHECK(isFoeTarget(pawn::G_TARGET_TARGETING_ALLY));
    CHECK(isFoeTarget(pawn::G_TARGET_TARGETING_SELF));
    CHECK_FALSE(isFoeTarget(static_cast<G_TARGET>(104)));
    CHECK_FALSE(isFoeTarget(G_TARGET::SELF));
    CHECK_FALSE(isFoeTarget(G_TARGET::TARGET)); // her fight, not a foe she has yet to take

    CHECK(isEngageRow(*parseRow("100|0:0|0:0:0|0")));
    CHECK_FALSE(isEngageRow(*parseRow("1|1:50|2:0:1|0")));
    CHECK_FALSE(isEngageRow(*parseRow("0|0:0|100:6:1|0")));
}

TEST_CASE("row pairing: a foe target with Attack stands, under any condition that keeps no clock", "[cardian][gambits][engage]")
{
    CHECK(pairing("100|0:0|0:0:0|0").empty());
    CHECK(pairing("101|0:0|0:0:0|0").empty());
    CHECK(pairing("102|0:0|0:0:0|0").empty());
    CHECK(pairing("103|0:0|0:0:0|0").empty());
    CHECK(pairing("102|2:50|0:0:0|0").empty());        // HP at least 50%
    CHECK(pairing("100|?1:50,2:90|0:0:0|0").empty());  // an OR group
    CHECK(pairing("101|0:0|0:0:0+0:0:0|0").empty());   // Attack twice is still Attack alone
}

TEST_CASE("row pairing: a foe target goes with Attack only", "[cardian][gambits][engage]")
{
    CHECK_FALSE(pairing("102|0:0|2:0:1|0").empty());         // Cure (best)
    CHECK_FALSE(pairing("100|0:0|3:2:35|0").empty());        // an ability
    CHECK_FALSE(pairing("101|0:0|100:6:1|0").empty());       // a behaviour
    CHECK_FALSE(pairing("103|0:0|4:0:0|0").empty());         // a weapon skill
    CHECK_FALSE(pairing("100|0:0|0:0:0+2:0:1|0").empty());   // Attack, then a spell
}

TEST_CASE("row pairing: Attack needs a foe target", "[cardian][gambits][engage]")
{
    CHECK_FALSE(pairing("0|0:0|0:0:0|0").empty()); // Self
    CHECK_FALSE(pairing("1|0:0|0:0:0|0").empty()); // a party member
    CHECK_FALSE(pairing("2|0:0|0:0:0|0").empty()); // her own fight
    CHECK_FALSE(pairing("3|0:0|0:0:0|0").empty()); // the player
}

TEST_CASE("row pairing: an Attack row refuses a timer or a chance, which other rows keep", "[cardian][gambits][engage]")
{
    CHECK_FALSE(pairingError(attackWhen(G_CONDITION::TIMER, 30)).empty());
    CHECK_FALSE(pairingError(attackWhen(G_CONDITION::RANDOM, 50)).empty());

    // In a second group, or beside another condition, it is refused just the same
    auto g = attackWhen(G_CONDITION::ALWAYS, 0);
    g.predicate_groups.emplace_back(G_LOGIC::OR, std::vector<Predicate_t>{ Predicate_t(G_CONDITION::HPP_LT, 50), Predicate_t(G_CONDITION::RANDOM, 10) });
    CHECK_FALSE(pairingError(g).empty());

    // Any other row keeps them
    auto cure            = attackWhen(G_CONDITION::TIMER, 30);
    cure.target_selector = G_TARGET::PARTY;
    cure.actions         = { Action_t(G_REACTION::MA, G_SELECT::HIGHEST, 1) };
    CHECK(pairingError(cure).empty());
}

TEST_CASE("row pairing: every other row stands as it did", "[cardian][gambits][engage]")
{
    CHECK(pairing("1|1:50|2:0:1|0").empty());      // Party member: HP below 50% -> Cure (best)
    CHECK(pairing("0|0:0|100:6:1|0").empty());     // Self -> Rest with the player
    CHECK(pairing("0|0:0|100:11:1|0").empty());    // Self -> Role: Support Mage
    CHECK(pairing("1|101:0|2:0:1|0").empty());     // Party member: Tactician's choice -> Cure (best)
    CHECK(pairing("2|2:60|4:0:0|0").empty());      // Target: HP at least 60% -> Best weapon skill
    CHECK(pairing("2|0:0|1:0:0|0").empty());       // Target -> Ranged attack
}
