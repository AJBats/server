// Cardian: food (pawn/food_math.h, RESEARCH §19): a food's facts as its item
// script states them -- by the rule tools/economy/gamedata.py reads them by --
// what a food gives her and its score by the gear scorer's weights (the same
// numbers tools/world/check.py pins for the census), an owned cardian's pick
// from her bags, and the moments she eats: with the player, and a Healer's
// cookie as she kneels.
#include "pawn/food_math.h"

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <iterator>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using namespace cardian::food;
using cardian::party::Role;

namespace
{
    // Sausage as its script has it, the duration written as a product
    const std::string kSausage = "-----------------------------------\n"
                                 "-- ID: 4578\n"
                                 "-- Item: sausage\n"
                                 "-----------------------------------\n"
                                 "itemObject.onItemCheck = function(target, item, caster)\n"
                                 "    return xi.itemUtils.foodOnItemCheck(target, xi.foodType.BASIC)\n"
                                 "end\n"
                                 "\n"
                                 "itemObject.onItemUse = function(target, user, item, action)\n"
                                 "    target:addStatusEffect(xi.effect.FOOD, { duration = 30 * 60, origin = user, sourceType = xi.effectSourceType.FOOD })\n"
                                 "end\n"
                                 "\n"
                                 "itemObject.onEffectGain = function(target, effect)\n"
                                 "    effect:addMod(xi.mod.STR, 3)\n"
                                 "    effect:addMod(xi.mod.INT, -1) -- if a comment says if, it is no branch\n"
                                 "    effect:addMod(xi.mod.FOOD_ATTP, 27)\n"
                                 "    effect:addMod(xi.mod.FOOD_ATT_CAP, 30)\n"
                                 "end\n"
                                 "\n"
                                 "itemObject.onEffectLose = function(target, effect)\n"
                                 "end\n";

    // The text with its first `from` replaced by `to`
    auto replaced(std::string text, const std::string& from, const std::string& to) -> std::string
    {
        return text.replace(text.find(from), from.size(), to);
    }

    // A food lasting `duration`, with these mods
    auto food(const uint32 duration, std::initializer_list<std::pair<std::string_view, int32>> mods = {}) -> Facts
    {
        Facts f;
        f.duration = duration;
        for (const auto& [name, value] : mods)
        {
            f.add(name, value);
        }
        return f;
    }

    auto attack(const int32 pct, const int32 cap, const uint32 duration = 1800) -> Facts
    {
        return food(duration, { { "FOOD_ATTP", pct }, { "FOOD_ATT_CAP", cap } });
    }

    auto mp(const int32 heal, const uint32 duration) -> Facts
    {
        return food(duration, { { "MPHEAL", heal } });
    }

    // A food's gains as (name, value) pairs, for a readable check
    auto given(const Facts& f, const Stats& her) -> std::vector<std::pair<std::string, int32>>
    {
        std::vector<std::pair<std::string, int32>> out;
        for (const auto& m : gains(f, her))
        {
            out.emplace_back(m.name, m.value);
        }
        return out;
    }
} // namespace

TEST_CASE("Food: a script's facts read as the census reads them", "[cardian][food]")
{
    const auto f = parseScript(kSausage);
    REQUIRE(f.has_value());
    CHECK(f->duration == 1800);
    CHECK(f->kind == Kind::Basic);
    CHECK_FALSE(f->conditional);
    CHECK(f->mod("STR") == 3);
    CHECK(f->mod("INT") == -1);
    CHECK(f->mod("FOOD_ATTP") == 27);
    CHECK(f->mod("FOOD_ATT_CAP") == 30);
    CHECK(f->mod("MPHEAL") == 0);
    CHECK(f->mods.size() == 4);

    // raw fish and raw meat by the check its script makes
    CHECK(parseScript(replaced(kSausage, "xi.foodType.BASIC", "xi.foodType.RAW_FISH"))->kind == Kind::RawFish);
    CHECK(parseScript(replaced(kSausage, "xi.foodType.BASIC", "xi.foodType.RAW_MEAT"))->kind == Kind::RawMeat);
}

TEST_CASE("Food: a script that branches or loops is conditional, its mods left at zero", "[cardian][food]")
{
    const std::string strLine = "    effect:addMod(xi.mod.STR, 3)\n";
    const auto        f       = parseScript(replaced(kSausage, strLine, "    if target:getRace() ~= xi.race.GALKA then\n        effect:addMod(xi.mod.STR, 3)\n    end\n"));
    REQUIRE(f.has_value());
    CHECK(f->conditional);
    CHECK(f->mods.empty());
    CHECK(f->duration == 1800);

    CHECK(parseScript(replaced(kSausage, strLine, "    for i = 1, #dataTable do\n        effect:addMod(dataTable[i][1], dataTable[i][2])\n    end\n"))->conditional);
}

TEST_CASE("Food: a script that puts no food effect on, or no duration, is no food", "[cardian][food]")
{
    CHECK_FALSE(parseScript("itemObject.onItemUse = function(target)\n    target:addHP(50)\nend\n").has_value());
    CHECK_FALSE(parseScript("if target:hasStatusEffect(xi.effect.FOOD) then\nend\n").has_value());
    CHECK_FALSE(parseScript("target:addStatusEffect(xi.effect.FOOD, { origin = user })\n").has_value());
    // the first food effect with its braces is the one read
    const auto f = parseScript("if not member:hasStatusEffect(xi.effect.FOOD) then\n"
                               "    member:addStatusEffect(xi.effect.FOOD, { duration = 300 })\n"
                               "end\n");
    REQUIRE(f.has_value());
    CHECK(f->duration == 300);
}

TEST_CASE("Food: the real scripts read as the census reads them (tools/world/check.py pins the same foods)", "[cardian][food]")
{
    // xi_test runs from the server's root, as the map does
    const auto read = [](const std::string& name) -> std::optional<Facts>
    {
        std::ifstream file("./scripts/items/" + name + ".lua");
        INFO("item " << name);
        REQUIRE(file.is_open());
        return parseScript(std::string((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>()));
    };
    const auto cookie = read("ginger_cookie");
    REQUIRE(cookie.has_value());
    CHECK(cookie->duration == 180);
    CHECK(cookie->mod("MPHEAL") == 5);
    CHECK(cookie->kind == Kind::Basic);
    const auto crayfish = read("boiled_crayfish");
    REQUIRE(crayfish.has_value());
    CHECK(crayfish->mod("FOOD_DEFP") == 30);
    CHECK(crayfish->mod("FOOD_DEF_CAP") == 25);
    const auto sausage = read("sausage");
    REQUIRE(sausage.has_value());
    CHECK(sausage->mod("FOOD_ATTP") == 27);
    CHECK(sausage->mod("FOOD_ATT_CAP") == 30);
    CHECK(sausage->mod("INT") == -1);
    CHECK(read("sandfish")->kind == Kind::RawFish);
    CHECK(read("galkan_sausage")->conditional);
    CHECK(read("serving_of_red_curry")->conditional);
    CHECK_FALSE(read("bottle_of_antacid").has_value()); // it takes food off, it puts none on

    // every food script: more than 700 of them, a few dozen at most conditional
    std::size_t foods = 0;
    std::size_t branching = 0;
    for (const auto& entry : std::filesystem::directory_iterator("./scripts/items"))
    {
        std::ifstream     file(entry.path());
        const std::string text((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
        if (const auto f = parseScript(text); f.has_value())
        {
            ++foods;
            branching += f->conditional ? 1 : 0;
        }
    }
    CHECK(foods > 700);
    CHECK(branching > 5);
    CHECK(branching < 30);
}

TEST_CASE("Food: the seat by role, a Puller and no role eating as Damage", "[cardian][food]")
{
    CHECK(seatFor(Role::Tank, xi::Job::PLD) == Seat::BloodTank);
    CHECK(seatFor(Role::Tank, xi::Job::WAR) == Seat::BloodTank);
    CHECK(seatFor(Role::Tank, xi::Job::NIN) == Seat::NinjaTank);
    CHECK(seatFor(Role::Healer, xi::Job::WHM) == Seat::WhiteMage);
    CHECK(seatFor(Role::Healer, xi::Job::RDM) == Seat::WhiteMage);
    CHECK(seatFor(Role::Damage, xi::Job::WAR) == Seat::Melee);
    CHECK(seatFor(Role::Damage, xi::Job::WHM) == Seat::Melee);
    CHECK(seatFor(Role::Damage, xi::Job::BLM) == Seat::BlackMage);
    CHECK(seatFor(Role::Damage, xi::Job::THF) == Seat::Thief);
    CHECK(seatFor(Role::Puller, xi::Job::THF) == Seat::Thief);
    CHECK(seatFor(Role::None, xi::Job::BLM) == Seat::BlackMage);
    CHECK(planRole(Role::Puller) == Role::Damage);
    CHECK(planRole(Role::None) == Role::Damage);
    CHECK(planRole(Role::Tank) == Role::Tank);
    CHECK(planRole(Role::Healer) == Role::Healer);
}

TEST_CASE("Food: what a food gives her is the game's formula at her own numbers", "[cardian][food]")
{
    using Given = std::vector<std::pair<std::string, int32>>;
    // at a level-15 attack the cap rarely binds, and the cheap Sausage is the stronger (RESEARCH §19.3)
    const auto sausage = food(1800, { { "FOOD_ATTP", 27 }, { "FOOD_ATT_CAP", 30 }, { "STR", 3 }, { "INT", -1 } });
    CHECK(given(sausage, Stats{ .attack = 60 }) == Given{ { "ATT", 16 }, { "INT", -1 }, { "STR", 3 } });
    CHECK(given(attack(22, 65), Stats{ .attack = 60 }) == Given{ { "ATT", 13 } });
    CHECK(given(sausage, Stats{ .attack = 300 })[0] == std::pair<std::string, int32>{ "ATT", 30 });
    CHECK(given(attack(22, 65), Stats{ .attack = 300 }) == Given{ { "ATT", 65 } });

    // a flat part counts, and the percentage reads the stat it raised
    CHECK(given(food(1800, { { "ATT", 10 }, { "FOOD_ATTP", 10 }, { "FOOD_ATT_CAP", 99 } }), Stats{ .attack = 90 }) == Given{ { "ATT", 20 } });

    // a percentage with no cap adds nothing (Crayfish); FOOD_HP is HP, and HP% reads her HP
    CHECK(given(food(1800, { { "FOOD_DEFP", 30 }, { "FOOD_DEF_CAP", 25 } }), Stats{ .defence = 60 }) == Given{ { "DEF", 18 } });
    CHECK(given(food(1800, { { "FOOD_DEFP", 30 } }), Stats{ .defence = 200 }).empty());
    CHECK(given(food(1800, { { "FOOD_HP", 10 }, { "FOOD_HPP", 5 }, { "FOOD_HP_CAP", 20 } }), Stats{ .hp = 290 }) == Given{ { "HP", 25 } });
    CHECK(given(food(1800, { { "FOOD_ACCP", 10 }, { "FOOD_ACC_CAP", 15 }, { "FOOD_MP", 8 } }), Stats{ .accuracy = 120 }) == Given{ { "ACC", 12 }, { "MP", 8 } });
    // a plain percentage, uncapped
    CHECK(given(food(1800, { { "DEFP", 10 } }), Stats{ .defence = 150 }) == Given{ { "DEF", 15 } });
}

TEST_CASE("Food: a food's score is its stats by the gear scorer's weights for her seat (the census's numbers)", "[cardian][food]")
{
    const Stats her{ .attack = 150, .defence = 150, .accuracy = 150, .hp = 400, .mp = 200 };
    // a Black Mage's weights give STR nothing: Pamamas' -3 costs her nothing, its INT+1 is a point
    const auto pamamas = food(1800, { { "INT", 1 }, { "STR", -3 } });
    CHECK(score(pamamas, Seat::BlackMage, her) == 1.0);
    // the melee seat counts the STR-3 against it in full
    CHECK(score(pamamas, Seat::Melee, her) == -1.8);
    const auto parfait = food(1800, { { "INT", 3 }, { "MND", 2 }, { "MATT", 6 }, { "STR", -3 } });
    CHECK(score(parfait, Seat::BlackMage, her) == 15.0); // INT 3, Magic Attack 6 at 2.0 a point
    CHECK(std::round(score(parfait, Seat::WhiteMage, her) * 1000) / 1000 == 2.0); // MND 2, and Magic Attack a tiebreaker
    // accuracy, worth a point to a fighter, outweighs attack at 0.35
    CHECK(score(food(1800, { { "FOOD_ACCP", 10 }, { "FOOD_ACC_CAP", 15 } }), Seat::Melee, her) == 15.0);
    CHECK(score(attack(10, 15), Seat::Melee, her) == 5.25);
    // resting MP: a white mage's 2.0, a black mage's 0.25
    CHECK(score(mp(5, 180), Seat::WhiteMage, her) == 10.0);
    CHECK(score(mp(5, 180), Seat::BlackMage, her) == 1.25);
    // a food of tiebreakers alone (a killer effect) feeds no seat
    CHECK_FALSE(givesSomething(score(food(1800, { { "PLANTOID_KILLER", 10 } }), Seat::Melee, her)));
    CHECK(givesSomething(score(pamamas, Seat::BlackMage, her)));
}

TEST_CASE("Food: an owned cardian is judged at her own numbers, the game's formulas with no effect on her", "[cardian][food]")
{
    // a level-30 Warrior: sword skill 90, STR 30 at the one-handed 0.75,
    // her gear's 10 attack; VIT 28 at 1.5, the level's 30, her gear's 60;
    // DEX 24 at 0.75 and 5 accuracy from gear; evasion skill 85, AGI 26
    const Own warrior{ .level = 30, .skill = 90, .str = 30, .strMultiplier = 0.75f, .gearAtt = 10, .vit = 28, .vitFactor = 1.5f, .gearDef = 60,
                       .dex = 24, .dexMultiplier = 0.75f, .gearAcc = 5, .evasionSkill = 85, .agi = 26, .gearEva = 0, .hp = 400, .mp = 0 };
    const auto stats = statsOf(warrior);
    CHECK(stats.attack == 8 + 90 + 22 + 10);
    CHECK(stats.defence == 8 + 42 + 30 + 60);
    CHECK(stats.accuracy == 90 + 18 + 5);
    CHECK(stats.evasion == 85 + 13);
    CHECK(stats.hp == 400);
    CHECK(stats.macc == 90);

    // the level's part of defence, by the game's four bands, and accuracy past 200 skill
    CHECK(levelDefence(50) == 50);
    CHECK(levelDefence(51) == 60);
    CHECK(levelDefence(60) == 78);
    CHECK(levelDefence(61) == 79);
    CHECK(levelDefence(90) == 108);
    CHECK(levelDefence(99) == 122);
    CHECK(accuracyFromSkill(200) == 200);
    CHECK(accuracyFromSkill(250) == 245);
    CHECK(ownEvasion(Own{ .evasionSkill = 250 }) == 245);

    // nothing worn, nothing learned: never below 1
    CHECK(ownAttack(Own{}) == 8);
    CHECK(ownDefence(Own{ .level = 1 }) == 9);
}

TEST_CASE("Food: raw fish for a Mithra, raw meat for a Galka", "[cardian][food]")
{
    auto fish = food(1800);
    fish.kind = Kind::RawFish;
    auto meat = food(1800);
    meat.kind = Kind::RawMeat;
    CHECK(edible(fish, true, false));
    CHECK_FALSE(edible(fish, false, true));
    CHECK(edible(meat, false, true));
    CHECK_FALSE(edible(meat, true, false));
    CHECK(edible(food(1800), false, false));
}

TEST_CASE("Food: an owned cardian eats the best food for her seat her bags hold", "[cardian][food]")
{
    const std::vector<Carried> bag{
        { 4578, 12, food(1800, { { "FOOD_ATTP", 27 }, { "FOOD_ATT_CAP", 30 }, { "STR", 3 } }) }, // Sausage
        { 4574, 3, attack(22, 65) },                                                          // Meat Chiefkabob
        { 5000, 6, attack(50, 90, 300) },                                                     // a five-minute food: no candidate
        { 5001, 2, food(1800, { { "FOOD_ACCP", 10 }, { "FOOD_ACC_CAP", 30 } }) },              // an accuracy food
        { 5002, 4, food(1800, { { "INT", 3 }, { "MATT", 6 } }) },                              // a Black Mage's
        { 4394, 20, mp(5, 180) },                                                             // Ginger Cookie
        { 4576, 2, mp(7, 300) },                                                              // Wizard Cookie
        { 5592, 1, mp(3, 10800) },                                                            // Imperial Coffee
        { 6000, 0, attack(99, 999) },                                                         // none left
    };
    // a fighter at a low attack: the accuracy food (15 accuracy at a point each) over the Sausage (16
    // attack at 0.35 and STR 3 at 0.6); at a high one, the Chiefkabob's 65 attack beats both
    CHECK(pickOwned(bag, Seat::Melee, false, Stats{ .attack = 60, .accuracy = 150 }, false, false) == Pick{ 5001, false });
    CHECK(pickOwned(bag, Seat::Melee, false, Stats{ .attack = 300, .accuracy = 50 }, false, false) == Pick{ 4574, false });
    // a Black Mage on the damage seat: her INT and Magic Attack food
    CHECK(pickOwned(bag, Seat::BlackMage, false, Stats{}, false, false) == Pick{ 5002, false });
    // a Healer prefers a long MP food to any cookie
    CHECK(pickOwned(bag, Seat::WhiteMage, true, Stats{}, false, false) == Pick{ 5592, false });
    // nothing for the seat: nothing
    CHECK_FALSE(pickOwned(std::vector<Carried>{ { 4394, 20, mp(5, 180) } }, Seat::Melee, false, Stats{}, false, false).has_value());
    CHECK_FALSE(pickOwned({}, Seat::Melee, false, Stats{ .attack = 100 }, false, false).has_value());
}

TEST_CASE("Food: with no long MP food, a Healer's pick is her best cookie; the floor keeps the weak ones out", "[cardian][food]")
{
    const std::vector<Carried> bag{
        { 4394, 20, mp(5, 180) },  // Ginger Cookie
        { 4576, 2, mp(7, 300) },   // Wizard Cookie
        { 5100, 9, mp(2, 10800) }, // a long food under the floor
        { 5101, 9, mp(1, 300) },   // a cookie under the floor
    };
    CHECK(pickOwned(bag, Seat::WhiteMage, true, Stats{}, false, false) == Pick{ 4576, true });
    const std::vector<Carried> weak{ { 5100, 9, mp(2, 10800) }, { 5101, 9, mp(1, 300) } };
    CHECK_FALSE(pickOwned(weak, Seat::WhiteMage, true, Stats{}, false, false).has_value());
}

TEST_CASE("Food: a tie goes to the stack she carries more of, then the lower item; raw and conditional food are passed over", "[cardian][food]")
{
    const Stats her{ .attack = 60 };
    std::vector<Carried> bag{ { 7000, 2, attack(27, 30) }, { 7001, 5, attack(27, 30) }, { 6999, 5, attack(27, 30) } };
    CHECK(pickOwned(bag, Seat::Melee, false, her, false, false) == Pick{ 6999, false });

    auto raw = attack(50, 90);
    raw.kind = Kind::RawMeat;
    auto branch        = attack(50, 90);
    branch.conditional = true;
    bag.push_back({ 8000, 1, raw });
    bag.push_back({ 8001, 1, branch });
    CHECK(pickOwned(bag, Seat::Melee, false, her, false, false) == Pick{ 6999, false });
    CHECK(pickOwned(bag, Seat::Melee, false, her, false, true) == Pick{ 8000, false });
}

TEST_CASE("Food: she eats with the player when he has food on and she has none, between fights and free", "[cardian][food]")
{
    const WithPlayer due{ .rowOn = true, .playerFed = true, .selfFed = false, .betweenFights = true, .free = true };
    CHECK(eatsWithPlayer(due));

    auto m  = due;
    m.rowOn = false;
    CHECK_FALSE(eatsWithPlayer(m)); // the row is off
    m           = due;
    m.playerFed = false;
    CHECK_FALSE(eatsWithPlayer(m)); // she follows the player alone
    m         = due;
    m.selfFed = true;
    CHECK_FALSE(eatsWithPlayer(m)); // one food at a time
    m               = due;
    m.betweenFights = false;
    CHECK_FALSE(eatsWithPlayer(m));
    m      = due;
    m.free = false;
    CHECK_FALSE(eatsWithPlayer(m)); // still getting up from a kneel, walking, acting
}

TEST_CASE("Food: a Healer's cookie is eaten as she kneels short of MP, armed by the player's food", "[cardian][food]")
{
    const BeforeKneel due{ .rowOn = true, .hasCookie = true, .playerFed = true, .selfFed = false, .kneeling = true, .shortOfMp = true };
    CHECK(eatsBeforeKneel(due));

    auto k      = due;
    k.playerFed = false;
    CHECK_FALSE(eatsBeforeKneel(k)); // not armed
    k           = due;
    k.hasCookie = false;
    CHECK_FALSE(eatsBeforeKneel(k)); // her food is a long one: eaten with the player
    k         = due;
    k.selfFed = true;
    CHECK_FALSE(eatsBeforeKneel(k));
    k          = due;
    k.kneeling = false;
    CHECK_FALSE(eatsBeforeKneel(k));
    k           = due;
    k.shortOfMp = false;
    CHECK_FALSE(eatsBeforeKneel(k)); // a kneel for HP alone gains nothing from it
    k       = due;
    k.rowOn = false;
    CHECK_FALSE(eatsBeforeKneel(k));
}

TEST_CASE("Food: a body of the world is topped up to her stock, or a stack where a stack holds less", "[cardian][food]")
{
    CHECK(topUpBy(0, 99) == kStock);
    CHECK(topUpBy(11, 99) == 1);
    CHECK(topUpBy(kStock, 99) == 0);
    CHECK(topUpBy(30, 99) == 0);
    CHECK(topUpBy(0, 1) == 1);
    CHECK(topUpBy(1, 1) == 0);
    CHECK(topUpBy(4, 6) == 2);
    CHECK(topUpBy(0, 0) == 1);
}
