// Cardian: the linkshell's errands (pawn/errands.h, the rules in
// pawn/errand_math.h): whom each kind is for, the row's words, the clock an
// errand runs on and where it has her, what a call back keeps, the steps of
// gearing up in town, and a finished quest or mission in her log.
#include "map/pawn/cardian_link_messages.h"
#include "map/pawn/errand_math.h"

#include <catch2/catch_test_macros.hpp>

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
    CHECK_FALSE(stateOf("done").has_value());
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
    // level and money keep their shape and are never offered until built
    CHECK_FALSE(built(Kind::Level));
    CHECK_FALSE(built(Kind::Money));
    CHECK_FALSE(offered(Kind::Level, Member::Alt));
    CHECK_FALSE(offered(Kind::Money, Member::Alt));
    // whom they will be for: money never for the world's
    CHECK(forMember(Kind::Level, Member::Wild));
    CHECK(forMember(Kind::Money, Member::Owned));
    CHECK_FALSE(forMember(Kind::Money, Member::Wild));
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

TEST_CASE("Errands: a list of numbers reads and writes as the args keep it", "[cardian][errands]")
{
    CHECK(numbersOf("0,1,2,15") == std::vector<uint16_t>{ 0, 1, 2, 15 });
    CHECK(numbersOf("") == std::vector<uint16_t>{});
    CHECK(numbersOf("3,,x,70000,4") == std::vector<uint16_t>{ 3, 4 });
    CHECK(numbersText({ 20, 30 }) == "20,30");
    CHECK(numbersOf(numbersText({ 0, 5, 9 })) == std::vector<uint16_t>{ 0, 5, 9 });
}
