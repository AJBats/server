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

// The tactician line (ROADMAP K, RESEARCH §14.12 decisions 17-20): where
// the line sits, what each row means where it sits, which spells a row
// below it lets her tactician cast, and when her tactician's melee gives way
// to her rest.

#include <catch2/catch_test_macros.hpp>

#include "map/pawn/gambit_defaults.h"
#include "map/pawn/gambit_text.h"
#include "map/pawn/tactician_line.h"
#include "map/spell.h"

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

using namespace gambits;
using cardian::tactician::State;
using pawn::text::parseRow;

namespace
{
    auto row(const std::string& spec) -> Gambit_t
    {
        INFO("row " << spec);
        const auto g = parseRow(spec);
        REQUIRE(g.has_value());
        return *g;
    }

    auto rows(const std::vector<std::string>& specs) -> std::vector<Gambit_t>
    {
        std::vector<Gambit_t> out;
        for (const auto& spec : specs)
        {
            out.push_back(row(spec));
        }
        return out;
    }

    auto lineOf(const std::vector<Gambit_t>& list) -> std::optional<std::size_t>
    {
        return cardian::tactician::lineOf(list, [](const Gambit_t& g) -> const Gambit_t& { return g; });
    }

    auto statesOf(const std::vector<Gambit_t>& list) -> std::vector<State>
    {
        std::vector<State> out;
        const auto         line = lineOf(list);
        for (std::size_t i = 0; i < list.size(); ++i)
        {
            out.push_back(cardian::tactician::stateOf(list[i], i + 1, line));
        }
        return out;
    }

    // The state of one row placed below a Support Mage row
    auto below(const std::string& spec) -> State
    {
        return statesOf(rows({ "0|0:0|100:11:1|0", spec }))[1];
    }

    const std::string kSupportMage = "0|0:0|100:11:1|0";
    const std::string kRest        = "0|0:0|100:6:1|0";
    const std::string kCureBest    = "1|101:0|2:0:1|0";
} // namespace

TEST_CASE("tactician line: her lists are spell.h's numbers", "[cardian][gambits][tactician]")
{
    using cardian::tactician::kCureTiers;
    using cardian::tactician::kPricedDebuffs;
    CHECK(kCureTiers[0] == static_cast<uint16>(SpellID::Cure));
    CHECK(kCureTiers[5] == static_cast<uint16>(SpellID::Cure_VI));
    CHECK(cardian::tactician::kCureFamily == static_cast<uint32>(SPELLFAMILY_CURE));

    const std::vector<std::pair<SpellID, SPELLFAMILY>> priced{
        { SpellID::Paralyze, SPELLFAMILY_PARALYZE }, { SpellID::Slow, SPELLFAMILY_SLOW }, { SpellID::Blind, SPELLFAMILY_BLIND },
        { SpellID::Dia, SPELLFAMILY_DIA }, { SpellID::Diaga, SPELLFAMILY_DIAGA }, { SpellID::Poison, SPELLFAMILY_POISON },
        { SpellID::Poisonga, SPELLFAMILY_POISONGA }, { SpellID::Bio, SPELLFAMILY_BIO },
    };
    REQUIRE(priced.size() == kPricedDebuffs.size());
    for (std::size_t i = 0; i < priced.size(); ++i)
    {
        INFO("priced debuff " << i);
        CHECK(kPricedDebuffs[i].id == static_cast<uint16>(priced[i].first));
        CHECK(kPricedDebuffs[i].family == static_cast<uint32>(priced[i].second));
    }
}

TEST_CASE("tactician line: the line is her first Support Mage row, whatever its checkbox or condition", "[cardian][gambits][tactician]")
{
    // No Support Mage row, no line: every row is an order
    CHECK_FALSE(lineOf(rows({ "100|0:0|0:0:0|0", kRest, "0|0:0|100:11:3|0" })).has_value());

    CHECK(lineOf(rows({ kRest, kSupportMage, kCureBest })) == std::optional<std::size_t>(2));
    // A Support Mage row under a condition is the line all the same
    CHECK(lineOf(rows({ kRest, "0|3:50|100:11:1|0", kCureBest })) == std::optional<std::size_t>(2));

    // A second Support Mage row below the first is only a behaviour row
    // below the line: struck out
    const auto states = statesOf(rows({ kSupportMage, kCureBest, kSupportMage }));
    CHECK(states == std::vector<State>{ State::Line, State::Allows, State::NotBelow });
}

TEST_CASE("tactician line: above the line every row is an order, and Tactician's choice is struck out", "[cardian][gambits][tactician]")
{
    const auto states = statesOf(rows({ "2|0:0|4:2:1|0", kRest, "1|101:0|2:0:1|0", kSupportMage }));
    CHECK(states == std::vector<State>{ State::Order, State::Order, State::NoChoice, State::Line });

    // With no line at all, Tactician's choice has no tactician to leave it to
    CHECK(statesOf(rows({ kCureBest, kRest })) == std::vector<State>{ State::NoChoice, State::Order });
}

TEST_CASE("tactician line: below the line only her cures, her priced debuffs and the melee fit", "[cardian][gambits][tactician]")
{
    // Her cures, for someone on the party's side
    CHECK(below(kCureBest) == State::Allows);
    CHECK(below("0|1:50|2:2:2|0") == State::Allows);  // Self: HP < 50% -> Cure II
    CHECK(below("4|101:0|2:0:1|0") == State::Allows); // Tank: Tactician's choice -> Cure (best)
    CHECK(below("3|0:0|2:2:1|0") == State::Allows);   // The player -> Cure
    CHECK(below("2|101:0|2:0:1|0") == State::NotBelow); // a Cure on the mob is no cure of hers
    CHECK(below("10|0:0|2:2:1|0") == State::NotBelow);  // nor one on the dead

    // Her priced debuffs, on the mob, by id or by family
    CHECK(below("2|101:0|2:2:58|0") == State::Allows); // Target: Tactician's choice -> Paralyze
    CHECK(below("2|1:50|2:2:23|0") == State::Allows);  // Target: HP < 50% -> Dia
    CHECK(below("2|101:0|2:0:6|0") == State::Allows);  // Target: Tactician's choice -> Dia (best)
    CHECK(below("1|101:0|2:2:58|0") == State::NotBelow); // Paralyze on a party member

    // The melee of a fight a Foe target finds (decision 19)
    CHECK(below("102|0:0|0:0:0|0") == State::Allows);
    CHECK(below("100|0:0|0:0:0|0") == State::Allows);

    // Everything else is struck out there
    CHECK(below("1|0:0|2:2:43|0") == State::NotBelow);  // Protect
    CHECK(below("2|0:0|2:2:144|0") == State::NotBelow); // Fire
    CHECK(below("2|0:0|3:2:35|0") == State::NotBelow);  // an ability (Provoke)
    CHECK(below("2|0:0|4:2:1|0") == State::NotBelow);   // a weapon skill
    CHECK(below("2|0:0|1:0:0|0") == State::NotBelow);   // a ranged attack
    CHECK(below(kRest) == State::NotBelow);               // a behaviour row
    CHECK(below("1|0:0|2:0:1+2:2:58|0") == State::NotBelow); // two actions
}

TEST_CASE("tactician line: a timer or a chance below the line is struck out", "[cardian][gambits][tactician]")
{
    CHECK(below("1|39:30|2:0:1|0") == State::Clock);
    CHECK(below("2|22:50|2:2:58|0") == State::Clock);
    CHECK(cardian::tactician::struck(State::Clock));
    CHECK(cardian::tactician::struck(State::NotBelow));
    CHECK(cardian::tactician::struck(State::NoChoice));
    CHECK_FALSE(cardian::tactician::struck(State::Allows));
    CHECK_FALSE(cardian::tactician::struck(State::Line));
    CHECK_FALSE(cardian::tactician::struck(State::Order));
}

TEST_CASE("tactician line: moving her row swallows and releases rows", "[cardian][gambits][tactician]")
{
    // Her row moved up past Rest with the player: that row now sits below
    // her and is struck out; moved down past her Cure: the Cure is above her
    // and, left to a tactician that is not there, struck out
    CHECK(statesOf(rows({ kSupportMage, kRest, kCureBest })) == std::vector<State>{ State::Line, State::NotBelow, State::Allows });
    CHECK(statesOf(rows({ kRest, kCureBest, kSupportMage })) == std::vector<State>{ State::Order, State::NoChoice, State::Line });

    // Her row deleted: a gated Cure becomes a real order, Tactician's
    // choice is struck out
    CHECK(statesOf(rows({ kRest, "1|1:50|2:0:1|0", kCureBest })) == std::vector<State>{ State::Order, State::Order, State::NoChoice });
}

TEST_CASE("tactician line: which spells a row below the line lets her cast", "[cardian][gambits][tactician]")
{
    using cardian::tactician::allowsSpell;
    const auto best = row(kCureBest);
    for (const auto tier : cardian::tactician::kCureTiers)
    {
        CHECK(allowsSpell(best, tier));
    }
    CHECK_FALSE(allowsSpell(best, static_cast<uint16>(SpellID::Paralyze)));

    const auto cureIII = row("1|101:0|2:2:3|0");
    CHECK(allowsSpell(cureIII, static_cast<uint16>(SpellID::Cure_III)));
    CHECK_FALSE(allowsSpell(cureIII, static_cast<uint16>(SpellID::Cure_II)));

    const auto paralyze = row("2|101:0|2:2:58|0");
    CHECK(allowsSpell(paralyze, static_cast<uint16>(SpellID::Paralyze)));
    CHECK_FALSE(allowsSpell(paralyze, static_cast<uint16>(SpellID::Slow)));

    // A family names its priced spells alone: Dia's family is Dia, not Diaga
    const auto dia = row("2|101:0|2:0:6|0");
    CHECK(allowsSpell(dia, static_cast<uint16>(SpellID::Dia)));
    CHECK_FALSE(allowsSpell(dia, static_cast<uint16>(SpellID::Diaga)));

    // Nothing that does not fit below the line allows a spell
    CHECK_FALSE(allowsSpell(row("1|0:0|2:2:43|0"), static_cast<uint16>(SpellID::Protect)));
    CHECK_FALSE(allowsSpell(row("102|0:0|0:0:0|0"), static_cast<uint16>(SpellID::Cure)));
}

TEST_CASE("tactician line: her tactician's melee, and when it gives way to her rest", "[cardian][gambits][tactician]")
{
    using cardian::tactician::leavesToRest;
    using cardian::tactician::meleeAllowed;

    // She may melee while her tactician runs, her recovery is not due and
    // she is on her feet; standing again is being ready again
    CHECK(meleeAllowed(true, false, false));
    CHECK_FALSE(meleeAllowed(false, false, false));
    CHECK_FALSE(meleeAllowed(true, true, false));
    CHECK_FALSE(meleeAllowed(true, false, true));

    // She leaves a fight only her tactician's melee took, once her recovery
    // is due
    CHECK(leavesToRest(true, true, false, true, false));
    CHECK_FALSE(leavesToRest(true, false, false, true, false)); // recovery not due
    CHECK_FALSE(leavesToRest(true, true, true, true, false));   // an order above the line claims the mob
    CHECK_FALSE(leavesToRest(true, true, false, false, false)); // no row of hers took it (a pull, an answer)
    CHECK_FALSE(leavesToRest(true, true, false, true, true));   // the player ordered this fight himself
    CHECK_FALSE(leavesToRest(false, true, false, true, false)); // her tactician is not running
}

TEST_CASE("tactician line: the default sets mean what they say where they sit", "[cardian][gambits][tactician]")
{
    std::vector<std::string> mage;
    for (const auto& [spec, on] : pawn::defaultRowsFor(xi::Job::WHM))
    {
        mage.push_back(spec);
    }
    CHECK(statesOf(rows(mage)) == std::vector<State>{ State::Order, State::Line, State::Allows, State::Allows });

    std::vector<std::string> melee;
    for (const auto& [spec, on] : pawn::defaultRowsFor(xi::Job::WAR))
    {
        melee.push_back(spec);
    }
    const auto states = statesOf(rows(melee));
    CHECK_FALSE(lineOf(rows(melee)).has_value());
    for (const auto state : states)
    {
        CHECK(state == State::Order);
    }
}

TEST_CASE("tactician line: the editor's tokens", "[cardian][gambits][tactician]")
{
    using cardian::tactician::token;
    CHECK(token(State::Order) == "o");
    CHECK(token(State::Line) == "t");
    CHECK(token(State::Allows) == "a");
    CHECK(token(State::NotBelow) == "x-below");
    CHECK(token(State::Clock) == "x-clock");
    CHECK(token(State::NoChoice) == "x-choice");
}
