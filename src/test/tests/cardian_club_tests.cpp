// Cardian: the linkshell club's rules (pawn/club_math.h): the game's linkshell
// items and the shell a pearl belongs to, where a linkshell is sold, what a
// recruit tells him on her way, and a rank catch-up through her nation's
// missions.
#include "map/pawn/club_math.h"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstdint>
#include <set>
#include <string>
#include <vector>

using namespace cardian::club;

TEST_CASE("Club: a linkshell's items are the shell, the sack and the pearl", "[cardian][club]")
{
    CHECK(isLinkshellItem(513));
    CHECK(isLinkshellItem(514));
    CHECK(isLinkshellItem(515));
    CHECK_FALSE(isLinkshellItem(512)); // a new linkshell is not one yet: it is made into one
    CHECK_FALSE(isLinkshellItem(516)); // a ripped pearlsack
    CHECK_FALSE(isLinkshellItem(4181));
}

TEST_CASE("Club: a pearl's shell and kind read off its extra data", "[cardian][club]")
{
    std::array<uint8_t, 24> extra{};
    extra[0] = 0x78;
    extra[1] = 0x56;
    extra[2] = 0x34;
    extra[3] = 0x12;
    extra[8] = 3; // a pearl: Exdata::Linkshell's Flag, after the id, the key and the colour
    CHECK(lsidOf(extra.data(), extra.size()) == 0x12345678u);
    CHECK(lsTypeOf(extra.data(), extra.size()) == 3);
    extra[8] = kLsTypeBroken;
    CHECK(lsTypeOf(extra.data(), extra.size()) == kLsTypeBroken);
    CHECK(lsidOf(extra.data(), 3) == 0);   // too short to hold a shell
    CHECK(lsTypeOf(extra.data(), 8) == 0); // too short to hold a kind
    CHECK(lsidOf(nullptr, 24) == 0);
}

TEST_CASE("Club: a linkshell is sold in the three nations' cities, by their own vendor", "[cardian][club]")
{
    REQUIRE(vendorOf(19).has_value());
    CHECK(vendorOf(19)->name == "Paunelie");
    CHECK(vendorOf(19)->zone == "Southern San d'Oria");
    CHECK(vendorOf(20)->name == "Ilita");
    CHECK(vendorOf(20)->zone == "Port Bastok");
    CHECK(vendorOf(21)->name == "Khel Pahlhama");
    CHECK(vendorOf(21)->zone == "Port Windurst");
    CHECK_FALSE(vendorOf(22).has_value()); // Jeuno sells none
    CHECK_FALSE(vendorOf(0).has_value());  // a field region
}

TEST_CASE("Club: a recruit on her way says one of four lines, the same one each time", "[cardian][club]")
{
    std::set<std::string_view> said;
    for (uint32_t charid = 1000; charid < 1008; ++charid)
    {
        said.insert(headingYourWay(charid));
        CHECK(headingYourWay(charid) == headingYourWay(charid));
    }
    CHECK(said.size() == 4);
    for (const auto line : kHeadingYourWay)
    {
        // the player's text rules: no pronoun for her, no codename
        const std::string text(line);
        CHECK(text.find(" she ") == std::string::npos);
        CHECK(text.find(" her ") == std::string::npos);
        CHECK(text.find("ardian") == std::string::npos);
        CHECK((text.find("your way") != std::string::npos || text.find("to you") != std::string::npos));
    }
}

namespace
{
    // A nation's ladder: steps 2 and 3, with step 3's journeys after the
    // mission that heads it, and a step 4
    auto ladder() -> std::vector<Mission>
    {
        return {
            { 0, 2, 20, { 231, 100 } },
            { 1, 2, 20, { 231, 101, 190 } },
            { 2, 2, 30, { 231, 100, 140 } },
            { 3, 3, 30, { 231, 100, 102 } },
            { 5, 3, 20, { 233, 231 } },
            { 6, 3, 30, { 102, 108 } },
            { 7, 3, 30, { 118, 241 } },
            { 10, 4, 45, { 231, 104, 149 } },
        };
    }
} // namespace

TEST_CASE("Club: a rank catch-up takes her through every step's missions up to the rank", "[cardian][club]")
{
    const auto nothingDone = [](uint16_t) { return false; };
    const auto plan        = rankPlan(ladder(), 1, 3, nothingDone);
    REQUIRE(plan.has_value());
    CHECK(plan->missions == std::vector<uint16_t>{ 0, 1, 2, 3, 5, 6, 7 });
    CHECK(plan->minutes == std::vector<uint16_t>{ 20, 20, 30, 30, 20, 30, 30 });
    // each step's last mission grants its rank
    CHECK(plan->grants == std::vector<uint8_t>{ 0, 0, 2, 0, 0, 0, 3 });
    // her route through all their zones, a zone crossed twice in a row once
    CHECK(plan->route == std::vector<uint16_t>{ 231, 100, 231, 101, 190, 231, 100, 140, 231, 100, 102, 233, 231, 102, 108, 118, 241 });
}

TEST_CASE("Club: a rank catch-up skips the missions she has done", "[cardian][club]")
{
    const std::set<uint16_t> done{ 0, 1, 2 };
    const auto               plan = rankPlan(ladder(), 2, 3, [&](uint16_t id) { return done.contains(id); });
    REQUIRE(plan.has_value());
    CHECK(plan->missions == std::vector<uint16_t>{ 3, 5, 6, 7 });
    CHECK(plan->grants.back() == 3);
}

TEST_CASE("Club: a rank catch-up is only to a rank above hers that the ladder grants", "[cardian][club]")
{
    const auto nothingDone = [](uint16_t) { return false; };
    CHECK_FALSE(rankPlan(ladder(), 3, 3, nothingDone).has_value()); // at it already
    CHECK_FALSE(rankPlan(ladder(), 3, 2, nothingDone).has_value()); // below her
    CHECK_FALSE(rankPlan(ladder(), 1, 9, nothingDone).has_value()); // past the ladder
    // every mission done: nothing left to do
    CHECK_FALSE(rankPlan(ladder(), 1, 2, [](uint16_t) { return true; }).has_value());
    // to rank 4 from rank 1: the whole ladder
    const auto all = rankPlan(ladder(), 1, 4, nothingDone);
    REQUIRE(all.has_value());
    CHECK(all->missions.size() == 8);
    CHECK(all->grants.back() == 4);
}

TEST_CASE("Club: a catch-up goes as far as his rank and the ladder's top", "[cardian][club]")
{
    CHECK(rankCap(ladder(), 3) == 3);
    CHECK(rankCap(ladder(), 10) == 4); // the ladder ends at 4
    CHECK(rankCap(ladder(), 1) == 1);
    CHECK(rankCap({}, 5) == 0);
}
