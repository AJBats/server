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

#include <algorithm>
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

    auto fullLineOf(const std::vector<Gambit_t>& list) -> std::optional<cardian::tactician::Line>
    {
        return cardian::tactician::lineOf(list, [](const Gambit_t& g) -> const Gambit_t& { return g; });
    }

    auto lineOf(const std::vector<Gambit_t>& list) -> std::optional<std::size_t>
    {
        const auto line = fullLineOf(list);
        return line.has_value() ? std::optional<std::size_t>(line->place) : std::nullopt;
    }

    auto statesOf(const std::vector<Gambit_t>& list) -> std::vector<State>
    {
        std::vector<State> out;
        const auto         line = fullLineOf(list);
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

    // The state of one row placed below a Tank row
    auto belowTank(const std::string& spec) -> State
    {
        return statesOf(rows({ "0|0:0|100:11:2|0", spec }))[1];
    }

    const std::string kSupportMage = "0|0:0|100:11:1|0";
    const std::string kTank        = "0|0:0|100:11:2|0";
    const std::string kRest        = "0|0:0|100:6:1|0";
    const std::string kCureBest    = "1|101:0|2:0:1|0";
    const std::string kProvoke     = "2|101:0|3:2:35|0"; // Foe: tactician's choice -> Provoke
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
        { SpellID::Burn, SPELLFAMILY_ELE_DOT }, { SpellID::Frost, SPELLFAMILY_ELE_DOT }, { SpellID::Choke, SPELLFAMILY_ELE_DOT },
        { SpellID::Rasp, SPELLFAMILY_ELE_DOT }, { SpellID::Shock, SPELLFAMILY_ELE_DOT }, { SpellID::Drown, SPELLFAMILY_ELE_DOT },
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

TEST_CASE("tactician line: a Tank row is a line as Support Mage's is, and its tactician reads melee and hate", "[cardian][gambits][tactician]")
{
    using cardian::tactician::Line;
    // The Tank row is a line, and the line knows whose tactician it is
    CHECK(fullLineOf(rows({ kRest, kTank, kProvoke })) == std::optional<Line>(Line{ 2, pawn::Role::Tank }));
    CHECK(fullLineOf(rows({ kRest, kSupportMage, kCureBest })) == std::optional<Line>(Line{ 2, pawn::Role::SupportMage }));
    // A Damage row is a role and no line
    CHECK_FALSE(lineOf(rows({ "100|0:0|0:0:0|0", kRest, "0|0:0|100:11:3|0" })).has_value());
    // The first line row wins, whichever role: one line a list
    CHECK(fullLineOf(rows({ kTank, kSupportMage })) == std::optional<Line>(Line{ 1, pawn::Role::Tank }));
    CHECK(statesOf(rows({ kTank, kSupportMage })) == std::vector<State>{ State::Line, State::NotBelow });

    // Below a Tank line: the pull and Provoke are her tactician's; a cure,
    // a debuff or a -na is nothing the tank's tactician reads
    CHECK(belowTank("101|0:0|0:0:0|0") == State::Allows); // Foe: targeted by ally -> Attack
    CHECK(belowTank(kProvoke) == State::Allows);
    CHECK(belowTank("2|0:0|3:2:35|0") == State::Allows); // Foe -> Provoke, no condition: still the tactician's
    CHECK(belowTank(kCureBest) == State::NotBelow);
    CHECK(belowTank("1|101:0|2:0:4|0") == State::NotBelow); // -na (best)
    CHECK(belowTank("2|101:0|2:100:0|0") == State::NotBelow); // Enfeeble
    CHECK(belowTank("2|101:0|3:2:16|0") == State::NotBelow); // Foe -> Mighty Strikes: no hate tool
    CHECK(belowTank("0|101:0|3:2:35|0") == State::NotBelow); // Self -> Provoke: a hate tool wants a foe
    // Below a Support Mage line Provoke is nothing hers reads
    CHECK(below(kProvoke) == State::NotBelow);

    // Which rows name the tool
    CHECK(cardian::tactician::allowsAbility(row(kProvoke), 35));
    CHECK_FALSE(cardian::tactician::allowsAbility(row(kProvoke), 16));
    CHECK_FALSE(cardian::tactician::allowsAbility(row(kCureBest), 35));
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
    CHECK(below("4|101:0|2:0:1|0") == State::Allows); // Ally: tank, tactician's choice -> Cure (best)
    CHECK(below("3|0:0|2:2:1|0") == State::Allows);   // The player -> Cure
    CHECK(below("2|101:0|2:0:1|0") == State::NotBelow); // a Cure on the mob is no cure of hers
    CHECK(below("10|0:0|2:2:1|0") == State::NotBelow);  // nor one on the dead

    // Her priced debuffs, on the mob, by id or by family
    CHECK(below("2|101:0|2:2:58|0") == State::Allows); // Foe: tactician's choice -> Paralyze
    CHECK(below("2|1:50|2:2:23|0") == State::Allows);  // Foe: HP < 50% -> Dia
    CHECK(below("2|101:0|2:0:6|0") == State::Allows);  // Foe: tactician's choice -> Dia (best)
    CHECK(below("1|101:0|2:2:58|0") == State::NotBelow); // Paralyze on a party member

    // Any Foe row's debuff, on her fight when it is of the row's kind
    CHECK(below("100|0:0|2:2:58|0") == State::Allows); // Foe: party leader's target -> Paralyze

    // The melee of a fight a Foe row finds (decision 19)
    CHECK(below("102|0:0|0:0:0|0") == State::Allows);
    CHECK(below("100|0:0|0:0:0|0") == State::Allows);
    CHECK(below("2|2:75|0:0:0|0") == State::Allows); // Foe: HP >= 75% -> Attack

    // Enfeeble, every priced debuff at once, on the mob
    CHECK(below("2|101:0|2:100:0|0") == State::Allows);   // Foe: tactician's choice -> Enfeeble
    CHECK(below("100|0:0|2:100:0|0") == State::Allows);   // Foe: party leader's target -> Enfeeble
    CHECK(below("1|101:0|2:100:0|0") == State::NotBelow); // Enfeeble on a party member

    // Her -na and Erase, for someone on the party's side
    CHECK(below("1|101:0|2:0:4|0") == State::Allows);     // Ally: tactician's choice -> -na (best)
    CHECK(below("1|9:10000|2:0:4|0") == State::Allows);   // Ally: status = Enfeeble -> -na (best)
    CHECK(below("1|9:3|2:2:14|0") == State::Allows);      // Ally: status = Poison -> Poisona
    CHECK(below("0|0:0|2:2:143|0") == State::Allows);     // Self -> Erase
    CHECK(below("2|0:0|2:0:4|0") == State::NotBelow);     // -na (best) on the mob
    CHECK(below("1|0:0|2:2:95|0") == State::NotBelow);    // Esuna, a -na family spell on herself alone
    CHECK(below("1|39:30|2:0:4|0") == State::Clock);      // on a timer

    // Everything else is struck out there
    CHECK(below("1|0:0|2:2:43|0") == State::NotBelow);  // Protect
    CHECK(below("2|0:0|2:2:144|0") == State::NotBelow); // Fire
    CHECK(below("2|0:0|3:2:35|0") == State::NotBelow);  // an ability (Provoke)
    CHECK(below("2|0:0|4:2:1|0") == State::NotBelow);   // a weapon skill
    CHECK(below("2|0:0|1:0:0|0") == State::NotBelow);   // a ranged attack
    CHECK(below(kRest) == State::NotBelow);               // a behaviour row
    CHECK(below("1|0:0|2:0:1+2:2:58|0") == State::NotBelow); // two actions
}

TEST_CASE("tactician line: an action aimed at the wrong side is a misfit, struck out wherever it sits", "[cardian][gambits][tactician]")
{
    using cardian::tactician::fitsSide;
    using cardian::tactician::kTargetEnemy;
    constexpr uint16 self = 0x0001, party = 0x0002;

    // A Foe condition wants an action for an enemy; Self and Ally one for her side
    CHECK(fitsSide(gambits::G_TARGET::TARGET, kTargetEnemy));
    CHECK(fitsSide(pawn::G_TARGET_LEADERS_TARGET, kTargetEnemy));
    CHECK_FALSE(fitsSide(gambits::G_TARGET::TARGET, self | party));      // Foe: any -> Protect (Cure also carries the enemy flag, for the undead, so it fits)
    CHECK_FALSE(fitsSide(gambits::G_TARGET::PARTY, kTargetEnemy));       // Ally: status = Sleep -> a weapon skill
    CHECK_FALSE(fitsSide(gambits::G_TARGET::SELF, kTargetEnemy));        // Self -> Attack
    CHECK(fitsSide(gambits::G_TARGET::PARTY, self | party));             // Ally: HP < 50% -> Cure
    CHECK(fitsSide(gambits::G_TARGET::SELF, self));                      // Self -> Stoneskin
    CHECK(fitsSide(gambits::G_TARGET::PARTY, 0));                        // a behaviour: nothing to judge
    CHECK(fitsSide(gambits::G_TARGET::TRIGGER_SELF_ACTION_TARGET, kTargetEnemy)); // reads her, acts on the foe
    CHECK(fitsSide(gambits::G_TARGET::TRIGGER_TARGET_ACTION_SELF, self));         // reads the foe, acts on her

    // A weapon skill or an enemy ability lands on her fight whatever the
    // row names: under Self that is the row's meaning, under Ally a misfit
    CHECK(fitsSide(gambits::G_TARGET::SELF, kTargetEnemy, true));        // Self: TP >= 1000 -> a weapon skill
    CHECK_FALSE(fitsSide(gambits::G_TARGET::PARTY, kTargetEnemy, true)); // Ally: status = Sleep -> a weapon skill
    CHECK_FALSE(fitsSide(gambits::G_TARGET::SELF, kTargetEnemy));        // Self -> Dia: a spell goes where the row names

    // A misfit is struck out above the line, below it, and as no line at all
    const auto g = row("1|9:2|0:0:0|0");
    CHECK(cardian::tactician::stateOf(g, 1, std::nullopt, false) == State::Misfit);
    CHECK(cardian::tactician::stateOf(g, 3, cardian::tactician::Line{ 1, pawn::Role::SupportMage }, false) == State::Misfit);
    CHECK(cardian::tactician::struck(State::Misfit));
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

    // Enfeeble lets her tactician cast the single-target debuffs it prices,
    // and nothing else: never a -ga spell, which reaches the mobs around
    const auto enfeeble = row("2|101:0|2:100:0|0");
    for (const auto id : cardian::tactician::kEnfeebleOrder)
    {
        CHECK(allowsSpell(enfeeble, id));
    }
    CHECK_FALSE(allowsSpell(enfeeble, static_cast<uint16>(SpellID::Diaga)));
    CHECK_FALSE(allowsSpell(enfeeble, static_cast<uint16>(SpellID::Poisonga)));
    CHECK_FALSE(allowsSpell(enfeeble, static_cast<uint16>(SpellID::Cure)));
    CHECK_FALSE(allowsSpell(enfeeble, static_cast<uint16>(SpellID::Sleep)));
    CHECK_FALSE(allowsSpell(enfeeble, static_cast<uint16>(SpellID::Fire)));

    // A -na row is no spell of her tactician's: it runs as written (actsAlone)
    CHECK_FALSE(allowsSpell(row("1|101:0|2:0:4|0"), static_cast<uint16>(SpellID::Poisona)));

    // Nothing that does not fit below the line allows a spell
    CHECK_FALSE(allowsSpell(row("1|0:0|2:2:43|0"), static_cast<uint16>(SpellID::Protect)));
    CHECK_FALSE(allowsSpell(row("102|0:0|0:0:0|0"), static_cast<uint16>(SpellID::Cure)));
}

TEST_CASE("tactician line: an order acts alone, and below the line only a -na or Erase row does", "[cardian][gambits][tactician]")
{
    using cardian::tactician::actsAlone;
    const auto na = row("1|101:0|2:0:4|0");
    CHECK(actsAlone(State::Order, row(kRest), true));
    CHECK(actsAlone(State::Order, na, true));
    CHECK(actsAlone(State::Allows, na, true));
    CHECK(actsAlone(State::Allows, row("1|9:3|2:2:14|0"), true));  // Ally: status = Poison -> Poisona
    CHECK(actsAlone(State::Allows, row("0|0:0|2:2:143|0"), true)); // Self -> Erase

    // Her cures, her enfeebles and her melee wait for her tactician
    CHECK_FALSE(actsAlone(State::Allows, row(kCureBest), true));
    CHECK_FALSE(actsAlone(State::Allows, row("2|101:0|2:100:0|0"), true));
    CHECK_FALSE(actsAlone(State::Allows, row("102|0:0|0:0:0|0"), true));

    // With her tactician not running (her Support Mage row unchecked, her
    // gambits off), a -na row below the line waits as her Cure rows do; an
    // order still acts
    CHECK_FALSE(actsAlone(State::Allows, na, false));
    CHECK_FALSE(actsAlone(State::Allows, row("0|0:0|2:2:143|0"), false));
    CHECK(actsAlone(State::Order, na, false));

    // Struck out, or the line itself, never
    CHECK_FALSE(actsAlone(State::Clock, na, true));
    CHECK_FALSE(actsAlone(State::NoChoice, na, true));
    CHECK_FALSE(actsAlone(State::Misfit, na, true));
    CHECK_FALSE(actsAlone(State::Line, row(kSupportMage), true));
}

TEST_CASE("tactician line: an Enfeeble order casts the first single-target enfeeble she can that the foe lacks", "[cardian][gambits][tactician]")
{
    using cardian::tactician::firstEnfeeble;
    using cardian::tactician::kEnfeebleOrder;
    const std::vector<SpellID> want{ SpellID::Paralyze, SpellID::Slow, SpellID::Blind, SpellID::Dia, SpellID::Bio, SpellID::Poison,
                                     SpellID::Burn, SpellID::Frost, SpellID::Choke, SpellID::Rasp, SpellID::Shock, SpellID::Drown };
    REQUIRE(want.size() == kEnfeebleOrder.size());
    for (std::size_t i = 0; i < want.size(); ++i)
    {
        CHECK(kEnfeebleOrder[i] == static_cast<uint16>(want[i]));
    }

    // No -ga: as an order it would reach every mob around the foe
    CHECK(std::ranges::find(kEnfeebleOrder, static_cast<uint16>(SpellID::Diaga)) == kEnfeebleOrder.end());
    CHECK(std::ranges::find(kEnfeebleOrder, static_cast<uint16>(SpellID::Poisonga)) == kEnfeebleOrder.end());

    const auto only = [](std::vector<SpellID> castable)
    {
        return [castable = std::move(castable)](const uint16 spell)
        {
            return std::ranges::find(castable, static_cast<SpellID>(spell)) != castable.end();
        };
    };
    CHECK(firstEnfeeble(only({ SpellID::Dia, SpellID::Paralyze })) == std::optional<uint16>(static_cast<uint16>(SpellID::Paralyze)));
    CHECK(firstEnfeeble(only({ SpellID::Poison, SpellID::Dia })) == std::optional<uint16>(static_cast<uint16>(SpellID::Dia)));
    CHECK(firstEnfeeble(only({ SpellID::Shock, SpellID::Burn })) == std::optional<uint16>(static_cast<uint16>(SpellID::Burn)));
    CHECK_FALSE(firstEnfeeble(only({ SpellID::Diaga })).has_value());
    CHECK_FALSE(firstEnfeeble(only({})).has_value());
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
    CHECK(statesOf(rows(mage)) == std::vector<State>{ State::Order, State::Order, State::Line, State::Allows, State::Allows, State::Allows, State::Allows });

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

TEST_CASE("tactician line: a played character's list has no line, and his behaviour rows are struck out", "[cardian][gambits][tactician]")
{
    using cardian::tactician::ownClientStateOf;
    // What his hands do is an order wherever it sits
    CHECK(ownClientStateOf(row("0|1:50|2:2:2|0")) == State::Order);  // Self: HP < 50% -> Cure II
    CHECK(ownClientStateOf(row("2|0:0|4:2:1|0")) == State::Order);   // Foe -> a weapon skill
    CHECK(ownClientStateOf(row("101|0:0|0:0:0|0")) == State::Order); // Foe: targeted by ally -> Attack

    // A behaviour row -- a line row, a role, Rest with the player -- moves a
    // cardian or speaks to her tactician: struck out in his list
    CHECK(ownClientStateOf(row(kSupportMage)) == State::Client);
    CHECK(ownClientStateOf(row(kTank)) == State::Client);
    CHECK(ownClientStateOf(row(kRest)) == State::Client);
    CHECK(cardian::tactician::struck(State::Client));

    // Tactician's choice has nobody to choose; a misfit is one in his list too
    CHECK(ownClientStateOf(row(kCureBest)) == State::NoChoice);
    CHECK(ownClientStateOf(row("0|0:0|2:2:2|0"), false) == State::Misfit);
}

