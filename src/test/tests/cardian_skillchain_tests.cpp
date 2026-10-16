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
// the plan's book and when a plan ends; the tactician's weapon skill --
// close, keep clear, the plan's closer and opener, saving the chain for the
// next fight, a spare weapon skill alone, the mob kept clear, her best; the
// party's TP read as a rate, the chain's TP window and its time from it; the
// chain's worth on a mob; the party's lines and when the ask is said; and the
// Black Mage's nuke against the burst, the MP she regains counted. The game's
// own chain rules are the server's own, battleutils, as skillchains.cpp hands
// them in.

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
    closer.startIn       = 8.0;
    closer.planComing    = true;
    auto held            = call(closer);
    CHECK(held.move == WsMove::Hold);
    CHECK(held.planHold);
    CHECK_FALSE(held.saveHold);
    // at the TP cap she still holds while the opening is coming: the
    // prediction decides, not the cap
    closer.tp = cardian::chains::kTpCap;
    CHECK(call(closer).move == WsMove::Hold);
    closer.tp = 2400;
    // held past the safety limit with no opening come: her best goes
    closer.heldFor = cardian::chains::kPlanHoldSafety;
    CHECK(call(closer).move == WsMove::Free);
    closer.heldFor = cardian::chains::kPlanHoldSafety - 1.0;
    CHECK(call(closer).move == WsMove::Hold);
    closer.heldFor = 0.0;
    // the plan's weapon skill not hers now: no hold for it
    closer.planSkill = kCombo;
    CHECK(call(closer).move == WsMove::Free);
    closer.planSkill = kBurningBlade;
    // her opener out of this fight: the plan is not this fight's, her best goes
    closer.partnerFights = false;
    CHECK(call(closer).move == WsMove::Free);
    closer.partnerFights = true;
    // the prediction says the opening never comes (her opener gains no TP):
    // no hold, her best goes at once
    closer.planComing = false;
    closer.startIn.reset();
    auto released = call(closer);
    CHECK(released.move == WsMove::Free);
    CHECK_FALSE(released.planHold);

    auto opener          = warrior();
    opener.seat          = Seat::Opener;
    opener.planSkill     = kBurningBlade;
    opener.planChain     = SC_FUSION;
    opener.partnerFights = true;
    opener.partner       = "Misha";
    opener.startIn       = 6.0;
    opener.planComing    = true;
    // her closer short of TP: she waits
    auto c = call(opener);
    CHECK(c.move == WsMove::Hold);
    CHECK(c.planHold);
    // not past the safety limit: the plan's weapon skill goes alone
    opener.heldFor = cardian::chains::kPlanHoldSafety + 1.0;
    c              = call(opener);
    CHECK(c.move == WsMove::Free);
    CHECK(c.skill == kBurningBlade);
    opener.heldFor = 0.0;
    // his TP in: she opens with the plan's weapon skill, not her best
    opener.best         = kFastBlade;
    opener.partnerReady = true;
    opener.startIn      = 0.0;
    c                   = call(opener);
    CHECK(c.move == WsMove::Open);
    CHECK(c.skill == kBurningBlade);
    CHECK(c.chain == SC_FUSION);
    CHECK(c.plays);
    CHECK_FALSE(c.spare);
    // a mob under half its HP is no reason not to open: the chain's worth
    // decides (below), not the half-HP rule
    opener.mobHpp = 40;
    CHECK(call(opener).move == WsMove::Open);
    // the plan's weapon skill not hers now (another weapon): her best alone
    opener.mobHpp    = 80;
    opener.planSkill = kCombo;
    c                = call(opener);
    CHECK(c.move == WsMove::Free);
    CHECK(c.skill == kFastBlade);
}

TEST_CASE("skillchains: a chain that does not fit the mob is saved for the next fight, a spare weapon skill aside", "[cardian][skillchains]")
{
    // The opener with her closer's TP in, the chain not worth it here: she
    // keeps her TP, and says so (saveHold)
    auto opener          = warrior();
    opener.seat          = Seat::Opener;
    opener.planSkill     = kBurningBlade;
    opener.planChain     = SC_FUSION;
    opener.partnerFights = true;
    opener.partnerReady  = true;
    opener.partner       = "Misha";
    opener.saving        = true;
    auto c               = call(opener);
    CHECK(c.move == WsMove::Hold);
    CHECK(c.saveHold);
    CHECK_FALSE(c.planHold);
    // saving is no plan hold: the safety limit does not end it
    opener.heldFor = cardian::chains::kPlanHoldSafety + 5.0;
    CHECK(call(opener).move == WsMove::Hold);
    // back at a weapon skill's TP before the mob falls: a spare weapon skill
    // alone, her best
    opener.spareIn = 14.0;
    c              = call(opener);
    CHECK(c.move == WsMove::Free);
    CHECK(c.skill == kBurningBlade);

    // the closer the same
    auto closer          = warrior(2000);
    closer.seat          = Seat::Closer;
    closer.planSkill     = kBurningBlade;
    closer.planChain     = SC_LIQUEFACTION;
    closer.partnerFights = true;
    closer.partner       = "Misha";
    closer.saving        = true;
    c                    = call(closer);
    CHECK(c.move == WsMove::Hold);
    CHECK(c.saveHold);
    // the plan's own opening on the mob (her opener opened anyway, a player
    // by hand): she closes it, saving or not
    closer.resonance = opening(combo.props, 4.0);
    closer.planOpening = combo.props;
    c                  = call(closer);
    CHECK(c.move == WsMove::Close);
    CHECK(c.skill == kBurningBlade);
}

TEST_CASE("skillchains: a member ahead of the chain's TP window spends a spare weapon skill alone; any other close waits for the plan", "[cardian][skillchains]")
{
    // The closer at her TP, her opener's TP far off: a spare weapon skill
    // alone, her best, rather than a long hold
    auto closer          = warrior(1200);
    closer.seat          = Seat::Closer;
    closer.planSkill     = kBurningBlade;
    closer.planChain     = SC_LIQUEFACTION;
    closer.partnerFights = true;
    closer.partner       = "Misha";
    closer.startIn       = 30.0;
    closer.planComing    = true;
    closer.spareIn       = 18.0;
    auto c               = call(closer);
    CHECK(c.move == WsMove::Free);
    CHECK(c.skill == kBurningBlade);
    CHECK(c.spare);
    // not spare: she holds
    closer.spareIn.reset();
    CHECK(call(closer).move == WsMove::Hold);

    // the opener the same, while her closer's TP is still coming
    auto opener          = warrior();
    opener.seat          = Seat::Opener;
    opener.planSkill     = kBurningBlade;
    opener.planChain     = SC_FUSION;
    opener.partnerFights = true;
    opener.partner       = "Misha";
    opener.best          = kFastBlade;
    opener.startIn       = 25.0;
    opener.planComing    = true;
    opener.spareIn       = 15.0;
    c                    = call(opener);
    CHECK(c.move == WsMove::Free);
    CHECK(c.skill == kFastBlade);
    // his TP in: the plan goes, spare or not
    opener.partnerReady = true;
    CHECK(call(opener).move == WsMove::Open);

    // An opening that is not the plan's own close (another's, here Combo's
    // that her Burning Blade closes): a member of the plan keeps her TP for
    // the plan unless it is spare
    auto member          = warrior();
    member.seat          = Seat::Opener;
    member.planSkill     = kBurningBlade;
    member.planChain     = SC_FUSION;
    member.partnerFights = true;
    member.partner       = "Misha";
    member.startIn       = 5.0;
    member.planComing    = true;
    member.resonance     = opening(combo.props, 4.0);
    c                    = call(member);
    CHECK(c.move == WsMove::Hold);
    CHECK(c.planHold);
    member.spareIn = 12.0;
    member.startIn = 20.0;
    c              = call(member);
    CHECK(c.move == WsMove::Close);
    CHECK(c.chain == SC_LIQUEFACTION);
    CHECK(c.spare); // paid for with her spare TP
    CHECK_FALSE(c.plays);
    // the plan not coming (her partner gains no TP): nothing to keep it for
    member.spareIn.reset();
    member.startIn.reset();
    member.planComing = false;
    CHECK(call(member).move == WsMove::Close);
    // outside any plan she closes it as ever
    member.seat = Seat::None;
    member.spareIn.reset();
    CHECK(call(member).move == WsMove::Close);
}

TEST_CASE("skillchains: outside the plan, the mob is kept clear of her opening while the plan's chain is due", "[cardian][skillchains]")
{
    auto v      = warrior();
    v.planChain = SC_FUSION;
    // the plan's chain can start in 6 s: her opening would still be on the
    // mob then
    v.startIn = 6.0;
    auto c    = call(v);
    CHECK(c.move == WsMove::Hold);
    // far enough off that hers is gone by then: her best goes
    v.startIn = cardian::chains::kWsLands + cardian::chains::kOpeningLasts + 1.0;
    CHECK(call(v).move == WsMove::Free);
    // no chain coming (saved, overdue, or no plan): her best goes
    v.startIn.reset();
    CHECK(call(v).move == WsMove::Free);
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
    // the fight's start, as sampled
    CHECK(clock.since == 0.0);
    // out of a fight the baseline goes and the rate stays
    clock.note(150, 14.0, false);
    CHECK(clock.lastAt < 0.0);
    CHECK(clock.since < 0.0);
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

TEST_CASE("skillchains: a chain on its way far off, the MP she regains meanwhile keeps her burst in reach", "[cardian][skillchains]")
{
    const auto list = nukes();
    BurstView  v;
    v.mp       = 40;
    v.mpMax    = 200;
    v.elements = kEarth;
    v.chain    = SC_SCISSION;
    v.chainIn  = 20.0;
    // MP for one nuke only and nothing regained: she saves (as before)
    auto c = cardian::chains::nukeCall(list, byId(list, 144), v);
    CHECK(c.move == NukeMove::Hold);
    // her Refresh, a tick of 3 MP: 20 MP by the chain, Fire now and Stone II then
    v.mpPerSecond = 3.0 / cardian::chains::kRegenTick;
    c             = cardian::chains::nukeCall(list, byId(list, 144), v);
    CHECK(c.move == NukeMove::Free);
    CHECK(c.spell == 144);
    // the chain close: not enough regained in time, she saves
    v.chainIn = 2.0;
    c         = cardian::chains::nukeCall(list, byId(list, 144), v);
    CHECK(c.move == NukeMove::Hold);
    // what she regains stops at her maximum
    CHECK(v.mpAt(190, 100.0) == 200);
    CHECK(v.mpAt(40, 0.0) == 40);
}

TEST_CASE("skillchains: a plan ends when its opener, its closer or its player leaves the party, and with his session", "[cardian][skillchains]")
{
    using cardian::chains::Plan;
    using cardian::chains::PlanBook;
    constexpr uint32 kZapp = 13; // another player, with a plan of his own
    constexpr uint32 kRiko = 14;
    constexpr uint32 kTani = 15;

    PlanBook book;
    book.set(kMisha, Plan{ kJevyak, kBurningBlade, kKokoro, kCombo, SC_FUSION });
    book.set(kZapp, Plan{ kRiko, kFastBlade, kTani, kRedLotus, SC_SCISSION });
    REQUIRE(book.find(kMisha).has_value());

    // a member in neither plan leaves: both stand
    CHECK(book.memberLeft(99).empty());
    CHECK(book.find(kMisha).has_value());

    // the opener leaves: Misha's plan ends, Zapp's stands
    auto ended = book.memberLeft(kJevyak);
    REQUIRE(ended.size() == 1);
    CHECK(ended[0] == kMisha);
    CHECK_FALSE(book.find(kMisha).has_value());
    CHECK(book.find(kZapp).has_value());

    // the closer leaves
    book.set(kMisha, Plan{ kJevyak, kBurningBlade, kKokoro, kCombo, SC_FUSION });
    CHECK(book.memberLeft(kKokoro) == std::vector<uint32>{ kMisha });
    CHECK_FALSE(book.find(kMisha).has_value());

    // the player himself leaves his party: his own plan ends
    book.set(kMisha, Plan{ kJevyak, kBurningBlade, kKokoro, kCombo, SC_FUSION });
    CHECK(book.memberLeft(kMisha) == std::vector<uint32>{ kMisha });

    // a player in his own plan (he opens) leaving ends it once
    book.set(kMisha, Plan{ kMisha, kCombo, kJevyak, kBurningBlade, SC_LIQUEFACTION });
    CHECK(book.memberLeft(kMisha) == std::vector<uint32>{ kMisha });

    // his session ends (clear): his plan goes, nobody else's
    CHECK(book.clear(kZapp));
    CHECK_FALSE(book.clear(kZapp));
    CHECK_FALSE(book.find(kZapp).has_value());
}

TEST_CASE("skillchains: the chain's start is when the last of its members has the TP", "[cardian][skillchains]")
{
    using cardian::chains::Ready;
    using cardian::chains::startIn;
    // one at 1200 TP, one at 400 gaining 60 a second: ten seconds
    CHECK(*startIn({ { 1200, 50.0 }, { 400, 60.0 } }) == 10.0);
    // both in the window: now
    CHECK(*startIn({ { 1000, 0.0 }, { 2500, 0.0 } }) == 0.0);
    // one who gains no TP never arrives
    CHECK_FALSE(startIn({ { 1200, 50.0 }, { 400, 0.0 } }).has_value());
    // any number of members: the slowest decides
    CHECK(*startIn({ { 900, 100.0 }, { 0, 50.0 }, { 500, 50.0 } }) == 20.0);
}

TEST_CASE("skillchains: a weapon skill alone is spare when she is back in the window before her TP is wanted", "[cardian][skillchains]")
{
    using cardian::chains::spareWs;
    // 100 TP a second (a fast Samurai, say): back in 2 + 10 s
    const auto back = spareWs({ 1500, 100.0 }, 20.0);
    REQUIRE(back.has_value());
    CHECK(*back == cardian::chains::kNakedBusy + 10.0);
    // the chain wanted sooner than that and the margin: not spare
    CHECK_FALSE(spareWs({ 1500, 100.0 }, 13.0).has_value());
    // her opening would still be on the mob when the chain is due
    CHECK_FALSE(spareWs({ 1500, 1000.0 }, cardian::chains::kOpeningLasts).has_value());
    // short of a weapon skill's TP, or gaining none, or nothing to wait for
    CHECK_FALSE(spareWs({ 900, 100.0 }, 40.0).has_value());
    CHECK_FALSE(spareWs({ 1500, 0.0 }, 40.0).has_value());
    CHECK_FALSE(spareWs({ 1500, 100.0 }, std::nullopt).has_value());
    // a slow member (25 TP a second) needs 42 s and the margin
    CHECK_FALSE(spareWs({ 1000, 25.0 }, 40.0).has_value());
    CHECK(spareWs({ 1000, 25.0 }, 45.0).has_value());
}

TEST_CASE("skillchains: the chain fits a mob that will have most of its damage left in HP as it lands", "[cardian][skillchains]")
{
    using cardian::chains::Worth;
    Worth w;
    w.openerWs       = 250.0;
    w.closerWs       = 250.0;
    w.chainShare     = 0.5; // a level 1 chain of two
    w.burst          = 300.0;
    w.dealtPerSecond = 60.0;
    // its damage: both weapon skills, the chain's share of the closer's, the burst
    const double lag = cardian::chains::kThinkWait + cardian::chains::kWsLands + cardian::chains::kWindowOpens + cardian::chains::kWsLands;

    // a tough mob, 5000 HP: it fits
    w.hp    = 5000;
    auto c  = cardian::chains::worth(w);
    CHECK(c.total == 925.0);
    CHECK(c.landsIn == lag);
    CHECK(c.hpThen == 5000.0 - 60.0 * lag);
    CHECK(c.fits);

    // an even match at half its HP (750), dying fast: most of the chain
    // would be spent past its death (the user's "TP mob")
    w.hp = 750;
    CHECK_FALSE(cardian::chains::worth(w).fits);

    // the same mob with no burst coming: less of the chain to waste, still no
    w.burst = 0.0;
    CHECK_FALSE(cardian::chains::worth(w).fits);

    // a mob the party barely dents: its HP is what it has
    w.burst          = 300.0;
    w.hp             = 800;
    w.dealtPerSecond = 0.0;
    CHECK(cardian::chains::worth(w).fits);

    // the chain's start far off: the party's damage meanwhile counts
    w.dealtPerSecond = 60.0;
    w.hp             = 2000;
    CHECK(cardian::chains::worth(w).fits);
    w.startIn = 20.0;
    CHECK_FALSE(cardian::chains::worth(w).fits);

    // nothing priced yet: it fits a mob that outlives its landing
    Worth bare;
    bare.hp             = 400;
    bare.dealtPerSecond = 10.0;
    CHECK(cardian::chains::worth(bare).fits);
    bare.hp = 50;
    CHECK_FALSE(cardian::chains::worth(bare).fits);
}

TEST_CASE("skillchains: with no plan a cardian asks once a session, settled into one of its first fights", "[cardian][skillchains]")
{
    cardian::chains::AskWatch w;
    // her first think with the TP, a second in: not settled yet
    CHECK_FALSE(w.due(100, 1.0));
    // settled: look once in this fight
    CHECK(w.due(100, cardian::chains::kAskSettle));
    CHECK_FALSE(w.due(100, cardian::chains::kAskSettle + 5.0));
    // the party could not make one then: the next fight is looked at too
    CHECK(w.due(101, 20.0));
    CHECK(w.due(102, 20.0));
    // past the first few fights: never
    CHECK_FALSE(w.due(103, 20.0));

    // said once: never again
    cardian::chains::AskWatch once;
    CHECK(once.due(200, 10.0));
    once.said = true;
    CHECK_FALSE(once.due(201, 10.0));
}

TEST_CASE("skillchains: the party's lines are five each, in the party's words", "[cardian][skillchains]")
{
    for (const auto& lines : { cardian::chains::kAskLines, cardian::chains::kSaveLines })
    {
        CHECK(lines.size() == 5);
        for (const auto line : lines)
        {
            const std::string text(line);
            CHECK_FALSE(text.empty());
            // player text names nobody "she" or "her", nor a cardian
            CHECK(text.find(" she ") == std::string::npos);
            CHECK(text.find(" her ") == std::string::npos);
            CHECK(text.find("ardian") == std::string::npos);
        }
    }
}

TEST_CASE("skillchains: saving the chain, a weapon skill alone goes only when holding would pass the TP cap", "[cardian][skillchains]")
{
    using cardian::chains::saveSpare;
    // 2000 TP at 50 a second, the mob 30 s from falling: 3500 by then, past
    // the cap -- the weapon skill goes, and she is back at 1000 in 22 s
    const auto spent = saveSpare({ 2000, 50.0 }, 30.0);
    REQUIRE(spent.has_value());
    CHECK(*spent == cardian::chains::kNakedBusy + 20.0);
    // 1200 TP: 2700 by then, under the cap -- she keeps building for the next pull
    CHECK_FALSE(saveSpare({ 1200, 50.0 }, 30.0).has_value());
    // at the cap with the mob about to fall: she would not be back in time
    CHECK_FALSE(saveSpare({ 3000, 50.0 }, 12.0).has_value());
    // the mob's life unknown: no weapon skill alone
    CHECK_FALSE(saveSpare({ 3000, 50.0 }, std::nullopt).has_value());
}

TEST_CASE("skillchains: the opener's TP spent alone when her closer's never comes keeps the half-HP rule", "[cardian][skillchains]")
{
    auto opener          = warrior();
    opener.seat          = Seat::Opener;
    opener.planSkill     = kBurningBlade;
    opener.planChain     = SC_FUSION;
    opener.partnerFights = true;
    opener.partner       = "Misha";
    // her closer gains no TP: no hold, the plan's weapon skill alone
    opener.planComing = false;
    auto c            = call(opener);
    CHECK(c.move == WsMove::Free);
    CHECK(c.skill == kBurningBlade);
    // past the safety limit the same
    opener.planComing = true;
    opener.heldFor    = cardian::chains::kPlanHoldSafety;
    CHECK(call(opener).move == WsMove::Free);
    // on a mob about to fall it is TP spent with no chain: she holds, and it
    // is no hold for the plan
    opener.mobHpp = cardian::chains::kSpendHpp - 1;
    c             = call(opener);
    CHECK(c.move == WsMove::Hold);
    CHECK_FALSE(c.planHold);
    opener.planComing = false;
    opener.heldFor    = 0.0;
    CHECK(call(opener).move == WsMove::Hold);
}

TEST_CASE("skillchains: a member of the plan keeps her TP for it rather than close another's opening, as the others read her too", "[cardian][skillchains]")
{
    using cardian::chains::keepsForPlan;
    using cardian::chains::StrayView;
    // in the plan, the chain coming, not her own close, nothing spare: she keeps it
    StrayView s{ true, false, false, false, true, 0.0 };
    CHECK(keepsForPlan(s));
    // the plan's own close goes
    s.plansOwn = true;
    CHECK_FALSE(keepsForPlan(s));
    s.plansOwn = false;
    // spare TP pays for it
    s.spare = true;
    CHECK_FALSE(keepsForPlan(s));
    s.spare = false;
    // the chain saved for the next fight: kept, whatever the hold's clock
    s.saving     = true;
    s.planComing = false;
    s.heldFor    = cardian::chains::kPlanHoldSafety + 10.0;
    CHECK(keepsForPlan(s));
    s.saving = false;
    // the chain not coming, or her hold past its limit: nothing to keep it for
    CHECK_FALSE(keepsForPlan(s));
    s.planComing = true;
    CHECK_FALSE(keepsForPlan(s));
    s.heldFor = 0.0;
    CHECK(keepsForPlan(s));
    // outside the plan she closes as ever
    s.inPlan = false;
    CHECK_FALSE(keepsForPlan(s));
}
