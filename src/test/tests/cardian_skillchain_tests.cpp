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

// Skillchains and magic bursts (ROADMAP, Road to subjob item 6, slice 3h):
// what the party can make and in what order the Shortcuts menu lists it;
// the tactician's weapon skill -- close, keep clear, the plan's closer and
// opener, her best; the party's TP read as a rate and the chain's time from
// it; and the Black Mage's nuke against the burst. The game's own chain
// rules are the server's own, battleutils, as skillchains.cpp hands them in.

#include <catch2/catch_test_macros.hpp>

#include "map/pawn/skillchain_math.h"

#include "map/entities/battle_entity.h"
#include "map/utils/battleutils.h"

#include <list>
#include <optional>
#include <vector>

using cardian::chains::BurstView;
using cardian::chains::Member;
using cardian::chains::Nuke;
using cardian::chains::NukeMove;
using cardian::chains::Properties;
using cardian::chains::Resonance;
using cardian::chains::Seat;
using cardian::chains::Skill;
using cardian::chains::WsMove;
using cardian::chains::WsView;

namespace
{
    // The server's rules, as skillchains.cpp hands them to the pure rules
    auto forms(const Properties& opening, const Properties& closing) -> uint8
    {
        const std::list<SKILLCHAIN_ELEMENT> a{ static_cast<SKILLCHAIN_ELEMENT>(opening[0]), static_cast<SKILLCHAIN_ELEMENT>(opening[1]), static_cast<SKILLCHAIN_ELEMENT>(opening[2]) };
        const std::list<SKILLCHAIN_ELEMENT> b{ static_cast<SKILLCHAIN_ELEMENT>(closing[0]), static_cast<SKILLCHAIN_ELEMENT>(closing[1]), static_cast<SKILLCHAIN_ELEMENT>(closing[2]) };
        return static_cast<uint8>(battleutils::FormSkillchain(a, b));
    }

    auto tierOf(const uint8 chain) -> uint8
    {
        return battleutils::GetSkillchainTier(static_cast<SKILLCHAIN_ELEMENT>(chain));
    }

    auto elementsOf(const uint8 chain) -> uint16
    {
        uint16 bits = 0;
        for (const auto element : battleutils::GetSkillchainMagicElement(static_cast<SKILLCHAIN_ELEMENT>(chain)))
        {
            bits |= cardian::chains::elementBit(static_cast<uint8>(element));
        }
        return bits;
    }

    // Weapon skills by their properties: the ids are the test's own
    constexpr uint16 kCombo        = 1; // Impaction
    constexpr uint16 kBurningBlade = 2; // Liquefaction
    constexpr uint16 kFastBlade    = 3; // Scission
    constexpr uint16 kRedLotus     = 4; // Liquefaction, Detonation
    constexpr uint16 kFlatBlade    = 5; // Reverberation

    const Skill combo{ kCombo, { SC_IMPACTION, 0, 0 } };
    const Skill burningBlade{ kBurningBlade, { SC_LIQUEFACTION, 0, 0 } };
    const Skill fastBlade{ kFastBlade, { SC_SCISSION, 0, 0 } };
    const Skill redLotus{ kRedLotus, { SC_LIQUEFACTION, SC_DETONATION, 0 } };
    const Skill flatBlade{ kFlatBlade, { SC_REVERBERATION, 0, 0 } };

    constexpr uint32 kMisha   = 10; // the player
    constexpr uint32 kJevyak  = 11; // a Warrior
    constexpr uint32 kKokoro  = 12; // a Black Mage
    constexpr uint16 kFire    = cardian::chains::elementBit(ELEMENT_FIRE);
    constexpr uint16 kEarth   = cardian::chains::elementBit(ELEMENT_EARTH);
    constexpr uint16 kThunder = cardian::chains::elementBit(ELEMENT_THUNDER);

    auto find(const std::vector<cardian::chains::Option>& options, const uint32 opener, const uint16 openerSkill, const uint32 closer, const uint16 closerSkill)
        -> std::optional<cardian::chains::Option>
    {
        for (const auto& o : options)
        {
            if (o.opener == opener && o.openerSkill == openerSkill && o.closer == closer && o.closerSkill == closerSkill)
            {
                return o;
            }
        }
        return std::nullopt;
    }

    // A Warrior with her TP, on a mob at 80%, her weapon skills Burning
    // Blade and Fast Blade, Burning Blade her best
    auto warrior(const int32 tp = 1000) -> WsView
    {
        WsView v;
        v.tp     = tp;
        v.mobHpp = 80;
        v.skills = { fastBlade, burningBlade };
        v.best   = kBurningBlade;
        return v;
    }

    auto opening(const Properties& props, const double age, const double left = 6.0) -> Resonance
    {
        return Resonance{ props, 0, age, left };
    }

    auto call(const WsView& v)
    {
        return cardian::chains::wsCall(v, forms, tierOf);
    }
} // namespace

TEST_CASE("skillchains: the server's own rules, as the tests read them", "[cardian][skillchains]")
{
    // Combo's Impaction, then Burning Blade's Liquefaction: Liquefaction (the
    // player's Combo and a Warrior's Burning Blade, played 2026-08-30)
    CHECK(forms(combo.props, burningBlade.props) == SC_LIQUEFACTION);
    // the other way round: Fusion, a tier higher, burst by fire and light
    CHECK(forms(burningBlade.props, combo.props) == SC_FUSION);
    CHECK(tierOf(SC_FUSION) == 2);
    CHECK(elementsOf(SC_FUSION) == (kFire | cardian::chains::elementBit(ELEMENT_LIGHT)));
    CHECK(forms(combo.props, fastBlade.props) == SC_NONE);
}

TEST_CASE("skillchains: the options are every two-step chain one member opens and another closes, the burstable and the higher tier first", "[cardian][skillchains]")
{
    const std::vector<Member> party{
        { kMisha, { combo }, 0 },
        { kJevyak, { fastBlade, burningBlade }, 0 },
        { kKokoro, {}, kFire },
    };
    const auto options = cardian::chains::options(party, forms, tierOf, elementsOf);
    REQUIRE(options.size() == 2);

    const auto fusion = find(options, kJevyak, kBurningBlade, kMisha, kCombo);
    REQUIRE(fusion.has_value());
    CHECK(fusion->chain == SC_FUSION);
    CHECK(fusion->tier == 2);
    CHECK(fusion->burster == kKokoro);

    const auto liquefaction = find(options, kMisha, kCombo, kJevyak, kBurningBlade);
    REQUIRE(liquefaction.has_value());
    CHECK(liquefaction->chain == SC_LIQUEFACTION);
    CHECK(liquefaction->burster == kKokoro);

    // both burstable: the higher tier first
    CHECK(options[0].chain == SC_FUSION);
    CHECK(options[1].chain == SC_LIQUEFACTION);

    // nobody chains with herself: a member alone makes nothing
    CHECK(cardian::chains::options({ { kJevyak, { burningBlade, combo }, kFire } }, forms, tierOf, elementsOf).empty());
}

TEST_CASE("skillchains: a chain the party can burst ranks above a higher tier it cannot", "[cardian][skillchains]")
{
    // Kokoro bursts thunder alone: Jevyak's Reverberation and Misha's Combo
    // make Impaction, hers; Fusion (fire, light) and Liquefaction are beyond
    // her
    const std::vector<Member> party{
        { kMisha, { combo, burningBlade }, 0 },
        { kJevyak, { flatBlade, combo }, 0 },
        { kKokoro, {}, kThunder },
    };
    const auto options = cardian::chains::options(party, forms, tierOf, elementsOf);
    REQUIRE(options.size() == 3);
    CHECK(options.front().chain == SC_IMPACTION);
    CHECK(options.front().burster == kKokoro);
    CHECK(options[1].chain == SC_FUSION); // then the higher tier
    bool seenUnburstable = false;
    for (const auto& o : options)
    {
        if (o.burster == 0)
        {
            seenUnburstable = true;
        }
        else
        {
            CHECK_FALSE(seenUnburstable); // every burstable one comes before the first that is not
        }
    }
    CHECK(seenUnburstable);
    const auto fusion = find(options, kMisha, kBurningBlade, kJevyak, kCombo);
    REQUIRE(fusion.has_value());
    CHECK(fusion->burster == 0);

    // at most the cap
    CHECK(cardian::chains::options(party, forms, tierOf, elementsOf, 2).size() == 2);
}

TEST_CASE("skillchains: her weapon skill closes what is on the mob once the window opens, the plan's closer with the plan's", "[cardian][skillchains]")
{
    // Nothing without the TP
    CHECK(call(warrior(999)).move == WsMove::None);

    // Combo's opening, four seconds old: Burning Blade closes it
    auto v      = warrior();
    v.resonance = opening(combo.props, 4.0);
    auto c      = call(v);
    CHECK(c.move == WsMove::Close);
    CHECK(c.skill == kBurningBlade);
    CHECK(c.chain == SC_LIQUEFACTION);

    // two seconds old: the window has not opened, she waits for it
    v.resonance = opening(combo.props, 2.0);
    c           = call(v);
    CHECK(c.move == WsMove::Hold);
    CHECK(c.soon);
    CHECK(c.skill == kBurningBlade);

    // the highest tier among hers: Red Lotus Blade on a Scission opening
    // makes Liquefaction, Fast Blade on it nothing
    v.skills    = { fastBlade, redLotus };
    v.best      = kFastBlade;
    v.resonance = opening(fastBlade.props, 4.0);
    c           = call(v);
    CHECK(c.move == WsMove::Close);
    CHECK(c.skill == kRedLotus);

    // on Burning Blade's opening Combo makes Fusion and Fast Blade
    // Scission: the higher tier, unless she is the plan's closer with Fast
    // Blade, whose plan it is
    v           = warrior();
    v.skills    = { fastBlade, combo };
    v.best      = kFastBlade;
    v.resonance = opening(burningBlade.props, 4.0);
    c           = call(v);
    CHECK(c.skill == kCombo);
    CHECK(c.chain == SC_FUSION);
    v.seat      = Seat::Closer;
    v.planSkill = kFastBlade;
    c           = call(v);
    CHECK(c.skill == kFastBlade);
    CHECK(c.chain == SC_SCISSION);

    // a close goes on a mob under half its HP: the opener's TP is spent
    v           = warrior();
    v.mobHpp    = 20;
    v.resonance = opening(combo.props, 4.0);
    CHECK(call(v).move == WsMove::Close);
}

TEST_CASE("skillchains: she keeps clear of an opening or a chain she cannot close while somebody will use it", "[cardian][skillchains]")
{
    auto v      = warrior();
    v.skills    = { fastBlade };
    v.best      = kFastBlade;
    v.resonance = opening(combo.props, 4.0);

    // an opening another member, with the TP, can close
    v.othersClose = true;
    auto c        = call(v);
    CHECK(c.move == WsMove::Hold);
    CHECK_FALSE(c.soon);
    // one nobody can close is no reason to hold: her best goes
    v.othersClose = false;
    c             = call(v);
    CHECK(c.move == WsMove::Free);
    CHECK(c.skill == kFastBlade);

    // a chain's burst window: held while a member can burst it or extend it
    v.resonance   = Resonance{ { SC_FUSION, 0, 0 }, 2, 1.0, 8.0 };
    v.burstComing = true;
    CHECK(call(v).move == WsMove::Hold);
    v.burstComing = false;
    v.othersClose = true;
    CHECK(call(v).move == WsMove::Hold);
    v.othersClose = false;
    CHECK(call(v).move == WsMove::Free);

    // one about to lapse is nothing: her best goes
    v.othersClose = true;
    v.resonance   = opening(combo.props, 9.5, 0.5);
    c             = call(v);
    CHECK(c.move == WsMove::Free);
    CHECK(c.skill == kFastBlade);
}

TEST_CASE("skillchains: outside the plan, its opening is left to its closer", "[cardian][skillchains]")
{
    // Burning Blade is the plan's opening, Misha its closer; she could close
    // it herself with Combo (Fusion), and leaves it to him
    auto v             = warrior();
    v.skills           = { fastBlade, combo };
    v.best             = kFastBlade;
    v.planOpening      = burningBlade.props;
    v.planCloserFights = true;
    v.planCloser       = "Misha";
    v.resonance        = opening(burningBlade.props, 4.0);
    auto c             = call(v);
    CHECK(c.move == WsMove::Hold);
    // her closer out of the fight: the opening is anyone's
    v.planCloserFights = false;
    c                  = call(v);
    CHECK(c.move == WsMove::Close);
    CHECK(c.skill == kCombo);
    // another opening is not the plan's: she closes it as ever
    v.planCloserFights = true;
    v.resonance        = opening(combo.props, 4.0);
    v.skills           = { burningBlade };
    CHECK(call(v).move == WsMove::Close);
}

TEST_CASE("skillchains: the plan's closer holds her TP for the opening; its opener opens once the closer has his", "[cardian][skillchains]")
{
    auto closer          = warrior(2400);
    closer.seat          = Seat::Closer;
    closer.planSkill     = kBurningBlade;
    closer.planChain     = SC_LIQUEFACTION;
    closer.partnerFights = true;
    closer.partner       = "Misha";
    auto held            = call(closer);
    CHECK(held.move == WsMove::Hold);
    CHECK(held.planHold);
    // held for long enough with no opening, or at the TP cap: her best goes
    closer.heldFor = cardian::chains::kPlanHoldLimit;
    CHECK(call(closer).move == WsMove::Free);
    closer.heldFor = 0.0;
    closer.tp      = cardian::chains::kTpCap;
    CHECK(call(closer).move == WsMove::Free);
    closer.tp = 2400;
    // the plan's weapon skill not hers now: no hold for it
    closer.planSkill = kCombo;
    CHECK(call(closer).move == WsMove::Free);
    closer.planSkill = kBurningBlade;
    // her opener out of this fight: the plan is not this fight's, her best goes
    closer.partnerFights = false;
    CHECK(call(closer).move == WsMove::Free);

    auto opener          = warrior();
    opener.seat          = Seat::Opener;
    opener.planSkill     = kBurningBlade;
    opener.planChain     = SC_FUSION;
    opener.partnerFights = true;
    opener.partner       = "Misha";
    // her closer short of TP: she waits
    auto c = call(opener);
    CHECK(c.move == WsMove::Hold);
    CHECK(c.planHold);
    // not for ever: the plan's weapon skill goes alone
    opener.heldFor = cardian::chains::kPlanHoldLimit + 1.0;
    c              = call(opener);
    CHECK(c.move == WsMove::Free);
    CHECK(c.skill == kBurningBlade);
    opener.heldFor = 0.0;
    // his TP in: she opens with the plan's weapon skill, not her best
    opener.best         = kFastBlade;
    opener.partnerReady = true;
    c                   = call(opener);
    CHECK(c.move == WsMove::Open);
    CHECK(c.skill == kBurningBlade);
    CHECK(c.chain == SC_FUSION);
    // not on a mob under half its HP
    opener.mobHpp = 40;
    CHECK(call(opener).move == WsMove::Hold);
    // the plan's weapon skill not hers now (another weapon): her best alone
    opener.mobHpp    = 80;
    opener.planSkill = kCombo;
    c                = call(opener);
    CHECK(c.move == WsMove::Free);
    CHECK(c.skill == kFastBlade);
}

TEST_CASE("skillchains: without a plan her best goes alone, never on a mob under half its HP", "[cardian][skillchains]")
{
    auto v = warrior();
    auto c = call(v);
    CHECK(c.move == WsMove::Free);
    CHECK(c.skill == kBurningBlade);
    v.mobHpp = 49;
    CHECK(call(v).move == WsMove::Hold);
    v.mobHpp = 50;
    CHECK(call(v).move == WsMove::Free);
    v.best = 0;
    CHECK(call(v).move == WsMove::None);
}

TEST_CASE("skillchains: a member's TP is read as a rate while she fights, a weapon skill's spend skipped", "[cardian][skillchains]")
{
    cardian::chains::TpClock clock;
    // before it has seen six seconds, the prior
    clock.note(0, 0.0, true);
    clock.note(100, 2.0, true);
    CHECK(clock.rateOr(77.0) == 77.0);
    // 50 TP a second, steady
    clock.note(200, 4.0, true);
    clock.note(300, 6.0, true);
    clock.note(400, 8.0, true);
    CHECK(clock.measured >= cardian::chains::kTpTrusted);
    CHECK(clock.rateOr(77.0) > 49.0);
    CHECK(clock.rateOr(77.0) < 51.0);
    // a weapon skill: TP falls, the interval is skipped and the rate kept
    const double before = clock.rate;
    clock.note(50, 10.0, true);
    CHECK(clock.rate == before);
    clock.note(150, 12.0, true);
    CHECK(clock.rate > 49.0);
    CHECK(clock.rate < 51.0);
    // samples too close together are skipped
    clock.note(1000, 12.1, true);
    CHECK(clock.lastTp == 150);
    // out of a fight the baseline goes and the rate stays
    clock.note(150, 14.0, false);
    CHECK(clock.lastAt < 0.0);
    CHECK(clock.rate > 49.0);

    // at a weapon skill's TP she holds, or stands at the cap: nothing learned
    const double learned = clock.rate;
    clock.note(1000, 20.0, true);
    clock.note(3000, 22.0, true);
    clock.note(3000, 24.0, true);
    clock.note(3000, 26.0, true);
    CHECK(clock.rate == learned);
}

TEST_CASE("skillchains: the chain's time from the party's TP", "[cardian][skillchains]")
{
    using cardian::chains::chainIn;
    using cardian::chains::closeIn;
    using cardian::chains::timeToTp;
    CHECK(timeToTp(1200, 0.0) == 0.0);
    CHECK_FALSE(timeToTp(400, 0.0).has_value());
    CHECK(*timeToTp(400, 60.0) == 10.0);

    const double lag = cardian::chains::kThinkWait + cardian::chains::kWsLands + cardian::chains::kWindowOpens + cardian::chains::kWsLands;
    // the later of the two decides
    CHECK(*chainIn(4.0, 10.0) == 10.0 + lag);
    CHECK(*chainIn(0.0, 0.0) == lag);
    CHECK_FALSE(chainIn(4.0, std::nullopt).has_value());
    // an opening already there: its window's opening and the closer's landing
    CHECK(closeIn(1.0) == 2.0 + cardian::chains::kWsLands);
    CHECK(closeIn(5.0) == cardian::chains::kWsLands);
}

namespace
{
    // A Black Mage's nukes, priced: Fire (fire, cheap, quick) and Stone II
    // (earth, dear, slow), each with what it deals and burst
    auto nukes() -> std::vector<Nuke>
    {
        Nuke fire;
        fire.id      = 144;
        fire.element = ELEMENT_FIRE;
        fire.dealt   = 100.0;
        fire.burst   = 190.0;
        fire.cast    = 1.0;
        fire.seconds = 3.5;
        fire.mp      = 20;
        fire.recast  = 5.0;
        fire.now     = true;
        Nuke stone;
        stone.id      = 160;
        stone.element = ELEMENT_EARTH;
        stone.dealt   = 160.0;
        stone.burst   = 300.0;
        stone.cast    = 2.0;
        stone.seconds = 4.5;
        stone.mp      = 40;
        stone.recast  = 6.0;
        stone.now     = true;
        return { fire, stone };
    }

    auto byId(const std::vector<Nuke>& list, const uint16 id) -> const Nuke*
    {
        for (const auto& n : list)
        {
            if (n.id == id)
            {
                return &n;
            }
        }
        return nullptr;
    }
} // namespace

TEST_CASE("skillchains: a burst window open, she bursts with the chain's element unless her free pick deals more a second", "[cardian][skillchains]")
{
    auto       list = nukes();
    BurstView  v;
    v.mp       = 200;
    v.elements = kFire | cardian::chains::elementBit(ELEMENT_LIGHT); // Fusion
    v.chain    = SC_FUSION;
    v.open     = 6.0;
    // Stone II is her free pick (it deals more a second than Fire); Fire
    // bursts, ~190 in 3.5 s against ~160 in 4.5 s
    auto c = cardian::chains::nukeCall(list, byId(list, 160), v);
    CHECK(c.move == NukeMove::Burst);
    CHECK(c.spell == 144);

    // a burst that does not end inside the window is no burst
    v.open = 1.0;
    c      = cardian::chains::nukeCall(list, byId(list, 160), v);
    CHECK(c.move == NukeMove::Free);
    CHECK(c.spell == 160);

    // a free pick that deals more a second than the burst wins
    v.open                 = 6.0;
    list[1].dealt          = 400.0;
    c                      = cardian::chains::nukeCall(list, byId(list, 160), v);
    CHECK(c.move == NukeMove::Free);

    // a chain of another element: no burst of hers
    list       = nukes();
    v.elements = kThunder;
    c          = cardian::chains::nukeCall(list, byId(list, 160), v);
    CHECK(c.move == NukeMove::Free);
}

TEST_CASE("skillchains: a chain on its way, she nukes freely when the burst stays in reach and saves when it would not", "[cardian][skillchains]")
{
    const auto list = nukes();
    BurstView  v;
    v.mp       = 200;
    v.elements = kEarth; // Scission, say
    v.chain    = SC_SCISSION;

    // nothing on the way: her free pick
    auto c = cardian::chains::nukeCall(list, byId(list, 144), v);
    CHECK(c.move == NukeMove::Free);

    // the chain in 20 s: Fire now leaves her free and with the MP for Stone II then
    v.chainIn = 20.0;
    c         = cardian::chains::nukeCall(list, byId(list, 144), v);
    CHECK(c.move == NukeMove::Free);
    CHECK(c.spell == 144);

    // MP for one nuke only: Stone II burst (~300) is worth more than Fire now (~100)
    v.mp = 40;
    c    = cardian::chains::nukeCall(list, byId(list, 144), v);
    CHECK(c.move == NukeMove::Hold);

    // the mob falls before the chain: nothing to save for
    v.mobLives = 10.0;
    c          = cardian::chains::nukeCall(list, byId(list, 144), v);
    CHECK(c.move == NukeMove::Free);
    v.mobLives = -1.0;

    // the chain lands in 1 s and Stone II is her free pick: casting it now
    // keeps her busy past the landing, and her MP short for a second one;
    // the burst (~300) beats Stone II now (~160)
    v.chainIn = 1.0;
    c         = cardian::chains::nukeCall(list, byId(list, 160), v);
    CHECK(c.move == NukeMove::Hold);

    // with the MP for both, Stone II now and Stone II burst inside the
    // window after it (its recast ends 6 s in, the window at 10 s): free
    v.mp = 200;
    c    = cardian::chains::nukeCall(list, byId(list, 160), v);
    CHECK(c.move == NukeMove::Free);

    // nothing castable now: she saves for the burst
    c = cardian::chains::nukeCall(list, nullptr, v);
    CHECK(c.move == NukeMove::Hold);

    // no nuke of the chain's element: no burst to save for
    v.elements = kThunder;
    c          = cardian::chains::nukeCall(list, byId(list, 160), v);
    CHECK(c.move == NukeMove::Free);
}
