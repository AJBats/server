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

// The seat ladder (seat_ladder.h): one sorted container, two thresholds,
// and the only thing that stands or fades a cardian. Driven here with a
// recording engine and a world of two or three zones, no map server.

#include <catch2/catch_test_macros.hpp>

#include "map/pawn/seat_ladder.h"

#include <set>
#include <string>
#include <vector>

using namespace pawn::seats;
using namespace std::chrono_literals;

namespace
{
    const auto t0 = std::chrono::steady_clock::now();

    // A world the ladder can look at, and an engine that only writes down
    // what it was asked. Zone 1 has the player in it, zone 2 is next door,
    // zone 9 is far away, unless a test says otherwise
    struct Rig
    {
        std::set<uint16>         playerIn{ 1 };
        std::set<uint16>         playerNear{ 1, 2 };
        std::set<uint32>         party;
        std::set<uint32>         cannotStand;
        std::vector<std::string> log;
        Ladder                   ladder;

        Rig(const uint32 standingCap, const uint32 fadedCap, const uint32 standsPerRun = 100)
        : ladder(Ladder::defaultOrder,
                 Lookup{
                     [this](const uint16 zone) { return playerNear.contains(zone); },
                     [this](const uint16 zone) { return playerIn.contains(zone); },
                     [this](const uint32 charid) { return party.contains(charid); },
                 },
                 Engine{
                     [this](const uint32 charid)
                     {
                         log.push_back("stand " + std::to_string(charid));
                         return !cannotStand.contains(charid);
                     },
                     [this](const uint32 charid) { log.push_back("fade " + std::to_string(charid)); },
                     [this](const uint32 charid) { log.push_back("signIn " + std::to_string(charid)); },
                     [this](const uint32 charid) { log.push_back("signOut " + std::to_string(charid)); },
                 },
                 standsPerRun)
        {
            ladder.setCaps(standingCap, fadedCap);
        }

        Rig(const Rig&)            = delete;
        Rig& operator=(const Rig&) = delete;

        void crowd(const uint32 charid, const uint16 zone = 1)
        {
            ladder.offer(charid, Facts{ .zone = zone, .tier = Tier::Crowd });
        }
        void owned(const uint32 charid, const Tier tier, const uint32 owner = 7, const uint16 zone = 1)
        {
            ladder.offer(charid, Facts{ .zone = zone, .tier = tier, .owner = owner });
        }
        auto run(const std::chrono::steady_clock::duration at = 0s) -> std::vector<std::string>
        {
            log.clear();
            ladder.run(t0 + at);
            return log;
        }
        auto stood(const uint32 charid) const -> bool
        {
            return ladder.stepOf(charid) == Step::Standing;
        }
    };
} // namespace

TEST_CASE("Ladder: the standing cap is spent in tier order", "[cardian][ladder]")
{
    Rig rig(2, 10);
    rig.crowd(1);
    rig.owned(2, Tier::Partied);
    rig.owned(3, Tier::Owned);
    rig.owned(4, Tier::Alt);

    const auto log = rig.run();

    CHECK(log == std::vector<std::string>{ "stand 4", "stand 3", "signIn 2", "signIn 1" });
    CHECK(rig.ladder.indexOf(4) == 0);
    CHECK(rig.ladder.indexOf(1) == 3);
    CHECK(rig.ladder.standing() == 2);
}

TEST_CASE("Ladder: a party member outranks every tier", "[cardian][ladder]")
{
    Rig rig(1, 10);
    rig.owned(1, Tier::Alt);
    rig.crowd(2);
    rig.party.insert(2);

    rig.run();

    CHECK(rig.stood(2));
    CHECK_FALSE(rig.stood(1));
}

TEST_CASE("Ladder: tier outranks proximity", "[cardian][ladder]")
{
    // an owned cardian next door against a stranger beside the player
    Rig rig(1, 10);
    rig.crowd(1, 1);
    rig.owned(2, Tier::Owned, 7, 2);

    rig.run();

    CHECK(rig.stood(2));
    CHECK_FALSE(rig.stood(1));
}

TEST_CASE("Ladder: proximity breaks a tie within a tier", "[cardian][ladder]")
{
    // the login case: a neighbour zone's crowd must not take the seat a
    // body in the player's own zone wants
    Rig rig(1, 10);
    rig.crowd(1, 2);
    rig.crowd(2, 1);

    const auto log = rig.run();

    CHECK(rig.stood(2));
    CHECK(log == std::vector<std::string>{ "stand 2", "signIn 1" }); // and the loser was never loaded
}

TEST_CASE("Ladder: nobody stands where no player can see her", "[cardian][ladder]")
{
    Rig rig(10, 10);
    rig.crowd(1, 9);
    rig.crowd(2, 9);

    CHECK(rig.run() == std::vector<std::string>{ "signIn 1", "signIn 2" }); // online, faded, no body

    rig.playerNear.insert(9);
    CHECK(rig.run() == std::vector<std::string>{ "stand 1", "stand 2" });
}

TEST_CASE("Ladder: a cold zone fades its bodies and warms them back", "[cardian][ladder]")
{
    Rig rig(10, 10);
    rig.crowd(1);
    rig.crowd(2);
    rig.run();
    REQUIRE(rig.ladder.standing() == 2);

    rig.playerIn.clear();
    rig.playerNear.clear();
    CHECK(rig.run() == std::vector<std::string>{ "fade 2", "fade 1" }); // worst first
    CHECK(rig.ladder.stepOf(1) == Step::Faded);

    rig.playerIn.insert(1);
    rig.playerNear.insert(1);
    CHECK(rig.run() == std::vector<std::string>{ "stand 1", "stand 2" });
}

TEST_CASE("Ladder: an equal newcomer never displaces a standing body", "[cardian][ladder]")
{
    Rig rig(1, 10);
    rig.crowd(1);
    rig.run();
    REQUIRE(rig.stood(1));

    rig.crowd(2);
    CHECK(rig.run() == std::vector<std::string>{ "signIn 2" });
    CHECK(rig.stood(1));

    // a touch is the one thing that moves her to the front
    rig.ladder.touch(2);
    CHECK(rig.run() == std::vector<std::string>{ "fade 1", "stand 2" });
}

TEST_CASE("Ladder: an offer with unchanged facts is a no-op", "[cardian][ladder]")
{
    Rig rig(1, 10);
    rig.crowd(1);
    rig.crowd(2);
    rig.run();
    REQUIRE(rig.stood(1));

    rig.crowd(1); // the same facts again, as a tick re-offering everyone would
    rig.crowd(2);
    CHECK(rig.run().empty());
    CHECK(rig.stood(1));
}

TEST_CASE("Ladder: changed facts land her below her new equals", "[cardian][ladder]")
{
    Rig rig(1, 10);
    rig.crowd(1);
    rig.crowd(2);
    rig.run();
    REQUIRE(rig.stood(1));

    // she is KO'd past the world's delay: not live, so she yields the seat
    rig.ladder.offer(1, Facts{ .zone = 1, .tier = Tier::Crowd, .down = true });
    CHECK(rig.run() == std::vector<std::string>{ "fade 1", "stand 2" });

    // she is up again, but she re-entered her class below 2 and 2 is standing
    rig.ladder.offer(1, Facts{ .zone = 1, .tier = Tier::Crowd, .down = false });
    CHECK(rig.run().empty());
    CHECK(rig.stood(2));
}

TEST_CASE("Ladder: a zone change keeps her place, a tier change does not", "[cardian][ladder]")
{
    Rig rig(1, 10);
    rig.playerIn = { 1, 2 };
    rig.crowd(1);
    rig.crowd(2);
    rig.run();
    REQUIRE(rig.stood(1));

    // she walks into the next zone with her player: still first among equals
    rig.ladder.offer(1, Facts{ .zone = 2, .tier = Tier::Crowd });
    CHECK(rig.run().empty());
    CHECK(rig.stood(1));

    // 2 is invited, a tier up: she re-enters above 1 and takes the seat
    rig.ladder.offer(2, Facts{ .zone = 1, .tier = Tier::Partied });
    CHECK(rig.run() == std::vector<std::string>{ "fade 1", "stand 2" });
}

TEST_CASE("Ladder: an owned cardian is live wherever she is", "[cardian][ladder]")
{
    // an alt left in a zone nobody is near still stands: she is her player's,
    // and an invite needs her on her feet to reach her
    Rig rig(10, 10);
    rig.owned(1, Tier::Alt, 7, 9);
    rig.crowd(2, 9);

    CHECK(rig.run() == std::vector<std::string>{ "stand 1", "signIn 2" });
}

TEST_CASE("Ladder: stands are rationed, fades are not", "[cardian][ladder]")
{
    Rig rig(6, 20, 2);
    for (uint32 i = 10; i <= 13; ++i)
    {
        rig.crowd(i);
    }
    rig.run();
    rig.run(); // the ration is two a run, so four take two runs
    REQUIRE(rig.ladder.standing() == 4);

    for (uint32 i = 1; i <= 6; ++i)
    {
        rig.owned(i, Tier::Alt);
    }
    const auto log = rig.run();
    CHECK(std::ranges::count_if(log, [](const auto& s) { return s.starts_with("fade"); }) == 4);
    CHECK(std::ranges::count_if(log, [](const auto& s) { return s.starts_with("stand"); }) == 2);

    rig.run();
    rig.run();
    CHECK(rig.ladder.standing() == 6);
}

TEST_CASE("Ladder: a failed stand waits, and drops below her equals meanwhile", "[cardian][ladder]")
{
    Rig rig(1, 10);
    rig.crowd(1);
    rig.crowd(2);
    rig.cannotStand.insert(1);

    CHECK(rig.run() == std::vector<std::string>{ "stand 1", "signIn 2" }); // tried, failed
    CHECK_FALSE(rig.stood(1));

    CHECK(rig.run(1s) == std::vector<std::string>{ "stand 2", "signIn 1" }); // 1 waits, online meanwhile; 2 takes the seat
    CHECK(rig.run(31s).empty());                                  // 1 is back in the order, below 2: no churn

    rig.ladder.withdraw(2);
    CHECK(rig.run(32s) == std::vector<std::string>{ "stand 1" }); // room appeared: she is tried again
}

TEST_CASE("Ladder: the faded cap signs out a zone's worst, and standing bodies count", "[cardian][ladder]")
{
    Rig rig(1, 3);
    for (uint32 i = 1; i <= 5; ++i)
    {
        rig.crowd(i);
    }
    rig.run();

    CHECK(rig.ladder.stepOf(1) == Step::Standing);
    CHECK(rig.ladder.stepOf(2) == Step::Faded);
    CHECK(rig.ladder.stepOf(3) == Step::Faded);
    CHECK(rig.ladder.stepOf(4) == Step::Absent);
    CHECK(rig.ladder.stepOf(5) == Step::Absent);

    // room appears: the absent come back on their own, nothing is stranded
    rig.ladder.setCaps(1, 10);
    CHECK(rig.run() == std::vector<std::string>{ "signIn 4", "signIn 5" });

    // and it tightens again from the bottom
    rig.ladder.setCaps(1, 2);
    CHECK(rig.run() == std::vector<std::string>{ "signOut 5", "signOut 4", "signOut 3" });
}

TEST_CASE("Ladder: the faded cap is per zone, the standing cap is not", "[cardian][ladder]")
{
    Rig rig(3, 2);
    rig.playerIn = { 1, 2 };
    for (uint32 i = 1; i <= 3; ++i)
    {
        rig.crowd(i, 1);
        rig.crowd(10 + i, 2);
    }
    rig.run();

    CHECK(rig.ladder.standing() == 3);
    const auto counts = rig.ladder.zoneCounts();
    CHECK(counts.at(1).second == 2); // online in zone 1
    CHECK(counts.at(2).second == 2); // online in zone 2
    CHECK(rig.ladder.zoneIndexOf(11) == 0);
}

TEST_CASE("Ladder: a standing body past both caps goes to absent in one run", "[cardian][ladder]")
{
    Rig rig(1, 10);
    rig.crowd(1);
    rig.run();
    REQUIRE(rig.stood(1));

    rig.ladder.setCaps(0, 0);
    CHECK(rig.run() == std::vector<std::string>{ "fade 1", "signOut 1" });
    CHECK(rig.ladder.stepOf(1) == Step::Absent);
}

TEST_CASE("Ladder: withdraw takes the body and the row through the engine", "[cardian][ladder]")
{
    Rig rig(2, 10);
    rig.crowd(1);
    rig.crowd(2);
    rig.run();

    rig.log.clear();
    rig.ladder.withdraw(1);
    CHECK(rig.log == std::vector<std::string>{ "fade 1", "signOut 1" });
    CHECK(rig.ladder.indexOf(1) == -1);
    CHECK(rig.ladder.size() == 1);

    rig.ladder.setCaps(2, 1);
    rig.run();
    rig.log.clear();
    rig.ladder.withdraw(2); // standing
    CHECK(rig.log == std::vector<std::string>{ "fade 2", "signOut 2" });
}

TEST_CASE("Ladder: the recall list is her player's cardians without a body", "[cardian][ladder]")
{
    Rig rig(1, 10);
    rig.owned(1, Tier::Alt, 7);
    rig.owned(2, Tier::Alt, 7);
    rig.owned(3, Tier::Alt, 8);
    rig.run();

    CHECK(rig.ladder.belowTheLine(7) == std::vector<uint32>{ 2 });
    CHECK(rig.ladder.ownedBy(7) == std::vector<uint32>{ 1, 2 });
    CHECK(rig.ladder.belowTheLine(8) == std::vector<uint32>{ 3 });
    CHECK(rig.ladder.belowTheLine(9).empty());

    // a recall is a touch: she goes to the front of her tier and takes the seat
    rig.ladder.touch(2);
    CHECK(rig.run() == std::vector<std::string>{ "fade 1", "stand 2" });
}

TEST_CASE("Ladder: a run with nothing changed does nothing", "[cardian][ladder]")
{
    Rig rig(2, 3);
    for (uint32 i = 1; i <= 5; ++i)
    {
        rig.crowd(i);
    }
    rig.run();
    CHECK(rig.run().empty());
    CHECK(rig.run(60s).empty());
}
