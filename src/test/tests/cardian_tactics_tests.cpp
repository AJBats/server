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

    // A nuker's part in the kill: her nukes, their MP, what the ones her
    // tactician chose landed against what the formula expected, and her share
    FightRecord nuked = quiet;
    nuked.mobDamage   = 100;
    nuked.member(2, "Jevyak").damageDealt += 60;
    nuked.casts.push_back(CastNote{ .caster = 2, .spell = 159, .spellName = "stone", .mp = 4, .landed = 14, .nuke = true, .expected = 12.0 });
    nuked.casts.push_back(CastNote{ .caster = 2, .spell = 159, .spellName = "stone", .mp = 4, .landed = 6, .nuke = true, .expected = 12.0 });
    nuked.casts.push_back(CastNote{ .caster = 2, .spell = 159, .spellName = "stone", .mp = 4, .landed = 40, .nuke = true }); // a plain row's: the formula never asked
    CHECK_THAT(summary(nuked), ContainsSubstring("; Jevyak nuked 3 times for 12 MP, landed 20 against ~24 expected, dealt 60 (60% of the kill)"));
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

TEST_CASE("wholeAt: a known heal waits for a quarter more, a floor for twice", "[cardian][tactics][bank]")
{
    const auto options = tiers();
    CHECK(wholeAt(options[0]) == 28);  // 22 x 1.25 = 27.5, rounded
    CHECK(wholeAt(options[1]) == 79);  // 63 x 1.25 = 78.75
    CHECK(wholeAt(options[2]) == 260); // a floor of 130, doubled
}

TEST_CASE("pickWhole: the biggest tier that lands whole, none under the smallest's line", "[cardian][tactics][bank]")
{
    auto options = tiers();
    CHECK(pickWhole(options, 0) == kNoPick);
    CHECK(pickWhole(options, 27) == kNoPick); // under Cure's line
    CHECK(pickWhole(options, 28) == 0);
    CHECK(pickWhole(options, 45) == 0);       // a top-up: Cure, never Cure II's 18 over
    CHECK(options[0].lands == 22);
    CHECK(options[0].over == 0);
    CHECK(pickWhole(options, 78) == 0);
    CHECK(pickWhole(options, 79) == 1);
    CHECK(pickWhole(options, 200) == 1);      // Cure III's floor is not yet whole here
    CHECK(pickWhole(options, 260) == 2);

    // a top-up judges the gap the cures in flight leave: 120 missing, a
    // Cure II of 63 in the air, 57 left -- Cure fits, Cure II would not
    CHECK(pickWhole(options, 120 - 63) == 0);
    CHECK(pickWhole(options, 80 - 63) == kNoPick); // 17 left: nothing lands whole

    std::vector<CureOption> none;
    CHECK(pickWhole(none, 100) == kNoPick);
}

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

TEST_CASE("Defence down's rate: the party's plain swings alone, never its magic", "[cardian][tactics][bank]")
{
    FightRecord r;
    r.openedAt = 100.0;
    // a Monk's swings and a Black Mage's nukes: the defence down's rate is
    // the swings alone, while all the party dealt is both
    auto& monk       = r.member(1, "Misha");
    monk.damageDealt = 300;
    monk.meleeDealt  = 300;
    auto& nuker       = r.member(2, "Jevyak");
    nuker.damageDealt = 1000;
    CHECK_THAT(r.meleePerSecond(110.0), WithinAbs(30.0, 1e-9));
    CHECK_THAT(r.dealtPerSecond(110.0), WithinAbs(130.0, 1e-9));
    // nobody swinging, nothing for a defence down to strengthen
    FightRecord mages;
    mages.openedAt                        = 100.0;
    mages.member(2, "Jevyak").damageDealt = 1000;
    CHECK(mages.meleePerSecond(110.0) == 0.0);
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

TEST_CASE("Explicit best-Cure rows choose an affordable tier even at full HP", "[cardian][tactics][bank]")
{
    std::vector<CureOption> options{
        { .spell = "Cure II", .mp = 24, .heals = 90 },
        { .spell = "Cure", .mp = 8, .heals = 30 },
    };
    CHECK(pickCure(options, 0) == kNoPick); // the autonomous role still declines
    CHECK(pickCure(options, 0, true) == 1); // a row still casts
    CHECK(options[1].lands == 0);
    CHECK(pickCure(options, 60, true) == 0);
    options.clear();
    CHECK(pickCure(options, 0, true) == kNoPick); // no available spell
}

TEST_CASE("NukePrice: the seed by her correction, capped by what the mob has left, and the pick by damage a second of hers", "[cardian][tactics][bank]")
{
    // LSB's Thunder tiers: base damage, MP, and her seconds a cast (the
    // cast, then the 2.5 s before her next action)
    const auto thunder = [](const std::string& name, const uint16 id, const int32 mp, const double seed, const double seconds)
    {
        return NukePrice{ .id = id, .spell = name, .mp = mp, .seconds = seconds, .seed = seed };
    };

    // What she expects is the seed by what her nukes of the element have
    // landed against it, and what it takes off the mob is capped by its HP:
    // overkill counts for nothing
    NukePrice corrected  = thunder("Thunder IV", 167, 195, 541.0, 7.5);
    corrected.correction = 0.9;
    corrected.learned    = 3;
    priceNukeDamage(corrected, 4000, 25.0, 200.0);
    CHECK_THAT(corrected.expected, WithinAbs(486.9, 1e-9));
    CHECK_THAT(corrected.dealt, WithinAbs(486.9, 1e-9));
    CHECK_THAT(corrected.perSecond(), WithinAbs(64.92, 1e-9));
    CHECK_THAT(corrected.line(), ContainsSubstring("; the formula's ~541 x0.90 from 3 landed"));

    NukePrice capped = thunder("Thunder IV", 167, 195, 541.0, 7.5);
    priceNukeDamage(capped, 50, 20.0, 2.5);
    CHECK_THAT(capped.dealt, WithinAbs(50.0, 1e-9));
    CHECK_THAT(capped.cut, WithinAbs(2.5, 1e-9)); // never more than the mob has left
    CHECK_THAT(capped.line(), ContainsSubstring("Thunder IV ~50 of ~541"));

    // On a tough mob the biggest she has ready wins: damage a second of
    // hers, not damage a MP (Thunder is 6.7 a MP, Thunder IV 2.8)
    NukePrice one  = thunder("Thunder", 164, 9, 60.0, 3.0);      // 20 a second
    NukePrice two  = thunder("Thunder II", 165, 37, 178.0, 4.0); // 44.5 a second
    NukePrice four = thunder("Thunder IV", 167, 195, 541.0, 7.5); // 72 a second
    for (auto* p : { &one, &two, &four })
    {
        priceNukeDamage(*p, 4000, 25.0, 200.0);
    }
    std::vector<NukePrice> tough{ one, two, four };
    REQUIRE(pickNuke(tough) != nullptr);
    CHECK(pickNuke(tough)->spell == "Thunder IV");
    CHECK_THAT(four.line(), ContainsSubstring("Thunder IV ~541 for 195 MP in 7.5 s: 72 a second of hers, the fight ~21.6 s shorter"));

    // Near the end the quickest that finishes it wins -- no "too late" rule
    // (a cast whose mob dies first is cancelled before it spends her MP)
    for (auto* p : { &one, &two, &four })
    {
        priceNukeDamage(*p, 50, 25.0, 2.0);
    }
    std::vector<NukePrice> end{ one, two, four };
    REQUIRE(pickNuke(end) != nullptr);
    CHECK(pickNuke(end)->spell == "Thunder");

    // Equal a second of hers: the cheaper
    NukePrice dear  = thunder("Dear", 1, 20, 60.0, 3.0);
    NukePrice cheap = thunder("Cheap", 2, 10, 60.0, 3.0);
    priceNukeDamage(dear, 4000, 25.0, 200.0);
    priceNukeDamage(cheap, 4000, 25.0, 200.0);
    std::vector<NukePrice> tie{ dear, cheap };
    CHECK(pickNuke(tie)->spell == "Cheap");

    // Nothing that deals nothing (an element the mob nullifies or absorbs)
    NukePrice nullified = thunder("Thunder", 164, 9, 0.0, 3.0);
    priceNukeDamage(nullified, 4000, 25.0, 200.0);
    std::vector<NukePrice> none{ nullified };
    CHECK(pickNuke(none) == nullptr);

    // With the party's rate unknown she is still ranked a second of hers;
    // there is just no fight cut to say
    NukePrice blindOne  = thunder("Thunder", 164, 9, 60.0, 3.0);
    NukePrice blindFour = thunder("Thunder IV", 167, 195, 541.0, 7.5);
    priceNukeDamage(blindOne, 4000, 0.0, -1.0);
    priceNukeDamage(blindFour, 4000, 0.0, -1.0);
    CHECK(blindFour.noRate);
    std::vector<NukePrice> blind{ blindOne, blindFour };
    CHECK(pickNuke(blind)->spell == "Thunder IV");
    CHECK_THAT(blindFour.line(), ContainsSubstring("72 a second of hers, the party's rate unknown"));
}

TEST_CASE("summary: each Thief's Sneak Attacks, held for her weapon skill with what the waiting cost, and spent on a plain hit", "[cardian][tactics]")
{
    FightRecord r;
    r.mobName  = "Forest_Hare";
    r.zoneName = "West_Ronfaure";
    r.closeWhy = "killed";

    auto& paired        = r.member(2, "Jevyak");
    paired.sneakAttacks = 2;
    paired.sneakWait    = 41.0;
    CHECK_THAT(summary(r), ContainsSubstring("; Sneak Attack: Jevyak 2 before her weapon skill (held 41 s)"));

    paired.sneakNaked = 1;
    CHECK_THAT(summary(r), ContainsSubstring("; Sneak Attack: Jevyak 2 before her weapon skill (held 41 s), 1 on a plain hit"));

    auto& naked      = r.member(3, "Ilani");
    naked.sneakNaked = 3;
    CHECK_THAT(summary(r), ContainsSubstring("1 on a plain hit; Ilani 3 on a plain hit"));

    // Ready and never used before the fight ended is booked too: holding it
    // cost that time as well
    bookSneak(naked, SneakUse::Unused, 30.4);
    bookSneak(naked, SneakUse::BeforeWs, 12.0);
    CHECK_THAT(summary(r), ContainsSubstring("Ilani 1 before her weapon skill (held 12 s), 3 on a plain hit, ready 30 s unused"));
    FightRecord idle;
    bookSneak(idle.member(4, "Thaata"), SneakUse::Unused, 45.0);
    CHECK_THAT(summary(idle), ContainsSubstring("; Sneak Attack: Thaata ready 45 s unused"));

    // Nobody's Sneak Attack, no section
    FightRecord quiet;
    quiet.member(2, "Jevyak");
    CHECK_THAT(summary(quiet), !ContainsSubstring("Sneak Attack"));
}

TEST_CASE("Bio's attack down: its share of the mob's melee over the window, if it lands", "[cardian][tactics][bank]")
{
    using cardian::tactics::attackDownSaved;

    // 60 s at a 3 s delay is 20 rounds; 10 a round is 200 of melee; 10% of
    // it is 20 HP, or 18 at a 90% chance to land
    CHECK_THAT(attackDownSaved(1.0, 60.0, 3.0, 10.0, 0.10), WithinAbs(20.0, 1e-9));
    CHECK_THAT(attackDownSaved(0.9, 60.0, 3.0, 10.0, 0.10), WithinAbs(18.0, 1e-9));
    // a harder hitter makes it worth more, a shorter window less
    CHECK_THAT(attackDownSaved(1.0, 60.0, 3.0, 30.0, 0.10), WithinAbs(60.0, 1e-9));
    CHECK_THAT(attackDownSaved(1.0, 30.0, 3.0, 10.0, 0.10), WithinAbs(10.0, 1e-9));
    // no delay on record, nothing
    CHECK(attackDownSaved(1.0, 60.0, 0.0, 10.0, 0.10) == 0.0);
}

TEST_CASE("Burn: its INT down by power, and the party's nukes over its window with it against without", "[cardian][tactics][bank]")
{
    using cardian::tactics::BurnNuker;
    using cardian::tactics::burnIntDown;
    using cardian::tactics::burnPlan;
    using cardian::tactics::NukeOption;
    using cardian::tactics::nukesWithin;

    // scripts/effects/burn.lua: (power - 1) x 2 + 5
    CHECK(burnIntDown(1.0) == 5);
    CHECK(burnIntDown(3.0) == 9);
    CHECK(burnIntDown(5.0) == 13);

    // Fire II, 300 a cast, 330 under Burn, 5.5 s and 34 MP a cast
    const NukeOption fire{ 300.0, 5.5, 34.0 };
    const NukeOption burned{ 330.0, 5.5, 34.0 };

    // The casts she makes: the tighter of her time and her MP
    CHECK_THAT(nukesWithin(40.0, 200.0, fire), WithinAbs(200.0 / 34.0, 1e-9)); // MP runs out first
    CHECK_THAT(nukesWithin(40.0, 600.0, fire), WithinAbs(40.0 / 5.5, 1e-9));   // the window ends first
    CHECK(nukesWithin(0.0, 600.0, fire) == 0.0);
    CHECK(nukesWithin(40.0, -10.0, fire) == 0.0);

    // One Black Mage alone, short of MP over a 40 s window: the Burn's 25 MP
    // is most of a Fire II, so the plan without it deals more (RESEARCH
    // §17.13, the worked example: ~1,765 against ~1,739)
    const BurnNuker jevyak{ 200.0, fire, burned, true };
    const auto      alone = burnPlan({ jevyak }, 40.0, 5.0, 25.0, 1.0, 40.0);
    CHECK_THAT(alone.without, WithinAbs(200.0 / 34.0 * 300.0, 1e-6));
    CHECK_THAT(alone.with, WithinAbs(175.0 / 34.0 * 330.0 + 40.0, 1e-6));
    CHECK(alone.gain() < 0.0);

    // Two more nukers with the same numbers: theirs land harder at no cost to
    // them, and the plan with it wins (~5,621 against ~5,294)
    const BurnNuker other{ 200.0, fire, burned, false };
    const auto      three = burnPlan({ jevyak, other, other }, 40.0, 5.0, 25.0, 1.0, 40.0);
    CHECK_THAT(three.gain(), WithinAbs(alone.gain() + 2.0 * (200.0 / 34.0 * 30.0), 1e-6));
    CHECK(three.gain() > 300.0);

    // A Burn that may not land: the nukes after it land harder only by its
    // chance, and its cost is paid either way
    const auto half = burnPlan({ jevyak, other, other }, 40.0, 5.0, 25.0, 0.5, 40.0);
    CHECK_THAT(half.with, WithinAbs(0.5 * three.with + 0.5 * (175.0 / 34.0 * 300.0 + 2.0 * 200.0 / 34.0 * 300.0), 1e-6));

    // With MP to spare the window binds instead: the Burn's 5 s are what
    // it costs, judged by damage a second
    const BurnNuker rich{ 600.0, fire, burned, true };
    const auto      timed = burnPlan({ rich }, 40.0, 5.0, 25.0, 1.0, 0.0);
    CHECK_THAT(timed.without, WithinAbs(40.0 / 5.5 * 300.0, 1e-6));
    CHECK_THAT(timed.with, WithinAbs(35.0 / 5.5 * 330.0, 1e-6));
}

TEST_CASE("NukeCorrection: the seed's word counts as four nukes, so one resist moves it a little, and her landed nukes teach it", "[cardian][tactics][bank]")
{
    NukeCorrection fresh;
    CHECK_THAT(fresh.factor(), WithinAbs(1.0, 1e-12));
    CHECK(fresh.landed == 0);

    // A half resist on her first nuke is one fifth of what it knows
    fresh.learn(50.0, 100.0);
    CHECK_THAT(fresh.factor(), WithinAbs(0.9, 1e-12));
    CHECK(fresh.landed == 1);

    // No seed, nothing to learn from
    fresh.learn(10.0, 0.0);
    CHECK(fresh.landed == 1);

    // A seed that runs high is learned down to what she lands
    NukeCorrection high;
    for (int i = 0; i < 40; ++i)
    {
        high.learn(80.0, 100.0);
    }
    CHECK_THAT(high.factor(), WithinAbs(0.8, 0.01));
}
