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

// The puller (ROADMAP, Pulling, P1): who hunts at a camp, the con in the
// game's words and phrases, the chat line and its codes, the tagging spot,
// the shot and the run back, and her pulling kit.

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "map/pawn/chat_codes.h"
#include "map/pawn/pull_math.h"

#include <array>
#include <string>
#include <vector>

using namespace cardian::pull;
using cardian::chat::autoTranslate;
using cardian::chat::callBytes;
using cardian::chat::keptCall;
using cardian::chat::kNoCall;

TEST_CASE("Cardian pull: at a camp only the Puller seat hunts", "[cardian][pull]")
{
    // Pull on, free roaming: as before, whoever the party's hunt reaches
    CHECK(hunts(true, false, false, true));
    CHECK(hunts(true, false, true, true));
    CHECK_FALSE(hunts(true, false, true, false));
    // Pull on, at a camp: the seat alone, whatever the hunt's own rule says
    CHECK(hunts(true, true, true, false));
    CHECK(hunts(true, true, true, true));
    CHECK_FALSE(hunts(true, true, false, true));
    // Hold: nobody, at a camp or not
    CHECK_FALSE(hunts(false, true, true, true));
    CHECK_FALSE(hunts(false, false, true, true));
}

TEST_CASE("Cardian pull: the con in the game's words and its phrase", "[cardian][pull]")
{
    CHECK(conWord(0) == "Too Weak");
    CHECK(conWord(1) == "Incredibly Easy Prey");
    CHECK(conWord(2) == "Easy Prey");
    CHECK(conWord(3) == "Decent Challenge");
    CHECK(conWord(4) == "Even Match");
    CHECK(conWord(5) == "Tough");
    CHECK(conWord(6) == "Very Tough");
    CHECK(conWord(7) == "Incredibly Tough");
    CHECK(conWord(8) == "?");

    // group 4 of the game's auto-translate dictionary
    CHECK(conPhrase(0) == 2); // Too weak
    CHECK(conPhrase(1) == 0); // Incredibly Easy Prey has no phrase
    CHECK(conPhrase(2) == 6); // Easy prey
    CHECK(conPhrase(3) == 5); // Decent challenge
    CHECK(conPhrase(4) == 1); // Even match
    CHECK(conPhrase(5) == 4); // Tough
    CHECK(conPhrase(6) == 7); // Very tough
    CHECK(conPhrase(7) == 3); // Incredibly tough
}

TEST_CASE("Cardian chat: a call sound and an auto-translate phrase as the client writes them", "[cardian][pull]")
{
    // the game's calls run <call0> to <call20>; none is apart from Call 0
    CHECK(callBytes(kNoCall).empty());
    CHECK(callBytes(21).empty());
    CHECK(callBytes(0) == "<call0>");
    CHECK(callBytes(11) == "<call11>");
    // the bytes the player's own "<call11>" arrived as
    const std::string eleven = callBytes(11);
    const std::array<uint8_t, 8> measured{ 0x3C, 0x63, 0x61, 0x6C, 0x6C, 0x31, 0x31, 0x3E };
    REQUIRE(eleven.size() == measured.size());
    for (std::size_t i = 0; i < measured.size(); ++i)
    {
        CHECK(static_cast<uint8_t>(eleven[i]) == measured[i]);
    }
    CHECK(callBytes(1) == "<call1>");
    CHECK(callBytes(20) == "<call20>");

    // a call is kept as given when the game has it, else none
    CHECK(keptCall(0) == 0);
    CHECK(keptCall(20) == 20);
    CHECK(keptCall(21) == kNoCall);
    CHECK(keptCall(kNoCall) == kNoCall);

    // "Incredibly tough" as the player's client sent it: FD 02 02 04 03 FD
    const std::string phrase = autoTranslate(4, 3);
    const std::array<uint8_t, 6> tough{ 0xFD, 0x02, 0x02, 0x04, 0x03, 0xFD };
    REQUIRE(phrase.size() == tough.size());
    for (std::size_t i = 0; i < tough.size(); ++i)
    {
        CHECK(static_cast<uint8_t>(phrase[i]) == tough[i]);
    }
}

TEST_CASE("Cardian pull: the pull line", "[cardian][pull]")
{
    CHECK(mobWords("Forest_Hare") == "Forest Hare");
    // the con as its phrase, the call after it
    CHECK(pullLine("Forest_Hare", 2, 14) == "Incoming! Forest Hare " + autoTranslate(4, 6) + " <call14>");
    // no call chosen: nothing after the con
    CHECK(pullLine("Forest_Hare", 4, kNoCall) == "Incoming! Forest Hare " + autoTranslate(4, 1));
    // Call 0 is a call of its own
    CHECK(pullLine("Forest_Hare", 4, 0) == "Incoming! Forest Hare " + autoTranslate(4, 1) + " <call0>");
    // the con with no phrase, in plain words
    CHECK(pullLine("Bumblebee", 1, 3) == "Incoming! Bumblebee (Incredibly Easy Prey) <call3>");
}

TEST_CASE("Cardian pull: the tagging spot", "[cardian][pull]")
{
    CHECK(shotReach(25.0f) == Catch::Approx(23.0f));
    CHECK(shotReach(2.0f) == Catch::Approx(1.0f));

    const position_t her(0.0f, 0.0f, 0.0f, 0, 0);
    // within reach and in sight: she shoots where she stands
    CHECK_FALSE(tagSpot(her, position_t(20.0f, 0.0f, 0.0f, 0, 0), 23.0f, true).has_value());
    // out of sight: toward the mob itself, until she sees it
    const auto blind = tagSpot(her, position_t(10.0f, 0.0f, 5.0f, 0, 0), 23.0f, false);
    REQUIRE(blind.has_value());
    CHECK(blind->x == Catch::Approx(10.0f));
    CHECK(blind->z == Catch::Approx(5.0f));
    // too far: on the line to it, half a yalm inside reach of it
    const auto spot = tagSpot(her, position_t(40.0f, 0.0f, 0.0f, 0, 0), 23.0f, true);
    REQUIRE(spot.has_value());
    CHECK(spot->x == Catch::Approx(17.5f));
    CHECK(distance(*spot, position_t(40.0f, 0.0f, 0.0f, 0, 0)) == Catch::Approx(22.5f));
}

TEST_CASE("Cardian pull: the shot", "[cardian][pull]")
{
    CHECK(shot(0, 0.0, false) == Shot::Fire);
    CHECK(shot(1, 2.0, false) == Shot::Wait);
    CHECK(shot(1, kReshootSeconds, false) == Shot::Fire);
    CHECK(shot(static_cast<uint8_t>(kMaxShots - 1), kReshootSeconds + 1.0, false) == Shot::Fire);
    CHECK(shot(kMaxShots, kReshootSeconds - 1.0, false) == Shot::Wait);
    CHECK(shot(kMaxShots, kReshootSeconds, false) == Shot::GiveUp);
    // on the party, whatever the count: it is coming
    CHECK(shot(0, 0.0, true) == Shot::Coming);
    CHECK(shot(kMaxShots, 100.0, true) == Shot::Coming);
}

TEST_CASE("Cardian pull: the run back and the hand-off", "[cardian][pull]")
{
    const double limit = backSeconds(40.0f);
    // on her, far out: she runs
    CHECK(back(true, true, true, 20.0f, 5.0, limit) == Back::Run);
    // the tank's Provoke turned it: handed
    CHECK(back(true, false, true, 12.0f, 5.0, limit) == Back::Handed);
    // still on her at the landing point: the camp takes it
    CHECK(back(true, true, true, kHomeYalms, 5.0, limit) == Back::Home);
    // it stopped fighting, or fights someone else's party: lost
    CHECK(back(false, false, false, 20.0f, 5.0, limit) == Back::Lost);
    CHECK(back(true, false, false, 20.0f, 5.0, limit) == Back::Lost);
    // too long on the way
    CHECK(back(true, true, true, 20.0f, limit + 1.0, limit) == Back::Late);
}

TEST_CASE("Cardian pull: how near a mob stands to the way back", "[cardian][pull]")
{
    const position_t a(0.0f, 0.0f, 0.0f, 0, 0);
    const position_t b(100.0f, 0.0f, 0.0f, 0, 0);
    // beside the line, across the ground: the height counts for nothing
    CHECK(segmentDistance(a, b, position_t(50.0f, 30.0f, 12.0f, 0, 0)) == Catch::Approx(12.0f));
    // past either end: to the end itself
    CHECK(segmentDistance(a, b, position_t(-6.0f, 0.0f, 8.0f, 0, 0)) == Catch::Approx(10.0f));
    CHECK(segmentDistance(a, b, position_t(103.0f, 0.0f, 4.0f, 0, 0)) == Catch::Approx(5.0f));
    // a way of no length: the distance to its point
    CHECK(segmentDistance(a, a, position_t(3.0f, 0.0f, 4.0f, 0, 0)) == Catch::Approx(5.0f));
}

TEST_CASE("Cardian pull: she runs past the landing point, so the mob stops on it", "[cardian][pull]")
{
    const position_t landing(10.0f, 5.0f, 0.0f, 0, 0);
    // coming in from 40 yalms out along x: kRunPast further on
    const auto past = pastLanding(landing, position_t(50.0f, 7.0f, 0.0f, 0, 0));
    CHECK(past.x == Catch::Approx(10.0f - kRunPast));
    CHECK(past.z == Catch::Approx(0.0f));
    CHECK(past.y == Catch::Approx(5.0f)); // the landing's height: the walker finds the floor
    // coming in on a slant: the same distance past, on her line
    const auto slant = pastLanding(landing, position_t(40.0f, 5.0f, 40.0f, 0, 0));
    CHECK(distance(slant, landing) == Catch::Approx(kRunPast));
    CHECK(slant.x < landing.x);
    CHECK(slant.z < landing.z);
    // already on it: the landing point itself
    const auto here = pastLanding(landing, landing);
    CHECK(here.x == Catch::Approx(landing.x));
}

TEST_CASE("Cardian pull: a far pull is given the time its way takes", "[cardian][pull]")
{
    // a margin, and the way at a slow run's pace: a mob at the list's reach
    // is given more than a minute out, where one at the camp's edge is not
    CHECK(outSeconds(0.0f) == Catch::Approx(20.0));
    CHECK(outSeconds(kPullReach) == Catch::Approx(20.0 + kPullReach / kRunPace));
    CHECK(outSeconds(kPullReach) > 60.0);
    CHECK(backSeconds(kPullReach) > outSeconds(kPullReach));
}

TEST_CASE("Cardian pull: the Pull page's list", "[cardian][pull]")
{
    const std::vector<ListRow> list{ { "Forest_Hare", true }, { "Tunnel_Worm", false }, { "Bumblebee", true } };

    SECTION("only what is on, the first on the list first")
    {
        CHECK(rankOf(list, "Forest_Hare") == 0u);
        CHECK(rankOf(list, "Bumblebee") == 1u);
        CHECK_FALSE(rankOf(list, "Tunnel_Worm").has_value());
        CHECK_FALSE(rankOf(list, "Goblin_Thug").has_value());
        CHECK(anyOn(list));
        CHECK_FALSE(anyOn(std::vector<ListRow>{ { "Tunnel_Worm", false } }));
        CHECK_FALSE(anyOn(std::vector<ListRow>{}));
    }

    SECTION("kept as text, and read back")
    {
        CHECK(listText(list) == "Forest_Hare:1|Tunnel_Worm:0|Bumblebee:1");
        CHECK(parseList(listText(list)) == list);
        CHECK(parseList("").empty());
        // a broken field is skipped, the rest read
        CHECK(parseList("Forest_Hare:1|nonsense|Bumblebee:0") == std::vector<ListRow>{ { "Forest_Hare", true }, { "Bumblebee", false } });
    }

    SECTION("the page's rows first, then the kinds of another camp in their old order")
    {
        const std::vector<ListRow> sent{ { "Bumblebee", true }, { "Forest_Hare", false }, { "Wild_Sheep", true } };
        const std::vector<ListRow> before{ { "Goblin_Thug", true }, { "Forest_Hare", true }, { "Orcish_Fodder", false } };
        CHECK(mergeList(sent, before) == std::vector<ListRow>{ { "Bumblebee", true }, { "Forest_Hare", false }, { "Wild_Sheep", true },
                                                               { "Goblin_Thug", true }, { "Orcish_Fodder", false } });
    }
}

TEST_CASE("Cardian pull: the pick weighs the list against the walk", "[cardian][pull]")
{
    // a rabbit (third on the list) at the camp beats an orc (first) 80 yalms out
    CHECK(pickScore(5.0f, 2) < pickScore(80.0f, 0));
    // and an orc beats a rabbit as far
    CHECK(pickScore(80.0f, 0) < pickScore(80.0f, 2));
    // a place lower is kYalmsPerPlace nearer to tie
    CHECK(pickScore(50.0f, 1) == Catch::Approx(pickScore(80.0f, 0)));
}

TEST_CASE("Cardian pull: the kinds round the camp", "[cardian][pull]")
{
    const std::vector<Seen> seen{
        { "Tunnel_Worm", 2, 5.0f }, { "Forest_Hare", 3, 20.0f }, { "Tunnel_Worm", 3, 7.0f },
        { "Forest_Hare", 2, 10.0f }, { "Wild_Sheep", 4, 30.0f }, { "Bumblebee", 1, 8.0f },
    };

    SECTION("each name once, its con from the easiest to the toughest, its average distance")
    {
        const auto kinds = kindsOf(seen, {});
        REQUIRE(kinds.size() == 4);
        // none on his list: nearest first
        CHECK(kinds[0].name == "Tunnel_Worm");
        CHECK(kinds[0].conLow == 2);
        CHECK(kinds[0].conHigh == 3);
        CHECK(kinds[0].count == 2);
        CHECK(kinds[0].distance == Catch::Approx(6.0f));
        CHECK_FALSE(kinds[0].on);
        CHECK(kinds[1].name == "Bumblebee");
        CHECK(kinds[2].name == "Forest_Hare");
        CHECK(kinds[2].distance == Catch::Approx(15.0f));
        CHECK(kinds[3].name == "Wild_Sheep");
    }

    SECTION("his list's kinds first in its order, a kind of it not here left out")
    {
        const std::vector<ListRow> list{ { "Wild_Sheep", false }, { "Goblin_Thug", true }, { "Forest_Hare", true } };
        const auto                 kinds = kindsOf(seen, list);
        REQUIRE(kinds.size() == 4);
        CHECK(kinds[0].name == "Wild_Sheep");
        CHECK_FALSE(kinds[0].on);
        CHECK(kinds[1].name == "Forest_Hare");
        CHECK(kinds[1].on);
        CHECK(kinds[2].name == "Tunnel_Worm");
        CHECK(kinds[3].name == "Bumblebee");
    }
}

TEST_CASE("Cardian pull: the con is read at the party's average level under Cardian's exp rule", "[cardian][pull]")
{
    const std::vector<uint8_t> party{ 20, 23, 25 };
    CHECK(conLevel(true, 20, party) == 22); // 68 / 3, rounded down
    CHECK(conLevel(false, 20, party) == 20);
    CHECK(conLevel(true, 20, std::vector<uint8_t>{}) == 20);
}

TEST_CASE("Cardian pull: the pipeline lands the next mob as the mages can stand", "[cardian][pull]")
{
    // A mage at 100 of 200 MP, the next fight costing her 120, spending 1
    // MP a second while the fight lasts; rest ticks every 10 s, the first
    // empty, then 12, 13, 14, 15 ...
    Mage m;
    m.mp             = 100.0;
    m.maxMp          = 200.0;
    m.need           = 120.0;
    m.spendPerSecond = 1.0;

    SECTION("with a fight on: she spends through it, kneels as it ends, and rests to her need")
    {
        // 80 MP at its end, 40 short: the fifth tick (12+13+14+15 = 54) covers it,
        // 50 s into a kneel begun a second after the fight's 20 s
        CHECK(readyAt(m, 20.0) == Catch::Approx(20.0 + 1.0 + 50.0));
        const auto arrival = wantArrive(std::vector<Mage>{ m }, 20.0);
        CHECK(arrival.in == Catch::Approx(71.0 - kRestInto));
        CHECK(arrival.pacer == 0);
    }

    SECTION("between fights: her kneel goes on, or she kneels now")
    {
        Mage down     = m;
        down.mp       = 100.0;
        down.kneeling = true;
        down.ticks    = 2;
        down.nextTick = 4.0;
        // 20 short: ticks three and four (13 + 14) cover it, the fourth 14 s from now
        CHECK(readyAt(down, 0.0) == Catch::Approx(14.0));
        // kneeling through a fight too: her kneel goes on, not a fresh one after it
        CHECK(readyAt(down, 30.0) == Catch::Approx(14.0));
        CHECK(wantArrive(std::vector<Mage>{ down }, 30.0).in == Catch::Approx(30.0)); // never before the fight ends
        Mage full = m;
        full.mp   = 150.0;
        CHECK(readyAt(full, 0.0) == Catch::Approx(0.0));
    }

    SECTION("the mage ready last paces the pull")
    {
        Mage early = m;
        early.mp   = 180.0; // 60 over her need by the fight's end: ready as it ends
        Mage late  = m;     // 40 short at the fight's end: ready 71 s from now
        const auto arrival = wantArrive(std::vector<Mage>{ late, early }, 20.0);
        CHECK(arrival.in == Catch::Approx(71.0 - kRestInto));
        CHECK(arrival.pacer == 0);
        const auto swapped = wantArrive(std::vector<Mage>{ early, late }, 20.0);
        CHECK(swapped.in == Catch::Approx(71.0 - kRestInto));
        CHECK(swapped.pacer == 1);
    }

    SECTION("never before the fight under way ends, and with no mage at its end")
    {
        Mage full = m;
        full.mp   = 200.0;
        full.spendPerSecond = 0.0;
        CHECK(wantArrive(std::vector<Mage>{ full }, 30.0).in == Catch::Approx(30.0));
        CHECK(wantArrive(std::vector<Mage>{ full }, 30.0).pacer == -1);
        CHECK(wantArrive(std::vector<Mage>{}, 12.0).in == Catch::Approx(12.0));
        CHECK(wantArrive(std::vector<Mage>{}, 0.0).in == Catch::Approx(0.0));
    }

    SECTION("planned backward: she sets out, and tags, so it lands on time")
    {
        // 5 yalms a second: 40 out and 50 back are 9.6 s and 12 s of way
        CHECK(runSeconds(40.0f, 5.0) == Catch::Approx(40.0 * kWayFactor / 5.0));
        CHECK(leaveIn(60.0, 40.0f, 50.0f, 5.0) == Catch::Approx(60.0 - (9.6 + kAimSeconds + 12.0) - kSlackSeconds));
        CHECK(leaveIn(10.0, 40.0f, 50.0f, 5.0) < 0.0); // late already: she goes now
        CHECK(shootIn(30.0, 50.0f, 5.0) == Catch::Approx(30.0 - kAimSeconds - 12.0));
    }
}

TEST_CASE("Cardian pull: a Bard's song", "[cardian][pull]")
{
    // an Elegy, which does no damage over time, once she has one; Foe Requiem before
    CHECK(pullSong([](const uint16_t id) { return id == 368 || id == 369 || id == 421; }) == 421);
    CHECK(pullSong([](const uint16_t id) { return id == 368 || id == 369; }) == 369);
    CHECK(pullSong([](const uint16_t id) { return id == 368; }) == 368);
    CHECK(pullSong([](const uint16_t) { return false; }) == 0);
}

namespace
{
    // the map's own (kPebble, kDart, kShortbow, kStoneArrow) are pull_math.h's
    constexpr uint16_t kCrossbow    = 17216;
    constexpr uint16_t kBolt        = 17339;
    constexpr uint16_t kGun         = 17257;
    constexpr uint16_t kBullet      = 17340;
    constexpr uint16_t kBoomerang   = 17280;
    constexpr uint16_t kShuriken    = 17301;
    constexpr uint16_t kTiphiaSting = 18242;
    constexpr uint16_t kHarp        = 17344;

    auto ranged(const uint16_t id, const uint8_t skill, const uint8_t level, const uint8_t subskill = 0) -> Held
    {
        return Held{ .id = id, .ammo = false, .skill = skill, .subskill = subskill, .level = level, .count = 1, .wearable = true };
    }

    auto ammo(const uint16_t id, const uint8_t skill, const uint32_t count, const uint8_t level = 1, const uint8_t subskill = 0) -> Held
    {
        return Held{ .id = id, .ammo = true, .skill = skill, .subskill = subskill, .level = level, .count = count, .wearable = true };
    }

    auto worn(Held h) -> Held
    {
        h.worn = true;
        return h;
    }

    auto planned(Held h) -> Held
    {
        h.planned = true;
        return h;
    }

    auto unwearable(Held h) -> Held
    {
        h.wearable = false;
        return h;
    }

    // The map's fallbacks: a coarse boomerang, pebbles, darts, a shortbow and
    // stone arrows; the job decides which she wears
    auto fallbacks(const bool pebble, const bool dart, const bool bow, const bool boomerang = false) -> std::vector<Fallback>
    {
        return { { kCoarseBoomerang, 0, boomerang }, { 0, kPebble, pebble }, { 0, kDart, dart }, { kShortbow, kStoneArrow, bow } };
    }
} // namespace

TEST_CASE("Cardian pull: the kit", "[cardian][pull]")
{
    const auto all = fallbacks(true, true, true);

    SECTION("what she wears already shoots: kept")
    {
        const std::vector<Held> ranger{ worn(ranged(kShortbow, kArchery, 1)), worn(ammo(kStoneArrow, kArchery, 99)), ranged(kCrossbow, kMarksmanship, 30) };
        CHECK(chooseKit(ranger, all) == Kit{ kShortbow, kStoneArrow });
        const std::vector<Held> ninja{ worn(ammo(kShuriken, kThrowing, 50, 18, 3)) };
        CHECK(chooseKit(ninja, all) == Kit{ 0, kShuriken });
        const std::vector<Held> thrower{ worn(ranged(kBoomerang, kThrowing, 14)) };
        CHECK(chooseKit(thrower, all) == Kit{ kBoomerang, 0 });
    }

    SECTION("a bow worn with its quiver empty, or an instrument, does not shoot")
    {
        const std::vector<Held> empty{ worn(ranged(kShortbow, kArchery, 1)), ammo(kStoneArrow, kArchery, 0) };
        CHECK(chooseKit(empty, all) == Kit{ 0, kPebble, true });
        const std::vector<Held> bard{ worn(ranged(kHarp, 41, 1)) };
        CHECK(chooseKit(bard, all) == Kit{ 0, kPebble, true });
    }

    SECTION("the census's pull kit before anything else she carries")
    {
        const std::vector<Held> thief{ worn(ammo(kTiphiaSting, 0, 1)), planned(ranged(kCrossbow, kMarksmanship, 1)), planned(ammo(kBolt, kMarksmanship, 99)),
                                       ranged(kShortbow, kArchery, 40), ammo(kStoneArrow, kArchery, 99) };
        CHECK(chooseKit(thief, all) == Kit{ kCrossbow, kBolt });
    }

    SECTION("the best bow or gun she carries, with ammunition for it")
    {
        // bolts are a crossbow's, not a gun's
        const std::vector<Held> marks{ ranged(kGun, kMarksmanship, 40, 1), ranged(kCrossbow, kMarksmanship, 1, 0), ammo(kBolt, kMarksmanship, 99, 1, 0) };
        CHECK(chooseKit(marks, all) == Kit{ kCrossbow, kBolt });
        const std::vector<Held> both{ ranged(kGun, kMarksmanship, 40, 1), ranged(kCrossbow, kMarksmanship, 1, 0), ammo(kBolt, kMarksmanship, 99, 1, 0),
                                      ammo(kBullet, kMarksmanship, 12, 1, 1) };
        CHECK(chooseKit(both, all) == Kit{ kGun, kBullet });
        // a bow she may not wear is passed over
        const std::vector<Held> barred{ unwearable(ranged(kShortbow, kArchery, 50)), ammo(kStoneArrow, kArchery, 99) };
        CHECK(chooseKit(barred, fallbacks(false, true, false)) == Kit{ 0, kDart, true });
    }

    SECTION("ammunition thrown by itself, worn under a ranged item it would share the shot with, is no worn kit")
    {
        // an instrument and shuriken both worn: the shuriken cannot be thrown
        // with the harp on, so the census's kit comes first
        const std::vector<Held> harp{ worn(ranged(kHarp, 41, 1)), worn(ammo(kShuriken, kThrowing, 50, 18, 3)), planned(ranged(kCrossbow, kMarksmanship, 1)),
                                      planned(ammo(kBolt, kMarksmanship, 99)) };
        CHECK(chooseKit(harp, all) == Kit{ kCrossbow, kBolt });
        // with nothing better, the shuriken, the harp coming off for it
        const std::vector<Held> harpOnly{ worn(ranged(kHarp, 41, 1)), worn(ammo(kShuriken, kThrowing, 50, 18, 3)) };
        CHECK(chooseKit(harpOnly, all) == Kit{ 0, kShuriken });
    }

    SECTION("a weapon thrown by itself, then ammunition thrown by itself")
    {
        const std::vector<Held> boomerang{ ranged(kBoomerang, kThrowing, 14), ammo(kShuriken, kThrowing, 20, 18, 3) };
        CHECK(chooseKit(boomerang, all) == Kit{ kBoomerang, 0 });
        const std::vector<Held> shuriken{ ammo(kPebble, kThrowing, 99, 1), ammo(kShuriken, kThrowing, 20, 18, 3) };
        CHECK(chooseKit(shuriken, all) == Kit{ 0, kShuriken });
    }

    SECTION("nothing of her own: the map's, the first her job wears")
    {
        // the map's own, in its order: a boomerang, which comes back, first
        CHECK(kGifts[0].ranged == kCoarseBoomerang);
        CHECK(kGifts[0].ammo == 0);
        CHECK(kGifts[1].ranged == 0);
        CHECK(kGifts[1].ammo == kPebble);
        CHECK(kGifts[2].ammo == kDart);
        CHECK(kGifts[3].ranged == kShortbow);
        CHECK(kGifts[3].ammo == kStoneArrow);
        const std::vector<Held> none{ worn(ammo(kTiphiaSting, 0, 1)) };
        CHECK(chooseKit(none, all) == Kit{ 0, kPebble, true });
        // a job that throws a boomerang (a Warrior from 6): the boomerang
        CHECK(chooseKit(none, fallbacks(true, false, false, true)) == Kit{ kCoarseBoomerang, 0, true });
        // a job pebbles are not for: darts, then the shortbow and its arrows
        CHECK(chooseKit(none, fallbacks(false, true, true)) == Kit{ 0, kDart, true });
        CHECK(chooseKit(none, fallbacks(false, false, true)) == Kit{ kShortbow, kStoneArrow, true });
        // a job with no ranged attack at all: none
        CHECK_FALSE(chooseKit(none, fallbacks(false, false, false)).has_value());
    }
}

TEST_CASE("Cardian pull: ammunition topped up to a stack once half is gone", "[cardian][pull]")
{
    CHECK(ammoTopUp(0, 99) == 99);
    CHECK(ammoTopUp(49, 99) == 50);
    CHECK(ammoTopUp(50, 99) == 0);
    CHECK(ammoTopUp(99, 99) == 0);
    CHECK(ammoTopUp(6, 12) == 6);
    CHECK(ammoTopUp(7, 12) == 0);
    // a stack size past 99 is taken as 99; a broken one as 1
    CHECK(ammoTopUp(0, 255) == 99);
    CHECK(ammoTopUp(0, 0) == 1);
}
