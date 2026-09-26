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

// The rows every character starts with, by job (ROADMAP K): which jobs
// take the mage set, the exact rows and their checkboxes, and that each
// row reads back through the grammar as the ids it is meant to carry, so a
// renumbered id or a mistyped spec fails here before a cardian is seeded
// with it.

#include <catch2/catch_test_macros.hpp>

#include "map/pawn/engage_math.h"
#include "map/pawn/gambit_defaults.h"
#include "map/pawn/gambit_text.h"
#include "map/spell.h"

#include <algorithm>
#include <cstddef>
#include <set>
#include <string>
#include <utility>
#include <vector>

using namespace gambits;
using pawn::defaultRowsFor;
using pawn::isMageJob;
using pawn::text::formatRow;
using pawn::text::parseRow;

namespace
{
    using Rows = std::vector<std::pair<std::string, bool>>;

    const Rows kMelee{
        { "100|0:0|0:0:0|0", true },
        { "101|0:0|0:0:0|0", true },
        { "102|0:0|0:0:0|0", true },
        { "2|2:50|4:0:0|0", true },
        { "0|0:0|100:6:1|0", true },
        { "0|0:0|100:11:3|0", true },
    };

    const Rows kMage{
        { "2|2:50|4:0:0|0", true },
        { "0|0:0|100:6:1|0", true },
        { "0|0:0|100:11:1|0", true },
        { "1|101:0|2:0:1|0", true },
        { "102|0:0|0:0:0|0", false },
    };

    const std::set<xi::Job> kMageJobs{ xi::Job::WHM, xi::Job::BLM, xi::Job::RDM, xi::Job::SMN, xi::Job::SCH, xi::Job::GEO };

    // Every job a character can hold, WAR through RUN
    auto allJobs() -> std::vector<xi::Job>
    {
        std::vector<xi::Job> jobs;
        for (auto id = std::to_underlying(xi::Job::WAR); id <= std::to_underlying(xi::Job::RUN); ++id)
        {
            jobs.push_back(static_cast<xi::Job>(id));
        }
        return jobs;
    }

    // One row as the ids it must carry: one condition, one action, no retry
    struct Expected
    {
        G_TARGET    target;
        G_CONDITION condition;
        G_REACTION  reaction;
        uint16      select;
        uint32      arg;
        uint32      conditionArg = 0;
    };

    void requireRow(const std::string& spec, const Expected& want)
    {
        INFO("row " << spec);
        const auto g = parseRow(spec);
        REQUIRE(g.has_value());
        CHECK(g->target_selector == want.target);
        REQUIRE(g->predicate_groups.size() == 1);
        REQUIRE(g->predicate_groups[0].predicates.size() == 1);
        CHECK(g->predicate_groups[0].predicates[0].condition == want.condition);
        CHECK(g->predicate_groups[0].predicates[0].condition_arg == want.conditionArg);
        REQUIRE(g->actions.size() == 1);
        CHECK(g->actions[0].reaction == want.reaction);
        CHECK(static_cast<uint16>(g->actions[0].select) == want.select);
        CHECK(g->actions[0].select_arg == want.arg);
        CHECK(g->retry_delay == 0);
    }

    constexpr auto kAttack   = G_REACTION::ATTACK;
    constexpr auto kBehavior = pawn::G_REACTION_BEHAVIOR;
    constexpr auto kRest     = static_cast<uint16>(pawn::Behavior::RestWithPlayer);
    constexpr auto kRole     = static_cast<uint16>(pawn::Behavior::Role);
} // namespace

TEST_CASE("gambit defaults: the six mage jobs take the mage set, every other job the melee set", "[cardian][gambits][defaults]")
{
    const auto jobs = allJobs();
    REQUIRE(jobs.size() == 22);

    std::size_t mages = 0;
    for (const auto job : jobs)
    {
        INFO("job " << static_cast<int>(job));
        const bool mage = kMageJobs.contains(job);
        CHECK(isMageJob(job) == mage);
        CHECK(defaultRowsFor(job) == (mage ? kMage : kMelee));
        if (mage)
        {
            ++mages;
        }
    }
    CHECK(mages == 6);

    // BRD and BLU cast, but take the melee set; BLM is a mage for now
    CHECK_FALSE(isMageJob(xi::Job::BRD));
    CHECK_FALSE(isMageJob(xi::Job::BLU));
    CHECK(isMageJob(xi::Job::BLM));
    // No job, or a monster's, is not a mage either
    CHECK_FALSE(isMageJob(xi::Job::NONE));
    CHECK_FALSE(isMageJob(xi::Job::MON));
}

TEST_CASE("gambit defaults: the melee set is the assist trio, her best weapon skill, rest with the player and the Damage role, all on", "[cardian][gambits][defaults]")
{
    const auto& rows = defaultRowsFor(xi::Job::WAR);
    REQUIRE(rows == kMelee);
    for (const auto& [spec, on] : rows)
    {
        INFO("row " << spec);
        CHECK(on);
    }

    requireRow(rows[0].first, { pawn::G_TARGET_LEADERS_TARGET, G_CONDITION::ALWAYS, kAttack, 0, 0 });
    requireRow(rows[1].first, { pawn::G_TARGET_TARGETED_BY_ALLY, G_CONDITION::ALWAYS, kAttack, 0, 0 });
    requireRow(rows[2].first, { pawn::G_TARGET_TARGETING_ALLY, G_CONDITION::ALWAYS, kAttack, 0, 0 });
    requireRow(rows[3].first, { G_TARGET::TARGET, G_CONDITION::HPP_GTE, G_REACTION::WS, static_cast<uint16>(G_SELECT::HIGHEST), 0, 50 });
    requireRow(rows[4].first, { G_TARGET::SELF, G_CONDITION::ALWAYS, kBehavior, kRest, 1 });
    requireRow(rows[5].first, { G_TARGET::SELF, G_CONDITION::ALWAYS, kBehavior, kRole, static_cast<uint32>(pawn::Role::MeleeDamage) });
}

TEST_CASE("gambit defaults: the mage set is a Support Mage who cures, her weapon skill above her line, her Attack row below it and off", "[cardian][gambits][defaults]")
{
    const auto& rows = defaultRowsFor(xi::Job::WHM);
    REQUIRE(rows == kMage);

    // Checking the Attack row is what makes her a melee mage, so it ships off
    CHECK(rows[0].second);
    CHECK(rows[1].second);
    CHECK(rows[2].second);
    CHECK(rows[3].second);
    CHECK_FALSE(rows[4].second);

    requireRow(rows[0].first, { G_TARGET::TARGET, G_CONDITION::HPP_GTE, G_REACTION::WS, static_cast<uint16>(G_SELECT::HIGHEST), 0, 50 });
    requireRow(rows[1].first, { G_TARGET::SELF, G_CONDITION::ALWAYS, kBehavior, kRest, 1 });
    requireRow(rows[2].first, { G_TARGET::SELF, G_CONDITION::ALWAYS, kBehavior, kRole, static_cast<uint32>(pawn::Role::SupportMage) });
    requireRow(rows[3].first, { G_TARGET::PARTY, pawn::G_CONDITION_TACTICIANS_CHOICE, G_REACTION::MA, static_cast<uint16>(G_SELECT::HIGHEST), static_cast<uint32>(SPELLFAMILY_CURE) });
    requireRow(rows[4].first, { pawn::G_TARGET_TARGETING_ALLY, G_CONDITION::ALWAYS, kAttack, 0, 0 });
}

TEST_CASE("gambit defaults: every default row parses, round-trips through the grammar and pairs as the editor asks", "[cardian][gambits][defaults]")
{
    for (const auto* rows : { &defaultRowsFor(xi::Job::WAR), &defaultRowsFor(xi::Job::WHM) })
    {
        for (const auto& row : *rows)
        {
            INFO("row " << row.first);
            const auto g = parseRow(row.first);
            REQUIRE(g.has_value());
            CHECK(formatRow(*g) == row.first);
            CHECK(cardian::engage::pairingError(*g).empty());
        }
    }
}

TEST_CASE("gambit defaults: no default row avoids aggro or links; every set carries the weapon skill row", "[cardian][gambits][defaults]")
{
    for (const auto* rows : { &defaultRowsFor(xi::Job::WAR), &defaultRowsFor(xi::Job::WHM) })
    {
        for (const auto& row : *rows)
        {
            INFO("row " << row.first);
            const auto g = parseRow(row.first);
            REQUIRE(g.has_value());
            for (const auto& action : g->actions)
            {
                CHECK_FALSE((action.reaction == kBehavior && static_cast<uint16>(action.select) == static_cast<uint16>(pawn::Behavior::AvoidAggro)));
                CHECK_FALSE((action.reaction == kBehavior && static_cast<uint16>(action.select) == static_cast<uint16>(pawn::Behavior::AvoidLinks)));
            }
        }
        CHECK(std::ranges::count(*rows, std::make_pair(std::string("2|2:50|4:0:0|0"), true)) == 1);
    }
}
