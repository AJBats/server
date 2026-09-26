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

// The conveyor's arithmetic (RESEARCH §12.12 item 2, §12.14): how requests
// merge into needs, what a need opens and withdraws, who is picked to cast
// one, the order a caster reads her slot in, and a mage's pace at a spot.
// Pure, so a change here fails before a map server runs.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include "map/pawn/conveyor_math.h"

#include <cstddef>
#include <string>
#include <vector>

using namespace cardian::tactics;
using Catch::Matchers::ContainsSubstring;
using Catch::Matchers::WithinAbs;

namespace
{
    auto row(const uint32 caster, const uint32 index, const uint16 spell = 0) -> Request
    {
        return Request{ .source = Source::Row, .caster = caster, .row = index, .spell = spell };
    }

    // The role names its holder but prefers nobody: her caster is the voice,
    // not a preference
    auto role(const double score, const uint32 caster = 0, const double landChance = 1.0) -> Request
    {
        return Request{ .source = Source::Role, .caster = caster, .score = score, .landChance = landChance };
    }

    auto reflexRequest(const double score, const uint32 caster = 0) -> Request
    {
        return Request{ .source = Source::Reflex, .caster = caster, .score = score };
    }

    auto needWith(const uint32 assigned, std::vector<Request> requests) -> Need
    {
        Need n;
        n.assigned = assigned;
        n.requests = std::move(requests);
        return n;
    }

    // The picked caster's id; 0 when the need is held
    auto pickedId(const Need& n, const std::vector<Candidate>& candidates) -> uint32
    {
        const Candidate* c = pickCaster(n, candidates);
        return c == nullptr ? 0 : c->id;
    }
} // namespace

TEST_CASE("Needs::feed merges by key and replaces the same voice", "[cardian][tactics][conveyor]")
{
    Needs      needs;
    const auto cureOnSeven = NeedKey{ .kind = NeedKind::Cure, .arg = 0, .target = 7 };

    needs.feed(cureOnSeven, Request{ .source = Source::Row, .caster = 1, .row = 2, .fedAt = 0.0 });
    needs.feed(cureOnSeven, Request{ .source = Source::Row, .caster = 1, .row = 2, .fedAt = 3.0 });
    REQUIRE(needs.needs.size() == 1);
    REQUIRE(needs.needs[0].requests.size() == 1); // the same voice re-feeding replaces its request
    CHECK_THAT(needs.needs[0].requests[0].fedAt, WithinAbs(3.0, 1e-9));

    needs.feed(cureOnSeven, role(5.0)); // another voice joins the need
    CHECK(needs.needs.size() == 1);
    CHECK(needs.needs[0].requests.size() == 2);

    needs.feed(NeedKey{ .kind = NeedKind::Cure, .arg = 0, .target = 8 }, row(1, 2));
    CHECK(needs.needs.size() == 2); // another target, another need

    // a status spell keys on the effect it writes: two effects, two needs
    needs.feed(NeedKey{ .kind = NeedKind::Status, .arg = 134, .target = 9 }, row(1, 3));
    needs.feed(NeedKey{ .kind = NeedKind::Status, .arg = 4, .target = 9 }, row(1, 4));
    CHECK(needs.needs.size() == 4);

    // two role holders are two voices on one need, each with her own chance
    Needs      role_holders;
    const auto paralyzeOnNine = NeedKey{ .kind = NeedKind::Status, .arg = 5, .target = 9 };
    role_holders.feed(paralyzeOnNine, role(2.0, 7, 0.4));
    role_holders.feed(paralyzeOnNine, role(2.0, 8, 0.9));
    REQUIRE(role_holders.needs.size() == 1);
    REQUIRE(role_holders.needs[0].requests.size() == 2);
    CHECK(role_holders.needs[0].requests[0].caster == 7);
    CHECK_THAT(role_holders.needs[0].requests[0].landChance, WithinAbs(0.4, 1e-9));
    CHECK(role_holders.needs[0].requests[1].caster == 8);
    CHECK_THAT(role_holders.needs[0].requests[1].landChance, WithinAbs(0.9, 1e-9));

    // the reflex is a voice of its own: the same holder re-feeding replaces it
    role_holders.feed(paralyzeOnNine, reflexRequest(-1.0, 7));
    CHECK(role_holders.needs[0].requests.size() == 3);
    role_holders.feed(paralyzeOnNine, reflexRequest(-4.0, 7));
    REQUIRE(role_holders.needs[0].requests.size() == 3);
    CHECK_THAT(role_holders.needs[0].requests[2].score, WithinAbs(-4.0, 1e-9));
}

TEST_CASE("Needs::open returns the existing need or opens an empty one", "[cardian][tactics][conveyor]")
{
    Needs      needs;
    const auto key = NeedKey{ .kind = NeedKind::Damage, .arg = 3, .target = 11 };

    needs.open(key); // a cast in flight nobody asked for
    needs.open(key);
    REQUIRE(needs.needs.size() == 1);
    CHECK(needs.needs[0].requests.empty());
    CHECK(needs.needs[0].key == key);
    CHECK(needs.find(key) != nullptr);

    needs.feed(key, row(3, 1)); // a voice joins the need already open
    REQUIRE(needs.needs.size() == 1);
    CHECK(needs.needs[0].requests.size() == 1);
}

TEST_CASE("Needs::expire withdraws stale requests and empty needs, but never a locked need", "[cardian][tactics][conveyor]")
{
    Needs      needs;
    const auto key = NeedKey{ .kind = NeedKind::Cure, .arg = 0, .target = 7 };

    needs.feed(key, Request{ .source = Source::Row, .caster = 1, .row = 1, .fedAt = 0.0 });
    needs.feed(key, Request{ .source = Source::Row, .caster = 2, .row = 1, .fedAt = 3.0 });
    REQUIRE(needs.needs.size() == 1);

    needs.expire(5.0, 4.0);
    REQUIRE(needs.needs.size() == 1);
    REQUIRE(needs.needs[0].requests.size() == 1);
    CHECK_THAT(needs.needs[0].requests[0].fedAt, WithinAbs(3.0, 1e-9));

    needs.expire(8.0, 4.0);
    CHECK(needs.needs.empty()); // nothing left to speak for it

    // a cast in flight keeps its requests however stale: the rows that asked
    // are owed their retry stamps when it lands
    Needs      casting;
    const auto locked = NeedKey{ .kind = NeedKind::Status, .arg = 2, .target = 9 };
    casting.feed(locked, Request{ .source = Source::Row, .caster = 1, .row = 1, .fedAt = 0.0 });
    casting.open(locked).lockedBy = 5;
    casting.expire(100.0, 4.0);
    REQUIRE(casting.needs.size() == 1);
    CHECK(casting.needs[0].requests.size() == 1);

    // and a locked need nobody asked for outlives the sweep too
    Needs      unasked;
    const auto quiet = NeedKey{ .kind = NeedKind::Damage, .arg = 4, .target = 9 };
    unasked.open(quiet).lockedBy = 5;
    unasked.expire(100.0, 4.0);
    CHECK(unasked.needs.size() == 1);

    Needs      abandoned;
    const auto unlocked = NeedKey{ .kind = NeedKind::Damage, .arg = 4, .target = 9 };
    abandoned.open(unlocked);
    abandoned.expire(100.0, 4.0);
    CHECK(abandoned.needs.empty()); // no requests, no cast: gone
}

TEST_CASE("Needs::forget removes exactly the key", "[cardian][tactics][conveyor]")
{
    Needs      needs;
    const auto cureOnSeven = NeedKey{ .kind = NeedKind::Cure, .arg = 0, .target = 7 };
    const auto cureOnEight = NeedKey{ .kind = NeedKind::Cure, .arg = 0, .target = 8 };
    const auto naOnSeven   = NeedKey{ .kind = NeedKind::Na, .arg = 14, .target = 7 };

    needs.feed(cureOnSeven, row(1, 1));
    needs.feed(cureOnEight, row(1, 2));
    needs.feed(naOnSeven, row(1, 3));
    REQUIRE(needs.needs.size() == 3);

    needs.forget(cureOnSeven);
    CHECK(needs.needs.size() == 2);
    CHECK(needs.find(cureOnSeven) == nullptr);
    CHECK(needs.find(cureOnEight) != nullptr); // the same kind on another target stays
    CHECK(needs.find(naOnSeven) != nullptr);   // another kind on the same target stays

    needs.forget(cureOnSeven); // a key not held is no error
    CHECK(needs.needs.size() == 2);
}

TEST_CASE("Need: preferred, rowFed, fedBy, held, reflex", "[cardian][tactics][conveyor]")
{
    Need roleOnly;
    roleOnly.requests.push_back(role(5.0, 7));
    CHECK(roleOnly.preferred() == 0); // the role names nobody
    CHECK_FALSE(roleOnly.rowFed());
    CHECK_FALSE(roleOnly.reflex());
    CHECK(roleOnly.fedBy(7)); // she is the voice, not the preference
    CHECK_FALSE(roleOnly.fedBy(5));

    Need n;
    n.requests.push_back(row(5, 3, 2));
    n.requests.push_back(row(6, 1, 9));
    CHECK(n.preferred() == 5); // the mage of the first row fed, not the lowest row
    CHECK(n.rowFed());
    CHECK(n.fedBy(5));
    CHECK(n.fedBy(6));
    CHECK_FALSE(n.fedBy(7));

    n.requests.push_back(role(1.0, 7));
    CHECK(n.fedBy(7));
    CHECK(n.preferred() == 5); // a role holder joining changes nothing
    CHECK_FALSE(n.reflex());

    n.requests.push_back(reflexRequest(-3.0, 7));
    CHECK(n.reflex());

    CHECK(n.held()); // nobody assigned, nothing in flight
    n.assigned = 6;
    CHECK_FALSE(n.held());
    n.assigned = 0;
    n.lockedBy = 6;
    CHECK_FALSE(n.held());
    n.assigned = 6;
    CHECK_FALSE(n.held());
}

TEST_CASE("Need::lead and askedSpell agree: the preferred caster's lowest row", "[cardian][tactics][conveyor]")
{
    Need n;
    n.requests.push_back(row(5, 3, 2));
    n.requests.push_back(row(6, 1, 9));
    n.requests.push_back(row(5, 1, 4));

    CHECK(n.preferred() == 5);
    CHECK(n.askedSpell() == 4); // her lowest row names the spell, not her first fed
    CHECK(n.lead().source == Source::Row);
    CHECK(n.lead().caster == 5); // the preferred caster before another's lower row
    CHECK(n.lead().row == 1);
    CHECK(n.lead().spell == 4);

    n.requests.push_back(reflexRequest(-3.0, 6));
    CHECK(n.lead().source == Source::Reflex); // the reflex speaks before everything
    CHECK(n.askedSpell() == 4);               // and leaves the asked tier alone

    const Need empty; // a cast in flight nobody asked for
    CHECK(empty.lead().source == Source::Row);
    CHECK(empty.lead().caster == 0);
    CHECK(empty.lead().spell == 0);
    CHECK(empty.askedSpell() == 0);
    CHECK(empty.preferred() == 0);
}

TEST_CASE("pickCaster: standing before kneeling, the preferred before the rest, then land chance, load, id; none when nobody is open", "[cardian][tactics][conveyor]")
{
    Need n;
    n.requests.push_back(row(1, 1)); // preferred caster 1

    const std::vector<Candidate> preferredStanding{
        Candidate{ .id = 1, .open = true },
        Candidate{ .id = 2, .open = true, .landChance = 0.99, .load = 0 },
    };
    CHECK(pickedId(n, preferredStanding) == 1); // the preferred beats a better land chance

    const std::vector<Candidate> preferredKneeling{
        Candidate{ .id = 1, .open = true, .kneeling = true },
        Candidate{ .id = 2, .open = true },
    };
    CHECK(pickedId(n, preferredKneeling) == 2); // standing before kneeling

    const std::vector<Candidate> kneelingAlone{
        Candidate{ .id = 1, .open = true, .kneeling = true },
    };
    CHECK(pickedId(n, kneelingAlone) == 1); // she casts from the kneel rather than nobody

    const std::vector<Candidate> preferredAway{
        Candidate{ .id = 1 },
        Candidate{ .id = 2, .open = true, .kneeling = true },
        Candidate{ .id = 3, .open = true },
    };
    CHECK(pickedId(n, preferredAway) == 3);

    const std::vector<Candidate> byChance{
        Candidate{ .id = 2, .open = true, .landChance = 0.6 },
        Candidate{ .id = 3, .open = true, .landChance = 0.9 },
    };
    CHECK(pickedId(n, byChance) == 3);

    const std::vector<Candidate> byLoad{
        Candidate{ .id = 2, .open = true, .landChance = 0.9, .load = 2 },
        Candidate{ .id = 3, .open = true, .landChance = 0.9, .load = 0 },
    };
    CHECK(pickedId(n, byLoad) == 3);

    const std::vector<Candidate> allEqual{
        Candidate{ .id = 3, .open = true },
        Candidate{ .id = 2, .open = true },
    };
    CHECK(pickedId(n, allEqual) == 2); // the lower id

    const std::vector<Candidate> nobodyOpen{
        Candidate{ .id = 1 },
        Candidate{ .id = 2 },
    };
    CHECK(pickCaster(n, nobodyOpen) == nullptr); // the need is held
}

TEST_CASE("rankFor and slotOrder: the reflex, her rows in order, others' rows, then the role by score", "[cardian][tactics][conveyor]")
{
    std::vector<Need> needs;
    needs.push_back(needWith(1, { role(5.0) }));         // 0: A
    needs.push_back(needWith(1, { row(1, 3) }));         // 1: B
    needs.push_back(needWith(1, { row(1, 1) }));         // 2: C
    needs.push_back(needWith(1, { reflexRequest(0.0) })); // 3: D
    needs.push_back(needWith(1, { row(9, 2) }));         // 4: E, another's row
    needs.push_back(needWith(1, { role(-2.0) }));        // 5: F
    needs.push_back(needWith(0, { row(1, 0) }));         // 6: G, a cast in flight
    needs.back().lockedBy = 9;
    needs.push_back(needWith(2, { row(2, 1) }));         // 7: H, another caster's
    needs.push_back(needWith(1, { role(-1.0) }));        // 8: I, her top-up of another's cure in flight
    needs.back().lockedBy = 9;

    const auto order = slotOrder(needs, 1);
    CHECK(order == std::vector<std::size_t>{ 3, 2, 1, 4, 5, 8, 0 });

    CHECK(slotOrder(needs, 0).empty()); // nothing for nobody

    CHECK(rankFor(needs[3], 1).tier == 0); // the reflex
    const auto herRow = rankFor(needs[2], 1);
    CHECK(herRow.tier == 1);
    CHECK_THAT(herRow.order, WithinAbs(1.0, 1e-9));
    CHECK(rankFor(needs[4], 1).tier == 2); // another's row
    CHECK(rankFor(needs[0], 1).tier == 3); // the role

    const auto both = rankFor(needWith(1, { row(1, 4), role(-9.0) }), 1);
    CHECK(both.tier == 1); // the best of its requests speaks
    CHECK_THAT(both.order, WithinAbs(4.0, 1e-9));
}

TEST_CASE("margin and cureWanted", "[cardian][tactics][conveyor]")
{
    CHECK_THAT(margin(70, 40.0, 3.0, 2.0), WithinAbs(24.0, 1e-9));
    CHECK_THAT(margin(44, 40.0, 3.0, 2.0), WithinAbs(-2.0, 1e-9)); // waiting is unsafe

    CHECK(cureWanted(25, 22));
    CHECK_FALSE(cureWanted(15, 22)); // the tier would overcure
    CHECK_FALSE(cureWanted(25, 0));  // no tier priced
    CHECK(cureWanted(22, 22));
}

TEST_CASE("Pace: a cycle's spend against the net MP between cycles, behind by a fifth, back once resting wins", "[cardian][tactics][conveyor]")
{
    Pace p;
    p.opened(50); // no close on record: nothing to measure the rest against
    CHECK(p.regained.n == 0);
    CHECK_FALSE(p.measured());
    CHECK_FALSE(p.behind());

    p.closed(60, 30);
    p.opened(78);
    CHECK(p.regained.n == 1);
    CHECK_THAT(p.regained.mean, WithinAbs(18.0, 1e-9));
    CHECK_FALSE(p.measured());

    for (int i = 0; i < 2; ++i)
    {
        p.closed(60, 30);
        p.opened(78);
    }
    CHECK(p.spent.n == 3);
    CHECK(p.regained.n == 3);
    CHECK_THAT(p.spent.mean, WithinAbs(30.0, 1e-9));
    CHECK_THAT(p.regained.mean, WithinAbs(18.0, 1e-9));
    CHECK(p.measured());
    CHECK(p.behind()); // 30 against 18 * 1.2 + 2
    CHECK_THAT(p.line("Zapp"), ContainsSubstring("Zapp"));
    CHECK_THAT(p.line("Zapp"), ContainsSubstring("behind pace"));
    CHECK_THAT(p.line("Zapp"), ContainsSubstring("net +18"));

    // the hysteresis: a fifth to fall behind, but only even to come back
    Pace close;
    for (int i = 0; i < 3; ++i)
    {
        close.closed(60, 20);
        close.opened(78);
    }
    REQUIRE(close.measured());
    CHECK_THAT(close.spent.mean, WithinAbs(20.0, 1e-9));
    CHECK_THAT(close.regained.mean, WithinAbs(18.0, 1e-9));
    CHECK_FALSE(close.behind()); // 20 is under 23.6: not enough to say it
    CHECK_THAT(close.line("Zapp"), ContainsSubstring("on pace"));

    close.saidBehind = true;
    CHECK(close.behind()); // said once, it holds until resting wins outright
    CHECK_THAT(close.line("Zapp"), ContainsSubstring("behind pace"));

    // a spot where the fights outrun the rest entirely
    Pace losing;
    for (int i = 0; i < 3; ++i)
    {
        losing.closed(60, 30);
        losing.opened(50);
    }
    REQUIRE(losing.measured());
    CHECK_THAT(losing.regained.mean, WithinAbs(-10.0, 1e-9));
    CHECK(losing.behind());
    CHECK_THAT(losing.line("Zapp"), ContainsSubstring("net -10"));
}

TEST_CASE("Pace is an EWMA after three samples", "[cardian][tactics][conveyor]")
{
    // Running::fold weighs max(0.3, 1/n): a plain mean over the first three,
    // then the last samples lead
    Pace p;
    p.closed(60, 100);
    CHECK_THAT(p.spent.mean, WithinAbs(100.0, 1e-9)); // n = 1

    p.closed(60, 10);
    CHECK_THAT(p.spent.mean, WithinAbs(55.0, 1e-9)); // n = 2

    p.closed(60, 10);
    CHECK_THAT(p.spent.mean, WithinAbs(40.0, 1e-9)); // n = 3, the plain mean

    p.closed(60, 10);
    CHECK_THAT(p.spent.mean, WithinAbs(31.0, 1e-9)); // n = 4: 40 + 0.3 * (10 - 40)
    CHECK(p.spent.mean < 32.5);                      // not the plain mean of the four
    CHECK(p.spent.mean > 10.0);                      // and not the last sample either
}

TEST_CASE("Conveyor: an in-range helper casts before the requester walks; otherwise the requester can approach", "[cardian][tactics][conveyor]")
{
    Need n;
    n.requests.push_back(row(3, 5, 1));
    std::vector<Candidate> casters{
        { .id = 3, .open = true, .spell = 1, .inRange = false },
        { .id = 4, .open = true, .spell = 1, .inRange = true },
    };
    REQUIRE(pickCaster(n, casters) != nullptr);
    CHECK(pickCaster(n, casters)->id == 4);
    casters[1].open = false;
    REQUIRE(pickCaster(n, casters) != nullptr);
    CHECK(pickCaster(n, casters)->id == 3);
    casters[0].open = false;
    CHECK(pickCaster(n, casters) == nullptr);
}
