// Cardian: the linkshell club's rules (pawn/club_math.h): the game's linkshell
// items and the shell a pearl belongs to, where a linkshell is sold, what a
// recruit tells him on her way, a rank catch-up through her nation's
// missions, and a pearl given by the game's own trade (pawn/club_trade.h):
// the offer read off the game's own trade packet, and what she makes of it.
#include "map/pawn/club_math.h"
#include "map/pawn/club_trade.h"

#include "map/items/item.h"
#include "map/items/item_linkshell.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <set>
#include <string>
#include <string_view>
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

TEST_CASE("Club: a slot of his trade offer read off the game's own packet", "[cardian][club]")
{
    SECTION("a Linkpearl carries its shell and its kind")
    {
        CItemLinkshell pearl(kLinkpearl);
        pearl.SetLSID(0x00012345);
        pearl.SetLSType(LSTYPE_LINKPEARL);
        GP_SERV_COMMAND_ITEM_TRADE_LIST packet(&pearl, 3, 1);
        const auto                      read = offeredSlot(packet);
        CHECK(read.slot == 3);
        CHECK(read.offered.item == kLinkpearl);
        CHECK(read.offered.qty == 1);
        CHECK(read.offered.lsid == 0x00012345u);
        CHECK(read.offered.lsType == kLsTypeLinkpearl);
    }

    SECTION("a broken pearl says so")
    {
        CItemLinkshell pearl(kLinkpearl);
        pearl.SetLSID(77);
        pearl.SetLSType(LSTYPE_BROKEN);
        GP_SERV_COMMAND_ITEM_TRADE_LIST packet(&pearl, 1, 1);
        CHECK(offeredSlot(packet).offered.lsType == kLsTypeBroken);
    }

    SECTION("any other item: its id and how many, no shell")
    {
        CItem water(4509);
        GP_SERV_COMMAND_ITEM_TRADE_LIST packet(&water, 2, 12);
        const auto                      read = offeredSlot(packet);
        CHECK(read.slot == 2);
        CHECK(read.offered.item == 4509);
        CHECK(read.offered.qty == 12);
        CHECK(read.offered.lsid == 0);
    }

    SECTION("a slot emptied")
    {
        GP_SERV_COMMAND_ITEM_TRADE_LIST packet(nullptr, 4, 0);
        const auto                      read = offeredSlot(packet);
        CHECK(read.slot == 4);
        CHECK(read.offered.qty == 0);
        CHECK(read.offered.item == 0);
    }
}

TEST_CASE("Club: wearing a pearl already, she turns his trade request down", "[cardian][club]")
{
    const std::set<uint32_t> his{ 100, 200 };
    CHECK(judgeRequest(his, 0) == Verdict::Take);                // no pearl: the window opens for the handover
    CHECK(judgeRequest(his, 200) == Verdict::HasHisPearl);       // his already
    CHECK(judgeRequest(his, 300) == Verdict::PearledElsewhere);  // another shell's
}

TEST_CASE("Club: she takes one Linkpearl of his shell by trade, and nothing else", "[cardian][club]")
{
    const std::set<uint32_t>         his{ 100, 200 };
    std::array<Offered, kTradeSlots> slots{};
    const Offered                    pearl{ kLinkpearl, 1, 100, kLsTypeLinkpearl };

    CHECK(judgeOffer(slots, his, 0) == Verdict::NothingOffered);

    slots[1] = pearl;
    CHECK(judgeOffer(slots, his, 0) == Verdict::Take);
    slots[5] = pearl; // in any slot
    slots[1] = {};
    CHECK(judgeOffer(slots, his, 0) == Verdict::Take);

    // already wearing one: his, or another shell's
    CHECK(judgeOffer(slots, his, 200) == Verdict::HasHisPearl);
    CHECK(judgeOffer(slots, his, 999) == Verdict::PearledElsewhere);

    // something beside the pearl: gil (slot 0) or an item
    slots[0] = Offered{ 0xFFFF, 500 };
    CHECK(judgeOffer(slots, his, 0) == Verdict::NotOnlyAPearl);
    slots[0] = {};
    slots[2] = Offered{ 4509, 1 };
    CHECK(judgeOffer(slots, his, 0) == Verdict::NotOnlyAPearl);
    slots[2] = {};

    // two pearls in one slot, or a sack, or the shell itself
    slots[5] = Offered{ kLinkpearl, 2, 100, kLsTypeLinkpearl };
    CHECK(judgeOffer(slots, his, 0) == Verdict::NotOnlyAPearl);
    slots[5] = Offered{ kPearlsack, 1, 100, 2 };
    CHECK(judgeOffer(slots, his, 0) == Verdict::NotOnlyAPearl);
    slots[5] = Offered{ kLinkshell, 1, 100, 1 };
    CHECK(judgeOffer(slots, his, 0) == Verdict::NotOnlyAPearl);

    // a pearl of a shell he does not hold, or a broken one
    slots[5] = Offered{ kLinkpearl, 1, 300, kLsTypeLinkpearl };
    CHECK(judgeOffer(slots, his, 0) == Verdict::NotHisShell);
    slots[5] = Offered{ kLinkpearl, 1, 100, kLsTypeBroken };
    CHECK(judgeOffer(slots, his, 0) == Verdict::NotHisShell);
}

namespace
{
    // A line the player reads names no one: no "she", no "her", no codename
    void checkNamesNoOne(const std::string_view text)
    {
        CHECK_FALSE(text.empty());
        std::string lower(text);
        std::ranges::transform(lower, lower.begin(), [](const unsigned char c) { return static_cast<char>(std::tolower(c)); });
        lower = " " + lower + " ";
        CHECK(lower.find("cardian") == std::string::npos);
        CHECK(lower.find(" she ") == std::string::npos);
        CHECK(lower.find(" her ") == std::string::npos);
    }
} // namespace

TEST_CASE("Club: her tell when she declines a trade names no one", "[cardian][club]")
{
    for (const auto verdict : { Verdict::NotTrading, Verdict::TooSoon, Verdict::NothingOffered, Verdict::NotOnlyAPearl, Verdict::NotHisShell,
                                Verdict::HasHisPearl, Verdict::PearledElsewhere })
    {
        checkNamesNoOne(declineLine(verdict));
    }
    CHECK(declineLine(Verdict::Take).empty());
}

TEST_CASE("Club: a stranger's trade is turned down with a neutral word, another each try", "[cardian][club]")
{
    std::set<std::string_view> said;
    for (const auto line : kNoThanks)
    {
        checkNamesNoOne(line);
        said.insert(line);
    }
    CHECK(said.size() == kNoThanks.size());
    // the stranger and the one short of the lock hear the same kind of word
    CHECK(declineLine(Verdict::NotTrading, 40, 2) == noThanks(40, 2));
    CHECK(declineLine(Verdict::TooSoon, 40, 2) == noThanks(40, 2));
    // the same try, the same word; the next try, another
    CHECK(noThanks(40, 0) == noThanks(40, 0));
    CHECK(noThanks(40, 0) != noThanks(40, 1));
    CHECK(noThanks(40, static_cast<uint32_t>(kNoThanks.size())) == noThanks(40, 0));
    // the rest say what is wrong with the trade
    CHECK(declineLine(Verdict::HasHisPearl, 40, 2) == "I already have your linkpearl!");
}

TEST_CASE("Club: a recruit is one of the world's at the pearl's lock", "[cardian][club]")
{
    // the defaults: affinity 6 and two story missions together
    CHECK(qualifies(6, 2, 6, 2));
    CHECK(qualifies(9, 5, 6, 2));
    CHECK_FALSE(qualifies(5, 2, 6, 2)); // affinity short
    CHECK_FALSE(qualifies(6, 1, 6, 2)); // a mission short
    CHECK_FALSE(qualifies(0, 0, 6, 2));
    CHECK(qualifies(0, 0, 0, 0)); // a lock set open lets anyone the two have partied with through
}

TEST_CASE("Club: a recruit's answer to joining, in her own words", "[cardian][club]")
{
    CHECK(kDecideMinMs < kDecideMaxMs);
    for (uint32_t charid = 1; charid <= kJoinYes.size(); ++charid)
    {
        checkNamesNoOne(joinYes(charid));
    }
    CHECK(joinYes(7) == joinYes(7 + static_cast<uint32_t>(kJoinYes.size()))); // the same recruit says the same thing
    checkNamesNoOne(joinLater());
}

TEST_CASE("Club: a recruit comes for her pearl, out of sight from afar", "[cardian][club]")
{
    // a zone line or three takes a little longer each; past three, or with
    // no route, it counts as three; the same zone as one
    CHECK(tripSeconds(1) == kTripSecondsPerZone);
    CHECK(tripSeconds(3) == 3 * kTripSecondsPerZone);
    CHECK(tripSeconds(7) == tripSeconds(kTrekZones));
    CHECK(tripSeconds(UINT32_MAX) == tripSeconds(kTrekZones));
    CHECK(tripSeconds(0) == kTripSecondsPerZone);
    // she waits at his side closer than she comes in, and fades farther off
    CHECK(kVisitRingFar < kVisitArrive);
    CHECK(kVisitArrive < kVisitLeaveTo);
    CHECK(kVisitLeaveTo < kVisitFadeAt);
    CHECK(kVisitWaitSeconds < kVisitGiveUpSeconds);
    // her distance from him is hers, within the ring, and recruits differ
    std::set<float> rings;
    for (uint32_t charid = 100; charid < 108; ++charid)
    {
        CHECK(visitRing(charid) >= kVisitRingNear);
        CHECK(visitRing(charid) <= kVisitRingFar);
        CHECK(visitRing(charid) == visitRing(charid + 8));
        rings.insert(visitRing(charid));
    }
    CHECK(rings.size() == 8);
    // she greets him with one of the emotes, and says she is here
    std::set<uint8_t> emotes;
    for (uint32_t charid = 0; charid < kGreetEmotes.size(); ++charid)
    {
        emotes.insert(greetEmote(charid));
        checkNamesNoOne(imHere(charid * static_cast<uint32_t>(kGreetEmotes.size())));
    }
    CHECK(emotes.size() == kGreetEmotes.size());
    CHECK(emotes.contains(8));  // wave
    CHECK(emotes.contains(43)); // hurray
    std::set<std::string_view> said;
    for (uint32_t charid = 1; charid <= kCatchYouLater.size(); ++charid)
    {
        checkNamesNoOne(catchYouLater(charid));
        said.insert(catchYouLater(charid));
    }
    CHECK(said.size() == kCatchYouLater.size());
    CHECK(catchYouLater(5) == catchYouLater(5 + static_cast<uint32_t>(kCatchYouLater.size())));
}
