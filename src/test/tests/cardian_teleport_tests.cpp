// Cardian: the teleport shout's rules (pawn/teleport_rules.h, ROADMAP "The
// teleport shout"): the crags, who answers and at what price, his payment,
// her keeping to town, who goes when the spell lands, and when the White
// Mage casts.
#include "pawn/finder_rules.h"
#include "pawn/teleport_rules.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cctype>
#include <limits>
#include <random>
#include <set>
#include <string>
#include <vector>

using namespace pawn::teleport;
using namespace pawn::teleport::rules;

namespace
{
    // Beside her, alive, on foot, with the crystal: one who goes
    auto standing(const float distance = 3.0f) -> Member
    {
        return Member{ .distance = distance };
    }

    auto lower(std::string s) -> std::string
    {
        for (auto& c : s)
        {
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        }
        return " " + s + " ";
    }
} // namespace

TEST_CASE("Teleport: the six crags, by the goal's log", "[cardian][teleport]")
{
    REQUIRE(kCrags.size() == 6);
    for (uint8 log = 0; log < kCrags.size(); ++log)
    {
        REQUIRE(cragOf(log) != nullptr);
        CHECK(cragOf(log)->log == log);
    }
    CHECK(cragOf(6) == nullptr);
    CHECK(cragOf(255) == nullptr);

    // the game's own numbers: the spell, the gate crystal, the zone it lands
    // in and the TELEPORT effect's power (xi.teleport.id)
    CHECK(cragOf(0)->name == "Holla");
    CHECK(cragOf(0)->spell == 122);
    CHECK(cragOf(0)->keyItem == 352);
    CHECK(cragOf(0)->zone == 102); // La Theine Plateau
    CHECK(cragOf(0)->teleport == 2);
    CHECK(cragOf(1)->name == "Dem");
    CHECK(cragOf(1)->spell == 123);
    CHECK(cragOf(1)->keyItem == 353);
    CHECK(cragOf(1)->zone == 108); // Konschtat Highlands
    CHECK(cragOf(1)->teleport == 1);
    CHECK(cragOf(2)->name == "Mea");
    CHECK(cragOf(2)->spell == 124);
    CHECK(cragOf(2)->keyItem == 354);
    CHECK(cragOf(2)->zone == 117); // Tahrongi Canyon
    CHECK(cragOf(2)->teleport == 5);
    CHECK(cragOf(3)->name == "Altep");
    CHECK(cragOf(3)->spell == 121);
    CHECK(cragOf(3)->keyItem == 357);
    CHECK(cragOf(3)->zone == 114); // Eastern Altepa Desert
    CHECK(cragOf(3)->teleport == 6);
    CHECK(cragOf(4)->name == "Yhoat");
    CHECK(cragOf(4)->spell == 120);
    CHECK(cragOf(4)->keyItem == 356);
    CHECK(cragOf(4)->zone == 124); // Yhoator Jungle
    CHECK(cragOf(4)->teleport == 3);
    CHECK(cragOf(5)->name == "Vahzl");
    CHECK(cragOf(5)->spell == 139);
    CHECK(cragOf(5)->keyItem == 355);
    CHECK(cragOf(5)->zone == 112); // Xarcabard
    CHECK(cragOf(5)->teleport == 4);
}

TEST_CASE("Teleport: her jobs cast it at the spell's level, main or support", "[cardian][teleport]")
{
    // A White Mage at 36 casts the crags' Teleport; at 35 not yet
    CHECK(castsAt(36, 36, 255, 18));
    CHECK(castsAt(36, 50, 255, 25));
    CHECK_FALSE(castsAt(36, 35, 255, 17));
    // a support White Mage at 36, under a main job that never casts it
    CHECK(castsAt(255, 72, 36, 36));
    CHECK_FALSE(castsAt(255, 70, 36, 35));
    // neither job casts it, whatever the level
    CHECK_FALSE(castsAt(255, 99, 255, 99));
}

TEST_CASE("Teleport: she answers when she knows it, casts it and has the MP", "[cardian][teleport]")
{
    CHECK(answers(true, true, 75, 75));
    CHECK(answers(true, true, 400, 75));
    CHECK_FALSE(answers(true, true, 74, 75));  // short of MP as she stands
    CHECK_FALSE(answers(false, true, 400, 75)); // never learned it
    CHECK_FALSE(answers(true, false, 400, 75)); // not at its level
}

TEST_CASE("Teleport: the spell decides who goes", "[cardian][teleport]")
{
    CHECK(stayOf(standing()) == Stay::Goes);
    CHECK(stayOf(standing(kRadius)) == Stay::Goes); // its edge is inside
    CHECK(stayOf(standing(kRadius + 0.1f)) == Stay::OutOfRange);

    Member above = standing();
    above.rise   = kRise; // the game's vertical cap
    CHECK(stayOf(above) == Stay::OutOfRange);
    above.rise = kRise - 0.5f;
    CHECK(stayOf(above) == Stay::Goes);

    Member m = standing();
    m.mounted = true;
    CHECK(stayOf(m) == Stay::Mounted);
    m.hasCrystal = false;
    CHECK(stayOf(m) == Stay::NoCrystal); // the reason that lasts is told first
    m.alive = false;
    CHECK(stayOf(m) == Stay::KnockedOut);
    m.sameZone = false;
    CHECK(stayOf(m) == Stay::Elsewhere);
}

TEST_CASE("Teleport: who stays, in plain words", "[cardian][teleport]")
{
    CHECK(reasonOf(Stay::NoCrystal, "Holla") == "No gate crystal for Holla.");
    CHECK(reasonOf(Stay::OutOfRange, "Dem") == "Too far away.");
    CHECK(reasonOf(Stay::Mounted, "Mea") == "Riding a chocobo.");
    CHECK(reasonOf(Stay::KnockedOut, "Mea") == "Knocked out.");
    CHECK(reasonOf(Stay::Elsewhere, "Mea") == "In another area.");
    CHECK(reasonOf(Stay::Goes, "Mea").empty());
    for (const auto stay : { Stay::Elsewhere, Stay::KnockedOut, Stay::NoCrystal, Stay::Mounted, Stay::OutOfRange })
    {
        const auto words = lower(reasonOf(stay, "Holla"));
        CHECK(words.find(" she ") == std::string::npos);
        CHECK(words.find(" her ") == std::string::npos);
        CHECK(words.find("cardian") == std::string::npos);
    }
}

TEST_CASE("Teleport: she casts once he is gathered, and the others gathered or given time", "[cardian][teleport]")
{
    const std::vector<Member> nobody;

    // He comes first: out of her reach, or near its edge, she waits for him
    CHECK(ready(standing(kRadius + 5.0f), nobody, 0.0) == Ready::WaitForPlayer);
    CHECK(ready(standing(kRadius - kGatherMargin + 0.5f), nobody, 0.0) == Ready::WaitForPlayer);
    // On a chocobo beside her, he is asked to get down
    Member riding = standing();
    riding.mounted = true;
    CHECK(ready(riding, nobody, 0.0) == Ready::PlayerMounted);

    // Gathered, with nobody else to wait for
    CHECK(ready(standing(), nobody, 0.0) == Ready::Cast);

    // A member who could go but stands off waits her a while...
    const std::vector<Member> straggler{ standing(), standing(kRadius + 4.0f) };
    CHECK(ready(standing(), straggler, 0.0) == Ready::WaitForOthers);
    CHECK(ready(standing(), straggler, kPatience - 1.0) == Ready::WaitForOthers);
    // ...and no longer than the patience
    CHECK(ready(standing(), straggler, kPatience) == Ready::Cast);

    // Those who could not go however near -- no crystal, knocked out, in
    // another zone, mounted -- never hold her
    Member noCrystal = standing(30.0f);
    noCrystal.hasCrystal = false;
    Member down = standing(30.0f);
    down.alive = false;
    Member away = standing(300.0f);
    away.sameZone = false;
    Member mounted = standing(30.0f);
    mounted.mounted = true;
    CHECK(ready(standing(), { noCrystal, down, away, mounted }, 0.0) == Ready::Cast);
}

TEST_CASE("Teleport: the finder's goal is heard in every city and town", "[cardian][teleport][finder]")
{
    using pawn::finder::Goal;
    namespace fr = pawn::finder::rules;
    const Goal teleport{ Goal::Kind::Teleport, 0 };
    CHECK(fr::hears(true, 0, teleport));
    CHECK(fr::hears(true, 1, teleport));
    CHECK(fr::hears(true, 2, teleport));
    CHECK_FALSE(fr::hears(false, 0, teleport)); // in the field: busy
}

TEST_CASE("Teleport: the few nearest answer, every one a yes, nearest first", "[cardian][teleport][finder]")
{
    namespace fr = pawn::finder::rules;
    std::mt19937          rng(11);
    std::vector<fr::Seat> pool;
    for (std::size_t i = 0; i < 12; ++i)
    {
        pool.push_back(fr::Seat{ .index = i, .yes = i % 4 != 0, .hops = static_cast<uint32>(i % 5) });
    }
    pool[2].hops = std::numeric_limits<uint32>::max(); // no line reaches her
    const auto picked = fr::pickNearest(pool, kAnswersAtMost, rng);
    REQUIRE(picked.seats.size() == kAnswersAtMost);
    std::set<std::size_t> seen;
    for (const auto& seat : picked.seats)
    {
        CHECK(seat.yes);
        seen.insert(seat.index);
    }
    CHECK(seen.size() == kAnswersAtMost); // nobody twice
    CHECK(std::is_sorted(picked.seats.begin(), picked.seats.end(), [](const fr::Seat& a, const fr::Seat& b) { return a.hops < b.hops; }));
    // the yeses at no zone line and at one: 1, 5, 6, 9, 10 -- three of them
    for (const auto& seat : picked.seats)
    {
        CHECK(seat.hops <= 1);
    }
    CHECK_FALSE(picked.friendSeat.has_value());
    CHECK_FALSE(picked.affinitySeat.has_value());

    // Fewer than the few: all of them; nobody willing: nobody
    std::vector<fr::Seat> two{ fr::Seat{ .index = 0, .yes = true, .hops = 3 }, fr::Seat{ .index = 1, .yes = true, .hops = 1 } };
    const auto            both = fr::pickNearest(two, kAnswersAtMost, rng);
    REQUIRE(both.seats.size() == 2);
    CHECK(both.seats[0].index == 1);
    CHECK(fr::pickNearest({ fr::Seat{ .index = 0, .yes = false } }, kAnswersAtMost, rng).seats.empty());
}

TEST_CASE("Teleport: her price is 500 to 1,500 gil in hundreds", "[cardian][teleport]")
{
    std::set<uint32> prices;
    for (uint32 roll = 0; roll <= kPriceSteps; ++roll)
    {
        const auto price = priceOf(roll);
        CHECK(price >= kPriceLeast);
        CHECK(price <= kPriceMost);
        CHECK(price % 100 == 0);
        prices.insert(price);
    }
    CHECK(prices.size() == 11); // every hundred from 500 to 1,500
    CHECK(priceOf(0) == 500);
    CHECK(priceOf(kPriceSteps) == 1500);
    CHECK(priceOf(kPriceSteps + 1) == 500); // a roll past the last wraps, never past the range
}

TEST_CASE("Teleport: a White Mage names the same price every time she answers a crag", "[cardian][teleport]")
{
    std::set<uint32> prices;
    for (uint32 charid = 1000; charid < 1200; ++charid)
    {
        CHECK(priceFor(charid, 0) == priceFor(charid, 0));
        const auto price = priceFor(charid, 0);
        CHECK(price >= kPriceLeast);
        CHECK(price <= kPriceMost);
        prices.insert(price);
    }
    CHECK(prices.size() == 11); // across the world every price is someone's
}

TEST_CASE("Teleport: gil as people write it", "[cardian][teleport]")
{
    CHECK(gilWords(0) == "0");
    CHECK(gilWords(500) == "500");
    CHECK(gilWords(1000) == "1,000");
    CHECK(gilWords(1500) == "1,500");
    CHECK(gilWords(1234567) == "1,234,567");
}

TEST_CASE("Teleport: her answer names the crag five ways, never the price; her price said again five ways", "[cardian][teleport]")
{
    std::set<std::string> answers;
    for (uint32 seed = 0; seed < 10; ++seed)
    {
        const auto quote = quoteLine(seed, "Holla");
        CHECK(quote.find("Holla") != std::string::npos);
        CHECK(quote.find("gil") == std::string::npos);
        answers.insert(quote);
    }
    CHECK(answers.size() == 5);
    std::set<std::string> said;
    for (uint32 seed = 0; seed < 10; ++seed)
    {
        const auto line = priceLine(seed, 1000);
        CHECK(line.find("1,000") != std::string::npos);
        said.insert(line);
    }
    CHECK(said.size() == 5);
    CHECK(priceLine(0, 1000) == "That'll be 1,000g!");
}

TEST_CASE("Teleport: his trade pays her price exactly, and nothing else", "[cardian][teleport]")
{
    CHECK(paysExactly(1200, false, 1200));
    CHECK_FALSE(paysExactly(1100, false, 1200)); // short
    CHECK_FALSE(paysExactly(1300, false, 1200)); // over: exact means exact
    CHECK_FALSE(paysExactly(1200, true, 1200));  // with an item beside it
    CHECK_FALSE(paysExactly(0, true, 1200));     // an item and no gil
    CHECK(wrongTradeLine(0, 1200).find("1,200") != std::string::npos);
    CHECK(wrongTradeLine(1, 1200).find("1,200") != std::string::npos);
}

TEST_CASE("Teleport: hired in town, she keeps to town until she casts", "[cardian][teleport]")
{
    CHECK(keepsToTown(true, false, true));        // still in town
    CHECK_FALSE(keepsToTown(true, false, false)); // he left before her cast: she goes
    CHECK(keepsToTown(true, true, false));        // the spell under way, or landed
    CHECK(keepsToTown(false, false, false));      // hired out of town: no such rule
}
