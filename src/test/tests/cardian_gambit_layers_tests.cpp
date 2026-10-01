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
// The fit (RESEARCH §15.3): the rows her party role lends laid onto her own
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
    // every checked behaviour row speaks, and speak keeps the first
    using Behaviors = std::array<std::optional<uint16>, pawn::BehaviorCount>;
    auto behaviorsOf(const Layers<Row>& layers) -> Behaviors
    {
        Behaviors out{};
        forEachRow(layers, [&](const Row& row, std::size_t, bool)
                   {
                       if (row.enabled)
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
    auto       own   = ownRows(pawn::defaultRowsFor(xi::Job::WAR));
    const auto wild  = layersFor<Row>(true, world, own);
    const auto party = layersFor<Row>(false, world, own);

    CHECK(order(wild) == std::vector<std::string>{ "w1", "w2", "w3", "1", "2", "3", "4", "5", "6" });
    CHECK(order(party) == std::vector<std::string>{ "1", "2", "3", "4", "5", "6" });

    // The place is 1-based across both layers: the conveyor's order
    std::vector<std::size_t> places;
    forEachRow(wild, [&](const Row&, const std::size_t place, bool)
               {
                   places.push_back(place);
                   return false;
               });
    CHECK(places == std::vector<std::size_t>{ 1, 2, 3, 4, 5, 6, 7, 8, 9 });

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

    // A mage's own rows: her weapon skill, rest with the player and the Support Mage role
    auto own = ownRows(pawn::defaultRowsFor(xi::Job::WHM));

    // In the wild she avoids aggro and links (the world's) and plays her own role
    const auto wild = behaviorsOf(layersFor<Row>(true, world, own));
    CHECK(behavior(wild, pawn::Behavior::AvoidAggro) == uint16{ 1 });
    CHECK(behavior(wild, pawn::Behavior::AvoidLinks) == uint16{ 1 });
    CHECK(behavior(wild, pawn::Behavior::RestWithPlayer) == uint16{ 1 });
    CHECK(behavior(wild, pawn::Behavior::Role) == static_cast<uint16>(pawn::Role::SupportMage));

    // With a player the world's rows are gone: no Avoid aggro, no Avoid links
    const auto party = behaviorsOf(layersFor<Row>(false, world, own));
    CHECK_FALSE(behavior(party, pawn::Behavior::AvoidAggro).has_value());
    CHECK_FALSE(behavior(party, pawn::Behavior::AvoidLinks).has_value());
    CHECK(behavior(party, pawn::Behavior::RestWithPlayer) == uint16{ 1 });
    CHECK(behavior(party, pawn::Behavior::Role) == static_cast<uint16>(pawn::Role::SupportMage));

    // A world row speaks before her own for the same behaviour: the world's
    // role row (none ships; a hand-edited file) would outrank hers in the wild
    auto worldRole = worldRows({ "0|0:0|100:11:2|0" }, next);
    const auto both = behaviorsOf(layersFor<Row>(true, worldRole, own));
    CHECK(behavior(both, pawn::Behavior::Role) == static_cast<uint16>(pawn::Role::Tank));
    CHECK(behavior(behaviorsOf(layersFor<Row>(false, worldRole, own)), pawn::Behavior::Role) == static_cast<uint16>(pawn::Role::SupportMage));

    // An unchecked row of hers does not speak; the next one does
    own[1].enabled  = false; // rest with the player
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
    auto   own   = ownRows(pawn::defaultRowsFor(xi::Job::WAR));
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

// --- the fit: a role's rows onto hers (RESEARCH §15.3) ----------------------

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

    // The line among the fitted rows, as the states are judged
    auto fittedLine(std::vector<Row>& own, std::vector<Row>& lent) -> std::optional<cardian::tactician::Line>
    {
        const auto rows = fit<Row>(own, lent, gambitOf, enabledOf);
        return cardian::tactician::lineOf(rows, [](const Placed<Row>& p) -> const Gambit_t& { return p.row->gambit; });
    }

    // Its 1-based place
    auto linePlace(std::vector<Row>& own, std::vector<Row>& lent) -> std::optional<std::size_t>
    {
        const auto line = fittedLine(own, lent);
        return line.has_value() ? std::optional<std::size_t>(line->place) : std::nullopt;
    }

    // The states of the fitted rows, 1-based places
    auto fittedStates(std::vector<Row>& own, std::vector<Row>& lent) -> std::vector<cardian::tactician::State>
    {
        const auto                             rows = fit<Row>(own, lent, gambitOf, enabledOf);
        const auto                             line = fittedLine(own, lent);
        std::vector<cardian::tactician::State> out;
        for (std::size_t i = 0; i < rows.size(); ++i)
        {
            out.push_back(cardian::tactician::stateOf(rows[i].row->gambit, i + 1, line));
        }
        return out;
    }

    const std::vector<std::pair<std::string, bool>> kHealer = pawn::bundles::bundleFor(cardian::party::Role::Healer);
    const std::vector<std::pair<std::string, bool>> kTank   = pawn::bundles::bundleFor(cardian::party::Role::Tank);
    const std::vector<std::pair<std::string, bool>> kDamage = pawn::bundles::bundleFor(cardian::party::Role::Damage);
} // namespace

TEST_CASE("gambit layers: Healer's bundle onto a melee list adds a line and its rows under hers", "[cardian][gambits][layers][fit]")
{
    uint32 next = 0;
    auto   own  = ownRows(pawn::defaultRowsFor(xi::Job::BRD));
    auto   lent = lentRows(kHealer, next);

    // her six orders stay orders; the role's Support Mage row is her line,
    // and the role's Cure and -na follow it
    CHECK(fitted(own, lent) == std::vector<std::string>{ "1", "2", "3", "4", "5", "6", "r1", "r2", "r3" });
    CHECK(linePlace(own, lent) == 7);

    // the lent rows are found by their ids among the running rows, her own
    // by theirs, and a lent id of a row the role did not lend is nobody's
    auto layers = layersFor<Row>(false, std::span<Row>{}, own, lent, gambitOf, enabledOf);
    CHECK(findRow(layers, "r2", idOf) == &lent[1]);
    CHECK(findRow(layers, "6", idOf) == &own[5]);
    CHECK(findRow(layers, "r9", idOf) == nullptr);
    CHECK(order(layers) == std::vector<std::string>{ "1", "2", "3", "4", "5", "6", "r1", "r2", "r3" });
}

TEST_CASE("gambit layers: Healer's bundle onto a default mage adds nothing: the role's rows stand in her rows' places", "[cardian][gambits][layers][fit]")
{
    uint32 next = 0;
    auto   own  = ownRows(pawn::defaultRowsFor(xi::Job::WHM));
    auto   lent = lentRows(kHealer, next);

    // the bundle's three stand in the places of her Support Mage row (3),
    // her Cure (4) and her -na (5), under her numbers; the rest of her list
    // is untouched, and her line is where it was
    CHECK(fitted(own, lent) == std::vector<std::string>{ "1", "2", "r1*", "r2*", "r3*", "6", "7" });
    CHECK(linePlace(own, lent) == 3);
    const auto rows = fit<Row>(own, lent, gambitOf, enabledOf);
    CHECK(rows[3].row == &lent[1]);
    CHECK(rows[3].index == 4);
    CHECK(rows[3].origin == Origin::Both);
}

TEST_CASE("gambit layers: a lent row overlaps hers by action and side, in the same part, whatever the condition", "[cardian][gambits][layers][fit]")
{
    uint32 next = 0;
    auto   lent = lentRows(kHealer, next);

    SECTION("a Cure she gated below her line is overridden: the role's ungated Cure stands in its place while the role is held")
    {
        auto own = ownRows({
            { "0|0:0|100:11:1|0", true }, // Self -> Support Mage
            { "1|1:75|2:0:1|0", true },   // Ally: HP < 75% -> Cure (best)
        });
        CHECK(fitted(own, lent) == std::vector<std::string>{ "r1*", "r2*", "r3" });
        const auto rows = fit<Row>(own, lent, gambitOf, enabledOf);
        CHECK(rows[1].row == &lent[1]);
        CHECK(rows[1].index == 2);
        // her own gate is kept, untouched, for when the role goes
        CHECK(own[1].gambit.predicate_groups.size() == 1);
    }
    SECTION("a Cure order above her line is another part: the role's Cure joins below")
    {
        auto own = ownRows({
            { "1|1:50|2:0:1|0", true },   // Ally: HP < 50% -> Cure (best), an order
            { "0|0:0|100:11:1|0", true }, // Self -> Support Mage
        });
        CHECK(fitted(own, lent) == std::vector<std::string>{ "1", "r1*", "r2", "r3" });
    }
    SECTION("an unchecked row of hers is taken over: the role's stands in its place, on, her checkbox untouched")
    {
        auto own = ownRows({
            { "0|0:0|100:11:1|0", true },  // Self -> Support Mage
            { "1|101:0|2:0:1|0", false },  // Ally: tactician's choice -> Cure (best), off
        });
        CHECK(fitted(own, lent) == std::vector<std::string>{ "r1*", "r2*", "r3" });
        const auto rows = fit<Row>(own, lent, gambitOf, enabledOf);
        CHECK(rows[1].on);
        CHECK_FALSE(own[1].enabled);
    }
    SECTION("a row of hers runs by its checkbox, a lent row always")
    {
        auto own = ownRows({
            { "0|0:0|100:6:1|0", false }, // Self -> Rest with the player, off
        });
        const auto rows = fit<Row>(own, lent, gambitOf, enabledOf);
        REQUIRE(rows.size() == 4);
        CHECK_FALSE(rows[0].on);
        CHECK(rows[1].on);
        CHECK(rows[2].on);
        CHECK(rows[3].on);
    }
    SECTION("the same action for the other side is not the same row")
    {
        uint32 n     = 0;
        auto   other = lentRows({ { "0|0:0|2:0:1|0", true } }, n); // Self -> Cure (best), an order
        auto   own   = ownRows({ { "1|1:50|2:0:1|0", true } });    // Ally: HP < 50% -> Cure (best)
        CHECK(fitted(own, other) == std::vector<std::string>{ "1", "r1" });
    }
}

TEST_CASE("gambit layers: her own Support Mage row unchecked is still her line's place, and the role's row stands there, on", "[cardian][gambits][layers][fit]")
{
    uint32 next = 0;
    auto   lent = lentRows(kHealer, next);
    auto   own  = ownRows({
        { "0|0:0|100:6:1|0", true },   // Self -> Rest with the player, an order
        { "0|0:0|100:11:1|0", false }, // Self -> Support Mage, off: her tactician is off
        { "1|101:0|2:0:1|0", true },   // Ally: tactician's choice -> Cure (best)
    });
    // her order; the role's line in her line's place; the role's Cure in
    // her Cure's place; the role's -na
    CHECK(fitted(own, lent) == std::vector<std::string>{ "1", "r1*", "r2*", "r3" });
    CHECK(linePlace(own, lent) == 2);

    const auto rows  = fit<Row>(own, lent, gambitOf, enabledOf);
    const auto line  = fittedLine(own, lent);
    const auto state = [&](const std::size_t place)
    {
        return cardian::tactician::stateOf(rows[place - 1].row->gambit, place, line);
    };
    CHECK(state(1) == cardian::tactician::State::Order);
    CHECK(state(2) == cardian::tactician::State::Line);
    CHECK(state(3) == cardian::tactician::State::Allows);
    CHECK(state(4) == cardian::tactician::State::Allows);
    // the role's line stands in her line's place and runs; her own row is
    // kept underneath, its checkbox untouched
    CHECK(rows[1].on);
    CHECK(rows[1].row == &lent[0]);
    CHECK(rows[1].index == 2);
    CHECK_FALSE(own[1].enabled);
}

TEST_CASE("gambit layers: Tank's bundle onto a melee list is a line under her orders, with the pull and Provoke below it", "[cardian][gambits][layers][fit]")
{
    using cardian::tactician::State;
    uint32 next = 0;
    auto   own  = ownRows(pawn::defaultRowsFor(xi::Job::WAR));
    auto   lent = lentRows(kTank, next);

    // her six orders stay orders, her own trio among them; the role's Tank
    // row is her line, and the pull and Provoke are her tank tactician's
    CHECK(fitted(own, lent) == std::vector<std::string>{ "1", "2", "3", "4", "5", "6", "r1", "r2", "r3" });
    CHECK(fittedLine(own, lent) == std::optional<cardian::tactician::Line>(cardian::tactician::Line{ 7, pawn::Role::Tank }));
    const auto states = fittedStates(own, lent);
    REQUIRE(states.size() == 9);
    CHECK(states[6] == State::Line);
    CHECK(states[7] == State::Allows); // Foe: targeted by ally -> Attack: her tactician's melee
    CHECK(states[8] == State::Allows); // Foe: tactician's choice -> Provoke
}

TEST_CASE("gambit layers: Tank's bundle onto a default mage stands in her line's place: her tactician is the tank's while she holds the role", "[cardian][gambits][layers][fit]")
{
    using cardian::tactician::State;
    uint32 next = 0;
    auto   own  = ownRows(pawn::defaultRowsFor(xi::Job::RDM));
    auto   lent = lentRows(kTank, next);

    // her two orders; the role's Tank row where her Support Mage row was;
    // her rows below, the role's pull standing in her own (unchecked)
    // Attack row's place, then Provoke. Her Cure, -na and Enfeeble are
    // nothing the tank's tactician reads: struck out while she holds Tank,
    // her own rows untouched for when the role goes
    CHECK(fitted(own, lent) == std::vector<std::string>{ "1", "2", "r1*", "4", "5", "6", "r2*", "r3" });
    CHECK(fittedLine(own, lent) == std::optional<cardian::tactician::Line>(cardian::tactician::Line{ 3, pawn::Role::Tank }));
    const auto states = fittedStates(own, lent);
    REQUIRE(states.size() == 8);
    CHECK(states[2] == State::Line);
    CHECK(states[3] == State::NotBelow); // Cure (best)
    CHECK(states[4] == State::NotBelow); // -na (best)
    CHECK(states[5] == State::NotBelow); // Enfeeble
    CHECK(states[6] == State::Allows);   // the pull, in her Attack row's place: the tank's melee
    CHECK(states[7] == State::Allows);   // Provoke
    const auto rows = fit<Row>(own, lent, gambitOf, enabledOf);
    CHECK(rows[6].on);
    CHECK_FALSE(own[6].enabled);
    CHECK(own[2].enabled);
}

TEST_CASE("gambit layers: Damage's bundle adds nothing to a default melee, and gives a mage the trio as orders", "[cardian][gambits][layers][fit]")
{
    uint32 next = 0;
    auto   lent = lentRows(kDamage, next);

    SECTION("a default melee has every row of it: the role's stand in theirs")
    {
        auto own = ownRows(pawn::defaultRowsFor(xi::Job::MNK));
        CHECK(fitted(own, lent) == std::vector<std::string>{ "r1*", "r2*", "r3*", "4", "5", "r4*" });
        CHECK_FALSE(linePlace(own, lent).has_value());
    }
    SECTION("a default mage gets the trio and the Damage row as orders above her line; her own Attack row stays below it")
    {
        auto own = ownRows(pawn::defaultRowsFor(xi::Job::BLM));
        CHECK(fitted(own, lent) == std::vector<std::string>{ "1", "2", "r1", "r2", "r3", "r4", "3", "4", "5", "6", "7" });
        CHECK(linePlace(own, lent) == 7);
    }
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

TEST_CASE("gambit layers: the roles she holds are a set, where a behaviour keeps its first speaker", "[cardian][gambits][layers][fit]")
{
    uint32 held = 0;
    CHECK_FALSE(holdsRole(held, static_cast<uint16>(pawn::Role::SupportMage)));
    holdRole(held, static_cast<uint16>(pawn::Role::MeleeDamage));
    holdRole(held, static_cast<uint16>(pawn::Role::SupportMage));
    CHECK(holdsRole(held, static_cast<uint16>(pawn::Role::MeleeDamage)));
    CHECK(holdsRole(held, static_cast<uint16>(pawn::Role::SupportMage)));
    CHECK_FALSE(holdsRole(held, static_cast<uint16>(pawn::Role::Tank)));
    // a role number past the set is never held
    holdRole(held, 40);
    CHECK_FALSE(holdsRole(held, 40));
}

TEST_CASE("gambit layers: a bundle's rows are rows the editor could make, and Healer's are the mage defaults' own", "[cardian][gambits][layers][fit]")
{
    for (const auto role : { cardian::party::Role::None, cardian::party::Role::Tank, cardian::party::Role::Healer, cardian::party::Role::Damage, cardian::party::Role::Puller })
    {
        for (const auto& [spec, enabled] : pawn::bundles::bundleFor(role))
        {
            INFO(spec);
            CHECK(pawn::text::parseRow(spec).has_value());
            CHECK(enabled);
        }
    }
    // Healer lends the mage defaults' Support Mage row, Cure and -na, in that order
    const auto& mage = pawn::defaultRowsFor(xi::Job::WHM);
    REQUIRE(kHealer.size() == 3);
    CHECK(kHealer[0].first == mage[2].first);
    CHECK(kHealer[1].first == mage[3].first);
    CHECK(kHealer[2].first == mage[4].first);
    // Damage lends the melee defaults' trio and their Damage row
    const auto& melee = pawn::defaultRowsFor(xi::Job::WAR);
    REQUIRE(kDamage.size() == 4);
    CHECK(kDamage[0].first == melee[0].first);
    CHECK(kDamage[1].first == melee[1].first);
    CHECK(kDamage[2].first == melee[2].first);
    CHECK(kDamage[3].first == melee[5].first);
    // Tank lends its line, then the pull (the melee defaults' "targeted by
    // ally" row: an ally is the player as much as a cardian) and Provoke
    // as the tactician's choice (RESEARCH §15.11)
    REQUIRE(kTank.size() == 3);
    CHECK(cardian::tactician::lineRoleOf(*pawn::text::parseRow(kTank[0].first)) == pawn::Role::Tank);
    CHECK(kTank[1].first == melee[1].first);
    CHECK(cardian::tactician::allowsAbility(*pawn::text::parseRow(kTank[2].first), 35));
    CHECK(cardian::tactician::carries(*pawn::text::parseRow(kTank[2].first), pawn::G_CONDITION_TACTICIANS_CHOICE));
    // Puller never lends
    CHECK(pawn::bundles::bundleFor(cardian::party::Role::Puller).empty());
    CHECK(pawn::bundles::bundleFor(cardian::party::Role::None).empty());
}
