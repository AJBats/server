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

// The party's roles and the join rule (RESEARCH §15.6): the user's order
// of candidates for Tank, Healer and Puller, how the rule settles a roster
// given in the order its members joined, and how the player's own choice
// on the party screen stands beside the rule. The party screen and the
// tacticians read these roles, so a reordered rank or a rule that moves a
// role the player set fails here before any party is settled by it.

#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_tostring.hpp>

#include "map/pawn/party_roles.h"

#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <vector>

using cardian::party::choose;
using cardian::party::healerRank;
using cardian::party::heldByOne;
using cardian::party::Member;
using cardian::party::pullerRank;
using cardian::party::Role;
using cardian::party::roleName;
using cardian::party::settle;
using cardian::party::tankRank;

// A failed check names the roles, not their numbers
namespace Catch
{
    template <>
    struct StringMaker<cardian::party::Role>
    {
        static auto convert(const cardian::party::Role role) -> std::string
        {
            return std::string(cardian::party::roleName(role));
        }
    };
} // namespace Catch

namespace
{
    using Roles = std::vector<Role>;
    using Ids   = std::vector<uint32>;

    // A member of this job with no shield, no shadows and no choice of the
    // player's: the rule decides her role
    auto member(const uint32 id, const xi::Job job) -> Member
    {
        Member m;
        m.id  = id;
        m.job = job;
        return m;
    }

    auto withShield(Member m) -> Member
    {
        m.shield = true;
        return m;
    }

    auto castingNi(Member m) -> Member
    {
        m.castsNi = true;
        return m;
    }

    auto castingIchi(Member m) -> Member
    {
        m.castsIchi = true;
        return m;
    }

    // The player's own choice for her, None included
    auto chosen(Member m, const Role role) -> Member
    {
        m.chosen = role;
        return m;
    }

    // Every member's settled role, in the order the members were given
    auto rolesOf(const std::vector<Member>& members) -> Roles
    {
        Roles roles;
        for (const auto& held : settle(members))
        {
            roles.push_back(held.role);
        }
        return roles;
    }

    // The ids of every member the settled party shows in this role, in the
    // order they joined
    auto holders(const std::vector<Member>& members, const Role role) -> Ids
    {
        Ids ids;
        for (const auto& held : settle(members))
        {
            if (held.role == role)
            {
                ids.push_back(held.id);
            }
        }
        return ids;
    }

    const std::vector<xi::Job> kMageJobs{ xi::Job::WHM, xi::Job::BLM, xi::Job::RDM, xi::Job::SMN, xi::Job::SCH, xi::Job::GEO };
} // namespace

TEST_CASE("Party roles: the tank's order is a Paladin, a Ninja with Utsusemi: Ni, a shield Warrior with Ichi, then a shield Warrior without", "[cardian][party_roles]")
{
    CHECK(tankRank(member(1, xi::Job::PLD)) == 0);
    CHECK(tankRank(castingNi(member(1, xi::Job::NIN))) == 1);
    CHECK(tankRank(castingIchi(withShield(member(1, xi::Job::WAR)))) == 2);
    CHECK(tankRank(withShield(member(1, xi::Job::WAR))) == 3);

    // The shadows are asked of the spell: a Ninja who cannot cast Ni is no
    // tank, whatever she holds in her off hand
    CHECK(tankRank(member(1, xi::Job::NIN)) == std::nullopt);
    CHECK(tankRank(withShield(member(1, xi::Job::NIN))) == std::nullopt);
    CHECK(tankRank(castingIchi(withShield(member(1, xi::Job::NIN)))) == std::nullopt);

    // A Warrior without a shield tanks only by the player's press
    CHECK(tankRank(member(1, xi::Job::WAR)) == std::nullopt);
    CHECK(tankRank(castingIchi(member(1, xi::Job::WAR))) == std::nullopt);

    CHECK(tankRank(member(1, xi::Job::MNK)) == std::nullopt);
}

TEST_CASE("Party roles: the healer's order is a White Mage, a Red Mage, then a Summoner, and nobody past them", "[cardian][party_roles]")
{
    CHECK(healerRank(member(1, xi::Job::WHM)) == 0);
    CHECK(healerRank(member(1, xi::Job::RDM)) == 1);
    CHECK(healerRank(member(1, xi::Job::SMN)) == 2);

    CHECK(healerRank(member(1, xi::Job::BLM)) == std::nullopt);
    CHECK(healerRank(member(1, xi::Job::PLD)) == std::nullopt);
    CHECK(healerRank(member(1, xi::Job::BRD)) == std::nullopt);
}

TEST_CASE("Party roles: the puller's order is a Thief, a Ranger, a Bard, then any other melee, and a mage never pulls", "[cardian][party_roles]")
{
    CHECK(pullerRank(member(1, xi::Job::THF)) == 0);
    CHECK(pullerRank(member(1, xi::Job::RNG)) == 1);
    CHECK(pullerRank(member(1, xi::Job::BRD)) == 2);

    // Any other melee pulls with a ranged attack or pebbles
    CHECK(pullerRank(member(1, xi::Job::WAR)) == 3);
    CHECK(pullerRank(member(1, xi::Job::MNK)) == 3);
    CHECK(pullerRank(member(1, xi::Job::PLD)) == 3);
    CHECK(pullerRank(member(1, xi::Job::NIN)) == 3);

    for (const auto job : kMageJobs)
    {
        INFO("job " << static_cast<int>(job));
        CHECK(pullerRank(member(1, job)) == std::nullopt);
    }

    CHECK(pullerRank(member(1, xi::Job::NONE)) == std::nullopt);
}

TEST_CASE("Party roles: only Puller is one member's, and every role has its name", "[cardian][party_roles]")
{
    CHECK(heldByOne(Role::Puller));
    CHECK_FALSE(heldByOne(Role::Tank));
    CHECK_FALSE(heldByOne(Role::Healer));
    CHECK_FALSE(heldByOne(Role::Damage));
    CHECK_FALSE(heldByOne(Role::None));

    CHECK(roleName(Role::Tank) == "Tank");
    CHECK(roleName(Role::Healer) == "Healer");
    CHECK(roleName(Role::Damage) == "Damage");
    CHECK(roleName(Role::Puller) == "Puller");
    CHECK(roleName(Role::None) == "none");
}

TEST_CASE("Party roles: the rule gives every member a role, in the order they joined", "[cardian][party_roles]")
{
    SECTION("an empty party settles to no roles")
    {
        CHECK(settle(std::span<const Member>{}).empty());
    }

    SECTION("a Paladin, a White Mage, a Thief and a Monk are Tank, Healer, Puller and Damage, all the rule's")
    {
        const std::vector<Member> party{
            member(11, xi::Job::PLD),
            member(12, xi::Job::WHM),
            member(13, xi::Job::THF),
            member(14, xi::Job::MNK),
        };
        CHECK(rolesOf(party) == Roles{ Role::Tank, Role::Healer, Role::Puller, Role::Damage });

        const auto held = settle(party);
        REQUIRE(held.size() == party.size());
        for (std::size_t i = 0; i < held.size(); ++i)
        {
            INFO("member " << i);
            CHECK(held[i].id == party[i].id);
            CHECK_FALSE(held[i].byPlayer);
        }
    }
}

TEST_CASE("Party roles: a role the rule gave moves to a better candidate who joins later", "[cardian][party_roles]")
{
    const auto warrior   = withShield(member(1, xi::Job::WAR));
    const auto whiteMage = member(2, xi::Job::WHM);
    const auto paladin   = member(3, xi::Job::PLD);
    const auto thief     = member(4, xi::Job::THF);

    // Alone with a healer, the shield Warrior tanks
    CHECK(rolesOf({ warrior, whiteMage }) == Roles{ Role::Tank, Role::Healer });
    // A Paladin takes the tank from her; with no Thief, Ranger or Bard she
    // pulls as the first melee with no other role
    CHECK(rolesOf({ warrior, whiteMage, paladin }) == Roles{ Role::Puller, Role::Healer, Role::Tank });
    // A Thief takes the pull from her in turn
    CHECK(rolesOf({ warrior, whiteMage, paladin, thief }) == Roles{ Role::Damage, Role::Healer, Role::Tank, Role::Puller });
}

TEST_CASE("Party roles: the tank goes down the user's order as the better tanks leave", "[cardian][party_roles]")
{
    const auto plainWarrior = withShield(member(1, xi::Job::WAR));
    const auto ichiWarrior  = castingIchi(withShield(member(2, xi::Job::WAR)));
    const auto niNinja      = castingNi(member(3, xi::Job::NIN));
    const auto paladin      = member(4, xi::Job::PLD);

    CHECK(holders({ plainWarrior, ichiWarrior, niNinja, paladin }, Role::Tank) == Ids{ 4 });
    CHECK(holders({ plainWarrior, ichiWarrior, niNinja }, Role::Tank) == Ids{ 3 });
    CHECK(holders({ plainWarrior, ichiWarrior }, Role::Tank) == Ids{ 2 });
    CHECK(holders({ plainWarrior }, Role::Tank) == Ids{ 1 });
}

TEST_CASE("Party roles: within one rank the first to have joined wins, and the rule names one tank and one puller", "[cardian][party_roles]")
{
    SECTION("of two Paladins, the first tanks")
    {
        CHECK(holders({ member(1, xi::Job::PLD), member(2, xi::Job::PLD) }, Role::Tank) == Ids{ 1 });
    }

    SECTION("of two Thieves, the first pulls and the second deals damage")
    {
        CHECK(rolesOf({ member(1, xi::Job::THF), member(2, xi::Job::THF) }) == Roles{ Role::Puller, Role::Damage });
    }

    SECTION("a second Paladin is not a second tank: with a Thief to pull, she deals damage")
    {
        CHECK(rolesOf({ member(1, xi::Job::PLD), member(2, xi::Job::PLD), member(3, xi::Job::THF) }) == Roles{ Role::Tank, Role::Damage, Role::Puller });
    }
}

TEST_CASE("Party roles: every healer job heals, and a Black Mage deals damage", "[cardian][party_roles]")
{
    SECTION("a White Mage, two Red Mages and a Summoner are all Healers")
    {
        const std::vector<Member> party{
            member(1, xi::Job::WHM),
            member(2, xi::Job::RDM),
            member(3, xi::Job::RDM),
            member(4, xi::Job::SMN),
        };
        CHECK(rolesOf(party) == Roles{ Role::Healer, Role::Healer, Role::Healer, Role::Healer });
    }

    SECTION("a Black Mage has no healer rank and never pulls, so she deals damage even with the pull free")
    {
        CHECK(rolesOf({ member(1, xi::Job::BLM) }) == Roles{ Role::Damage });
        CHECK(rolesOf({ member(1, xi::Job::PLD), member(2, xi::Job::BLM) }) == Roles{ Role::Tank, Role::Damage });
    }
}

TEST_CASE("Party roles: a Bard pulls only without a Thief or a Ranger, and a Ninja without Ni pulls instead of tanking", "[cardian][party_roles]")
{
    SECTION("a Bard beside a Monk pulls")
    {
        CHECK(rolesOf({ member(1, xi::Job::BRD), member(2, xi::Job::MNK) }) == Roles{ Role::Puller, Role::Damage });
    }

    SECTION("a Ranger takes the pull from a Bard")
    {
        CHECK(rolesOf({ member(1, xi::Job::BRD), member(2, xi::Job::RNG) }) == Roles{ Role::Damage, Role::Puller });
    }

    SECTION("a Ninja still learning is no tank: she pulls as other melee, and nobody tanks")
    {
        const std::vector<Member> party{ member(1, xi::Job::NIN), member(2, xi::Job::WHM) };
        CHECK(rolesOf(party) == Roles{ Role::Puller, Role::Healer });
        CHECK(holders(party, Role::Tank).empty());
    }
}

TEST_CASE("Party roles: the last puller rank takes the first melee with no other role", "[cardian][party_roles]")
{
    SECTION("past the Paladin's tank, the Monk who joined before the Warrior pulls")
    {
        CHECK(rolesOf({ member(1, xi::Job::PLD), member(2, xi::Job::MNK), member(3, xi::Job::WAR) }) == Roles{ Role::Tank, Role::Puller, Role::Damage });
    }

    SECTION("a lone Monk, no tank and no healer, pulls")
    {
        CHECK(rolesOf({ member(1, xi::Job::MNK) }) == Roles{ Role::Puller });
    }
}

TEST_CASE("Party roles: the player's choice is kept and marked, and the rule works around it", "[cardian][party_roles]")
{
    SECTION("a Paladin chosen for Damage deals damage, and the rule gives Tank to the shield Warrior")
    {
        const auto held = settle(std::vector<Member>{ chosen(member(1, xi::Job::PLD), Role::Damage), withShield(member(2, xi::Job::WAR)) });
        REQUIRE(held.size() == 2);
        CHECK(held[0].role == Role::Damage);
        CHECK(held[0].byPlayer);
        CHECK(held[1].role == Role::Tank);
        CHECK_FALSE(held[1].byPlayer);
    }

    SECTION("a Tank the player chose means the rule names no tank of its own")
    {
        const auto monk = chosen(member(1, xi::Job::MNK), Role::Tank);

        const auto held = settle(std::vector<Member>{ monk, member(2, xi::Job::PLD) });
        REQUIRE(held.size() == 2);
        CHECK(held[0].role == Role::Tank);
        CHECK(held[0].byPlayer);
        // The Paladin is other melee to the puller rank
        CHECK(held[1].role == Role::Puller);
        CHECK_FALSE(held[1].byPlayer);

        // With a Thief to pull, the Paladin deals damage
        CHECK(rolesOf({ monk, member(2, xi::Job::PLD), member(3, xi::Job::THF) }) == Roles{ Role::Tank, Role::Damage, Role::Puller });
    }

    SECTION("a Puller the player chose means the rule names no puller of its own")
    {
        CHECK(rolesOf({ chosen(member(1, xi::Job::MNK), Role::Puller), member(2, xi::Job::THF) }) == Roles{ Role::Puller, Role::Damage });
    }

    SECTION("a Healer the player chose leaves the rule's healers healing")
    {
        const auto held = settle(std::vector<Member>{ chosen(member(1, xi::Job::PLD), Role::Healer), member(2, xi::Job::WHM) });
        REQUIRE(held.size() == 2);
        CHECK(held[0].role == Role::Healer);
        CHECK(held[0].byPlayer);
        CHECK(held[1].role == Role::Healer);
        CHECK_FALSE(held[1].byPlayer);
    }

    SECTION("a member the player chose None for keeps none, and the rule gives her nothing")
    {
        const auto paladin = chosen(member(1, xi::Job::PLD), Role::None);

        const auto alone = settle(std::vector<Member>{ paladin });
        REQUIRE(alone.size() == 1);
        CHECK(alone[0].role == Role::None);
        CHECK(alone[0].byPlayer);

        // The tank she would have been goes to the next candidate
        const auto held = settle(std::vector<Member>{ paladin, withShield(member(2, xi::Job::WAR)) });
        REQUIRE(held.size() == 2);
        CHECK(held[0].role == Role::None);
        CHECK(held[0].byPlayer);
        CHECK(held[1].role == Role::Tank);
        CHECK_FALSE(held[1].byPlayer);
    }
}

TEST_CASE("Party roles: a member who leaves frees her role for the next best candidate", "[cardian][party_roles]")
{
    const auto paladin   = member(1, xi::Job::PLD);
    const auto warrior   = withShield(member(2, xi::Job::WAR));
    const auto whiteMage = member(3, xi::Job::WHM);
    const auto thief     = member(4, xi::Job::THF);

    CHECK(rolesOf({ paladin, warrior, whiteMage, thief }) == Roles{ Role::Tank, Role::Damage, Role::Healer, Role::Puller });
    CHECK(rolesOf({ warrior, whiteMage, thief }) == Roles{ Role::Tank, Role::Healer, Role::Puller });
}

TEST_CASE("Party roles: choosing a role on the party screen", "[cardian][party_roles]")
{
    SECTION("Puller chosen for a second member is taken from the first the player had chosen")
    {
        std::vector<Member> party{ member(1, xi::Job::THF), member(2, xi::Job::MNK), member(3, xi::Job::WHM) };

        choose(party, 1, Role::Puller);
        CHECK(party[0].chosen == Role::Puller);

        choose(party, 2, Role::Puller);
        CHECK(party[0].chosen == Role::None);
        CHECK(party[1].chosen == Role::Puller);
        CHECK_FALSE(party[2].chosen.has_value());

        CHECK(rolesOf(party) == Roles{ Role::None, Role::Puller, Role::Healer });
    }

    SECTION("Tank chosen for a second member leaves the first a tank too")
    {
        std::vector<Member> party{ member(1, xi::Job::PLD), withShield(member(2, xi::Job::WAR)), member(3, xi::Job::WHM) };

        choose(party, 1, Role::Tank);
        choose(party, 2, Role::Tank);
        CHECK(party[0].chosen == Role::Tank);
        CHECK(party[1].chosen == Role::Tank);

        const auto held = settle(party);
        REQUIRE(held.size() == 3);
        CHECK(held[0].role == Role::Tank);
        CHECK(held[0].byPlayer);
        CHECK(held[1].role == Role::Tank);
        CHECK(held[1].byPlayer);
        CHECK(held[2].role == Role::Healer);
    }

    SECTION("a choice for an id not in the party changes nothing")
    {
        const std::vector<Member> before{
            chosen(member(1, xi::Job::THF), Role::Puller),
            chosen(member(2, xi::Job::PLD), Role::Tank),
            member(3, xi::Job::WHM),
        };

        for (const auto role : { Role::None, Role::Tank, Role::Healer, Role::Damage, Role::Puller })
        {
            INFO("role " << roleName(role));
            auto party = before;
            choose(party, 99, role);
            CHECK(party[0].chosen == Role::Puller);
            CHECK(party[1].chosen == Role::Tank);
            CHECK_FALSE(party[2].chosen.has_value());
        }
    }

    SECTION("the rule's puller deals damage once the player chooses Puller for another, and stays the rule's")
    {
        std::vector<Member> party{
            member(1, xi::Job::PLD),
            member(2, xi::Job::WHM),
            member(3, xi::Job::THF),
            member(4, xi::Job::MNK),
        };
        REQUIRE(rolesOf(party) == Roles{ Role::Tank, Role::Healer, Role::Puller, Role::Damage });

        choose(party, 4, Role::Puller);
        CHECK_FALSE(party[2].chosen.has_value());

        const auto held = settle(party);
        REQUIRE(held.size() == 4);
        CHECK(held[2].role == Role::Damage);
        CHECK_FALSE(held[2].byPlayer);
        CHECK(held[3].role == Role::Puller);
        CHECK(held[3].byPlayer);
    }
}
