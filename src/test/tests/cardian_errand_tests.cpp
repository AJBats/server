// Cardian: the linkshell's errands (pawn/errands.h, the rules in
// pawn/errand_math.h): whom each kind is for, the row's words, the clock an
// errand runs on and where it has her, what a call back keeps, the steps of
// gearing up in town, a level venture's bounds, the share it keeps, its
// banked levels and the conquest points of what it gained, and a finished
// quest or mission in her log.
#include "map/pawn/cardian_link_messages.h"
#include "map/pawn/errand_math.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstdint>
#include <string>
#include <vector>

using namespace cardian::errand;

TEST_CASE("Errands: the enums are the Link's numbers", "[cardian][errands]")
{
    STATIC_REQUIRE(static_cast<uint8_t>(Kind::None) == CL_ERRAND_NONE);
    STATIC_REQUIRE(static_cast<uint8_t>(Kind::Gear) == CL_ERRAND_GEAR);
    STATIC_REQUIRE(static_cast<uint8_t>(Kind::Quest) == CL_ERRAND_QUEST);
    STATIC_REQUIRE(static_cast<uint8_t>(Kind::Level) == CL_ERRAND_LEVEL);
    STATIC_REQUIRE(static_cast<uint8_t>(Kind::Money) == CL_ERRAND_MONEY);
    STATIC_REQUIRE(static_cast<uint8_t>(Kind::Rank) == CL_ERRAND_RANK);
    STATIC_REQUIRE(static_cast<uint8_t>(State::Going) == CL_ERRAND_GOING);
    STATIC_REQUIRE(static_cast<uint8_t>(State::Away) == CL_ERRAND_AWAY);
    STATIC_REQUIRE(static_cast<uint8_t>(Member::Alt) == CL_CLUB_ALT);
    STATIC_REQUIRE(static_cast<uint8_t>(Member::Owned) == CL_CLUB_OWNED);
    STATIC_REQUIRE(static_cast<uint8_t>(Member::Wild) == CL_CLUB_WILD);
    STATIC_REQUIRE(static_cast<uint8_t>(Member::Recruit) == CL_CLUB_RECRUIT);
}

TEST_CASE("Errands: the row's words read back as the kind and the state", "[cardian][errands]")
{
    for (const auto kind : { Kind::Gear, Kind::Quest, Kind::Level, Kind::Money, Kind::Rank })
    {
        CHECK(kindOf(kindName(kind)) == kind);
    }
    CHECK_FALSE(kindOf("").has_value());
    CHECK_FALSE(kindOf("Quest").has_value());
    CHECK(stateOf("going") == State::Going);
    CHECK(stateOf("away") == State::Away);
    CHECK(stateOf(stateName(State::Away)) == State::Away);
    CHECK_FALSE(stateOf("returning").has_value()); // the venture keeper's, gone with it
    CHECK_FALSE(stateOf("back").has_value());
    for (const auto state : { State::Going, State::Away })
    {
        CHECK(stateOf(stateName(state)) == state);
    }
    CHECK_FALSE(stateOf("done").has_value());
}

TEST_CASE("Errands: a money venture is out of the world while away, for 1 or 2 hours", "[cardian][errands]")
{
    CHECK_FALSE(gone(State::Going));
    CHECK(gone(State::Away));
    for (const uint32_t hours : { 1u, 2u })
    {
        CHECK(ventureHours(hours));
    }
    for (const uint32_t hours : { 0u, 3u, 4u, 24u })
    {
        CHECK_FALSE(ventureHours(hours));
    }
}

TEST_CASE("Errands: the menu offers what is built and hers", "[cardian][errands]")
{
    // gear up is the census's re-dress: the world's, wearing his pearl
    CHECK(offered(Kind::Gear, Member::Wild));
    CHECK_FALSE(offered(Kind::Gear, Member::Alt));
    CHECK_FALSE(offered(Kind::Gear, Member::Owned));
    // a quest and a rank catch-up for every member
    CHECK(offered(Kind::Rank, Member::Alt));
    CHECK(offered(Kind::Rank, Member::Owned));
    CHECK(offered(Kind::Rank, Member::Wild));
    CHECK_FALSE(offered(Kind::Rank, Member::Recruit));
    CHECK(offered(Kind::Quest, Member::Alt));
    CHECK(offered(Kind::Quest, Member::Owned));
    CHECK(offered(Kind::Quest, Member::Wild));
    // a recruit is no member: nothing until she wears his pearl
    CHECK_FALSE(offered(Kind::Quest, Member::Recruit));
    CHECK_FALSE(offered(Kind::Gear, Member::Recruit));
    // a level venture for every member (the user, 2026-10-18), never a recruit
    CHECK(built(Kind::Level));
    CHECK(offered(Kind::Level, Member::Alt));
    CHECK(offered(Kind::Level, Member::Owned));
    CHECK(offered(Kind::Level, Member::Wild));
    CHECK_FALSE(offered(Kind::Level, Member::Recruit));
    // money, a venture at a profession: his alts and owned cardians, never the world's
    CHECK(built(Kind::Money));
    CHECK(offered(Kind::Money, Member::Alt));
    CHECK(offered(Kind::Money, Member::Owned));
    CHECK_FALSE(offered(Kind::Money, Member::Wild));
    CHECK_FALSE(offered(Kind::Money, Member::Recruit));
}

TEST_CASE("Errands: the clock starts as she leaves the world, not as she is sent", "[cardian][errands]")
{
    Errand e{ .kind = Kind::Quest, .started = 1000 };
    CHECK(e.state == State::Going);
    CHECK(secondsLeft(e, 5000) == 0); // walking off: off the clock
    CHECK_FALSE(due(e, 999999));

    e = goAway(e, 1100, 3600);
    CHECK(e.state == State::Away);
    CHECK(e.left == 1100);
    CHECK(e.ends == 4700);
    CHECK(secondsLeft(e, 1100) == 3600);
    CHECK(secondsLeft(e, 4000) == 700);
    CHECK_FALSE(due(e, 4699));
    CHECK(due(e, 4700));
    CHECK(secondsLeft(e, 4700) == 0);
    CHECK(secondsLeft(e, 9000) == 0);
}

TEST_CASE("Errands: a kind that ends on its target runs off the clock", "[cardian][errands]")
{
    const auto e = goAway(Errand{ .kind = Kind::Level, .started = 10, .target = 20 }, 50, 0);
    CHECK(e.state == State::Away);
    CHECK(e.ends == 0);
    CHECK(secondsLeft(e, 100000) == 0);
    CHECK_FALSE(due(e, 100000));
}

TEST_CASE("Errands: a level venture goes to an unlocked job, above her level and at most his highest", "[cardian][errands][level]")
{
    constexpr uint8_t  kWar = 1;
    constexpr uint8_t  kMnk = 2;
    constexpr uint8_t  kPld = 7;
    constexpr uint8_t  kDnc = 19;
    constexpr uint32_t unlocked = (1u << kWar) | (1u << kMnk) | 1u; // Warrior, Monk and the support job
    // his highest on any job is 37: a Warrior 20 may go to 21 through 37
    CHECK(levelFits(unlocked, kWar, 20, 21, 37));
    CHECK(levelFits(unlocked, kWar, 20, 37, 37));
    CHECK_FALSE(levelFits(unlocked, kWar, 20, 38, 37)); // past him
    CHECK_FALSE(levelFits(unlocked, kWar, 20, 20, 37)); // not above her
    CHECK_FALSE(levelFits(unlocked, kWar, 20, 5, 37));
    // a job she has not unlocked, the support job's bit, and a job past the era's never go
    CHECK_FALSE(levelFits(unlocked, kPld, 0, 10, 37));
    CHECK_FALSE(levelFits(unlocked, 0, 0, 10, 37));
    CHECK_FALSE(levelFits(unlocked | (1u << kDnc), kDnc, 1, 10, 37));
    // an unlocked job never played reads 0 and starts at 1
    CHECK(levelOn(0) == 1);
    CHECK(levelFits(unlocked, kMnk, 0, 2, 37));
    CHECK_FALSE(levelFits(unlocked, kMnk, 0, 1, 37));

    // the page offers Level while one of her unlocked jobs is below his highest
    std::array<uint8_t, 24> levels{};
    levels[kWar] = 37;
    levels[kMnk] = 37;
    CHECK_FALSE(anyLevelFits(unlocked, levels, 37));
    levels[kMnk] = 12;
    CHECK(anyLevelFits(unlocked, levels, 37));
    CHECK_FALSE(anyLevelFits(1u << kWar, levels, 37)); // her Monk is not unlocked
    CHECK_FALSE(anyLevelFits(unlocked, levels, 0));    // his own jobs all at 0
}

TEST_CASE("Errands: a level venture called back keeps its share of the next level as experience", "[cardian][errands][level]")
{
    // half of a level that asks 2000 is 1000; nothing earned is nothing kept
    CHECK(keptExp(0.5, 2000) == 1000);
    CHECK(keptExp(0.0, 2000) == 0);
    CHECK(keptExp(-0.2, 2000) == 0);
    CHECK(keptExp(0.25, 0) == 0);
    // never the whole of it: a ding is the census's, or the game's as she earns it
    CHECK(keptExp(1.0, 2000) == 1999);
    CHECK(keptExp(0.9999999, 2000) == 1999);
    CHECK(keptExp(0.75, 2000) == 1500);
    // the page's share, in hundredths, never a whole level
    CHECK(sharePercent(0.0) == 0);
    CHECK(sharePercent(0.456) == 45);
    CHECK(sharePercent(1.0) == 99);
    CHECK(sharePercent(-1.0) == 0);
}

TEST_CASE("Errands: a level venture's banked levels land in one ding, and a call back waits for them", "[cardian][errands][level]")
{
    // every whole level the count holds, up to the levels left; none short of a whole one
    CHECK(wholeLevels(0.99, 5) == 0);
    CHECK(wholeLevels(1.0 - 1e-10, 5) == 1);
    CHECK(wholeLevels(2.3, 5) == 2);
    CHECK(wholeLevels(3.0 - 1e-10, 5) == 3);
    CHECK(wholeLevels(9.0, 5) == 5);
    CHECK(wholeLevels(1.5, 0) == 0);
    // the time the census has not counted: 2.4 levels an hour for half an hour since its last count
    CHECK(ventureCount(0.3, 2.4, 1000.0, 2800.0, 1.0, 5) == Catch::Approx(1.5));
    CHECK(ventureCount(0.3, 2.4, 1000.0, 2800.0, 2.0, 5) == Catch::Approx(2.7)); // a test world's clock at twice the speed
    CHECK(ventureCount(4.5, 2.4, 1000.0, 2800.0, 1.0, 5) == Catch::Approx(5.0)); // never past her target
    // nothing before the census's first count, nothing for a second already counted, nothing with no pace
    CHECK(ventureCount(0.3, 2.4, 0.0, 2800.0, 1.0, 5) == Catch::Approx(0.3));
    CHECK(ventureCount(0.3, 2.4, 2800.0, 2800.0, 1.0, 5) == Catch::Approx(0.3));
    CHECK(ventureCount(0.3, 0.0, 1000.0, 2800.0, 1.0, 5) == Catch::Approx(0.3));
    // called back, she waits while a whole level is banked or a ding of it is open
    CHECK(recallWaits(1.2, 5, false));
    CHECK(recallWaits(0.4, 5, true));
    CHECK_FALSE(recallWaits(0.4, 5, false));
    CHECK_FALSE(recallWaits(1.2, 0, false)); // at her target: nothing left to land
}

TEST_CASE("Errands: a level venture pays conquest points on what it gained her alone", "[cardian][errands][level]")
{
    // three levels of 1,000 experience each, 250 of the first held as she set out
    CHECK(ventureGain(3000, 0, 250) == 2750);
    // called back before a level, 600 kept where she had 250: 350 gained
    CHECK(ventureGain(0, 600, 250) == 350);
    // sent and called back at once: nothing gained, nothing paid however often
    CHECK(ventureGain(0, 250, 250) == 0);
    CHECK(ventureGain(0, 100, 250) == 0);
    CHECK(venturePoints(ventureGain(3000, 0, 250), false) == 275);
}

TEST_CASE("Errands: her route is crossed evenly over her time away", "[cardian][errands]")
{
    Errand e{ .kind = Kind::Quest, .started = 0, .route = { 230, 100, 104, 149 } };
    CHECK(legAt(e, 50) == 230); // walking off: the first

    e = goAway(e, 1000, 400); // away 1000 to 1400, a hundred seconds a zone
    CHECK(legAt(e, 1000) == 230);
    CHECK(legAt(e, 1099) == 230);
    CHECK(legAt(e, 1100) == 100);
    CHECK(legAt(e, 1250) == 104);
    CHECK(legAt(e, 1399) == 149);
    CHECK(legAt(e, 1400) == 149);
    CHECK(legAt(e, 99999) == 149); // due: the last
    CHECK(legAt(e, 10) == 230);    // a clock read before she left (a restart's drift): the first

    CHECK(legAt(Errand{ .kind = Kind::Quest }, 5) == 0);
}

TEST_CASE("Errands: a call back keeps what she earned", "[cardian][errands]")
{
    const Errand going{ .kind = Kind::Quest, .started = 0 };
    CHECK(keptOnCallBack(going, 10) == Kept::Nothing);

    const auto quest = goAway(going, 100, 3600);
    CHECK(keptOnCallBack(quest, 200) == Kept::Nothing); // a quest is done whole or not at all
    CHECK(keptOnCallBack(quest, 3700) == Kept::Whole);  // its clock ran out: done

    auto level     = goAway(Errand{ .kind = Kind::Level, .target = 30 }, 100, 0);
    CHECK(keptOnCallBack(level, 200) == Kept::Nothing);
    level.progress = 2;
    CHECK(keptOnCallBack(level, 200) == Kept::Progress);

    auto gear = Errand{ .kind = Kind::Gear }; // in town, seen: it never goes away
    gear.progress = 3;
    CHECK(keptOnCallBack(gear, 200) == Kept::Nothing);
}

TEST_CASE("Errands: the args read as key and value, and write back sorted", "[cardian][errands]")
{
    const auto args = parseArgs("log=0;id=29;route=230,100;goal=2");
    REQUIRE(args.size() == 4);
    CHECK(args.at("log") == "0");
    CHECK(args.at("route") == "230,100");
    CHECK(numberArg(args, "id") == 29u);
    CHECK(numberArg(args, "goal") == 2u);
    CHECK_FALSE(numberArg(args, "job").has_value());
    CHECK_FALSE(numberArg(args, "route").has_value()); // not a number
    CHECK(argsText(args) == "goal=2;id=29;log=0;route=230,100");
    CHECK(parseArgs(argsText(args)) == args);

    // a piece that does not read is left out; an empty text is no args
    const auto rough = parseArgs("=3;noequals;;x=;y=4");
    CHECK(rough.size() == 2);
    CHECK(rough.at("x").empty());
    CHECK(numberArg(rough, "y") == 4u);
    CHECK_FALSE(numberArg(rough, "x").has_value());
    CHECK(parseArgs("").empty());

    // a value that would break the row is not written
    CHECK(argsText(Args{ { "a", "1;b=2" }, { "c", "3" } }) == "c=3");
}

TEST_CASE("Errands: a route reads as zone ids", "[cardian][errands]")
{
    CHECK(routeOf("230,100,104") == std::vector<uint16_t>{ 230, 100, 104 });
    CHECK(routeOf("") == std::vector<uint16_t>{});
    CHECK(routeOf("230,,x,0,70000,5") == std::vector<uint16_t>{ 230, 5 });
    CHECK(routeText({ 230, 100 }) == "230,100");
    CHECK(routeOf(routeText({ 1, 2, 3 })) == std::vector<uint16_t>{ 1, 2, 3 });
}

TEST_CASE("Errands: gearing up walks to the counter, waits for the census, then the guard, and back", "[cardian][errands]")
{
    GearFacts f{ .hasGuard = true, .returns = true };
    CHECK(nextGearStep(GearStep::ToCounter, f) == GearStep::ToCounter);
    f.arrived = true;
    CHECK(nextGearStep(GearStep::ToCounter, f) == GearStep::AtCounter);

    f = GearFacts{ .hasGuard = true, .returns = true };
    CHECK(nextGearStep(GearStep::AtCounter, f) == GearStep::AtCounter); // the census has not answered
    f.dressed = true;
    CHECK(nextGearStep(GearStep::AtCounter, f) == GearStep::ToGuard);

    f = GearFacts{ .hasGuard = true, .returns = true, };
    f.arrived = true;
    CHECK(nextGearStep(GearStep::ToGuard, f) == GearStep::AtGuard);
    CHECK(nextGearStep(GearStep::AtGuard, f) == GearStep::Back);
    CHECK(nextGearStep(GearStep::Back, f) == GearStep::Done);
    // turned away by another nation's guard, she walks on to her own consulate
    f.toConsulate = true;
    CHECK(nextGearStep(GearStep::AtGuard, f) == GearStep::ToGuard);
    f.toConsulate = false;
    CHECK(walks(GearStep::ToCounter));
    CHECK(walks(GearStep::ToGuard));
    CHECK(walks(GearStep::Back));
    CHECK_FALSE(walks(GearStep::AtCounter));
}

TEST_CASE("Errands: gearing up moves on past a walk or a wait that runs too long", "[cardian][errands]")
{
    GearFacts f{ .timedOut = true, .hasGuard = true, .returns = true };
    CHECK(nextGearStep(GearStep::ToCounter, f) == GearStep::ToGuard); // the counter out of reach: on to the guard
    CHECK(nextGearStep(GearStep::AtCounter, f) == GearStep::ToGuard); // the census never answered
    CHECK(nextGearStep(GearStep::ToGuard, f) == GearStep::Back);
    CHECK(nextGearStep(GearStep::Back, f) == GearStep::Done);
}

TEST_CASE("Errands: gearing up skips a guard the zone lacks, and rejoins him rather than walk back", "[cardian][errands]")
{
    GearFacts f{ .dressed = true, .hasGuard = false, .returns = true };
    CHECK(nextGearStep(GearStep::AtCounter, f) == GearStep::Back);
    f.returns = false; // in his party, with him in her zone
    CHECK(nextGearStep(GearStep::AtCounter, f) == GearStep::Done);
    f.hasGuard = true;
    CHECK(nextGearStep(GearStep::AtCounter, f) == GearStep::ToGuard);
    CHECK(nextGearStep(GearStep::AtGuard, f) == GearStep::Done);
    CHECK(nextGearStep(GearStep::Done, f) == GearStep::Done);
}

TEST_CASE("Errands: a quest done goes from her current list to her completed one", "[cardian][errands]")
{
    uint8_t current[32]{};
    uint8_t complete[32]{};
    current[29 / 8] = static_cast<uint8_t>(1 << (29 % 8)); // under way

    CHECK_FALSE(questDone(complete, 29));
    CHECK(markQuestDone(current, complete, 29));
    CHECK(questDone(complete, 29));
    CHECK((current[29 / 8] & (1 << (29 % 8))) == 0);
    CHECK_FALSE(markQuestDone(current, complete, 29)); // done already: nothing changes

    // its neighbours untouched
    CHECK_FALSE(questDone(complete, 28));
    CHECK_FALSE(questDone(complete, 30));

    CHECK(markQuestDone(current, complete, 255));
    CHECK_FALSE(markQuestDone(current, complete, 256)); // past the log
    CHECK_FALSE(questDone(complete, 300));
}

TEST_CASE("Errands: a mission done is done, and no longer her current one", "[cardian][errands]")
{
    uint16_t current = 0;
    bool     complete[64]{};
    CHECK(markMissionDone(current, complete, 0, 65535)); // the nation's first, under way
    CHECK(complete[0]);
    CHECK(current == 65535);
    CHECK(missionDone(complete, 0));
    CHECK_FALSE(markMissionDone(current, complete, 0, 65535));

    current = 3;
    CHECK(markMissionDone(current, complete, 1, 65535)); // another than her current: hers stays
    CHECK(current == 3);
    CHECK_FALSE(markMissionDone(current, complete, 64, 65535));
    CHECK_FALSE(missionDone(complete, 64));
}

TEST_CASE("Errands: a rank catch-up called back keeps the missions her time covered", "[cardian][errands]")
{
    const std::vector<uint16_t> minutes{ 20, 20, 30, 30 };
    CHECK(missionsDone(minutes, 0) == 0);
    CHECK(missionsDone(minutes, 20 * 60 - 1) == 0);
    CHECK(missionsDone(minutes, 20 * 60) == 1);
    CHECK(missionsDone(minutes, 70 * 60) == 3);
    CHECK(missionsDone(minutes, 100 * 60) == 4);
    CHECK(missionsDone(minutes, 999 * 60) == 4);
    CHECK(missionsDone({}, 999) == 0);

    const auto away = goAway(Errand{ .kind = Kind::Rank }, 100, 6000);
    CHECK(keptOnCallBack(away, 200) == Kept::Progress); // what her time covers, counted at the call
    CHECK(keptOnCallBack(away, 6100) == Kept::Whole);
    CHECK(keptOnCallBack(Errand{ .kind = Kind::Rank }, 200) == Kept::Nothing); // still walking off
}

TEST_CASE("Errands: a level venture's report reads its conquest points, its pace and its skills", "[cardian][errands]")
{
    // a tenth of the experience under Signet; one of the world's holds the census's three quarters of it
    CHECK(venturePoints(10000, false) == 1000);
    CHECK(venturePoints(10000, true) == 750);
    CHECK(venturePoints(0, false) == 0);
    // 1.4 levels an hour at a level asking 3,000 experience: 4,200 experience an hour
    CHECK(expPerHour(1.4, 3000) == 4200);
    CHECK(expPerHour(0.0, 3000) == 0);
    // 34% of a level earned at 1.5 levels an hour: 26.4 minutes still to go, said as 27
    CHECK(minutesToNext(0.34, 1.5) == 27);
    CHECK(minutesToNext(0.5, 0.0) == 0);
    // her skills kept and read back, and what rose between two readings
    const std::map<uint16_t, uint16_t> before{ { 1, 680 }, { 28, 600 }, { 29, 660 }, { 31, 100 } };
    const std::map<uint16_t, uint16_t> after{ { 1, 740 }, { 28, 630 }, { 29, 690 }, { 31, 100 }, { 12, 5 } };
    CHECK(skillsText(before) == "1:680,28:600,29:660,31:100");
    CHECK(skillsOfText(skillsText(after)) == after);
    CHECK(skillsOfText("1:680,,x:5,28:600") == std::map<uint16_t, uint16_t>{ { 1, 680 }, { 28, 600 } });
    CHECK(skillsOfText("").empty());
    const auto risen = skillsRisen(before, after);
    REQUIRE(risen.size() == 4); // Parry did not move
    CHECK(risen[0] == SkillRise{ 1, 680, 740 });
    CHECK(risen[1] == SkillRise{ 12, 0, 5 }); // a skill she had none of
    CHECK(risen[3] == SkillRise{ 29, 660, 690 });
}

TEST_CASE("Errands: a list of numbers reads and writes as the args keep it", "[cardian][errands]")
{
    CHECK(numbersOf("0,1,2,15") == std::vector<uint16_t>{ 0, 1, 2, 15 });
    CHECK(numbersOf("") == std::vector<uint16_t>{});
    CHECK(numbersOf("3,,x,70000,4") == std::vector<uint16_t>{ 3, 4 });
    CHECK(numbersText({ 20, 30 }) == "20,30");
    CHECK(numbersOf(numbersText({ 0, 5, 9 })) == std::vector<uint16_t>{ 0, 5, 9 });
}
