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

// The tactician's mark (RESEARCH §17.13; before it the tactician line,
// ROADMAP K, RESEARCH §14.12 decisions 17-20): which rows are the
// tactician's and what each means, wherever it sits; which spells a marked
// row lets her tactician cast; and when her tactician's melee gives way to
// her rest.

#include <catch2/catch_test_macros.hpp>

#include "map/pawn/gambit_defaults.h"
#include "map/pawn/gambit_text.h"
#include "map/pawn/tactician_line.h"
#include "map/spell.h"

#include <algorithm>
#include <cstddef>
#include <fstream>
#include <iterator>
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

    auto statesOf(const std::vector<Gambit_t>& list) -> std::vector<State>
    {
        std::vector<State> out;
        for (const auto& g : list)
        {
            out.push_back(cardian::tactician::stateOf(g));
        }
        return out;
    }

    auto stateOf(const std::string& spec) -> State
    {
        return cardian::tactician::stateOf(row(spec));
    }

    // The spec with the tactician's mark added to its conditions: the row
    // as the editor's Alt menu makes it, its own condition kept as the gate
    auto marked(const std::string& spec) -> std::string
    {
        const auto first  = spec.find('|');
        const auto second = spec.find('|', first + 1);
        return spec.substr(0, second) + "&101:0" + spec.substr(second);
    }

    const std::string kRest     = "0|0:0|100:6:1|0";
    const std::string kCureBest = "1|101:0|2:0:1|0"; // * Ally -> Cure (best)
    const std::string kProvoke  = "2|101:0|3:2:35|0"; // * Foe -> Provoke
    const std::string kPull     = "101|101:0|0:0:0|0"; // * Foe: targeted by ally -> Attack
} // namespace

TEST_CASE("tactician: her lists are spell.h's numbers", "[cardian][gambits][tactician]")
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

TEST_CASE("tactician: a row carrying the mark is the tactician's wherever it sits, and every other row is an order", "[cardian][gambits][tactician]")
{
    using cardian::tactician::isMarked;
    CHECK(isMarked(row(kCureBest)));
    CHECK(isMarked(row(marked("1|3:45|2:0:1|0"))));
    CHECK_FALSE(isMarked(row("1|3:45|2:0:1|0")));
    CHECK_FALSE(isMarked(row(kRest)));

    // Position means nothing: the same rows in any order mean the same
    CHECK(statesOf(rows({ "100|0:0|0:0:0|0", kRest, kCureBest })) == std::vector<State>{ State::Order, State::Order, State::Tool });
    CHECK(statesOf(rows({ kCureBest, kRest, "100|0:0|0:0:0|0" })) == std::vector<State>{ State::Tool, State::Order, State::Order });

    // A gated Cure is an order wherever it sits; marked, the gate stays and
    // the tactician decides the when
    CHECK(stateOf("1|1:50|2:0:1|0") == State::Order);
    CHECK(stateOf(marked("1|1:50|2:0:1|0")) == State::Tool);
    CHECK(cardian::tactician::carries(row(marked("1|1:50|2:0:1|0")), G_CONDITION::HPP_LT));

    // The Role row, the line's, is retired with it: the grammar refuses it
    // whatever it says (Support Mage 1, Tank 2, Damage 3), so no list has
    // a line any more
    for (const auto value : { 1, 2, 3 })
    {
        CHECK_FALSE(parseRow("0|0:0|100:11:" + std::to_string(value) + "|0").has_value());
    }
    CHECK(pawn::isRetiredBehavior(11));
}

TEST_CASE("tactician: the tank's tools are the marked pull and the marked hate tool", "[cardian][gambits][tactician]")
{
    CHECK(stateOf(kPull) == State::Tool);     // the fight her tactician picks (RESEARCH §17.11)
    CHECK(stateOf(kProvoke) == State::Tool);
    CHECK(stateOf("2|0:0|3:2:35|0") == State::Order);           // Foe -> Provoke, unmarked: an order, hers to fire
    CHECK(stateOf("0|101:0|3:2:35|0") == State::NoJudgement);   // * Self -> Provoke: a hate tool wants a foe
    CHECK(stateOf("2|101:0|3:2:16|0") == State::NoJudgement);   // * Foe -> Mighty Strikes: no judgement for it yet

    // Which rows name the tool: a marked one that does
    CHECK(cardian::tactician::allowsAbility(row(kProvoke), 35));
    CHECK_FALSE(cardian::tactician::allowsAbility(row(kProvoke), 16));
    CHECK_FALSE(cardian::tactician::allowsAbility(row(kCureBest), 35));
    CHECK_FALSE(cardian::tactician::allowsAbility(row("2|0:0|3:2:35|0"), 35)); // unmarked: the order's, not the tactician's
}

TEST_CASE("tactician: a marked row is a tool when it names her cures, her priced debuffs, her melee or her -na, and struck out otherwise", "[cardian][gambits][tactician]")
{
    // Her cures, for someone on the party's side
    CHECK(stateOf(kCureBest) == State::Tool);
    CHECK(stateOf(marked("0|1:50|2:2:2|0")) == State::Tool); // * Self: HP < 50% -> Cure II
    CHECK(stateOf("4|101:0|2:0:1|0") == State::Tool);        // * Ally: tank -> Cure (best)
    CHECK(stateOf(marked("3|0:0|2:2:1|0")) == State::Tool);  // * The player -> Cure
    CHECK(stateOf("2|101:0|2:0:1|0") == State::NoJudgement); // a Cure on the mob is no cure of hers
    CHECK(stateOf(marked("10|0:0|2:2:1|0")) == State::NoJudgement); // nor one on the dead

    // Her priced debuffs, on the mob, by id or by family
    CHECK(stateOf("2|101:0|2:2:58|0") == State::Tool);        // * Foe -> Paralyze
    CHECK(stateOf(marked("2|1:50|2:2:23|0")) == State::Tool); // * Foe: HP < 50% -> Dia
    CHECK(stateOf("2|101:0|2:0:6|0") == State::Tool);         // * Foe -> Dia (best)
    CHECK(stateOf("1|101:0|2:2:58|0") == State::NoJudgement); // Paralyze on a party member

    // Any Foe row's debuff, on her fight when it is of the row's kind
    CHECK(stateOf(marked("100|0:0|2:2:58|0")) == State::Tool); // * Foe: party leader's target -> Paralyze

    // The melee of a fight a Foe row finds (decision 19)
    CHECK(stateOf(marked("102|0:0|0:0:0|0")) == State::Tool);
    CHECK(stateOf(marked("100|0:0|0:0:0|0")) == State::Tool);
    CHECK(stateOf(marked("2|2:75|0:0:0|0")) == State::Tool); // * Foe: HP >= 75% -> Attack

    // Enfeeble, every priced debuff at once, on the mob
    CHECK(stateOf("2|101:0|2:100:0|0") == State::Tool);          // * Foe -> Enfeeble
    CHECK(stateOf(marked("100|0:0|2:100:0|0")) == State::Tool);  // * Foe: party leader's target -> Enfeeble
    CHECK(stateOf("1|101:0|2:100:0|0") == State::NoJudgement);   // Enfeeble on a party member

    // Her -na and Erase, for someone on the party's side
    CHECK(stateOf("1|101:0|2:0:4|0") == State::Tool);            // * Ally -> -na (best)
    CHECK(stateOf(marked("1|9:10000|2:0:4|0")) == State::Tool);  // * Ally: status = Enfeeble -> -na (best)
    CHECK(stateOf(marked("1|9:3|2:2:14|0")) == State::Tool);     // * Ally: status = Poison -> Poisona
    CHECK(stateOf(marked("0|0:0|2:2:143|0")) == State::Tool);    // * Self -> Erase
    CHECK(stateOf(marked("2|0:0|2:0:4|0")) == State::NoJudgement);  // -na (best) on the mob
    CHECK(stateOf(marked("1|0:0|2:2:95|0")) == State::NoJudgement); // Esuna, a -na family spell on herself alone
    CHECK(stateOf(marked("1|39:30|2:0:4|0")) == State::Clock);      // on a timer

    // Her rest: the MP pacing's handle, a Self -> Rest row (RESEARCH §17.13)
    CHECK(stateOf("0|101:0|100:14:1|0") == State::Tool);         // * Self -> Rest
    CHECK(stateOf("0|0:0|100:14:1|0") == State::Order);          // Self -> Rest: an order, her own rest until full when it holds
    CHECK(stateOf("0|3:30&101:0|100:14:1|0") == State::Tool);    // * Self: MP < 30% -> Rest: the pacing, gated
    CHECK(stateOf("1|101:0|100:14:1|0") == State::NoJudgement);  // Ally -> Rest: nobody rests another
    CHECK(cardian::tactician::allowanceOf(row("0|101:0|100:14:1|0")) == cardian::tactician::Allowance::Rest);
    CHECK_FALSE(cardian::tactician::isSpellTool(cardian::tactician::Allowance::Rest));

    // Everything else the tactician has no judgement for: struck out
    CHECK(stateOf(marked("1|0:0|2:2:43|0")) == State::NoJudgement);  // Protect
    CHECK(stateOf(marked("2|0:0|2:2:144|0")) == State::NoJudgement); // Fire
    CHECK(stateOf(marked("2|0:0|4:2:1|0")) == State::NoJudgement);   // a weapon skill
    CHECK(stateOf(marked("2|0:0|1:0:0|0")) == State::NoJudgement);   // a ranged attack
    CHECK(stateOf(marked(kRest)) == State::NoJudgement);             // a behaviour row takes no mark
    CHECK(stateOf(marked("1|0:0|2:0:1+2:2:58|0")) == State::NoJudgement); // two actions

    // Unmarked, every one of them is an order
    for (const auto* spec : { "1|0:0|2:2:43|0", "2|0:0|2:2:144|0", "2|0:0|4:2:1|0", "2|1:50|2:2:23|0", "1|9:3|2:2:14|0", "102|0:0|0:0:0|0" })
    {
        INFO("row " << spec);
        CHECK(stateOf(spec) == State::Order);
    }
    CHECK(stateOf(kRest) == State::Order);
}

TEST_CASE("tactician: an action aimed at the wrong side is a misfit, struck out marked or not", "[cardian][gambits][tactician]")
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

    // A misfit is struck out, an order or a marked row alike
    CHECK(cardian::tactician::stateOf(row("1|9:2|0:0:0|0"), false) == State::Misfit);
    CHECK(cardian::tactician::stateOf(row(marked("1|9:2|0:0:0|0")), false) == State::Misfit);
    CHECK(cardian::tactician::struck(State::Misfit));
}

TEST_CASE("tactician: a marked row on a timer or a chance is struck out", "[cardian][gambits][tactician]")
{
    CHECK(stateOf(marked("1|39:30|2:0:1|0")) == State::Clock);
    CHECK(stateOf(marked("2|22:50|2:2:58|0")) == State::Clock);
    // as an order a clock is what it always was
    CHECK(stateOf("1|39:30|2:0:1|0") == State::Order);
    CHECK(cardian::tactician::struck(State::Clock));
    CHECK(cardian::tactician::struck(State::NoJudgement));
    CHECK_FALSE(cardian::tactician::struck(State::Tool));
    CHECK_FALSE(cardian::tactician::struck(State::Order));
}

TEST_CASE("tactician: which spells a marked row lets her cast", "[cardian][gambits][tactician]")
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

    // An unmarked Cure is an order's, not the tactician's; nothing the
    // tactician has no judgement for allows a spell
    CHECK_FALSE(allowsSpell(row("1|1:50|2:0:1|0"), static_cast<uint16>(SpellID::Cure)));
    CHECK_FALSE(allowsSpell(row(marked("1|0:0|2:2:43|0")), static_cast<uint16>(SpellID::Protect)));
    CHECK_FALSE(allowsSpell(row(marked("102|0:0|0:0:0|0")), static_cast<uint16>(SpellID::Cure)));
}

TEST_CASE("tactician: an order acts alone, and of the marked rows only a -na or Erase does", "[cardian][gambits][tactician]")
{
    using cardian::tactician::actsAlone;
    const auto na = row("1|101:0|2:0:4|0");
    CHECK(actsAlone(State::Order, row(kRest), true));
    CHECK(actsAlone(State::Order, row("1|9:3|2:2:14|0"), true));
    CHECK(actsAlone(State::Tool, na, true));
    CHECK(actsAlone(State::Tool, row(marked("1|9:3|2:2:14|0")), true));  // * Ally: status = Poison -> Poisona
    CHECK(actsAlone(State::Tool, row(marked("0|0:0|2:2:143|0")), true)); // * Self -> Erase

    // Her cures, her enfeebles and her melee wait for her tactician
    CHECK_FALSE(actsAlone(State::Tool, row(kCureBest), true));
    CHECK_FALSE(actsAlone(State::Tool, row("2|101:0|2:100:0|0"), true));
    CHECK_FALSE(actsAlone(State::Tool, row(kPull), true));

    // With her tactician not running (her gambits off), a marked -na row
    // waits as her marked Cure rows do; an order still acts
    CHECK_FALSE(actsAlone(State::Tool, na, false));
    CHECK_FALSE(actsAlone(State::Tool, row(marked("0|0:0|2:2:143|0")), false));
    CHECK(actsAlone(State::Order, row("1|9:3|2:2:14|0"), false));

    // Struck out, never
    CHECK_FALSE(actsAlone(State::Clock, na, true));
    CHECK_FALSE(actsAlone(State::NoJudgement, na, true));
    CHECK_FALSE(actsAlone(State::Misfit, na, true));
}

TEST_CASE("tactician: an Enfeeble order casts the first single-target enfeeble she can that the foe lacks", "[cardian][gambits][tactician]")
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

TEST_CASE("tactician: her tactician's melee, and when it gives way to her rest", "[cardian][gambits][tactician]")
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
    CHECK_FALSE(leavesToRest(true, true, true, true, false));   // an order claims the mob
    CHECK_FALSE(leavesToRest(true, true, false, false, false)); // no row of hers took it (a pull, an answer)
    CHECK_FALSE(leavesToRest(true, true, false, true, true));   // the player ordered this fight himself
    CHECK_FALSE(leavesToRest(false, true, false, true, false)); // her tactician is not running
}

TEST_CASE("tactician: the default sets mean what they say", "[cardian][gambits][tactician]")
{
    std::vector<std::string> mage;
    for (const auto& [spec, on] : pawn::defaultRowsFor(xi::Job::WHM))
    {
        mage.push_back(spec);
    }
    // the tactician's tools first (her cures, her rest), then her orders,
    // and her marked Attack row (off: the melee mage's switch) last
    CHECK(statesOf(rows(mage)) == std::vector<State>{ State::Tool, State::Tool, State::Tool, State::Tool, State::Order, State::Order, State::Tool });

    std::vector<std::string> melee;
    for (const auto& [spec, on] : pawn::defaultRowsFor(xi::Job::PLD))
    {
        melee.push_back(spec);
    }
    for (const auto state : statesOf(rows(melee)))
    {
        CHECK(state == State::Order);
    }

    // a Monk's and a Warrior's: the trio, her three tools, her weapon
    // skill and rest with the player
    for (const auto job : { xi::Job::MNK, xi::Job::WAR })
    {
        std::vector<std::string> specs;
        for (const auto& [spec, on] : pawn::defaultRowsFor(job))
        {
            specs.push_back(spec);
        }
        CHECK(statesOf(rows(specs)) == std::vector<State>{ State::Order, State::Order, State::Order, State::Tool, State::Tool, State::Tool, State::Order, State::Order });
    }
}

TEST_CASE("tactician: the self buffs, Boost and Sneak Attack are tools on a Self row, each with its own when", "[cardian][gambits][tactician]")
{
    using cardian::tactician::Allowance;
    using cardian::tactician::allowanceOf;
    using cardian::tactician::buffNow;
    using cardian::tactician::actsAlone;
    namespace t = cardian::tactician;

    // On a Self row each is a tool; on a Foe row none has a judgement;
    // unmarked, each is an order
    for (const auto ability : { t::kBerserk, t::kDefender, t::kAggressor, t::kFocus, t::kDodge })
    {
        INFO("ability " << ability);
        const auto self = "0|101:0|3:2:" + std::to_string(ability) + "|0";
        CHECK(stateOf(self) == State::Tool);
        CHECK(allowanceOf(row(self)) == Allowance::Buff);
        CHECK(stateOf("2|101:0|3:2:" + std::to_string(ability) + "|0") == State::NoJudgement);
        CHECK(stateOf("0|0:0|3:2:" + std::to_string(ability) + "|0") == State::Order);
        // a buff never acts alone: its when does (CGambits::BuffNow)
        CHECK_FALSE(actsAlone(State::Tool, row(self), true));
    }
    CHECK(stateOf("0|101:0|3:2:39|0") == State::Tool); // * Self -> Boost
    CHECK(allowanceOf(row("0|101:0|3:2:39|0")) == Allowance::Boost);
    CHECK(stateOf("2|101:0|3:2:39|0") == State::NoJudgement);
    CHECK(stateOf("0|101:0|3:2:44|0") == State::Tool); // * Self -> Sneak Attack
    CHECK(allowanceOf(row("0|101:0|3:2:44|0")) == Allowance::SneakAttack);
    CHECK(stateOf("2|101:0|3:2:44|0") == State::NoJudgement);
    CHECK(stateOf("0|0:0|3:2:44|0") == State::Order); // plain: Sneak Attack whenever it holds, a plain hit's
    CHECK_FALSE(actsAlone(State::Tool, row("0|101:0|3:2:44|0"), true)); // it waits for her weapon skill
    CHECK(stateOf(marked("0|3:30|3:2:31|0")) == State::Tool); // * Self: MP < 30% -> Berserk: gated
    CHECK_FALSE(t::isBuffAbility(t::kBoost));                // Boost waits for the weapon skill, not its clock
    CHECK_FALSE(t::isBuffAbility(t::kSneakAttack));          // so does Sneak Attack
    CHECK_FALSE(t::isBuffAbility(35));                       // Provoke is the tank's hate tool

    // The when: fighting, not on her already, the seat allowing
    for (const auto ability : { t::kAggressor, t::kFocus, t::kDodge })
    {
        INFO("ability " << ability);
        CHECK(buffNow(ability, true, false, false));
        CHECK(buffNow(ability, true, false, true)); // any seat
        CHECK_FALSE(buffNow(ability, false, false, false)); // not fighting
        CHECK_FALSE(buffNow(ability, true, true, false));   // already up
    }
    CHECK(buffNow(t::kBerserk, true, false, false));
    CHECK_FALSE(buffNow(t::kBerserk, true, false, true));  // never seated Tank
    CHECK(buffNow(t::kDefender, true, false, true));
    CHECK_FALSE(buffNow(t::kDefender, true, false, false)); // only seated Tank
    CHECK_FALSE(buffNow(t::kBoost, true, false, false));    // Boost is no clock buff
    CHECK_FALSE(buffNow(35, true, false, true));            // nor Provoke

    // Berserk and Defender are a stance: seated Tank, Berserk is the one to
    // take off; any other seat, Defender (both up, their numbers cancel)
    CHECK(t::isStanceAbility(t::kBerserk));
    CHECK(t::isStanceAbility(t::kDefender));
    CHECK_FALSE(t::isStanceAbility(t::kAggressor));
    CHECK_FALSE(t::isStanceAbility(t::kBoost));
    CHECK(t::wrongStance(true) == t::kBerserk);
    CHECK(t::wrongStance(false) == t::kDefender);
    CHECK(t::otherStance(t::kBerserk) == t::kDefender);
    CHECK(t::otherStance(t::kDefender) == t::kBerserk);

    // The player's own stance is his for the one use it put up: the effect
    // that went up as it landed, never one older than the order, nor a
    // later one of the tactician's
    using namespace std::chrono_literals;
    CHECK(t::isOrderedUse(0ms));
    CHECK(t::isOrderedUse(800ms)); // landed two ticks on
    CHECK(t::isOrderedUse(5s));
    CHECK_FALSE(t::isOrderedUse(-1ms));   // up before the order: not his
    CHECK_FALSE(t::isOrderedUse(5001ms)); // up after his use: the tactician's
    CHECK_FALSE(t::isOrderedUse(std::chrono::minutes(5)));
}

TEST_CASE("tactician: a row's weapon skill waits while its mob runs, not while it steps", "[cardian][gambits][tactician]")
{
    namespace t = cardian::tactician;
    CHECK_FALSE(t::mobRunning(false, 0.0f));
    CHECK_FALSE(t::mobRunning(false, 30.0f)); // a path ended, wherever it was going
    CHECK_FALSE(t::mobRunning(true, 1.5f));   // a step to turn or settle
    CHECK_FALSE(t::mobRunning(true, t::kMobRunningYalms));
    CHECK(t::mobRunning(true, t::kMobRunningYalms + 0.1f));
    CHECK(t::mobRunning(true, 20.0f)); // chasing, or running off
}

TEST_CASE("tactician: Damage spell (any) marked is her nukes, a tool on a Foe row, her tactician's when and which; an order unmarked", "[cardian][gambits][tactician]")
{
    using cardian::tactician::Allowance;
    using cardian::tactician::allowanceOf;
    using cardian::tactician::actsAlone;
    namespace t = cardian::tactician;

    const std::string nuke = "2|101:0|2:3:0|0"; // * Foe -> Damage spell (any)
    CHECK(stateOf(nuke) == State::Tool);
    CHECK(allowanceOf(row(nuke)) == Allowance::Nuke);
    // a spell tool: she attends the party's fights at cure range for it
    CHECK(t::isSpellTool(Allowance::Nuke));
    // it never acts alone: her tactician's judgement does (CGambits::CastNuke)
    CHECK_FALSE(actsAlone(State::Tool, row(nuke), true));
    // gated, it is still hers to judge; on a Self or Ally row it has nothing to aim at
    CHECK(stateOf(marked("2|2:50|2:3:0|0")) == State::Tool);
    CHECK(stateOf("0|101:0|2:3:0|0") == State::NoJudgement);
    CHECK(stateOf("1|101:0|2:3:0|0") == State::NoJudgement);
    // unmarked, an order: a random one of her damage spells whenever its condition holds
    CHECK(stateOf("2|0:0|2:3:0|0") == State::Order);
    // it lends no spell to the conveyor's admission (Cures and Debuffs do)
    CHECK_FALSE(t::allowsSpell(row(nuke), 144)); // Fire
}

TEST_CASE("tactician: a weapon skill takes Sneak Attack when its own script goes through the server's physical path", "[cardian][gambits][tactician]")
{
    // The real scripts, read as CPawnController::TakesSneakAttack reads
    // them (xi_test runs from the server's root)
    const auto takes = [](const std::string& name)
    {
        std::ifstream file("./scripts/actions/weaponskills/" + name + ".lua");
        INFO("weapon skill " << name);
        REQUIRE(file.is_open());
        const std::string source((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
        return cardian::tactician::scriptTakesSneakAttack(source);
    };

    // The physical dagger skills a Thief reaches
    for (const auto* name : { "wasp_sting", "viper_bite", "shadowstitch", "dancing_edge", "shark_bite", "evisceration" })
    {
        CHECK(takes(name));
    }
    // Magical (Gust Slash, Cyclone, Aeolian Edge), ranged (Sidewinder), and
    // scripts of their own (Energy Steal, Energy Drain, Spirits Within)
    for (const auto* name : { "gust_slash", "cyclone", "aeolian_edge", "sidewinder", "energy_steal", "energy_drain", "spirits_within" })
    {
        CHECK_FALSE(takes(name));
    }
}

TEST_CASE("tactician: a weapon skill row takes Sneak Attack only when it would fire and its skill would take it", "[cardian][gambits][tactician]")
{
    using cardian::tactician::wsRowTakesSneak;
    // Wasp Sting (16) is hers and takes it; Gust Slash (19) is hers and
    // does not; Viper Bite (17) would, but is not hers
    const auto takesIfHers = [](const uint16 wsid)
    {
        return wsid == 16;
    };

    // Weapon skill (best), an order: while one of hers takes it
    const auto best = row("2|2:50|4:0:0|0");
    CHECK(wsRowTakesSneak(best, State::Order, true, takesIfHers));
    CHECK_FALSE(wsRowTakesSneak(best, State::Order, false, takesIfHers));

    // A row naming a weapon skill: as named, the player's wish kept
    CHECK(wsRowTakesSneak(row("2|2:50|4:2:16|0"), State::Order, true, takesIfHers));
    CHECK_FALSE(wsRowTakesSneak(row("2|2:50|4:2:19|0"), State::Order, true, takesIfHers));
    CHECK_FALSE(wsRowTakesSneak(row("2|2:50|4:2:17|0"), State::Order, true, takesIfHers));

    // A row that never fires does not count: a marked weapon skill row has
    // no judgement behind it, and a struck-out row does nothing
    const auto markedBest = row("2|101:0|4:0:0|0");
    CHECK(cardian::tactician::stateOf(markedBest) == State::NoJudgement);
    CHECK_FALSE(wsRowTakesSneak(markedBest, State::NoJudgement, true, takesIfHers));
    CHECK_FALSE(wsRowTakesSneak(best, State::Misfit, true, takesIfHers));

    // A row with no weapon skill in it
    CHECK_FALSE(wsRowTakesSneak(row(kRest), State::Order, true, takesIfHers));
}

TEST_CASE("tactician: a played character's rows read as a cardian's; his behaviour rows are struck out", "[cardian][gambits][tactician]")
{
    using cardian::tactician::ownClientStateOf;
    // What his hands do is an order wherever it sits
    CHECK(ownClientStateOf(row("0|1:50|2:2:2|0")) == State::Order);  // Self: HP < 50% -> Cure II
    CHECK(ownClientStateOf(row("2|0:0|4:2:1|0")) == State::Order);   // Foe -> a weapon skill
    CHECK(ownClientStateOf(row("101|0:0|0:0:0|0")) == State::Order); // Foe: targeted by ally -> Attack

    // A behaviour row moves a cardian or speaks to her tactician: struck
    // out in his list
    CHECK(ownClientStateOf(row(kRest)) == State::Client);              // Rest with the player
    CHECK(ownClientStateOf(row("0|0:0|100:1:1|0")) == State::Client); // Avoid aggro
    CHECK(cardian::tactician::struck(State::Client));

    // A marked row is his tactician's tool, exactly as a cardian's is (the
    // user, 2026-10-03); a mark it has no judgement for is struck the same;
    // a misfit is one in his list too
    CHECK(ownClientStateOf(row(kCureBest)) == State::Tool);
    CHECK(ownClientStateOf(row(kCureBest)) == stateOf(kCureBest));
    CHECK(ownClientStateOf(row(kPull)) == stateOf(kPull));
    CHECK(ownClientStateOf(row("2|101:0|3:2:16|0")) == State::NoJudgement); // * Foe -> Mighty Strikes
    CHECK(ownClientStateOf(row("0|0:0|2:2:2|0"), false) == State::Misfit);
}
