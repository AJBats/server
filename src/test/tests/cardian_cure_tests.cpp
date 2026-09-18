// Cardian: Cure measurement/selection and quiet-fight MP budget regressions.
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include "map/pawn/cure_math.h"
#include "map/pawn/fight_math.h"
#include "map/pawn/rest_math.h"

using namespace cardian::cure;
using Catch::Matchers::WithinAbs;

namespace
{
    auto cure(uint32_t caster, double land, double heals = 30, double wake = 0, uint16_t spell = 1) -> Option
    {
        return {.caster = caster, .target = 99, .spell = spell, .heals = heals, .mp = 8,
            .time = {land - 2, land}, .wakeCost = wake};
    }
    const Target injured{99, 60, 100, 30, 10, false};
    auto plan(std::vector<Option> options, Target target = injured) -> std::vector<Choice>
    {
        return choose(options, std::vector<Target>{target});
    }
}

TEST_CASE("Cure readiness overlaps recast with standing, busy time and travel", "[cardian][cure]")
{
    CHECK_THAT(timing(true, 0, 1, 0, 0, 2).start, WithinAbs(1, 1e-9));
    CHECK_THAT(timing(true, 6, 1, 0, 2, 2).land, WithinAbs(8, 1e-9));
    CHECK_THAT(timing(true, 1, 0, 5, 3, 2).start, WithinAbs(8, 1e-9));
    CHECK_THAT(timing(true, 0, 0.4, 0, 0, 2).land, WithinAbs(2.4, 1e-9));
    CHECK_FALSE(std::isfinite(timing(false, 0, 0, 0, 0, 2).start)); // unlearned/unaffordable/blocked
    CHECK_FALSE(std::isfinite(timing(true, 0, 0, 0, unavailable, 2).land)); // cannot walk into range
}

TEST_CASE("Ready resting healer beats awake healer on long cooldown", "[cardian][cure]")
{
    const auto result = plan({cure(1, 17), cure(2, 3, 30, 50)});
    REQUIRE(result.size() == 1);
    CHECK(result[0].cure.caster == 2);
}

TEST_CASE("Cure size is considered without waiting for a larger tier on cooldown", "[cardian][cure]")
{
    auto large = cure(1, 2, 90, 0, 2);
    large.mp = 24;
    const auto result = plan({cure(1, 2, 10), large}, {99, 40, 100, 30, 15, false});
    REQUIRE(result.size() == 1);
    CHECK(result[0].cure.spell == 2); // same arrival; small tier cannot make her safe
    large.time = {15, 17};
    const auto ready = plan({cure(1, 2), large}, {99, 40, 100, 30, 10, false});
    REQUIRE(ready.size() == 1);
    CHECK(ready[0].cure.spell == 1);
}

TEST_CASE("Finishing the stand does not cancel its emergency, but receiving healing does", "[cardian][cure]")
{
    const auto rising = plan({cure(1, 3, 30, 50)});
    REQUIRE(rising.size() == 1);
    const auto up = choose(std::vector<Option>{cure(1, 2)}, std::vector<Target>{injured}, rising);
    REQUIRE(up.size() == 1);
    CHECK(up[0].cast);
    auto healed = injured;
    healed.hp = 90;
    CHECK(choose(std::vector<Option>{cure(1, 2)}, std::vector<Target>{healed}, up).empty());
    auto casting = cure(1, 1);
    casting.inFlight = true;
    CHECK(choose(std::vector<Option>{casting, cure(2, 2)}, std::vector<Target>{injured}, up).empty());
}

TEST_CASE("Ready healer beats travel, active casting, and unavailable spells", "[cardian][cure]")
{
    auto unknown = cure(1, unavailable, 200);
    auto walking = cure(2, 8, 90);
    auto busy = cure(3, 7, 90);
    const auto result = plan({unknown, walking, busy, cure(4, 3, 30, 40)});
    REQUIRE(result.size() == 1);
    CHECK(result[0].cure.caster == 4);
    CHECK(plan({unknown}).empty());
}

TEST_CASE("An adequate Cure in flight prevents emergency duplicates", "[cardian][cure]")
{
    auto incoming = cure(1, 1, 40);
    incoming.inFlight = true;
    CHECK(plan({incoming, cure(2, 3)}).empty());
}

TEST_CASE("An insufficient or late Cure in flight allows another mage", "[cardian][cure]")
{
    auto incoming = cure(1, 1, 5);
    incoming.inFlight = true;
    auto result = plan({incoming, cure(2, 3)}, {99, 40, 100, 30, 10, false});
    REQUIRE(result.size() == 1);
    CHECK(result[0].cure.caster == 2);
    incoming.time.land = 15;
    incoming.heals = 100;
    result = plan({incoming, cure(2, 3)});
    REQUIRE(result.size() == 1);
    CHECK(result[0].cure.caster == 2);
}

TEST_CASE("Emergency may assign simultaneous cures but does not wake the whole party", "[cardian][cure]")
{
    const auto result = plan({cure(1, 2, 20), cure(2, 2, 20), cure(3, 2, 20)}, {99, 40, 100, 30, 15, false});
    REQUIRE(result.size() == 2);
    CHECK(result[0].cure.caster != result[1].cure.caster);
}

TEST_CASE("Equal useful response preserves the less expensive rest", "[cardian][cure]")
{
    const auto result = plan({cure(1, 3, 30, 100), cure(2, 3, 30, 20)});
    REQUIRE(result.size() == 1);
    CHECK(result[0].cure.caster == 2);
}

TEST_CASE("One healer cannot promise simultaneous first aid to two targets", "[cardian][cure]")
{
    auto second = cure(1, 3);
    second.target = 100;
    const auto result = choose(std::vector<Option>{cure(1, 3), second}, std::vector<Target>{injured, {100, 50, 100, 30, 10, false}});
    REQUIRE(result.size() == 1);
    CHECK(result[0].cure.target == 100);
}

TEST_CASE("TP readiness at full HP prepares a mage without casting a cure", "[cardian][cure]")
{
    const auto result = plan({cure(1, 3)}, {99, 100, 100, 30, 0, true});
    REQUIRE(result.size() == 1);
    CHECK_FALSE(result[0].cast);
    CHECK(plan({cure(1, 3)}, {99, 100, 100, 30, 0, false}).empty());
}

TEST_CASE("In-flight healing cannot overflow max HP or rescue after predicted death", "[cardian][cure]")
{
    CHECK_THAT(margin({99, 90, 100, 20, 10, false}, 4, std::vector<Option>{cure(1, 1, 80)}), WithinAbs(50, 1e-9));
    CHECK(margin({99, 10, 100, 20, 10, false}, 4, std::vector<Option>{cure(1, 2, 100)}) < 0);
}

TEST_CASE("Quiet fight attendance records zero MP without inventing absent members", "[cardian][cure][rest]")
{
    using cardian::tactics::FightRecord;
    FightRecord fight;
    fight.zone = 100;
    fight.attend(1, "resting mage", 100);
    fight.attend(2, "other zone", 101);
    REQUIRE(fight.find(1) != nullptr);
    CHECK(fight.find(1)->mpSpent == 0);
    CHECK(fight.find(2) == nullptr);
    fight.member(1, "resting mage").mpSpent = 7;
    fight.attend(1, "resting mage", 100);
    CHECK(fight.find(1)->mpSpent == 7);
    CHECK(fight.members.size() == 1);
    fight.settlingSince = 5;
    fight.attend(3, "arrived after kill", 100);
    CHECK(fight.find(3) == nullptr);
}

TEST_CASE("A quiet zero-cost fight lowers the personal fight budget", "[cardian][cure][rest]")
{
    cardian::tactics::FightRecord quiet;
    quiet.zone = 100;
    quiet.attend(1, "mage", 100);
    std::vector<double> costs{7, 7, 23, 16, 31, 31};
    REQUIRE(quiet.find(1) != nullptr);
    costs.push_back(quiet.find(1)->mpSpent);
    CHECK_THAT(cardian::rest::fightBudget(costs), WithinAbs(47.428571, 1e-6));
    CHECK_FALSE(cardian::rest::pacing(54, 85, {31.0/60, 18.0/60}, cardian::rest::fightBudget(costs), true, false, 20, 12, 8).recover);
}
