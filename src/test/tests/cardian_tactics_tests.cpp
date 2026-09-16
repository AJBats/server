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

#include "map/pawn/bank_math.h"
#include "map/pawn/fight_math.h"

#include <limits>
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

// --- the MP bank (RESEARCH §12.6, §12.13) ----------------------------------

TEST_CASE("Exchange: Cure II's floor until three casts, then the party's own rate", "[cardian][tactics][bank]")
{
    const auto floor = exchange(0, 0, 0);
    CHECK_THAT(floor.hpPerMp, WithinAbs(2.5, 1e-9));
    CHECK_FALSE(floor.measured);
    CHECK_FALSE(exchange(100, 48, 2).measured);

    const auto own = exchange(180, 48, 3);
    CHECK(own.measured);
    CHECK_THAT(own.hpPerMp, WithinAbs(3.75, 1e-9));
    CHECK_THAT(own.mp(75.0), WithinAbs(20.0, 1e-9));
    CHECK(Exchange{ 0.0, false }.mp(50.0) == 0.0);
}

namespace
{
    auto tiers() -> std::vector<CureOption>
    {
        return {
            CureOption{ .spell = "Cure", .mp = 8, .heals = 22, .known = true },
            CureOption{ .spell = "Cure II", .mp = 24, .heals = 63, .known = true },
            CureOption{ .spell = "Cure III", .mp = 46, .heals = 130, .known = false },
        };
    }
} // namespace

TEST_CASE("pickCure: the cheapest tier that covers, else the biggest heal, none when nothing is missing", "[cardian][tactics][bank]")
{
    auto options = tiers();
    CHECK(pickCure(options, 45) == 1);
    CHECK(options[0].lands == 22);
    CHECK_FALSE(options[0].covers);
    CHECK(options[1].lands == 45);
    CHECK(options[1].over == 18);
    CHECK(options[1].covers);
    CHECK_THAT(options[1].hpPerMp(), WithinAbs(45.0 / 24.0, 1e-9));

    CHECK(pickCure(options, 200) == 2); // nothing covers: the biggest heal
    CHECK(options[2].lands == 130);

    CHECK(pickCure(options, 0) == kNoPick);
    CHECK(options[1].lands == 0);

    auto       priced = tiers();
    const auto pick   = pickCure(priced, 45);
    const auto line   = cureLine("Zapp", "Jevyak", 45, priced, pick);
    CHECK_THAT(line, ContainsSubstring("bank: Zapp cures Jevyak, 45 missing: Cure 22/8 MP (2.8/MP), Cure II 63/24 MP (1.9/MP, 18 over, the pick), Cure III ~130/46 MP (1.0/MP, 85 over)"));
    CHECK_THAT(cureLine("Zapp", "Jevyak", 10, {}, kNoPick), ContainsSubstring("no cure tier priced"));
}

TEST_CASE("remainingLife: the live rate after ten seconds, the spot's before, unknown with neither", "[cardian][tactics][bank]")
{
    CHECK_THAT(remainingLife(200, 5.0, 12.0, 3.0), WithinAbs(40.0, 1e-9));
    CHECK_THAT(remainingLife(200, 5.0, 4.0, 4.0), WithinAbs(50.0, 1e-9));
    CHECK_THAT(remainingLife(200, 0.0, 0.0, 0.0), WithinAbs(-1.0, 1e-9));
    CHECK_THAT(window(120.0, 38.0), WithinAbs(38.0, 1e-9));
    CHECK_THAT(window(120.0, -1.0), WithinAbs(120.0, 1e-9));
}

TEST_CASE("priceRounds: Paralyze as the rounds it stops", "[cardian][tactics][bank]")
{
    DebuffPrice p;
    p.spell      = "Paralyze";
    p.target     = "Orcish_Grunt";
    p.mp         = 6;
    p.landChance = 0.78;
    p.duration   = 96.0;
    p.window     = 38.0;
    priceRounds(p, 4.0, 0.22, 22.0, "rounds stopped");
    CHECK_THAT(p.hpSaved, WithinAbs(0.78 * (38.0 / 4.0 * 0.22) * 22.0, 1e-9));
    p.mpWorth = Exchange{}.mp(p.hpSaved);
    CHECK(p.verdict() == "cast");
    const auto line = p.line("bank: Zapp could cast ");
    CHECK_THAT(line, ContainsSubstring("bank: Zapp could cast Paralyze on Orcish_Grunt: 78% to land, ~96 s, ~2.1 rounds stopped over 38 s at 22 a round, saves ~36 HP = 14 MP, costs 6 -> cast"));

    DebuffPrice blank = p;
    priceRounds(blank, 4.0, 0.22, 0.0, "rounds stopped");
    CHECK(blank.noData);
    CHECK(blank.verdict() == "not priced yet");
    CHECK_FALSE(blank.go());

    DebuffPrice dear = p;
    dear.mp         = 60;
    CHECK(dear.verdict() == "skip");
}

TEST_CASE("dotDamage, secondsCut and priceExtraDamage", "[cardian][tactics][bank]")
{
    CHECK_THAT(dotDamage(1, 3.0, 60.0), WithinAbs(20.0, 1e-9));
    CHECK_THAT(dotDamage(3, 3.0, 38.0), WithinAbs(36.0, 1e-9)); // twelve whole ticks
    CHECK_THAT(dotDamage(3, 0.0, 38.0), WithinAbs(0.0, 1e-9));
    CHECK_THAT(secondsCut(100.0, 10.0, -1.0), WithinAbs(10.0, 1e-9));
    CHECK_THAT(secondsCut(100.0, 10.0, 5.0), WithinAbs(5.0, 1e-9)); // never more than the mob has left
    CHECK_THAT(secondsCut(100.0, 0.0, 5.0), WithinAbs(0.0, 1e-9));

    DebuffPrice dia;
    dia.window = 38.0;
    priceExtraDamage(dia, 90.0, 10.0, 6.0, 38.0, "melee x1.11 at -10% defence, 1 a tick");
    CHECK_THAT(dia.hpSaved, WithinAbs(54.0, 1e-9)); // nine seconds shorter at six taken a second
    CHECK_THAT(dia.detail, ContainsSubstring("~90 extra dealt, the fight ~9 s shorter"));
    CHECK_FALSE(dia.noData);

    DebuffPrice blind;
    priceExtraDamage(blind, 90.0, 0.0, 6.0, 38.0, "");
    CHECK(blind.noData);

    CHECK_THAT(defenceDownExtra(1.11, 10.0, 60.0), WithinAbs(66.0, 1e-9));
    CHECK_THAT(defenceDownExtra(0.95, 10.0, 60.0), WithinAbs(0.0, 1e-9));
}

TEST_CASE("DebuffPrice: on already, moot and unpriced read as such", "[cardian][tactics][bank]")
{
    DebuffPrice on;
    on.spell = "Dia";
    on.target = "Orcish_Grunt";
    on.onFor  = 40.0;
    CHECK_THAT(on.line(""), ContainsSubstring("Dia on Orcish_Grunt: on already, 40 s to go"));

    DebuffPrice moot = on;
    moot.onFor       = -1.0;
    moot.moot        = 3.0;
    CHECK_THAT(moot.line(""), ContainsSubstring("moot, the mob has 3 s left"));

    DebuffPrice unpriced;
    unpriced.spell  = "Bind";
    unpriced.target = "Orcish_Grunt";
    unpriced.priced = false;
    unpriced.family = "EnfeeblingMagic, BIND";
    CHECK_THAT(unpriced.line(""), ContainsSubstring("Bind on Orcish_Grunt: unpriced (EnfeeblingMagic, BIND)"));
    CHECK_THAT(unpricedLine("Zapp", "Blink", "Zapp", "EnhancingMagic, Blink"), ContainsSubstring("bank: Zapp casts Blink on Zapp: unpriced (EnhancingMagic, Blink)"));

    DebuffPrice forGood = on;
    forGood.onFor       = std::numeric_limits<double>::infinity();
    CHECK_THAT(forGood.line(""), ContainsSubstring("Dia on Orcish_Grunt: on already, for good"));

    DebuffPrice blocked = on;
    blocked.onFor       = -1.0;
    blocked.blocked     = true;
    CHECK_THAT(blocked.line(""), ContainsSubstring("Dia on Orcish_Grunt: blocked by what is on it"));

    FightRecord tiny; // credits too small to print leave no dangling header
    tiny.mobName  = "Wild_Rabbit";
    tiny.zoneName = "West_Ronfaure";
    tiny.closeWhy = "killed";
    tiny.defDownExtra = 0.6;
    tiny.creditFor("dia").hp += 0.3;
    tiny.creditFor("choke").hp += 0.3;
    CHECK_THAT(summary(tiny), !ContainsSubstring("debuffs dealt"));
}

TEST_CASE("Defence down, the exact number: a hit without it, and the split per effect", "[cardian][tactics][bank]")
{
    CHECK_THAT(withoutDefenceDown(41, 1.10, 1.00), WithinAbs(41.0 / 1.10, 1e-9));
    CHECK_THAT(withoutDefenceDown(41, 0.0, 1.00), WithinAbs(41.0, 1e-9));

    std::vector<DefenceShare> shares{ DefenceShare{ .effect = "dia", .defp = -10 }, DefenceShare{ .effect = "choke", .defp = -2 } };
    apportion(12.0, shares);
    CHECK_THAT(shares[0].hp, WithinAbs(10.0, 1e-9));
    CHECK_THAT(shares[1].hp, WithinAbs(2.0, 1e-9));
    apportion(12.0, shares); // accumulates
    CHECK_THAT(shares[0].hp, WithinAbs(20.0, 1e-9));

    std::vector<DefenceShare> none;
    apportion(12.0, none); // nothing to share
    CHECK(none.empty());

    std::vector<std::pair<std::string, int32>>  regen{ { "dia", 1 }, { "poison", 3 } };
    std::vector<std::pair<std::string, double>> ticks;
    apportionTicks(4.0, regen, ticks);
    REQUIRE(ticks.size() == 2);
    CHECK_THAT(ticks[0].second, WithinAbs(1.0, 1e-9));
    CHECK_THAT(ticks[1].second, WithinAbs(3.0, 1e-9));
    std::vector<std::pair<std::string, int32>> noRegen;
    apportionTicks(4.0, noRegen, ticks);
    CHECK(ticks.size() == 2);

    auto r = sampleFight();
    r.defDownExtra = 37.4;
    r.defDownHits  = 9;
    r.dotDealt     = 9.0;
    r.creditFor("dia").hp += 37.4;
    r.creditFor("dia").ticks += 9.0; // the same effect again: one entry
    r.creditFor("poison").ticks += 12.0;
    r.member(2, "Zapp").paralysed = 2;
    CHECK(r.credits.size() == 2);
    const auto line = summary(r);
    CHECK_THAT(line, ContainsSubstring("; debuffs dealt: dia +37 by defence over 9 hits, +9 by ticks, poison +12 by ticks"));
    CHECK_THAT(line, ContainsSubstring("; paralysed: Zapp 2"));
}
