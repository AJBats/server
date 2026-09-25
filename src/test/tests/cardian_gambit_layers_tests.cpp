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

#include <catch2/catch_test_macros.hpp>

#include "map/pawn/gambit_defaults.h"
#include "map/pawn/gambit_ids.h"
#include "map/pawn/gambit_layers.h"
#include "map/pawn/gambit_text.h"

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

    // The world's block as brains.yaml compiles it: avoid aggro, rest with
    // leader
    const std::vector<std::string> kWorldBlock{ "0|0:0|100:1:1|0", "0|0:0|100:6:1|0" };

    // The ids in the running order
    auto order(const Layers<Row>& layers) -> std::vector<std::string>
    {
        std::vector<std::string> ids;
        forEachRow(layers, [&](const Row& row, std::size_t)
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
        forEachRow(layers, [&](const Row& row, std::size_t)
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

    CHECK(order(wild) == std::vector<std::string>{ "w1", "w2", "1", "2", "3", "4", "5" });
    CHECK(order(party) == std::vector<std::string>{ "1", "2", "3", "4", "5" });

    // The place is 1-based across both layers: the conveyor's order
    std::vector<std::size_t> places;
    forEachRow(wild, [&](const Row&, const std::size_t place)
               {
                   places.push_back(place);
                   return false;
               });
    CHECK(places == std::vector<std::size_t>{ 1, 2, 3, 4, 5, 6, 7 });

    // The first row to answer true ends the walk, in either layer
    std::vector<std::string> seen;
    CHECK(forEachRow(wild, [&](const Row& row, std::size_t)
                     {
                         seen.push_back(row.id);
                         return row.id == "2";
                     }));
    CHECK(seen == std::vector<std::string>{ "w1", "w2", "1", "2" });
    CHECK_FALSE(forEachRow(party, [](const Row&, std::size_t)
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

    // A mage's own rows: rest with the player and the Support Mage role
    auto own = ownRows(pawn::defaultRowsFor(xi::Job::WHM));

    // In the wild she avoids aggro (the world's) and plays her own role
    const auto wild = behaviorsOf(layersFor<Row>(true, world, own));
    CHECK(behavior(wild, pawn::Behavior::AvoidAggro) == uint16{ 1 });
    CHECK(behavior(wild, pawn::Behavior::RestWithPlayer) == uint16{ 1 });
    CHECK(behavior(wild, pawn::Behavior::Role) == static_cast<uint16>(pawn::Role::SupportMage));

    // With a player the world's rows are gone: no Avoid aggro
    const auto party = behaviorsOf(layersFor<Row>(false, world, own));
    CHECK_FALSE(behavior(party, pawn::Behavior::AvoidAggro).has_value());
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
