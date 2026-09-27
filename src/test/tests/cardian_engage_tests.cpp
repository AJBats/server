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

// The engage door (engage_math.h, ROADMAP K3): which fight a cardian takes
// and how. Her Attack rows pick new fights top down, each row's finder
// naming the first foe of its kind; a Support Mage with nothing picked
// attends the party's fight; a committed attendance is kept, upgraded or
// dropped; a hold for the player's strike follows his switch or stands
// down. Pure, over plain descriptions of the foes, so the rules are pinned
// here and the controller only gathers the facts.

#include <catch2/catch_test_macros.hpp>

#include "map/pawn/engage_math.h"
#include "map/pawn/gambit_ids.h"
#include "map/pawn/gambit_text.h"

#include <cstddef>
#include <optional>
#include <vector>

using namespace cardian::engage;

namespace
{
    // The foes as the controller gathers them, one kind each
    auto leadersTarget() -> Foe
    {
        Foe f;
        f.leadersTarget = true;
        return f;
    }

    auto allysFight() -> Foe
    {
        Foe f;
        f.allysFight = true;
        return f;
    }

    auto onAlly() -> Foe
    {
        Foe f;
        f.onParty = true;
        return f;
    }

    auto onSelf() -> Foe
    {
        Foe f;
        f.onParty = true;
        f.onSelf  = true;
        return f;
    }

    // The melee defaults' trio, in their order
    auto trio() -> std::vector<Row>
    {
        return {
            { 1, pawn::G_TARGET_LEADERS_TARGET, true },
            { 2, pawn::G_TARGET_TARGETED_BY_ALLY, true },
            { 3, pawn::G_TARGET_TARGETING_ALLY, true },
        };
    }

    // Every row's conditions hold, on every foe
    constexpr auto always = [](std::size_t, std::size_t)
    {
        return true;
    };

    auto choose(const std::vector<Row>& rows, const std::vector<Foe>& foes, const bool master = true, const bool retreating = false) -> std::optional<RowPick>
    {
        return chooseRow(master, retreating, rows, foes, always);
    }
} // namespace

TEST_CASE("engage door: each foe target names its finder", "[cardian][engage]")
{
    CHECK(finderOf(pawn::G_TARGET_LEADERS_TARGET) == Finder::LeadersTarget);
    CHECK(finderOf(pawn::G_TARGET_TARGETED_BY_ALLY) == Finder::AllysFight);
    CHECK(finderOf(pawn::G_TARGET_TARGETING_ALLY) == Finder::OnAlly);
    CHECK(finderOf(pawn::G_TARGET_TARGETING_SELF) == Finder::OnSelf);
    CHECK(finderOf(gambits::G_TARGET::TARGET) == Finder::Any); // `Foe: any` and the foe conditions
    CHECK_FALSE(finderOf(gambits::G_TARGET::SELF).has_value());
    CHECK_FALSE(finderOf(gambits::G_TARGET::PARTY).has_value());
}

TEST_CASE("engage door: a finder accepts its own kind; targeting ally includes a mob on her", "[cardian][engage]")
{
    CHECK(accepts(Finder::LeadersTarget, leadersTarget()));
    CHECK_FALSE(accepts(Finder::LeadersTarget, allysFight()));
    CHECK(accepts(Finder::AllysFight, allysFight()));
    CHECK_FALSE(accepts(Finder::AllysFight, onAlly()));
    CHECK(accepts(Finder::OnAlly, onAlly()));
    CHECK(accepts(Finder::OnAlly, onSelf()));
    CHECK(accepts(Finder::OnSelf, onSelf()));
    CHECK_FALSE(accepts(Finder::OnSelf, onAlly()));
    CHECK_FALSE(accepts(Finder::OnAlly, Foe{}));
}

TEST_CASE("engage door: Foe: any accepts every foe of the party's fight, and nothing else", "[cardian][engage]")
{
    CHECK(accepts(Finder::Any, leadersTarget()));
    CHECK(accepts(Finder::Any, allysFight()));
    CHECK(accepts(Finder::Any, onAlly()));
    CHECK(accepts(Finder::Any, onSelf()));
    CHECK_FALSE(accepts(Finder::Any, Foe{}));

    // Its why line borrows the words of the finder that would have found it
    CHECK(finderFor(leadersTarget()) == Finder::LeadersTarget);
    CHECK(finderFor(allysFight()) == Finder::AllysFight);
    CHECK(finderFor(onSelf()) == Finder::OnAlly);
    CHECK(finderFor(Foe{}) == Finder::Any);
}

TEST_CASE("engage door: a Foe condition with Attack takes the first foe of the party's fight it holds on", "[cardian][engage]")
{
    // `Foe: HP >= 75% -> Attack`: the leader's target is below 75%, the
    // mob on an ally is fresh; the row takes the fresh one
    const std::vector<Row> rows{ { 1, gambits::G_TARGET::TARGET, true } };
    const std::vector<Foe> foes{ leadersTarget(), onAlly() };
    const auto fresh = [](std::size_t, const std::size_t foe)
    {
        return foe == 1;
    };
    const auto pick = chooseRow(true, false, rows, foes, fresh);
    REQUIRE(pick.has_value());
    CHECK(pick->foe == 1);
    CHECK(pick->finder == Finder::Any);

    // No foe it holds on: nothing, and a foe outside the party's fight is never one
    CHECK_FALSE(chooseRow(true, false, rows, std::vector<Foe>{ Foe{} }, always).has_value());
    CHECK_FALSE(chooseRow(true, false, rows, foes, [](std::size_t, std::size_t) { return false; }).has_value());

    // It claims the mob she is at on the same terms
    CHECK(claimingRow(true, rows, onSelf(), [](std::size_t) { return true; }).has_value());
    CHECK_FALSE(claimingRow(true, rows, onSelf(), [](std::size_t) { return false; }).has_value());
}

TEST_CASE("engage door: the door reads enabled Attack rows only, and none with the master off", "[cardian][engage]")
{
    const auto attack = pawn::text::parseRow("100|0:0|0:0:0|0");
    const auto cure   = pawn::text::parseRow("1|1:50|2:0:1|0");
    const auto broken = pawn::text::parseRow("102|0:0|2:0:1|0"); // a foe target on a spell: the editor refuses it
    REQUIRE(attack.has_value());
    REQUIRE(cure.has_value());
    REQUIRE(broken.has_value());

    CHECK(doorReads(true, true, *attack));
    CHECK_FALSE(doorReads(true, false, *attack));
    CHECK_FALSE(doorReads(false, true, *attack));
    CHECK_FALSE(doorReads(true, true, *cure));
    CHECK_FALSE(doorReads(true, true, *broken));
}

TEST_CASE("engage door: list order wins", "[cardian][engage]")
{
    const std::vector<Foe> foes{ onAlly(), leadersTarget() };

    // The trio: the leader's target first, wherever it was gathered
    const auto first = choose(trio(), foes);
    REQUIRE(first.has_value());
    CHECK(first->row == 1);
    CHECK(first->finder == Finder::LeadersTarget);
    CHECK(first->foe == 1);

    // The same foes with "targeting ally" moved to the top
    const std::vector<Row> reordered{
        { 1, pawn::G_TARGET_TARGETING_ALLY, true },
        { 2, pawn::G_TARGET_LEADERS_TARGET, true },
    };
    const auto second = choose(reordered, foes);
    REQUIRE(second.has_value());
    CHECK(second->row == 1);
    CHECK(second->finder == Finder::OnAlly);
    CHECK(second->foe == 0);
}

TEST_CASE("engage door: a row with no foe of its kind gives the next row its turn", "[cardian][engage]")
{
    const auto pick = choose(trio(), { allysFight() });
    REQUIRE(pick.has_value());
    CHECK(pick->row == 2);
    CHECK(pick->finder == Finder::AllysFight);

    CHECK_FALSE(choose(trio(), {}).has_value());
    CHECK_FALSE(choose({}, { leadersTarget() }).has_value());
}

TEST_CASE("engage door: a disabled row is skipped", "[cardian][engage]")
{
    auto rows       = trio();
    rows[0].enabled = false;
    const auto pick = choose(rows, { leadersTarget(), onAlly() });
    REQUIRE(pick.has_value());
    CHECK(pick->row == 3);
    CHECK(pick->foe == 1);

    rows[2].enabled = false;
    CHECK_FALSE(choose(rows, { leadersTarget(), onAlly() }).has_value());
}

TEST_CASE("engage door: with the master switch off her rows take nothing", "[cardian][engage]")
{
    CHECK_FALSE(choose(trio(), { leadersTarget(), allysFight(), onSelf() }, false).has_value());

    Foe everything;
    everything.leadersTarget = true;
    everything.allysFight    = true;
    everything.onParty       = true;
    everything.onSelf        = true;
    CHECK_FALSE(claimingRow(false, trio(), everything, [](std::size_t) { return true; }).has_value());
}

TEST_CASE("engage door: a retreat takes nothing, rows or party's fight", "[cardian][engage]")
{
    const std::vector<Foe> foes{ leadersTarget(), allysFight(), onSelf() };
    CHECK_FALSE(choose(trio(), foes, true, true).has_value());
    CHECK_FALSE(partyFight(true, foes).has_value());
    REQUIRE(partyFight(false, foes).has_value());
}

TEST_CASE("engage door: an underground or held-off foe is absent, and the next row acts", "[cardian][engage]")
{
    auto burrowed        = leadersTarget();
    burrowed.underground = true;
    const auto underground = choose(trio(), { burrowed, onAlly() });
    REQUIRE(underground.has_value());
    CHECK(underground->row == 3);
    CHECK(underground->foe == 1);

    auto refused    = leadersTarget();
    refused.heldOff = true;
    const auto held = choose(trio(), { refused, allysFight() });
    REQUIRE(held.has_value());
    CHECK(held->row == 2);
    CHECK(held->foe == 1);

    CHECK_FALSE(foeCounts(burrowed));
    CHECK_FALSE(foeCounts(refused));
    CHECK(foeCounts(leadersTarget()));
}

TEST_CASE("engage door: an absent foe gives the next foe of its kind its turn", "[cardian][engage]")
{
    auto refused    = onAlly();
    refused.heldOff = true;
    const std::vector<Foe> foes{ refused, onAlly() };

    CHECK(firstFound(Finder::OnAlly, foes) == std::optional<std::size_t>(1));
    const auto pick = choose({ { 1, pawn::G_TARGET_TARGETING_ALLY, true } }, foes);
    REQUIRE(pick.has_value());
    CHECK(pick->foe == 1);
}

TEST_CASE("engage door: the party's fight scans with the same fall-through as the rows", "[cardian][engage]")
{
    auto burrowed        = leadersTarget();
    burrowed.underground = true;

    // The player waits on a burrowed mob while a mob hits an ally: the
    // melee answer it, and the party's fight (attendance, the rest) sees it
    const std::vector<Foe> waiting{ burrowed, onAlly() };
    const auto             scan = partyFight(false, waiting);
    REQUIRE(scan.has_value());
    CHECK(scan->finder == Finder::OnAlly);
    CHECK(scan->foe == 1);

    const std::vector<Foe> alone{ burrowed };
    CHECK_FALSE(partyFight(false, alone).has_value());

    // The leader's fight first, whatever order the foes came in
    const std::vector<Foe> mixed{ onSelf(), allysFight(), leadersTarget() };
    const auto             order = partyFight(false, mixed);
    REQUIRE(order.has_value());
    CHECK(order->finder == Finder::LeadersTarget);
    CHECK(order->foe == 2);
}

TEST_CASE("engage door: a row's conditions are read on each foe of its kind, in turn", "[cardian][engage]")
{
    const std::vector<Foe> foes{ onAlly(), onAlly(), leadersTarget() };
    const std::vector<Row> rows{
        { 1, pawn::G_TARGET_TARGETING_ALLY, true },
        { 2, pawn::G_TARGET_LEADERS_TARGET, true },
    };

    // Row 1's conditions hold only on the second mob on an ally
    const auto second = chooseRow(true, false, rows, foes, [](const std::size_t row, const std::size_t foe)
                                  {
                                      return row != 0 || foe == 1;
                                  });
    REQUIRE(second.has_value());
    CHECK(second->row == 1);
    CHECK(second->foe == 1);

    // They hold on neither: row 2 has its turn
    const auto next = chooseRow(true, false, rows, foes, [](const std::size_t row, std::size_t)
                                {
                                    return row != 0;
                                });
    REQUIRE(next.has_value());
    CHECK(next->row == 2);
    CHECK(next->foe == 2);
}

TEST_CASE("engage door: a row claims any foe of its kind, while a new fight is the first found", "[cardian][engage]")
{
    // Two mobs on allies: "targeting ally" picks the first for a new fight,
    // and claims the second too, so a melee mage attending it fights it
    const std::vector<Foe> foes{ onAlly(), onAlly() };
    const std::vector<Row> rows{ { 1, pawn::G_TARGET_TARGETING_ALLY, true } };
    CHECK(firstFound(Finder::OnAlly, foes) == std::optional<std::size_t>(0));
    const auto claim = claimingRow(true, rows, foes[1], [](std::size_t) { return true; });
    REQUIRE(claim.has_value());
    CHECK(claim->row == 1);
    CHECK(claim->finder == Finder::OnAlly);

    // A claim is about her rows, not whether the foe counts for a new fight
    auto refused    = onAlly();
    refused.heldOff = true;
    CHECK(claimingRow(true, rows, refused, [](std::size_t) { return true; }).has_value());

    // Not its kind, a disabled row, or conditions that fail: no claim
    const std::vector<Row> unchecked{ { 1, pawn::G_TARGET_TARGETING_ALLY, false } };
    CHECK_FALSE(claimingRow(true, rows, allysFight(), [](std::size_t) { return true; }).has_value());
    CHECK_FALSE(claimingRow(true, unchecked, onAlly(), [](std::size_t) { return true; }).has_value());
    CHECK_FALSE(claimingRow(true, rows, onAlly(), [](std::size_t) { return false; }).has_value());

    // The first claiming row, top down
    const std::vector<Row> two{
        { 4, pawn::G_TARGET_TARGETING_SELF, true },
        { 5, pawn::G_TARGET_TARGETING_ALLY, true },
    };
    const auto top = claimingRow(true, two, onSelf(), [](std::size_t) { return true; });
    REQUIRE(top.has_value());
    CHECK(top->row == 4);
}

TEST_CASE("engage door: a Support Mage attends unless a row claims the mob", "[cardian][engage]")
{
    CHECK(attendsFight(true, false));
    CHECK_FALSE(attendsFight(true, true)); // the melee mage
    CHECK_FALSE(attendsFight(false, false));
    CHECK_FALSE(attendsFight(false, true));
}

TEST_CASE("engage door: a Support Mage pulls only with an Attack row", "[cardian][engage]")
{
    CHECK_FALSE(huntsForParty(true, false));
    CHECK(huntsForParty(true, true));
    CHECK(huntsForParty(false, false));
    CHECK(huntsForParty(false, true));
}

TEST_CASE("engage door: the attend fallback", "[cardian][engage]")
{
    // A row's pick is drawn on, Support Mage or not
    CHECK(doorAnswer(true, true, true, true) == How::Draw);
    CHECK(doorAnswer(true, false, false, false) == How::Draw);

    // No row took one: a Support Mage with a place attends the party's fight
    CHECK(doorAnswer(false, true, true, true) == How::Attend);

    // Without a place, without the role, or with no fight: nothing
    CHECK_FALSE(doorAnswer(false, true, true, false).has_value());
    CHECK_FALSE(doorAnswer(false, true, false, true).has_value());
    CHECK_FALSE(doorAnswer(false, false, true, true).has_value());

    // An attendance waits no beat; a draw waits her reaction beat
    CHECK_FALSE(waitsBeat(How::Attend));
    CHECK(waitsBeat(How::Draw));
}

TEST_CASE("engage door: the party's fight backs up a camp member's rows", "[cardian][engage]")
{
    // No row of hers took one (an edited set, or rows that name no foe
    // here): a camp member in the wild draws on the party's fight
    CHECK(doorAnswer(false, true, false, true, true) == How::Draw);
    CHECK(doorAnswer(false, true, false, false, true) == How::Draw);

    // A Support Mage of the camp still attends it, as anywhere
    CHECK(doorAnswer(false, true, true, true, true) == How::Attend);

    // Her own row's pick comes first; with no fight there is nothing
    CHECK(doorAnswer(true, true, false, true, true) == How::Draw);
    CHECK_FALSE(doorAnswer(false, false, false, true, true).has_value());

    // Anyone else with no row picked takes no fight of her own
    CHECK_FALSE(doorAnswer(false, true, false, true, false).has_value());
}

TEST_CASE("engage door: a default mage attends, and one with targeting ally on melees", "[cardian][engage]")
{
    // The mage defaults' Attack row ships unchecked
    const std::vector<Row> mage{ { 1, pawn::G_TARGET_TARGETING_ALLY, false } };
    const std::vector<Foe> foes{ leadersTarget(), onAlly() };

    const auto rows  = chooseRow(true, false, mage, foes, always);
    const auto party = partyFight(false, foes);
    REQUIRE(party.has_value());
    CHECK_FALSE(rows.has_value());
    CHECK(doorAnswer(rows.has_value(), party.has_value(), true, true) == How::Attend);
    CHECK(party->foe == 0);

    // Checked, it takes the mob on an ally, and she draws on it
    const std::vector<Row> melee{ { 1, pawn::G_TARGET_TARGETING_ALLY, true } };
    const auto taken = chooseRow(true, false, melee, foes, always);
    REQUIRE(taken.has_value());
    CHECK(taken->foe == 1);
    CHECK(doorAnswer(taken.has_value(), party.has_value(), true, true) == How::Draw);
}

TEST_CASE("engage door: a committed attendance is kept, upgraded, or dropped", "[cardian][engage]")
{
    // Still a Support Mage, no row on the mob: she keeps attending
    CHECK(keptAttendance(true, false) == Kept::Attend);

    // A row now claims that same mob: she draws on it (the melee-mage upgrade)
    CHECK(keptAttendance(true, true) == Kept::Draw);

    // Gambits switched off mid-fight: no role and no row, so she stops
    // attending rather than drawing on the mob
    CHECK(keptAttendance(false, false) == Kept::Stop);

    // No role but a row on the mob: she fights it
    CHECK(keptAttendance(false, true) == Kept::Draw);
}

TEST_CASE("engage door: the hold follows a switch her rows take, and stands down otherwise", "[cardian][engage]")
{
    // His target stays: the hold stands
    CHECK(holdStep(false, false, false) == HoldStep::Keep);
    CHECK(holdStep(false, true, true) == HoldStep::Keep);

    // He switched, her rows take the new mob and she may draw: follow
    CHECK(holdStep(true, true, true) == HoldStep::Follow);

    // He switched to a mob her rows do not take, or one she may not draw on
    // outright (too far, claimed): she never holds on the mob he left
    CHECK(holdStep(true, false, true) == HoldStep::StandDown);
    CHECK(holdStep(true, true, false) == HoldStep::StandDown);
    CHECK(holdStep(true, false, false) == HoldStep::StandDown);
}

TEST_CASE("engage door: with only targeted by ally on, a hold follows once another cardian has switched", "[cardian][engage]")
{
    const std::vector<Row> rows{ { 1, pawn::G_TARGET_TARGETED_BY_ALLY, true } };

    // The player switched to the second mob; the other cardian still holds
    // the first. Her rows take the first, not his new one: she stands down
    std::vector<Foe> foes{ allysFight(), leadersTarget() };
    auto             pick = chooseRow(true, false, rows, foes, always);
    REQUIRE(pick.has_value());
    CHECK(holdStep(true, pick->foe == 1, true) == HoldStep::StandDown);

    // The other cardian has followed him: now her rows take his new mob
    foes[0]            = Foe{};
    foes[1].allysFight = true;
    pick               = chooseRow(true, false, rows, foes, always);
    REQUIRE(pick.has_value());
    CHECK(holdStep(true, pick->foe == 1, true) == HoldStep::Follow);
}
