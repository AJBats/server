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

// The fight log's arithmetic (RESEARCH §12.5): the figures a record keeps,
// the line it closes with, how a spot's memory folds and what a cure is
// worth. Pure, so a change here fails before a map server runs.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include "map/pawn/fight_math.h"

#include <string>

using namespace cardian::tactics;
using Catch::Matchers::ContainsSubstring;
using Catch::Matchers::WithinAbs;

TEST_CASE("Running: a plain mean at first, then the recent weigh more", "[cardian][tactics]")
{
    Running r;
    r.fold(10.0);
    CHECK_THAT(r.mean, WithinAbs(10.0, 1e-9));
    r.fold(20.0);
    CHECK_THAT(r.mean, WithinAbs(15.0, 1e-9)); // 1/2
    r.fold(30.0);
    CHECK_THAT(r.mean, WithinAbs(20.0, 1e-9)); // 1/3
    r.fold(40.0);
    CHECK_THAT(r.mean, WithinAbs(26.0, 1e-9)); // alpha 0.3 from here: 20 + 0.3 * 20
    CHECK(r.n == 4);
}

TEST_CASE("CureEstimate: a topped-up cure is a floor, an uncapped one is exact", "[cardian][tactics]")
{
    CureEstimate e;
    e.floor = 60; // the tier's minimum cure
    CHECK(e.predict() == 60);
    CHECK_FALSE(e.known());

    e.note(75, true); // filled the target: she heals for at least 75
    CHECK(e.predict() == 75);
    CHECK_FALSE(e.known());

    e.note(92, false); // did not fill the target: exactly 92
    CHECK(e.predict() == 92);
    CHECK(e.known());

    e.note(80, true); // a smaller topped-up cure changes nothing
    CHECK(e.predict() == 92);

    e.note(97, true); // a bigger one: she has grown
    CHECK(e.predict() == 97);

    CureEstimate seededHigh;
    seededHigh.floor = 30; // a floor above the truth (Cure I's minimum is 10; a level-9 WHM heals 22)
    seededHigh.note(22, false);
    CHECK(seededHigh.predict() == 22); // the measurement wins
}

TEST_CASE("CureTally and DebuffValue keep honest counts", "[cardian][tactics]")
{
    CureTally t;
    t.casts += 2;
    t.mpSpent += 48;
    t.hpLanded += 180;
    CHECK_THAT(t.hpPerMp(), WithinAbs(3.75, 1e-9));
    CHECK(CureTally{}.hpPerMp() == 0.0);

    DebuffValue d;
    d.note(true);
    d.note(false);
    d.note(true);
    CHECK(d.casts == 3);
    CHECK(d.lands == 2);
    CHECK(d.resists == 1);
}

namespace
{
    auto sampleFight() -> FightRecord
    {
        FightRecord r;
        r.mobId    = 1000;
        r.mobName  = "Orcish_Grunt";
        r.zoneName = "Ghelsba_Outpost";
        r.mobMaxHp = 900;
        r.openedAt = 100.0;
        r.closedAt = 147.0;
        r.closeWhy = "killed";

        auto& tank = r.member(1, "Jevyak");
        tank.damageTaken += 250;
        tank.hits += 5;
        tank.biggestHit   = 88;
        tank.damageDealt += 600;
        tank.wsDamage += 200;
        tank.wsCount += 1;

        // tank stays a valid reference while a second member is added (a deque)
        auto& mage = r.member(2, "Zapp");
        mage.damageTaken += 62;
        mage.hits += 1;
        mage.biggestHit = 62;
        mage.casts += 3;
        mage.mpSpent += 74;
        mage.hpCured += 190;
        mage.overcure += 22;
        mage.targeted += 1;

        r.member(1, "Jevyak").targeted += 1; // the same member again: no second entry
        r.switches  = 2;
        r.mobDamage = 620; // the 20 past what the members dealt: a damage-over-time tick
        tank.tpMoveDamage += 60;
        r.tpMoveNames.emplace_back("Whirl Claws", 60);

        r.casts.push_back(CastNote{ .caster = 2, .target = 1, .spell = 2, .spellName = "Cure II", .mp = 24, .landed = 90, .cure = true });
        r.casts.push_back(CastNote{ .caster = 2, .target = 1, .spell = 2, .spellName = "Cure II", .mp = 24, .landed = 90, .cure = true, .toppedUp = true });
        r.casts.push_back(CastNote{ .caster = 2, .target = 1, .spell = 1, .spellName = "Cure", .mp = 8, .landed = 10, .cure = true, .toppedUp = true });
        r.casts.push_back(CastNote{ .caster = 2, .target = 1000, .spell = 58, .spellName = "Paralyze", .mp = 6, .debuff = true, .tookEffect = true });
        r.casts.push_back(CastNote{ .caster = 2, .target = 1000, .spell = 23, .spellName = "Dia", .mp = 7, .debuff = true, .tookEffect = false });
        r.procs = 2;
        return r;
    }
} // namespace

TEST_CASE("FightRecord: one entry per member, and the sums", "[cardian][tactics]")
{
    const auto r = sampleFight();
    CHECK(r.members.size() == 2);
    CHECK(r.find(1)->targeted == 1);
    CHECK(r.find(3) == nullptr);
    CHECK(r.taken() == 312);
    CHECK(r.dealt() == 600);
    CHECK(r.cureMp() == 56);
    CHECK(r.cureCasts() == 3);
    CHECK(r.cureHp() == 190);
    CHECK(r.overcure() == 22);
    CHECK(r.biggest()->name == "Jevyak");
    CHECK_THAT(r.seconds(0.0), WithinAbs(47.0, 1e-9));
    CHECK_THAT(r.find(1)->meanHit(), WithinAbs(50.0, 1e-9));

    FightRecord open;
    open.openedAt = 10.0;
    CHECK_THAT(open.seconds(25.0), WithinAbs(15.0, 1e-9)); // still open: measured to now
}

TEST_CASE("summary: the one line the map log gets", "[cardian][tactics]")
{
    const auto line = summary(sampleFight());
    CHECK_THAT(line, ContainsSubstring("tactics: Orcish_Grunt in Ghelsba_Outpost, 47 s, killed: took 312 (6.6/s, biggest 88 on Jevyak, TP moves: Whirl Claws 60), dealt 600 of 620 the mob lost (1 weapon skill for 200)"));
    CHECK_THAT(line, ContainsSubstring("cures 3 for 56 MP, 190 HP landed, ~22 over"));
    CHECK_THAT(line, ContainsSubstring("Paralyze 1 cast, 1 landed"));
    CHECK_THAT(line, ContainsSubstring("Dia 1 cast, 0 landed"));
    CHECK_THAT(line, ContainsSubstring("2 procs"));
    CHECK_THAT(line, ContainsSubstring("the mob switched 2 times"));

    FightRecord quiet;
    quiet.mobName  = "Wild_Rabbit";
    quiet.zoneName = "West_Ronfaure";
    quiet.openedAt = 0.0;
    quiet.closedAt = 8.0;
    quiet.closeWhy = "killed";
    quiet.member(1, "Farmer").damageDealt += 40;
    const auto quietLine = summary(quiet);
    CHECK_THAT(quietLine, ContainsSubstring("took 0 (0.0/s), dealt 40"));
    CHECK_THAT(quietLine, !ContainsSubstring("cures"));
    CHECK_THAT(quietLine, !ContainsSubstring("switched"));
    CHECK_THAT(quietLine, !ContainsSubstring("left it at"));

    FightRecord walkedAway = quiet;
    walkedAway.closeWhy    = "left";
    walkedAway.mobMaxHp    = 218;
    walkedAway.mobHpLeft   = 98;
    CHECK_THAT(summary(walkedAway), ContainsSubstring("left: took 0 (0.0/s), dealt 40, left it at 98 HP (44%)"));
}

TEST_CASE("SpotAverages fold a fight and read back as one line", "[cardian][tactics]")
{
    SpotAverages spot;
    spot.fold(sampleFight());
    CHECK(spot.fights == 1);
    CHECK_THAT(spot.seconds.mean, WithinAbs(47.0, 1e-9));
    CHECK_THAT(spot.cureMp.mean, WithinAbs(56.0, 1e-9));
    CHECK_THAT(spot.biggestHit.mean, WithinAbs(88.0, 1e-9));
    CHECK_THAT(spot.takenPerSecond.mean, WithinAbs(312.0 / 47.0, 1e-9));

    auto second = sampleFight();
    second.closedAt = 100.0 + 53.0;
    spot.fold(second);
    CHECK(spot.fights == 2);
    CHECK_THAT(spot.seconds.mean, WithinAbs(50.0, 1e-9));

    CHECK_THAT(spot.line("Orcish_Grunt"), ContainsSubstring("Orcish_Grunt x2: 50 s, 56 MP of cures, biggest hit 88"));
}
