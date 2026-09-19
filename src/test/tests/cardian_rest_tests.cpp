// Cardian: resting lifecycle regressions, independent of a running map/save.
#include "pawn/rest_math.h"
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

using namespace cardian::rest;
using Catch::Matchers::WithinAbs;

TEST_CASE("MP pacing keeps separate spending and recovery across a fight and idle time", "[cardian][rest][pacing]")
{
    Flow flow;
    flow.observe(0, 85, 85);
    flow.observe(5, 77, 85);  // Cure completion
    flow.observe(5.1, 85, 85); // a recovery in the same server tick is still recorded
    CHECK_THAT(flow.rates(6).spent, WithinAbs(8.0 / 60, 1e-9));
    CHECK_THAT(flow.rates(6).recovered, WithinAbs(8.0 / 60, 1e-9));
    flow.observe(10, 78, 85); // Dia in the next fight
    CHECK_THAT(flow.rates(50).spent, WithinAbs(15.0 / 60, 1e-9));
    CHECK_THAT(flow.rates(65).spent, WithinAbs(7.0 / 60, 1e-9));
    CHECK(flow.rates(65).recovered == 0);
    CHECK(flow.rates(70).spent == 0);
}

TEST_CASE("MP pacing records capped recovery and free spells without fabricated credit or cost", "[cardian][rest][pacing]")
{
    Flow flow;
    flow.observe(0, 77, 85);
    flow.observe(1, 77, 85); // free cast / no actual MP loss
    flow.observe(20, 85, 85); // nominal twelve-MP tick, only eight fit
    CHECK(flow.rates(20).spent == 0);
    CHECK_THAT(flow.rates(20).recovered, WithinAbs(8.0 / 60, 1e-9));
    flow.observe(30, 85, 85);
    CHECK_THAT(flow.rates(30).recovered, WithinAbs(8.0 / 60, 1e-9));
}

TEST_CASE("MP history resets on capacity changes, observation gaps and a reversed clock", "[cardian][rest][pacing]")
{
    Flow flow;
    flow.observe(0, 85, 85);
    flow.observe(1, 77, 85);
    flow.observe(2, 90, 100);
    CHECK(flow.rates(2).spent == 0);
    CHECK(flow.rates(2).recovered == 0);
    flow.observe(3, 82, 100);
    flow.observe(100, 100, 100);
    CHECK(flow.rates(100).spent == 0);
    CHECK(flow.rates(100).recovered == 0);
    flow.observe(101, 90, 100);
    flow.observe(50, 100, 100);
    CHECK(flow.rates(50).spent == 0);
    CHECK(flow.rates(50).recovered == 0);
}

TEST_CASE("Near-full MP after Dia or Cure favors standing even without fight history", "[cardian][rest][pacing]")
{
    for (const double cost : {7.0, 8.0})
    {
        Flow flow;
        flow.observe(100, 85, 85);
        flow.observe(101, 85 - cost, 85);
        const auto p = pacing(85 - cost, 85, flow.rates(101), 85, false, false, 20, 12, 8);
        CHECK_FALSE(p.recover);
        CHECK_FALSE(p.critical);
        State s;
        CHECK(s.decide({.now=105, .want=p.recover}) == Decision::StayUp);
    }
}

TEST_CASE("Recovery matching expenditure lets a mage keep her feet", "[cardian][rest][pacing]")
{
    const auto falling = pacing(45, 85, {0.8, 0.0}, 30, true, false, 20, 12, 8);
    const auto balanced = pacing(45, 85, {0.8, 0.8}, 30, true, false, 20, 12, 8);
    CHECK(falling.recover);
    CHECK_FALSE(balanced.recover);
    CHECK(balanced.projected == 45);
    CHECK(falling.projected < falling.reserve);
}

TEST_CASE("A recovery rate falling as old ticks expire can create rest pressure", "[cardian][rest][pacing]")
{
    Flow flow;
    flow.observe(0, 25, 85);
    flow.observe(1, 55, 85);
    flow.observe(30, 43, 85);
    CHECK_FALSE(pacing(43, 85, flow.rates(50), 40, true, false, 20, 12, 8).recover);
    CHECK(pacing(43, 85, flow.rates(61), 40, true, false, 20, 12, 8).recover);
}

TEST_CASE("MP reserve prevents rates alone from trapping a nearly empty mage upright", "[cardian][rest][pacing]")
{
    const auto p = pacing(5, 85, {0.0, 1.0}, 0, false, false, 20, 12, 8);
    CHECK(p.recover);
    CHECK(p.critical);
    State s;
    CHECK(s.decide({.now=13, .want=p.recover}) == Decision::Kneel);
    // Urgent aid and physical safety still take precedence over MP pressure.
    CHECK(s.decide({.now=14, .resting=true, .want=true, .urgent=true}) == Decision::Stand);
}

TEST_CASE("Insufficient MP for the cheapest Cure creates pressure even above ten percent", "[cardian][rest][pacing]")
{
    const auto p = pacing(7, 50, {}, 0, false, false, 20, 12, 8);
    CHECK(p.critical);
    CHECK(p.recover);
}

TEST_CASE("An impossible learned budget cannot demand a refill after each Cure", "[cardian][rest][pacing]")
{
    const auto p = pacing(77, 85, {8.0 / 60, 0}, 200, true, false, 20, 12, 8);
    CHECK(p.reserve == 63.75);
    CHECK_FALSE(p.recover);
    CHECK_FALSE(pacing(0, 0, {}, 0, false, false, 20, 12, 0).recover);
}

TEST_CASE("An expensive fight budget survives a long rest and zero recent spending", "[cardian][rest][pacing]")
{
    Flow flow;
    flow.observe(0, 85, 85);
    flow.observe(30, 10, 85); // a seventy-five MP fight
    flow.observe(50, 22, 85);
    flow.observe(60, 35, 85);
    flow.observe(70, 49, 85);
    flow.observe(80, 64, 85);
    flow.observe(90, 80, 85);
    const auto rates = flow.rates(90);
    REQUIRE(rates.spent == 0); // the fight has aged out of the minute window
    const auto recovering = pacing(80, 85, rates, 150, true, true, 20, 12, 8);
    CHECK(recovering.target == 85);
    CHECK(recovering.recover); // do not call half an MP pool ready for another IT++
    CHECK_FALSE(pacing(85, 85, rates, 150, true, true, 20, 12, 8).recover);
    // Even after all rate samples expire, a interrupted rest still has a budget.
    CHECK(pacing(55, 85, flow.rates(300), 150, true, false, 20, 12, 8).recover);
}

TEST_CASE("Fight spending consumes its budget instead of requesting a fresh fight after each cast", "[cardian][rest][pacing]")
{
    const auto ongoing = pacing(45, 85, {}, 80, true, false, 20, 12, 8, 40);
    CHECK(ongoing.reserve == 40);
    CHECK_FALSE(ongoing.recover);
    const auto next = pacing(45, 85, {}, 80, true, false, 20, 12, 8);
    CHECK(next.recover);
    CHECK(next.target == 80);
    // An impossible link budget is capped before current-fight spending.
    CHECK_FALSE(pacing(77, 85, {8.0/60, 0}, 200, true, false, 20, 12, 8, 8).recover);
}

TEST_CASE("Starting rest early accounts for the real delay until its first paying tick", "[cardian][rest][pacing]")
{
    // No extra twenty seconds: at this pace a fresh rest spends ten MP
    // before its first paying tick, leaving five MP above the reserve.
    const auto normal = pacing(45, 100, {0.5, 0}, 30, true, false, 20, 12, 8);
    CHECK(normal.projected == 35);
    CHECK_FALSE(normal.recover);
    CHECK(pacing(45, 100, {0.5, 0}, 30, true, false, 40, 12, 8).recover);
}

TEST_CASE("Rest rebuilds reserve and recovery rate then releases on an actual tick", "[cardian][rest][pacing]")
{
    Flow flow;
    flow.observe(0, 60, 85);
    flow.observe(5, 30, 85);
    State s;
    auto p = pacing(30, 85, flow.rates(5), 30, true, false, 20, 12, 8);
    REQUIRE(p.recover);
    REQUIRE(s.decide({.now=5, .want=p.recover}) == Decision::Kneel);
    flow.observe(25, 42, 85);
    p = pacing(42, 85, flow.rates(25), 30, true, true, 20, 12, 8);
    REQUIRE(p.recover); // reserve restored, recovery still behind spending
    flow.observe(35, 55, 85);
    CHECK(pacing(55, 85, flow.rates(35), 30, true, true, 20, 12, 8).recover);
    flow.observe(45, 69, 85);
    p = pacing(69, 85, flow.rates(45), 30, true, true, 20, 12, 8);
    REQUIRE_FALSE(p.recover);
    CHECK(s.decide({.now=44.9, .resting=true, .want=true, .recovered=true}) == Decision::StayDown);
    CHECK(s.decide({.now=45, .resting=true, .want=true, .recovered=!p.recover, .tickLanded=true}) == Decision::Stand);
    CHECK_FALSE(pacing(69, 85, flow.rates(46), 30, true, false, 20, 12, 8).recover);
}

TEST_CASE("Full MP ends recovery even when its capped rate remains behind spending", "[cardian][rest][pacing]")
{
    CHECK_FALSE(pacing(85, 85, {2, 0.1}, 200, true, true, 20, 12, 8).recover);
    CHECK(pacing(32, 85, {}, 0, false, true, 20, 12, 8).recover); // rebuild buffer beyond entry line
    CHECK_FALSE(pacing(34, 85, {}, 0, false, true, 20, 12, 8).recover);
}

TEST_CASE("A recovery after the shared party sample can release the same paying tick", "[cardian][rest][pacing]")
{
    Flow flow;
    flow.observe(0, 60, 85);
    flow.observe(5, 30, 85);
    flow.observe(25, 42, 85);
    flow.observe(35, 55, 85);
    REQUIRE(pacing(55, 85, flow.rates(45), 30, true, true, 20, 12, 8).recover);
    // The mage's own decision refreshes her MP after Healing advances.
    flow.observe(45, 69, 85);
    const auto fresh = pacing(69, 85, flow.rates(45), 30, true, true, 20, 12, 8);
    State s;
    CHECK(s.decide({.now=45, .resting=true, .want=true, .recovered=!fresh.recover, .tickLanded=true}) == Decision::Stand);
}

TEST_CASE("Emergency Cure prediction uses actual recast and recovery timing", "[cardian][rest]")
{
    for (int elapsed = 0; elapsed <= 4; ++elapsed)
    {
        CHECK(cureBeforeRecovery(4 - elapsed, 2, 20));
    }
    CHECK_FALSE(cureBeforeRecovery(30, 2, 20));
    CHECK(cureBeforeRecovery(18, 2, 20));
    // A faster configured recovery interval must not inherit a fixed floor.
    CHECK_FALSE(cureBeforeRecovery(9, 2, 10));
    CHECK(cureBeforeRecovery(8, 2, 10));
    CHECK(cureBeforeRecovery(30, 2, 40));
}

TEST_CASE("Rest recovery follows Healing's empty first tick and Clear Mind ramp", "[cardian][rest]")
{
    REQUIRE(mpAtTick(1, 0, 0) == 0);
    REQUIRE(mpAtTick(2, 0, 0) == 12);
    REQUIRE(mpAtTick(3, 0, 0) == 13);
    REQUIRE(mpAtTick(4, 2, 3) == 21);
    REQUIRE(timeToReady(12, 0, 10, 10, 0, 0) == 20);
    REQUIRE(timeToReady(25, 0, 10, 10, 0, 0) == 30);
    REQUIRE(timeToReady(87, 0, 10, 10, 0, 0) == 70);
    REQUIRE(timeToReady(210, 0, 10, 10, 0, 0) == 130);
}

TEST_CASE("Rest readiness preserves progress toward the actual next tick", "[cardian][rest]")
{
    REQUIRE(timeToReady(15, 4, 2.5, 10, 0, 0) == 2.5);
    REQUIRE(timeToReady(16, 4, 2.5, 10, 0, 0) == 12.5);
    REQUIRE(timeToReady(0, 4, 2.5, 10, 0, 0) == 0);
    REQUIRE(std::isinf(timeToReady(1, 0, 0, 0, 0, 0)));
}

TEST_CASE("Rest interruption preserves long rests and stronger Clear Mind", "[cardian][rest]")
{
    REQUIRE(interruptionCost(9, 10, 10, 0, 0) > interruptionCost(3, 10, 10, 0, 0));
    REQUIRE(interruptionCost(9, 10, 10, 2, 0) > interruptionCost(9, 10, 10, 0, 0));
    REQUIRE(interruptionCost(4, 1, 10, 0, 0) >= interruptionCost(4, 10, 10, 0, 0));
}

TEST_CASE("Rest with leader delays both edges and cancels an unobserved kneel", "[cardian][rest]")
{
    Follow follow;
    REQUIRE_FALSE(follow.request(true, true, 10, 1));
    REQUIRE(follow.request(true, true, 11, 1));
    REQUIRE(follow.request(true, false, 12, 1));
    REQUIRE_FALSE(follow.request(true, false, 13, 1));
    REQUIRE_FALSE(follow.request(true, true, 14, 1));
    REQUIRE_FALSE(follow.request(true, false, 14.5, 1));
    REQUIRE_FALSE(follow.request(true, false, 16, 1));
}

TEST_CASE("Disabling Rest with leader cancels its pending request", "[cardian][rest]")
{
    Follow follow;
    REQUIRE(follow.request(true, true, 0, 0));
    REQUIRE_FALSE(follow.request(false, true, 1, 0));
    REQUIRE_FALSE(follow.request(true, true, 2, 1));
    REQUIRE(follow.request(true, true, 3, 1));
}

TEST_CASE("Support can rest through a new fight and ordinary matching spell rows", "[cardian][rest]")
{
    State s;
    REQUIRE(s.decide({.now=10, .want=true}) == Decision::Kneel);
    // Ordinary spell availability is not an input to the resting lifecycle.
    REQUIRE(s.decide({.now=11, .resting=true, .want=true}) == Decision::StayDown);
    REQUIRE_FALSE(s.canAct(11, true));
}

TEST_CASE("Rest With Player requests rest when support pacing prefers standing", "[cardian][rest][pacing]")
{
    const auto p = pacing(61, 85, {}, 48, true, false, 20, 12, 8);
    REQUIRE_FALSE(p.recover); // the reported playtest state
    Follow follow;
    State s;
    REQUIRE(s.decide({.now=10, .want=p.recover,
        .withPlayer=follow.request(true, true, 10, 1)}) == Decision::StayUp);
    REQUIRE(s.decide({.now=11, .want=p.recover,
        .withPlayer=follow.request(true, true, 11, 1)}) == Decision::Kneel);
    REQUIRE_FALSE(s.canAct(11, true));
}

TEST_CASE("Reaching the MP target or full MP cannot end Rest With Player", "[cardian][rest][pacing]")
{
    State s;
    for (const double mp : {73.0, 85.0})
    {
        const auto p = pacing(mp, 85, {0, 1}, 48, true, true, 20, 12, 8);
        REQUIRE_FALSE(p.recover);
        CHECK(s.decide({.now=mp, .resting=true, .want=true, .withPlayer=true,
            .recovered=!p.recover, .tickLanded=true}) == Decision::StayDown);
    }
}

TEST_CASE("Support recovery takes over when Rest With Player ends", "[cardian][rest][pacing]")
{
    for (const double mp : {30.0, 73.0})
    {
        const auto p = pacing(mp, 85, {0, 1}, 48, true, true, 20, 12, 8);
        State s;
        REQUIRE(s.decide({.now=10, .resting=true, .want=true, .withPlayer=true,
            .recovered=!p.recover, .tickLanded=true}) == Decision::StayDown);
        // Once the player's request ends, preserve the normal recovery tick
        // rule; low MP keeps her down, a satisfied budget releases on a tick.
        CHECK(s.decide({.now=11, .resting=true, .want=true,
            .recovered=!p.recover}) == Decision::StayDown);
        CHECK(s.decide({.now=20, .resting=true, .want=true,
            .recovered=!p.recover, .tickLanded=true}) == (p.recover ? Decision::StayDown : Decision::Stand));
    }
}

TEST_CASE("Rest With Player releases after the reaction beat without autonomous recovery", "[cardian][rest]")
{
    Follow follow;
    State s;
    REQUIRE(s.decide({.now=10, .withPlayer=follow.request(true, true, 10, 0)}) == Decision::Kneel);
    CHECK(s.decide({.now=11, .resting=true,
        .withPlayer=follow.request(true, false, 11, 1)}) == Decision::StayDown);
    CHECK(s.decide({.now=12, .resting=true,
        .withPlayer=follow.request(true, false, 12, 1)}) == Decision::Stand);
    CHECK_FALSE(s.canAct(12, false));
    CHECK(s.canAct(13, false));
}

TEST_CASE("Rest With Player yields to danger, orders, actions and urgent healing", "[cardian][rest]")
{
    for (const auto& facts : {
        Facts{.now=10, .resting=true, .withPlayer=true, .blocked=true},
        Facts{.now=10, .resting=true, .withPlayer=true, .urgent=true},
        Facts{.now=10, .resting=true, .withPlayer=true, .moving=true}})
    {
        State s;
        CHECK(s.decide(facts) == Decision::Stand);
        CHECK_FALSE(s.canAct(10, false));
        CHECK(s.canAct(11, false));
    }
    State s;
    CHECK(s.decide({.now=10, .withPlayer=true, .blocked=true}) == Decision::StayUp);
    CHECK(s.decide({.now=11, .withPlayer=true}) == Decision::Kneel);
}

TEST_CASE("An urgent cure stands now and casts only after standing", "[cardian][rest]")
{
    State s;
    REQUIRE(s.decide({.now=10, .want=true}) == Decision::Kneel);
    REQUIRE(s.decide({.now=11, .resting=true, .want=true, .urgent=true}) == Decision::Stand);
    REQUIRE_FALSE(s.canAct(11.9, false));
    REQUIRE(s.canAct(12, false));
}

TEST_CASE("A wake during kneeling waits for both physical transitions", "[cardian][rest][kneel]")
{
    for (auto facts : {
        Facts{.now=10.2, .resting=true, .want=true, .urgent=true},
        Facts{.now=10.2, .resting=true, .want=true, .blocked=true},
        Facts{.now=10.2, .resting=true, .want=true, .moving=true},
        Facts{.now=10.2, .resting=true}})
    {
        State s;
        REQUIRE(s.decide({.now=10, .want=true}) == Decision::Kneel);
        CHECK_THAT(s.readyIn(10.2, true), WithinAbs(1.8, 1e-9));
        REQUIRE(s.decide(facts) == Decision::StayDown);
        CHECK(s.standPending);
        CHECK_FALSE(s.canAct(10.2, true));
        facts.now = 10.9;
        CHECK(s.decide(facts) == Decision::StayDown);
        // Once requested, waking survives even a one-shot reason disappearing.
        REQUIRE(s.decide({.now=11, .resting=true, .want=true}) == Decision::Stand);
        CHECK_FALSE(s.standPending);
        CHECK_FALSE(s.canAct(11.2, false)); // the old early-Cure failure
        CHECK_FALSE(s.canAct(11.999, false));
        CHECK_THAT(s.readyIn(11.5, false), WithinAbs(0.5, 1e-9));
        CHECK(s.canAct(12, false));
    }
}

TEST_CASE("Repeated direct wake requests preserve the original kneel deadline", "[cardian][rest][kneel]")
{
    State s;
    REQUIRE(s.decide({.now=10, .want=true}) == Decision::Kneel);
    CHECK_FALSE(s.requestStand(10.2));
    CHECK_FALSE(s.requestStand(10.6));
    CHECK_FALSE(s.requestStand(10.999));
    REQUIRE(s.requestStand(11));
    s.stood(11);
    CHECK_THAT(s.readyIn(11, false), WithinAbs(1.0, 1e-9));
    CHECK_FALSE(s.canAct(11.999, false));
    CHECK(s.canAct(12, false));
}

TEST_CASE("A delayed movement tick still gives the actual rise a full second", "[cardian][rest][kneel]")
{
    State s;
    REQUIRE(s.decide({.now=10, .want=true}) == Decision::Kneel);
    REQUIRE_FALSE(s.requestStand(10.2));
    REQUIRE(s.requestStand(11.2));
    s.stood(11.2);
    CHECK_FALSE(s.canAct(12, false));
    CHECK_THAT(s.readyIn(12, false), WithinAbs(0.2, 1e-9));
    CHECK(s.canAct(12.2, false));
}

TEST_CASE("An engine interruption during kneeling cannot bypass either delay", "[cardian][rest][kneel]")
{
    State s;
    REQUIRE(s.decide({.now=10, .want=true}) == Decision::Kneel);
    // Healing can disappear before the next controller observation.
    CHECK_FALSE(s.canAct(10.2, false));
    CHECK_THAT(s.readyIn(10.2, false), WithinAbs(1.8, 1e-9));
    s.observe(false, 10.2);
    CHECK_THAT(s.readyIn(10.2, false), WithinAbs(1.8, 1e-9));
    CHECK_FALSE(s.standPending);
    CHECK(s.decide({.now=11.2, .want=true}) == Decision::StayUp);
    CHECK_FALSE(s.canAct(11.999, false));
    CHECK(s.canAct(12, false));
    REQUIRE(s.decide({.now=12, .want=true}) == Decision::Kneel);
    CHECK_FALSE(s.requestStand(12.2)); // a new rest has its own entry delay
    CHECK_THAT(s.readyIn(12.2, true), WithinAbs(1.8, 1e-9));
}

TEST_CASE("An established rest keeps the accepted one-second rise", "[cardian][rest][kneel]")
{
    State s;
    REQUIRE(s.decide({.now=10, .want=true}) == Decision::Kneel);
    CHECK_THAT(s.readyIn(30, true), WithinAbs(1.0, 1e-9));
    REQUIRE(s.requestStand(30));
    s.stood(30);
    CHECK_FALSE(s.canAct(30.999, false));
    CHECK(s.canAct(31, false));
}

TEST_CASE("An empty camp preserves recovery beyond the pacing target during a distant pull", "[cardian][rest][pacing]")
{
    Flow flow;
    State s;
    flow.observe(0, 47, 85);
    flow.observe(1, 24, 85);
    const auto start = pacing(24, 85, flow.rates(1), 30.25, true, false, 20, 12, 8);
    REQUIRE(start.recover);
    REQUIRE(s.decide({.now=2, .want=start.recover}) == Decision::Kneel);

    double mp = 24;
    for (int tick = 2; tick <= 6; ++tick)
    {
        const double now = 2 + tick * 10;
        mp = std::min(85.0, mp + mpAtTick(tick, 0, 0));
        flow.observe(now, mp, 85);
        const auto p = pacing(mp, 85, flow.rates(now), 30.25, true, true, 20, 12, 8);
        if (tick == 3)
        {
            REQUIRE(mp == 49); // the observed long-pull stand-up
            REQUIRE_FALSE(p.recover);
        }
        const auto decision = s.decide({.now=now, .resting=true, .want=true,
            .campClear=true, .mpMissing=mp < 85, .recovered=!p.recover, .tickLanded=true});
        CHECK(decision == (mp < 85 ? Decision::StayDown : Decision::Stand));
        CHECK_FALSE(s.canAct(now, mp < 85));
    }
    CHECK(s.canAct(63, false));
}

TEST_CASE("An empty camp does not make a standing mage start resting", "[cardian][rest][pacing]")
{
    const auto p = pacing(78, 85, {7.0/60, 0}, 30.25, true, false, 20, 12, 8);
    REQUIRE_FALSE(p.recover);
    State s;
    CHECK(s.decide({.now=10, .want=p.recover, .campClear=true, .mpMissing=true}) == Decision::StayUp);
    CHECK(s.canAct(10, false));
    // Pacing can still start a rest when the reserve actually needs it.
    const auto low = pacing(24, 85, {23.0/60, 0}, 30.25, true, false, 20, 12, 8);
    REQUIRE(low.recover);
    CHECK(s.decide({.now=11, .want=low.recover, .campClear=true, .mpMissing=true}) == Decision::Kneel);
}

TEST_CASE("An enemy reaching camp resumes pacing without forcing a wake", "[cardian][rest][pacing]")
{
    for (const double mp : {18.0, 49.0})
    {
        State s;
        const auto p = pacing(mp, 85, {23.0/60, 25.0/60}, 30.25, true, true, 20, 12, 8);
        REQUIRE(s.decide({.now=10, .resting=true, .want=true, .campClear=true,
            .mpMissing=true, .recovered=!p.recover, .tickLanded=true}) == Decision::StayDown);
        // Camp engagement now sees the incoming enemy; preserve a partial tick.
        CHECK(s.decide({.now=11, .resting=true, .want=true, .mpMissing=true,
            .recovered=!p.recover}) == Decision::StayDown);
        CHECK(s.decide({.now=20, .resting=true, .want=true, .mpMissing=true,
            .recovered=!p.recover, .tickLanded=true}) == (p.recover ? Decision::StayDown : Decision::Stand));
    }
}

TEST_CASE("A command-menu order overrides both empty-camp recovery and Rest With Player", "[cardian][rest]")
{
    State s;
    REQUIRE(s.decide({.now=10, .resting=true, .want=true, .withPlayer=true,
        .campClear=true, .mpMissing=true, .recovered=true, .tickLanded=true}) == Decision::StayDown);
    // HasQueuedOrder is a physical blocker in RestTick, even if no enemy exists.
    REQUIRE(s.decide({.now=11, .resting=true, .want=true, .withPlayer=true,
        .campClear=true, .mpMissing=true, .blocked=true}) == Decision::Stand);
    CHECK_FALSE(s.canAct(11.9, false));
    REQUIRE(s.decide({.now=12, .want=true, .withPlayer=true,
        .campClear=true, .mpMissing=true, .blocked=true}) == Decision::StayUp);
    CHECK(s.canAct(12, false));
}

TEST_CASE("An empty camp cannot suppress an emergency, danger or required movement", "[cardian][rest]")
{
    for (const auto& facts : {
        Facts{.now=10, .resting=true, .want=true, .campClear=true, .mpMissing=true, .urgent=true},
        Facts{.now=10, .resting=true, .want=true, .campClear=true, .mpMissing=true, .blocked=true},
        Facts{.now=10, .resting=true, .want=true, .campClear=true, .mpMissing=true, .moving=true}})
    {
        State s;
        CHECK(s.decide(facts) == Decision::Stand);
        CHECK_FALSE(s.canAct(10, false));
        CHECK(s.canAct(11, false));
    }
}

TEST_CASE("The empty-camp hold ends with its role or camp and retains no stale latch", "[cardian][rest]")
{
    State s;
    REQUIRE(s.decide({.now=10, .resting=true, .want=true, .campClear=true,
        .mpMissing=true, .recovered=true, .tickLanded=true}) == Decision::StayDown);
    // With no autonomous request or Rest With Player left, stand immediately.
    CHECK(s.decide({.now=11, .resting=true, .mpMissing=true}) == Decision::Stand);
    CHECK(s.decide({.now=12, .mpMissing=true}) == Decision::StayUp);
}

TEST_CASE("Rest With Player and an empty camp hand off without losing recovery", "[cardian][rest]")
{
    State s;
    REQUIRE(s.decide({.now=10, .resting=true, .want=true, .withPlayer=true,
        .campClear=true, .mpMissing=true, .recovered=true, .tickLanded=true}) == Decision::StayDown);
    CHECK(s.decide({.now=20, .resting=true, .want=true, .campClear=true,
        .mpMissing=true, .recovered=true, .tickLanded=true}) == Decision::StayDown);
    // Full MP ends the camp hold; a renewed player rest can still keep her down.
    CHECK(s.decide({.now=30, .resting=true, .want=true, .withPlayer=true,
        .campClear=true, .recovered=true, .tickLanded=true}) == Decision::StayDown);
    CHECK(s.decide({.now=40, .resting=true, .want=true,
        .campClear=true, .recovered=true, .tickLanded=true}) == Decision::Stand);
}

TEST_CASE("Recovery completion stands just after the tick lands", "[cardian][rest]")
{
    State s;
    REQUIRE(s.decide({.now=10, .resting=true, .want=true, .recovered=true}) == Decision::StayDown);
    REQUIRE(s.decide({.now=20, .resting=true, .want=true, .recovered=true, .tickLanded=true}) == Decision::Stand);
}

TEST_CASE("Danger and blocked recovery override every rest request", "[cardian][rest]")
{
    State s;
    REQUIRE(s.decide({.now=10, .want=true, .blocked=true}) == Decision::StayUp);
    REQUIRE(s.decide({.now=11, .resting=true, .want=true, .blocked=true}) == Decision::Stand);
    REQUIRE_FALSE(s.wantsDown);
}

TEST_CASE("Losing the rest effect cannot immediately re-kneel", "[cardian][rest]")
{
    State s;
    REQUIRE(s.decide({.now=10, .want=true}) == Decision::Kneel);
    s.observe(false, 11);
    REQUIRE(s.decide({.now=11, .want=true}) == Decision::StayUp);
    REQUIRE(s.decide({.now=11.9, .want=true}) == Decision::StayUp);
    REQUIRE(s.decide({.now=12, .want=true}) == Decision::Kneel);
}

TEST_CASE("Time since rising cannot override the same MP pacing decision", "[cardian][rest][pacing]")
{
    for (const double at : {1.0, 2.0, 19.9, 20.0, 100.0})
    {
        for (const double mp : {45.0, 77.0})
        {
            State s;
            s.stood(0);
            const auto p = pacing(mp, 85, {0.8, 0}, 30, true, false, 20, 12, 8);
            CHECK(s.decide({.now=at, .want=p.recover}) == (mp == 45 ? Decision::Kneel : Decision::StayUp));
        }
    }
}

TEST_CASE("Finishing an action leaves rest entry to pacing without a quiet period", "[cardian][rest][pacing]")
{
    for (const double mp : {45.0, 77.0})
    {
        State s;
        s.stood(0);
        const auto p = pacing(mp, 85, {0.8, 0}, 30, true, false, 20, 12, 8);
        // The real action is still running at 10; the engine clears it at 10.1.
        CHECK(s.decide({.now=10, .want=p.recover, .blocked=true}) == Decision::StayUp);
        CHECK(s.decide({.now=10.1, .want=p.recover}) == (mp == 45 ? Decision::Kneel : Decision::StayUp));
    }
}

TEST_CASE("Sustained spell traffic produces recovery cycles before MP exhaustion", "[cardian][rest][pacing]")
{
    Flow flow;
    State s;
    double mp = 85;
    int restStart = 0;
    int casts = 0;
    int rests = 0;
    int nextCast = 1;
    bool down = false;
    flow.observe(0, mp, 85);
    s.stood(0);
    for (int at = 1; at <= 600; ++at)
    {
        bool tick = false;
        if (down && (at - restStart) % 10 == 0)
        {
            const int ticks = (at - restStart) / 10;
            mp = std::min(85.0, mp + mpAtTick(ticks, 0, 0));
            tick = ticks >= 2;
        }
        flow.observe(at, mp, 85);
        const auto p = pacing(mp, 85, flow.rates(at), 30, true, down, 20, 12, 8);
        const auto decision = s.decide({.now=static_cast<double>(at), .resting=down, .want=down || p.recover,
            .recovered=!p.recover, .tickLanded=tick});
        if (decision == Decision::Kneel)
        {
            CHECK(mp > 8); // ordinary pacing gets ahead of an empty MP pool
            down = true;
            restStart = at;
            ++rests;
        }
        else if (decision == Decision::Stand)
        {
            down = false;
        }
        if (!down && s.canAct(at, false) && at >= nextCast)
        {
            REQUIRE(mp >= 8);
            mp -= 8;
            ++casts;
            nextCast = at + 8;
            flow.observe(at, mp, 85);
        }
    }
    CHECK(rests >= 3);
    CHECK(casts >= 25);
}

TEST_CASE("A direct rest request waits only for the physical rise to finish", "[cardian][rest]")
{
    State s;
    s.stood(10);
    REQUIRE(s.decide({.now=10.9, .want=true}) == Decision::StayUp);
    REQUIRE_FALSE(s.canAct(10.9, false));
    REQUIRE(s.decide({.now=11, .want=true}) == Decision::Kneel);
}

TEST_CASE("Removing a rest request releases the same action gate", "[cardian][rest]")
{
    State s;
    REQUIRE(s.decide({.now=10, .want=true}) == Decision::Kneel);
    REQUIRE(s.decide({.now=11, .resting=true}) == Decision::Stand);
    REQUIRE_FALSE(s.canAct(11, false));
    REQUIRE(s.canAct(12, false));
}

TEST_CASE("An incoming camp reposition cannot interrupt recovery to enable Dia", "[cardian][rest]")
{
    State s;
    REQUIRE(s.decide({.now=10, .want=true}) == Decision::Kneel);
    // A new pull changes the desired seat and feeds a spell request.
    REQUIRE(s.decide({.now=15, .resting=true, .want=true, .moving=true,
                      .routinePosition=true}) == Decision::StayDown);
    REQUIRE_FALSE(s.canAct(15, true));
    // An urgent cure still wakes her, through the normal stand gate.
    REQUIRE(s.decide({.now=16, .resting=true, .want=true, .urgent=true,
                      .moving=true, .routinePosition=true}) == Decision::Stand);
    REQUIRE_FALSE(s.canAct(16, false));
    REQUIRE(s.canAct(17, false));
}

TEST_CASE("Rest defers only an ongoing camp rest's routine positioning", "[cardian][rest]")
{
    State s;
    // An awake mage finishes positioning before beginning a new rest.
    REQUIRE(s.decide({.now=10, .want=true, .moving=true,
                      .routinePosition=true}) == Decision::StayUp);
    REQUIRE(s.decide({.now=11, .want=true}) == Decision::Kneel);
    // Walking with the leader or an active path still requires standing.
    REQUIRE(s.decide({.now=12, .resting=true, .want=true,
                      .moving=true}) == Decision::Stand);
}

TEST_CASE("Danger and orders still interrupt rest during a deferred seat move", "[cardian][rest]")
{
    State s;
    REQUIRE(s.decide({.now=10, .want=true}) == Decision::Kneel);
    REQUIRE(s.decide({.now=11, .resting=true, .want=true, .blocked=true,
                      .moving=true, .routinePosition=true}) == Decision::Stand);
    REQUIRE_FALSE(s.wantsDown);
}
