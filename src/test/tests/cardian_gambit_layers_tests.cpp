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

// The two layers of a character's rows (gambit_layers.h, ROADMAP K5): a
// world body in the wild runs the world's rows ahead of her own, anyone
// else her own alone; the first row in that order to speak for a behaviour
// wins, whichever layer it is in; a world row's id never meets one of hers,
// and a request her world row made is dropped once she is with a player.
// The fit (RESEARCH §17.3): the rows her party role lends laid onto her own
// by part, a lent row that means what one of hers means left out.

#include <catch2/catch_test_macros.hpp>

#include "map/pawn/gambit_defaults.h"
#include "map/pawn/gambit_ids.h"
#include "map/pawn/gambit_layers.h"
#include "map/pawn/gambit_text.h"
#include "map/pawn/role_bundles.h"

#include <array>
#include <cstddef>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <vector>

using namespace cardian::layers;
using namespace gambits;

namespace
{
    // A row as the layers see it: its id, its engine row and its checkbox
    struct Row
    {
        std::string id;
        Gambit_t    gambit;
        bool        enabled = true;
    };

    auto idOf(const Row& row) -> const std::string&
    {
        return row.id;
    }

    // Rows in the grammar, numbered as CGambits numbers them: her own with
    // plain numbers, the world's with worldRowId
    auto ownRows(const std::vector<std::pair<std::string, bool>>& specs) -> std::vector<Row>
    {
        std::vector<Row> out;
        uint32           next = 0;
        for (const auto& [spec, enabled] : specs)
        {
            auto gambit = pawn::text::parseRow(spec);
            REQUIRE(gambit.has_value());
            out.push_back({ std::to_string(++next), *gambit, enabled });
        }
        return out;
    }

    auto worldRows(const std::vector<std::string>& specs, uint32& next) -> std::vector<Row>
    {
        std::vector<Row> out;
        for (const auto& spec : specs)
        {
            auto gambit = pawn::text::parseRow(spec);
            REQUIRE(gambit.has_value());
            out.push_back({ worldRowId(++next), *gambit, true });
        }
        return out;
    }

    // The world's block as brains.yaml compiles it: avoid aggro, avoid
    // links, rest with leader
    const std::vector<std::string> kWorldBlock{ "0|0:0|100:1:1|0", "0|0:0|100:13:1|0", "0|0:0|100:6:1|0" };

    // The ids in the running order
    auto order(const Layers<Row>& layers) -> std::vector<std::string>
    {
        std::vector<std::string> ids;
        forEachRow(layers, [&](const Row& row, std::size_t, bool)
                   {
                       ids.push_back(row.id);
                       return false;
                   });
        return ids;
    }

    // The behaviour pass (CGambits::TickBehaviors) over the running order:
    // every checked behaviour row that is an order speaks, and speak keeps
    // the first; a marked one is the tactician's (a `* Self -> Rest`) and
    // is silent here
    using Behaviors = std::array<std::optional<uint16>, pawn::BehaviorCount>;
    auto behaviorsOf(const Layers<Row>& layers) -> Behaviors
    {
        Behaviors out{};
        forEachRow(layers, [&](const Row& row, std::size_t, bool)
                   {
                       if (row.enabled && cardian::tactician::stateOf(row.gambit) == cardian::tactician::State::Order)
                       {
                           for (const auto& action : row.gambit.actions)
                           {
                               if (action.reaction == pawn::G_REACTION_BEHAVIOR)
                               {
                                   speak(out, static_cast<uint16>(action.select), static_cast<uint16>(action.select_arg));
                               }
                           }
                       }
                       return false;
                   });
        return out;
    }

    auto behavior(const Behaviors& b, const pawn::Behavior which) -> std::optional<uint16>
    {
        return b[static_cast<std::size_t>(which)];
    }
} // namespace

TEST_CASE("gambit layers: the world's rows run first in the wild, her own alone elsewhere", "[cardian][gambits][layers]")
{
    uint32     next  = 0;
    auto       world = worldRows(kWorldBlock, next);
    auto       own   = ownRows(pawn::defaultRowsFor(xi::Job::PLD));
    const auto wild  = layersFor<Row>(true, world, own);
    const auto party = layersFor<Row>(false, world, own);

    CHECK(order(wild) == std::vector<std::string>{ "w1", "w2", "w3", "1", "2", "3", "4", "5" });
    CHECK(order(party) == std::vector<std::string>{ "1", "2", "3", "4", "5" });

    // The place is 1-based across both layers: the conveyor's order
    std::vector<std::size_t> places;
    forEachRow(wild, [&](const Row&, const std::size_t place, bool)
               {
                   places.push_back(place);
                   return false;
               });
    CHECK(places == std::vector<std::size_t>{ 1, 2, 3, 4, 5, 6, 7, 8 });

    // The first row to answer true ends the walk, in either layer
    std::vector<std::string> seen;
    CHECK(forEachRow(wild, [&](const Row& row, std::size_t, bool)
                     {
                         seen.push_back(row.id);
                         return row.id == "2";
                     }));
    CHECK(seen == std::vector<std::string>{ "w1", "w2", "w3", "1", "2" });
    CHECK_FALSE(forEachRow(party, [](const Row&, std::size_t, bool)
                           {
                               return false;
                           }));

    // No world rows at all (the file missing): her own alone, even in the wild
    CHECK(order(layersFor<Row>(true, std::span<Row>{}, own)) == order(party));
}

TEST_CASE("gambit layers: the first row to speak for a behaviour wins, across both layers", "[cardian][gambits][layers]")
{
    uint32 next = 0;
    auto   world = worldRows(kWorldBlock, next);

    // A mage's own rows: her marked cures, her weapon skill, rest with the player
    auto own = ownRows(pawn::defaultRowsFor(xi::Job::WHM));

    // In the wild she avoids aggro and links (the world's) and rests with
    // the player (hers); her marked Rest row is the tactician's and never
    // speaks as a behaviour
    const auto wild = behaviorsOf(layersFor<Row>(true, world, own));
    CHECK(behavior(wild, pawn::Behavior::AvoidAggro) == uint16{ 1 });
    CHECK(behavior(wild, pawn::Behavior::AvoidLinks) == uint16{ 1 });
    CHECK(behavior(wild, pawn::Behavior::RestWithPlayer) == uint16{ 1 });
    CHECK_FALSE(behavior(wild, pawn::Behavior::Rest).has_value());
    // a plain Rest row speaks as any switch does
    own.push_back({ "8", *pawn::text::parseRow("0|0:0|100:14:1|0"), true });
    CHECK(behavior(behaviorsOf(layersFor<Row>(false, world, own)), pawn::Behavior::Rest) == uint16{ 1 });
    own.pop_back();

    // With a player the world's rows are gone: no Avoid aggro, no Avoid links
    const auto party = behaviorsOf(layersFor<Row>(false, world, own));
    CHECK_FALSE(behavior(party, pawn::Behavior::AvoidAggro).has_value());
    CHECK_FALSE(behavior(party, pawn::Behavior::AvoidLinks).has_value());
    CHECK(behavior(party, pawn::Behavior::RestWithPlayer) == uint16{ 1 });

    // A world row speaks before her own for the same behaviour: the world's
    // formation row (none ships; a hand-edited file) would outrank hers in
    // the wild
    own.push_back({ "9", *pawn::text::parseRow("0|0:0|100:4:2|0"), true }); // Self -> Formation: flank left
    auto worldSeat = worldRows({ "0|0:0|100:4:6|0" }, next);           // Self -> Formation: behind
    const auto both = behaviorsOf(layersFor<Row>(true, worldSeat, own));
    CHECK(behavior(both, pawn::Behavior::Formation) == uint16{ 6 });
    CHECK(behavior(behaviorsOf(layersFor<Row>(false, worldSeat, own)), pawn::Behavior::Formation) == uint16{ 2 });

    // An unchecked row of hers does not speak; the next one does
    own[5].enabled  = false; // rest with the player
    const auto some = behaviorsOf(layersFor<Row>(false, world, own));
    CHECK_FALSE(behavior(some, pawn::Behavior::RestWithPlayer).has_value());
    CHECK(behavior(behaviorsOf(layersFor<Row>(true, world, own)), pawn::Behavior::RestWithPlayer) == uint16{ 1 });
}

TEST_CASE("gambit layers: speak keeps the first and ignores what is not a behaviour", "[cardian][gambits][layers]")
{
    std::array<std::optional<uint16>, 4> b{};
    CHECK(speak(b, 1, 7));
    CHECK_FALSE(speak(b, 1, 9));
    CHECK(b[1] == uint16{ 7 });
    CHECK(speak(b, 3, 0));
    CHECK(b[3] == uint16{ 0 });
    CHECK_FALSE(speak(b, 4, 1));
    CHECK_FALSE(b[0].has_value());
    CHECK_FALSE(b[2].has_value());
}

TEST_CASE("gambit layers: a world row's id never meets one of hers", "[cardian][gambits][layers]")
{
    CHECK(worldRowId(1) == "w1");
    CHECK(worldRowId(12) == "w12");
    CHECK(isWorldRowId("w1"));
    CHECK_FALSE(isWorldRowId("1"));
    CHECK_FALSE(isWorldRowId(""));

    // Her own ids are plain numbers, the world's are prefixed and numbered on
    // across rebuilds, so the two sets never share an id
    uint32     next   = 0;
    const auto first  = worldRows(kWorldBlock, next);
    const auto second = worldRows(kWorldBlock, next); // the layer rebuilt
    const auto own    = ownRows(pawn::defaultRowsFor(xi::Job::RDM));
    std::set<std::string> ids;
    for (const auto* rows : { &first, &second, &own })
    {
        for (const auto& row : *rows)
        {
            CHECK(ids.insert(row.id).second);
            CHECK(isWorldRowId(row.id) == (rows != &own));
        }
    }
}

TEST_CASE("gambit layers: a row is found in the layer its id names, while that layer runs", "[cardian][gambits][layers]")
{
    uint32 next  = 0;
    auto   world = worldRows(kWorldBlock, next);
    auto   own   = ownRows(pawn::defaultRowsFor(xi::Job::PLD));
    auto   wild  = layersFor<Row>(true, world, own);
    auto   party = layersFor<Row>(false, world, own);

    REQUIRE(findRow(wild, "w2", idOf) != nullptr);
    CHECK(findRow(wild, "w2", idOf) == &world[1]);
    CHECK(findRow(wild, "3", idOf) == &own[2]);
    CHECK(findRow(party, "3", idOf) == &own[2]);

    // With a player her world rows do not run: a request one made is gone
    CHECK(findRow(party, "w2", idOf) == nullptr);

    // Gone rows, and ids of the other kind, are nobody's
    CHECK(findRow(wild, "w9", idOf) == nullptr);
    CHECK(findRow(wild, "9", idOf) == nullptr);
}

// --- the fit: a role's rows onto hers (RESEARCH §17.3) ----------------------

namespace
{
    // The role's rows, numbered as CGambits numbers them, with roleRowId
    auto lentRows(const std::vector<std::pair<std::string, bool>>& specs, uint32& next) -> std::vector<Row>
    {
        std::vector<Row> out;
        for (const auto& [spec, enabled] : specs)
        {
            auto gambit = pawn::text::parseRow(spec);
            REQUIRE(gambit.has_value());
            out.push_back({ roleRowId(++next), *gambit, enabled });
        }
        return out;
    }

    auto gambitOf(const Row& row) -> const Gambit_t&
    {
        return row.gambit;
    }

    auto enabledOf(const Row& row) -> bool
    {
        return row.enabled;
    }

    // The fitted order as "id" for her own rows, "rN" for a lent row, and
    // "rN*" for a lent row standing in the place of one of hers
    auto fitted(std::vector<Row>& own, std::vector<Row>& lent) -> std::vector<std::string>
    {
        std::vector<std::string> out;
        for (const auto& p : fit<Row>(own, lent, gambitOf, enabledOf))
        {
            out.push_back(p.origin == Origin::Both ? p.row->id + "*" : p.row->id);
        }
        return out;
    }

    // The states of the fitted rows, in their order
    auto fittedStates(std::vector<Row>& own, std::vector<Row>& lent) -> std::vector<cardian::tactician::State>
    {
        std::vector<cardian::tactician::State> out;
        for (const auto& p : fit<Row>(own, lent, gambitOf, enabledOf))
        {
            out.push_back(cardian::tactician::stateOf(p.row->gambit));
        }
        return out;
    }

    // The levels the game's tables give the seats' tools, as far as these
    // cases need them (the server reads the abilities and spell_list tables)
    auto toolLevel(const pawn::bundles::Need need, const xi::Job job) -> uint8
    {
        using K     = pawn::bundles::Need::Kind;
        namespace t = cardian::tactician;
        if (need.kind == K::Ability)
        {
            switch (need.id)
            {
                case pawn::bundles::kProvoke:
                    return job == xi::Job::WAR ? 5 : 0;
                case t::kBerserk:
                    return job == xi::Job::WAR ? 15 : 0;
                case t::kDefender:
                    return job == xi::Job::WAR ? 25 : 0;
                case t::kAggressor:
                    return job == xi::Job::WAR ? 45 : 0;
                case t::kBoost:
                    return job == xi::Job::MNK ? 5 : 0;
                case t::kDodge:
                    return job == xi::Job::MNK ? 15 : 0;
                case t::kFocus:
                    return job == xi::Job::MNK ? 25 : 0;
                case t::kSneakAttack:
                    return job == xi::Job::THF ? 15 : 0;
                default:
                    return 0;
            }
        }
        if (need.kind == K::Spell && need.id == pawn::bundles::kSpellCure)
        {
            return job == xi::Job::WHM ? 1 : job == xi::Job::RDM ? 3 : job == xi::Job::PLD ? 5 : 0;
        }
        if (need.kind == K::Spell && need.id == pawn::bundles::kSpellPoisona)
        {
            return job == xi::Job::WHM ? 6 : 0;
        }
        if (need.kind == K::Enfeeble)
        {
            // the first debuff her tactician prices: Dia, Poison
            return job == xi::Job::RDM ? 1 : job == xi::Job::WHM ? 3 : job == xi::Job::BLM ? 3 : job == xi::Job::DRK ? 6 : 0;
        }
        return 0;
    }

    // The 75 cap with half-level sub jobs (prod), and retail's 99 (dev)
    constexpr pawn::bundles::Caps k75{ 75, 37 };
    constexpr pawn::bundles::Caps k99{ 99, 49 };

    // What a seat lends a main job and a sub job
    auto bundle(const cardian::party::Role role, const xi::Job main, const xi::Job sub = xi::Job::NONE, const pawn::bundles::Caps caps = k75)
        -> std::vector<std::pair<std::string, bool>>
    {
        return pawn::bundles::bundleFor(role, main, sub, caps, toolLevel);
    }

    // The bundles the fit cases lend: Healer's to a Warrior / White Mage
    // (every row of it), Tank's and Damage's to a Warrior
    const std::vector<std::pair<std::string, bool>> kHealer = bundle(cardian::party::Role::Healer, xi::Job::WAR, xi::Job::WHM);
    const std::vector<std::pair<std::string, bool>> kTank   = bundle(cardian::party::Role::Tank, xi::Job::WAR);
    const std::vector<std::pair<std::string, bool>> kDamage = bundle(cardian::party::Role::Damage, xi::Job::WAR);
} // namespace

TEST_CASE("gambit layers: Healer's bundle onto a melee list puts the tactician's cure rows ahead of hers", "[cardian][gambits][layers][fit]")
{
    using cardian::tactician::State;
    uint32 next = 0;
    auto   own  = ownRows(pawn::defaultRowsFor(xi::Job::BRD));
    auto   lent = lentRows(kHealer, next);

    // the role's marked Cure, -na, Enfeeble and Rest come first -- a role
    // is the quick override -- and her five orders follow in her order
    CHECK(fitted(own, lent) == std::vector<std::string>{ "r1", "r2", "r3", "r4", "1", "2", "3", "4", "5" });
    CHECK(fittedStates(own, lent) == std::vector<State>{ State::Tool, State::Tool, State::Tool, State::Tool, State::Order, State::Order, State::Order, State::Order, State::Order });

    // the lent rows are found by their ids among the running rows, her own
    // by theirs, and a lent id of a row the role did not lend is nobody's
    auto layers = layersFor<Row>(false, std::span<Row>{}, own, lent, gambitOf, enabledOf);
    CHECK(findRow(layers, "r2", idOf) == &lent[1]);
    CHECK(findRow(layers, "5", idOf) == &own[4]);
    CHECK(findRow(layers, "r9", idOf) == nullptr);
    CHECK(order(layers) == std::vector<std::string>{ "r1", "r2", "r3", "r4", "1", "2", "3", "4", "5" });
}

TEST_CASE("gambit layers: Healer's bundle onto a default mage adds nothing: the role's rows stand in her rows' places", "[cardian][gambits][layers][fit]")
{
    uint32 next = 0;
    auto   own  = ownRows(pawn::defaultRowsFor(xi::Job::WHM));
    auto   lent = lentRows(kHealer, next);

    // the bundle's four stand in the places of her Cure (1), her -na (2),
    // her Enfeeble (3) and her Rest (4), under her numbers; the rest of her
    // list is untouched
    CHECK(fitted(own, lent) == std::vector<std::string>{ "r1*", "r2*", "r3*", "r4*", "5", "6", "7" });
    const auto rows = fit<Row>(own, lent, gambitOf, enabledOf);
    CHECK(rows[1].row == &lent[1]);
    CHECK(rows[1].index == 2);
    CHECK(rows[1].origin == Origin::Both);
    CHECK(rows[3].row == &lent[3]);
    CHECK(rows[3].index == 4);
}

TEST_CASE("gambit layers: a lent row overlaps hers by action and side, whatever the condition or the mark", "[cardian][gambits][layers][fit]")
{
    uint32 next = 0;
    auto   lent = lentRows(kHealer, next);

    SECTION("a Cure she gated is overridden: the role's ungated, marked Cure stands in its place while the role is held")
    {
        auto own = ownRows({
            { "0|0:0|100:6:1|0", true }, // Self -> Rest with the player
            { "1|1:75|2:0:1|0", true },  // Ally: HP < 75% -> Cure (best), an order of hers
        });
        CHECK(fitted(own, lent) == std::vector<std::string>{ "r2", "r3", "r4", "1", "r1*" });
        const auto rows = fit<Row>(own, lent, gambitOf, enabledOf);
        CHECK(rows[4].row == &lent[0]);
        CHECK(rows[4].index == 2);
        // her own gate is kept, untouched, for when the role goes
        CHECK(own[1].gambit.predicate_groups.size() == 1);
    }
    SECTION("an unchecked row of hers is taken over: the role's stands in its place, on, her checkbox untouched")
    {
        auto own = ownRows({
            { "0|0:0|100:6:1|0", true },  // Self -> Rest with the player
            { "1|101:0|2:0:1|0", false }, // * Ally -> Cure (best), off
        });
        CHECK(fitted(own, lent) == std::vector<std::string>{ "r2", "r3", "r4", "1", "r1*" });
        const auto rows = fit<Row>(own, lent, gambitOf, enabledOf);
        CHECK(rows[4].on);
        CHECK_FALSE(own[1].enabled);
    }
    SECTION("a row of hers runs by its checkbox, a lent row always; Rest with the player is not the tactician's Rest")
    {
        auto own = ownRows({
            { "0|0:0|100:6:1|0", false }, // Self -> Rest with the player, off
        });
        const auto rows = fit<Row>(own, lent, gambitOf, enabledOf);
        REQUIRE(rows.size() == 5);
        CHECK(rows[0].on);
        CHECK(rows[1].on);
        CHECK(rows[2].on);
        CHECK(rows[3].on);
        CHECK_FALSE(rows[4].on);
        CHECK(rows[4].origin == Origin::Own);
    }
    SECTION("the same action for the other side is not the same row")
    {
        uint32 n     = 0;
        auto   other = lentRows({ { "0|0:0|2:0:1|0", true } }, n); // Self -> Cure (best), an order
        auto   own   = ownRows({ { "1|1:50|2:0:1|0", true } });    // Ally: HP < 50% -> Cure (best)
        CHECK(fitted(own, other) == std::vector<std::string>{ "r1", "1" });
    }
}

TEST_CASE("gambit layers: Tank's bundle onto a Warrior puts the marked pull and Provoke ahead of her orders and keeps her own tools", "[cardian][gambits][layers][fit]")
{
    using cardian::tactician::State;
    uint32 next = 0;
    auto   own  = ownRows(pawn::defaultRowsFor(xi::Job::WAR));
    auto   lent = lentRows(kTank, next); // r1 the pull, r2 Provoke, r3 Defender

    // the role's marked Provoke first -- the tank tactician's tool -- and
    // its marked pull standing in the place of her own "targeted by ally"
    // row (the same action on the same side, whatever the mark: the role's
    // wins while she holds it), so the pull is her tactician's and the
    // rest of her trio stays her orders; the seat's Defender stands in
    // hers, and her Berserk and Aggressor stay her tactician's tools, the
    // seat deciding which of Berserk and Defender it uses
    // (tactician_line.h buffNow)
    CHECK(fitted(own, lent) == std::vector<std::string>{ "r2", "1", "r1*", "3", "4", "r3*", "6", "7", "8" });
    const auto states = fittedStates(own, lent);
    REQUIRE(states.size() == 9);
    CHECK(states[0] == State::Tool);  // * Foe -> Provoke
    CHECK(states[1] == State::Order); // Foe: party leader's target -> Attack
    CHECK(states[2] == State::Tool);  // * Foe: targeted by ally -> Attack, in her row's place: her tactician's melee
    CHECK(states[3] == State::Order); // Foe: targeting ally -> Attack
    CHECK(states[4] == State::Tool);  // * Self -> Berserk
    CHECK(states[5] == State::Tool);  // * Self -> Defender
    CHECK(states[6] == State::Tool);  // * Self -> Aggressor
    CHECK(states[7] == State::Order); // her weapon skill
    CHECK(states[8] == State::Order); // Rest with the player
    const auto rows = fit<Row>(own, lent, gambitOf, enabledOf);
    CHECK(rows[2].origin == Origin::Both);
    CHECK(rows[2].index == 2);
    CHECK(rows[5].origin == Origin::Both);
    CHECK(rows[5].index == 5);
}

TEST_CASE("gambit layers: Tank's bundle onto a default Red Mage / Warrior keeps her cures: the tank's tools and the mage's sit side by side", "[cardian][gambits][layers][fit]")
{
    using cardian::tactician::State;
    uint32 next = 0;
    auto   own  = ownRows(pawn::defaultRowsFor(xi::Job::RDM));
    auto   lent = lentRows(bundle(cardian::party::Role::Tank, xi::Job::RDM, xi::Job::WAR), next); // the pull, Provoke, Defender

    // the role's marked pull, Provoke and Defender first; her marked Cure,
    // -na, Enfeeble and Rest stay her tactician's tools (no line strikes
    // them out any more: one tactician, every tool a row), then her orders,
    // and her own marked Attack row last, off: "targeting ally" is not the
    // pull's finder, so the pull does not stand in it
    CHECK(fitted(own, lent) == std::vector<std::string>{ "r1", "r2", "r3", "1", "2", "3", "4", "5", "6", "7" });
    const auto states = fittedStates(own, lent);
    REQUIRE(states.size() == 10);
    CHECK(states[0] == State::Tool);  // the pull: the tank's melee
    CHECK(states[1] == State::Tool);  // Provoke
    CHECK(states[2] == State::Tool);  // Defender
    CHECK(states[3] == State::Tool);  // Cure (best)
    CHECK(states[4] == State::Tool);  // -na (best)
    CHECK(states[5] == State::Tool);  // Enfeeble
    CHECK(states[6] == State::Tool);  // Rest
    CHECK(states[7] == State::Order); // her weapon skill
    CHECK(states[8] == State::Order); // Rest with the player
    CHECK(states[9] == State::Tool);  // her own marked Attack row
    const auto rows = fit<Row>(own, lent, gambitOf, enabledOf);
    CHECK_FALSE(rows[9].on);
    CHECK(rows[9].origin == Origin::Own);
}

TEST_CASE("gambit layers: an Attack row is its finder: a lent pull takes the same finder's row and leaves her other Attack rows standing", "[cardian][gambits][layers][fit]")
{
    uint32 next = 0;
    auto   lent = lentRows(kTank, next); // r1 the pull, r2 Provoke, r3 Defender
    SECTION("her 'targeting ally' order stays beside the lent 'targeted by ally' pull (played 2026-10-01: the pull had swallowed it)")
    {
        auto own = ownRows({
            { "102|0:0|0:0:0|0", true }, // Foe: targeting ally -> Attack
            { "2|2:50|4:0:0|0", true },  // Foe: HP >= 50% -> Weapon skill (best)
        });
        CHECK(fitted(own, lent) == std::vector<std::string>{ "r1", "r2", "r3", "1", "2" });
    }
    SECTION("her own 'targeted by ally' row, marked or not, is the one the pull stands in")
    {
        auto plain = ownRows({ { "101|0:0|0:0:0|0", true } });
        CHECK(fitted(plain, lent) == std::vector<std::string>{ "r2", "r3", "r1*" });
        auto marked = ownRows({ { "101|101:0|0:0:0|0", false } });
        CHECK(fitted(marked, lent) == std::vector<std::string>{ "r2", "r3", "r1*" });
        CHECK(fit<Row>(marked, lent, gambitOf, enabledOf)[2].on);
    }
}

namespace
{
    // A fitted order as fitted() writes it
    auto ids(const std::vector<Placed<Row>>& rows) -> std::vector<std::string>
    {
        std::vector<std::string> out;
        for (const auto& p : rows)
        {
            out.push_back(p.origin == Origin::Both ? p.row->id + "*" : p.row->id);
        }
        return out;
    }

    using Binds = std::vector<std::optional<std::size_t>>;
} // namespace

TEST_CASE("gambit layers: a lent row stays on the row of hers it stands in, whatever she carries past it", "[cardian][gambits][layers][fit]")
{
    // Played 2026-10-01 (the user): Zapp held two -na rows of her own, the
    // first off. The Healer's -na stood in the first; she carried the
    // second above it, and the role's row jumped onto the row she carried
    // while the one it left surfaced, off -- as if the row had changed its
    // words and switched its neighbour off
    uint32 next = 0;
    auto   lent = lentRows(kHealer, next); // r1 * Cure, r2 * -na, r3 * Enfeeble, r4 * Rest
    auto   own  = ownRows({
        { "1|9:10000|2:0:4|0", false }, // Ally: status = Enfeeble -> -na (best), off
        { "1|9:10000|2:0:4|0", true },  // the same, on
    });
    const auto fresh = bindLent<Row>(own, lent, gambitOf);
    REQUIRE(fresh.size() == 4);
    CHECK(fresh[1] == std::optional<std::size_t>{ 0 }); // the -na takes the first, the one off
    CHECK(ids(place<Row>(own, lent, fresh, enabledOf)) == std::vector<std::string>{ "r1", "r3", "r4", "r2*", "2" });

    // She carries the second above the first: the role's -na keeps its row,
    // now second, and the row she carried shows as itself
    std::swap(own[0], own[1]);
    const Binds kept{ std::nullopt, std::size_t{ 1 }, std::nullopt, std::nullopt };
    const auto  held = bindLent<Row>(own, lent, gambitOf, kept);
    CHECK(held[1] == std::optional<std::size_t>{ 1 });
    CHECK(ids(place<Row>(own, lent, held, enabledOf)) == std::vector<std::string>{ "r1", "r3", "r4", "2", "r2*" });
    // a fresh fit, nothing kept, is the jump
    CHECK(fitted(own, lent) == std::vector<std::string>{ "r1", "r3", "r4", "r2*", "1" });
}

TEST_CASE("gambit layers: a binding kept for a row gone or no longer the same binds afresh, and a row she adds takes a lent row standing alone", "[cardian][gambits][layers][fit]")
{
    uint32 next = 0;
    auto   lent = lentRows(kHealer, next); // r1 * Cure, r2 * -na, r3 * Enfeeble, r4 * Rest
    auto   own  = ownRows({
        { "0|0:0|100:6:1|0", true }, // Self -> Rest with the player
        { "1|1:40|2:0:1|0", true },  // Ally: HP < 40% -> Cure (best)
    });

    // the Cure kept onto a row that is no cure, or onto a place past her
    // last row: either way it binds afresh, to her cure row
    for (const auto& kept : { Binds{ std::size_t{ 0 }, std::nullopt, std::nullopt }, Binds{ std::size_t{ 7 }, std::nullopt, std::nullopt } })
    {
        CHECK(bindLent<Row>(own, lent, gambitOf, kept)[0] == std::optional<std::size_t>{ 1 });
    }
    const auto binds = bindLent<Row>(own, lent, gambitOf, Binds{ std::size_t{ 0 }, std::nullopt, std::nullopt });
    CHECK(binds[0] == std::optional<std::size_t>{ 1 }); // the Cure takes her cure row
    CHECK_FALSE(binds[1].has_value());                  // no -na of hers
    CHECK_FALSE(binds[2].has_value());                  // no Enfeeble of hers
    CHECK_FALSE(binds[3].has_value());                  // no Rest of hers: the role's stands at the top
    // a place past her rows counts as none: the lent row stands at the top
    CHECK(ids(place<Row>(own, lent, Binds{ std::nullopt, std::nullopt, std::nullopt, std::size_t{ 7 } }, enabledOf)) ==
          std::vector<std::string>{ "r1", "r2", "r3", "r4", "1", "2" });

    // she adds a Rest row of her own: the role's Rest, standing alone, takes
    // it, and the Cure keeps the row it holds
    auto rest  = ownRows({ { "0|0:0|100:14:1|0", true } }); // Self -> Rest
    rest[0].id = "3";
    own.push_back(rest[0]);
    const auto added = bindLent<Row>(own, lent, gambitOf, binds);
    CHECK(added[0] == std::optional<std::size_t>{ 1 });
    CHECK(added[3] == std::optional<std::size_t>{ 2 });
    CHECK(ids(place<Row>(own, lent, added, enabledOf)) == std::vector<std::string>{ "r2", "r3", "1", "r1*", "r4*" });
}

TEST_CASE("gambit layers: two lent rows never stand in one row of hers", "[cardian][gambits][layers][fit]")
{
    uint32 next = 0;
    auto   lent = lentRows({ { "1|101:0|2:0:1|0", true }, { "1|101:0|2:0:1|0", true } }, next); // two * Cure rows
    auto   own  = ownRows({ { "1|1:40|2:0:1|0", true } });                                    // one Cure of hers

    const auto binds = bindLent<Row>(own, lent, gambitOf, Binds{ std::size_t{ 0 }, std::size_t{ 0 } });
    CHECK(binds[0] == std::optional<std::size_t>{ 0 });
    CHECK_FALSE(binds[1].has_value());
    CHECK(ids(place<Row>(own, lent, binds, enabledOf)) == std::vector<std::string>{ "r2", "r1*" });
    // and place, handed both onto it, seats the first alone
    CHECK(ids(place<Row>(own, lent, Binds{ std::size_t{ 0 }, std::size_t{ 0 } }, enabledOf)) == std::vector<std::string>{ "r2", "r1*" });
}

TEST_CASE("gambit layers: Damage's bundle is the role's for her jobs: the melee tools for a main job that melees, nothing for a Black Mage", "[cardian][gambits][layers][fit]")
{
    uint32 next = 0;

    SECTION("a default Monk has every row of it: the role's trio, Boost and Focus stand in hers, and her Dodge stays hers")
    {
        auto lent = lentRows(bundle(cardian::party::Role::Damage, xi::Job::MNK), next);
        auto own  = ownRows(pawn::defaultRowsFor(xi::Job::MNK));
        CHECK(fitted(own, lent) == std::vector<std::string>{ "r1*", "r2*", "r3*", "r4*", "r5*", "6", "7", "8" });
    }
    SECTION("a melee job with a hand-made list is lent the trio as orders, ahead of hers")
    {
        auto lent = lentRows(bundle(cardian::party::Role::Damage, xi::Job::PLD), next);
        auto own  = ownRows({
            { "2|2:50|4:0:0|0", true }, // Foe: HP >= 50% -> Weapon skill (best)
        });
        CHECK(fitted(own, lent) == std::vector<std::string>{ "r1", "r2", "r3", "1" });
    }
    SECTION("the White and the Red Mage melee, enfeeble and nuke on the seat; the Black Mage enfeebles and nukes, and the Scholar and the Geomancer only nuke, their sub jobs' melee tools left out; every nuker rests on her tactician's pacing; the Summoner is lent nothing (RESEARCH §17.12, §17.13)")
    {
        const std::pair<std::string, bool> enfeeble{ "2|101:0|2:100:0|0", true };
        const std::pair<std::string, bool> nuke{ "2|101:0|2:3:0|0", true };
        const std::pair<std::string, bool> rest{ "0|101:0|100:14:1|0", true };
        for (const auto job : { xi::Job::WHM, xi::Job::RDM })
        {
            INFO("job " << static_cast<int>(job));
            auto want = bundle(cardian::party::Role::Damage, xi::Job::PLD);
            want.push_back(enfeeble);
            want.push_back(nuke);
            want.push_back(rest);
            CHECK(bundle(cardian::party::Role::Damage, job) == want);
        }
        {
            const std::vector<std::pair<std::string, bool>> want{ enfeeble, nuke, rest };
            CHECK(bundle(cardian::party::Role::Damage, xi::Job::BLM) == want);
            CHECK(bundle(cardian::party::Role::Damage, xi::Job::BLM, xi::Job::WAR) == want);
            CHECK(bundle(cardian::party::Role::Damage, xi::Job::BLM, xi::Job::MNK, k99) == want);
        }
        // the Scholar and the Geomancer learn no debuff her tactician prices
        // -- unless a sub job brings one
        for (const auto job : { xi::Job::SCH, xi::Job::GEO })
        {
            INFO("job " << static_cast<int>(job));
            const std::vector<std::pair<std::string, bool>> want{ nuke, rest };
            CHECK(bundle(cardian::party::Role::Damage, job) == want);
            CHECK(bundle(cardian::party::Role::Damage, job, xi::Job::WAR) == want);
            CHECK(bundle(cardian::party::Role::Damage, job, xi::Job::MNK, k99) == want);
            const std::vector<std::pair<std::string, bool>> withBlackMage{ enfeeble, nuke, rest };
            CHECK(bundle(cardian::party::Role::Damage, job, xi::Job::BLM) == withBlackMage);
        }
        CHECK(bundle(cardian::party::Role::Damage, xi::Job::SMN).empty());
        CHECK(bundle(cardian::party::Role::Damage, xi::Job::SMN, xi::Job::WAR).empty());
        // a melee main never enfeebles or nukes on the seat, her sub job's
        // spells aside
        CHECK(bundle(cardian::party::Role::Damage, xi::Job::WAR, xi::Job::BLM) == kDamage);
        // the seat's Enfeeble, Damage spell (any) and Rest stand in the Black
        // Mage's own
        auto lent = lentRows(bundle(cardian::party::Role::Damage, xi::Job::BLM), next);
        auto own  = ownRows(pawn::defaultRowsFor(xi::Job::BLM));
        CHECK(fitted(own, lent) == std::vector<std::string>{ "1", "2", "r1*", "r2*", "r3*", "6", "7", "8" });
        // a Black Mage whose own rows have none of them (a Warrior's, kept
        // through a job change) is lent them: her MP pacing comes with the seat
        auto melee = ownRows(pawn::defaultRowsFor(xi::Job::WAR));
        auto again = lentRows(bundle(cardian::party::Role::Damage, xi::Job::BLM), next);
        CHECK(fitted(melee, again) == std::vector<std::string>{ "r4", "r5", "r6", "1", "2", "3", "4", "5", "6", "7", "8" });
    }
}

TEST_CASE("gambit layers: a seat lends the tools her two jobs can ever use, the main by the server's cap and the sub by the sub job's", "[cardian][gambits][layers][fit]")
{
    using cardian::party::Role;
    using pawn::bundles::subLevelAt;
    const auto& war = pawn::defaultRowsFor(xi::Job::WAR);
    const auto& mnk = pawn::defaultRowsFor(xi::Job::MNK);
    const auto& mage = pawn::defaultRowsFor(xi::Job::WHM);
    const std::string pull     = "101|101:0|0:0:0|0";
    const std::string provoke  = "2|101:0|3:2:35|0";
    const std::string berserk  = war[3].first;
    const std::string defender = war[4].first;
    const std::string aggressor = war[5].first;
    const std::string boost    = mnk[3].first;
    const std::string focus    = mnk[4].first;
    const std::string dodge    = mnk[5].first;
    const auto        specs    = [](const std::vector<std::pair<std::string, bool>>& rows)
    {
        std::vector<std::string> out;
        for (const auto& [spec, enabled] : rows)
        {
            out.push_back(spec);
        }
        return out;
    };
    const auto trio = specs(bundle(Role::Damage, xi::Job::PLD));
    const auto plus = [&](std::vector<std::string> rows, std::initializer_list<std::string> more)
    {
        rows.insert(rows.end(), more);
        return rows;
    };

    // the sub job's level under the cap, by the server's rule (map.SUBJOB_RATIO)
    CHECK(subLevelAt(75, 1) == 37);
    CHECK(subLevelAt(99, 1) == 49);
    CHECK(subLevelAt(75, 0) == 0);
    CHECK(subLevelAt(75, 2) == 50);
    CHECK(subLevelAt(75, 3) == 75);
    CHECK(subLevelAt(1, 1) == 1);

    // Tank: the pull for anyone; a Warrior's tools from her main or her sub
    // (Provoke at 5, Defender at 25), a Monk's Focus (25) and Dodge (15)
    CHECK(specs(bundle(Role::Tank, xi::Job::WHM)) == std::vector<std::string>{ pull });
    CHECK(specs(bundle(Role::Tank, xi::Job::WHM, xi::Job::WAR)) == std::vector<std::string>{ pull, provoke, defender });
    CHECK(specs(bundle(Role::Tank, xi::Job::WAR, xi::Job::MNK)) == std::vector<std::string>{ pull, provoke, defender, focus, dodge });
    CHECK(specs(bundle(Role::Tank, xi::Job::PLD, xi::Job::NIN)) == std::vector<std::string>{ pull });
    // a server without sub jobs lends a sub job nothing
    CHECK(specs(bundle(Role::Tank, xi::Job::WHM, xi::Job::WAR, { 75, 0 })) == std::vector<std::string>{ pull });

    // Damage: a sub Warrior's Aggressor (45) is past a 75 server's sub jobs
    // (37), within a 99 server's (49)
    CHECK(specs(bundle(Role::Damage, xi::Job::MNK, xi::Job::WAR)) == plus(trio, { berserk, boost, focus }));
    CHECK(specs(bundle(Role::Damage, xi::Job::MNK, xi::Job::WAR, k99)) == plus(trio, { berserk, aggressor, boost, focus }));
    CHECK(specs(bundle(Role::Damage, xi::Job::WAR)) == plus(trio, { berserk, aggressor }));
    CHECK(specs(bundle(Role::Damage, xi::Job::WAR, xi::Job::MNK)) == plus(trio, { berserk, aggressor, boost, focus }));
    CHECK(specs(bundle(Role::Damage, xi::Job::RDM, xi::Job::WAR)) == plus(trio, { berserk, "2|101:0|2:100:0|0", "2|101:0|2:3:0|0", "0|101:0|100:14:1|0" }));
    // a Thief's Sneak Attack (15), from her main or her sub, to a main job
    // that melees; never to a Black Mage, whose seat is her nukes
    const std::string sneak = pawn::defaultRowsFor(xi::Job::THF)[3].first;
    CHECK(specs(bundle(Role::Damage, xi::Job::THF)) == plus(trio, { sneak }));
    CHECK(specs(bundle(Role::Damage, xi::Job::THF, xi::Job::WAR)) == plus(trio, { berserk, sneak }));
    CHECK(specs(bundle(Role::Damage, xi::Job::WAR, xi::Job::THF)) == plus(trio, { berserk, aggressor, sneak }));
    CHECK(specs(bundle(Role::Damage, xi::Job::MNK, xi::Job::THF)) == plus(trio, { boost, sneak, focus }));
    CHECK(specs(bundle(Role::Damage, xi::Job::BLM, xi::Job::THF)) == std::vector<std::string>{ "2|101:0|2:100:0|0", "2|101:0|2:3:0|0", "0|101:0|100:14:1|0" });

    // Healer: a job that learns Cure is lent the Cure and the Rest, the -na
    // only one that learns a -na (the White Mage, main or sub), and the
    // enfeebles a pair that learns Cure and a priced debuff, from either job
    CHECK(bundle(Role::Healer, xi::Job::WAR).empty());
    CHECK(bundle(Role::Healer, xi::Job::WAR, xi::Job::MNK).empty());
    CHECK(specs(bundle(Role::Healer, xi::Job::WAR, xi::Job::WHM)) == std::vector<std::string>{ mage[0].first, mage[1].first, mage[2].first, mage[3].first });
    CHECK(specs(bundle(Role::Healer, xi::Job::RDM)) == std::vector<std::string>{ mage[0].first, mage[2].first, mage[3].first });
    CHECK(specs(bundle(Role::Healer, xi::Job::PLD)) == std::vector<std::string>{ mage[0].first, mage[3].first });
    CHECK(specs(bundle(Role::Healer, xi::Job::PLD, xi::Job::BLM)) == std::vector<std::string>{ mage[0].first, mage[2].first, mage[3].first });
    CHECK(specs(bundle(Role::Healer, xi::Job::RDM, xi::Job::WHM)) == std::vector<std::string>{ mage[0].first, mage[1].first, mage[2].first, mage[3].first });
    // a Summoner heals through her sub job alone: a White Mage's gives her
    // the seat's every row; a Black Mage's enfeebles are no healer's, and
    // she is lent nothing
    CHECK(specs(bundle(Role::Healer, xi::Job::SMN, xi::Job::WHM)) == std::vector<std::string>{ mage[0].first, mage[1].first, mage[2].first, mage[3].first });
    CHECK(bundle(Role::Healer, xi::Job::SMN, xi::Job::BLM).empty());
    CHECK(bundle(Role::Healer, xi::Job::WAR, xi::Job::BLM).empty());
}

TEST_CASE("gambit layers: no bundle is her own rows alone, and the world's still go first in the wild", "[cardian][gambits][layers][fit]")
{
    uint32           next = 0;
    auto             world = worldRows(kWorldBlock, next);
    auto             own   = ownRows(pawn::defaultRowsFor(xi::Job::WHM));
    std::vector<Row> none;
    CHECK(order(layersFor<Row>(false, world, own, none, gambitOf, enabledOf)) == order(layersFor<Row>(false, world, own)));
    CHECK(order(layersFor<Row>(true, world, own, none, gambitOf, enabledOf)) == order(layersFor<Row>(true, world, own)));
    for (const auto& p : fit<Row>(own, none, gambitOf, enabledOf))
    {
        CHECK(p.origin == Origin::Own);
    }
}

TEST_CASE("gambit layers: a bundle's rows are rows the editor could make, and Healer's are the mage defaults' own", "[cardian][gambits][layers][fit]")
{
    // every tool of every seat is a row the editor could make
    for (const auto role : { cardian::party::Role::None, cardian::party::Role::Tank, cardian::party::Role::Healer, cardian::party::Role::Damage, cardian::party::Role::Puller })
    {
        for (const auto& tool : pawn::bundles::toolsOf(role))
        {
            INFO(tool.spec);
            CHECK(pawn::text::parseRow(std::string(tool.spec)).has_value());
        }
        for (const auto& [spec, enabled] : bundle(role, xi::Job::WAR, xi::Job::WHM))
        {
            CHECK(enabled);
        }
    }
    // Healer lends the mage defaults' marked Cure, -na, Enfeeble and Rest,
    // in that order
    const auto& mage = pawn::defaultRowsFor(xi::Job::WHM);
    REQUIRE(kHealer.size() == 4);
    CHECK(kHealer[0].first == mage[0].first);
    CHECK(kHealer[1].first == mage[1].first);
    CHECK(kHealer[2].first == mage[2].first);
    CHECK(kHealer[3].first == mage[3].first);
    CHECK(cardian::tactician::allowanceOf(*pawn::text::parseRow(kHealer[2].first)) == cardian::tactician::Allowance::Debuff);
    CHECK(cardian::tactician::allowanceOf(*pawn::text::parseRow(kHealer[3].first)) == cardian::tactician::Allowance::Rest);
    CHECK(bundle(cardian::party::Role::Healer, xi::Job::WHM) == kHealer);
    // Damage lends a melee job the melee defaults' trio, and a Warrior her
    // own marked Berserk and Aggressor
    const auto& melee   = pawn::defaultRowsFor(xi::Job::PLD);
    const auto& warrior = pawn::defaultRowsFor(xi::Job::WAR);
    REQUIRE(kDamage.size() == 5);
    CHECK(kDamage[0].first == melee[0].first);
    CHECK(kDamage[1].first == melee[1].first);
    CHECK(kDamage[2].first == melee[2].first);
    CHECK(kDamage[3].first == warrior[3].first);
    CHECK(kDamage[4].first == warrior[5].first);
    // Tank lends the pull, marked (the melee defaults' "targeted by ally"
    // finder, an ally being the player as much as a cardian, as the
    // tactician's melee), Provoke, marked (RESEARCH §17.11, §17.13), and a
    // Warrior her own marked Defender
    REQUIRE(kTank.size() == 3);
    const auto pull = *pawn::text::parseRow(kTank[0].first);
    CHECK(cardian::tactician::isMarked(pull));
    CHECK(cardian::tactician::stateOf(pull) == cardian::tactician::State::Tool);
    CHECK(cardian::tactician::allowanceOf(pull) == cardian::tactician::Allowance::Melee);
    CHECK(pull.target_selector == pawn::G_TARGET_TARGETED_BY_ALLY);
    CHECK(cardian::tactician::allowsAbility(*pawn::text::parseRow(kTank[1].first), 35));
    CHECK(cardian::tactician::isMarked(*pawn::text::parseRow(kTank[1].first)));
    CHECK(kTank[2].first == warrior[4].first);
    // Puller lends what Damage does, job for job
    CHECK(bundle(cardian::party::Role::Puller, xi::Job::THF, xi::Job::WAR) == bundle(cardian::party::Role::Damage, xi::Job::THF, xi::Job::WAR));
    CHECK(bundle(cardian::party::Role::Puller, xi::Job::BLM, xi::Job::WHM) == bundle(cardian::party::Role::Damage, xi::Job::BLM, xi::Job::WHM));
    CHECK(bundle(cardian::party::Role::Puller, xi::Job::SMN, xi::Job::WAR).empty());
    CHECK(bundle(cardian::party::Role::None, xi::Job::WAR, xi::Job::WHM).empty());
}
