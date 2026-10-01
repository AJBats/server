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

// The tank tactician's Provoke pacing (RESEARCH §17.11, the user
// 2026-10-01): kept for the pull, used on its clock in the fight, held at
// the end unless the mob is where it must not be, and an add left alone.

#include <catch2/catch_test_macros.hpp>

#include "map/pawn/tank_calls.h"

#include <optional>

using cardian::tank::Call;
using cardian::tank::Member;
using cardian::tank::Mob;
using cardian::tank::Reason;
using cardian::tank::View;

namespace
{
    constexpr uint32 kTank   = 1;
    constexpr uint32 kHealer = 2;
    constexpr uint32 kPuller = 3;
    constexpr uint32 kMelee  = 4;

    // Her party: herself, the Healer, the puller and a melee, all whole
    auto party() -> View
    {
        View v;
        v.self         = kTank;
        v.provokeReady = true;
        v.provokeRange = 16.0f;
        v.holdHp       = 25;
        v.members      = { { kTank, 100, false }, { kHealer, 100, true }, { kPuller, 100, false }, { kMelee, 100, false } };
        return v;
    }

    auto callOf(const View& v) -> std::optional<Call>
    {
        return cardian::tank::call(v);
    }
} // namespace

TEST_CASE("tank calls: nothing without Provoke, and nothing with nobody to take a mob off", "[cardian][tactics][tank]")
{
    auto v = party();
    v.mobs = { { 100, 100, kPuller, 10.0f } };
    v.provokeReady = false;
    CHECK_FALSE(callOf(v).has_value());

    v = party();
    CHECK_FALSE(callOf(v).has_value()); // no mob at all

    // a mob on nobody of ours is not the party's to take
    v.mobs = { { 100, 100, 0, 10.0f } };
    CHECK_FALSE(callOf(v).has_value());
}

TEST_CASE("tank calls: the pull -- a mob on someone else is Provoked the moment it is in range", "[cardian][tactics][tank]")
{
    auto v = party();
    v.mobs = { { 100, 100, kPuller, 20.0f } };
    CHECK_FALSE(callOf(v).has_value()); // still out of range

    v.mobs[0].distance = 16.0f;
    CHECK(callOf(v) == std::optional<Call>(Call{ 100, Reason::Pull }));

    // the pull's HP is nothing to it: a worn mob the puller brings is taken too
    v.mobs[0].hp = 10;
    CHECK(callOf(v) == std::optional<Call>(Call{ 100, Reason::Pull }));

    // a mob already on her is no pull
    v.mobs[0].target = kTank;
    CHECK_FALSE(callOf(v).has_value());
}

TEST_CASE("tank calls: two to choose from -- the one on the Healer first, then the one on whoever is lowest", "[cardian][tactics][tank]")
{
    auto v = party();
    v.mobs = { { 100, 100, kPuller, 10.0f }, { 101, 100, kHealer, 12.0f } };
    CHECK(callOf(v) == std::optional<Call>(Call{ 101, Reason::Pull }));

    v.mobs = { { 100, 100, kPuller, 10.0f }, { 101, 100, kMelee, 12.0f } };
    v.members[3].hp = 40; // the melee is hurt
    CHECK(callOf(v) == std::optional<Call>(Call{ 101, Reason::Pull }));

    // alike: the first in
    v.members[3].hp = 100;
    CHECK(callOf(v) == std::optional<Call>(Call{ 100, Reason::Pull }));
}

TEST_CASE("tank calls: upkeep -- her fight's mob is Provoked on its clock, whoever it is on", "[cardian][tactics][tank]")
{
    auto v  = party();
    v.fight = 100;
    v.mobs  = { { 100, 80, kTank, 3.0f } };
    CHECK(callOf(v) == std::optional<Call>(Call{ 100, Reason::Upkeep }));

    // ripped by the melee: the same call, no waiting
    v.mobs[0].target = kMelee;
    CHECK(callOf(v) == std::optional<Call>(Call{ 100, Reason::Upkeep }));

    // at the hold exactly it is still upkeep
    v.mobs[0].hp = 25;
    CHECK(callOf(v) == std::optional<Call>(Call{ 100, Reason::Upkeep }));

    // out of range (it ran to someone far): not now
    v.mobs[0].hp       = 80;
    v.mobs[0].distance = 17.0f;
    CHECK_FALSE(callOf(v).has_value());
}

TEST_CASE("tank calls: the end of the fight -- Provoke is held under the hold for the next pull, unless an emergency", "[cardian][tactics][tank]")
{
    auto v  = party();
    v.fight = 100;
    v.mobs  = { { 100, 20, kTank, 3.0f } };
    CHECK_FALSE(callOf(v).has_value());

    // on the melee, who is fine: still held
    v.mobs[0].target = kMelee;
    CHECK_FALSE(callOf(v).has_value());

    // on the Healer: an emergency, whatever the mob's HP
    v.mobs[0].target = kHealer;
    CHECK(callOf(v) == std::optional<Call>(Call{ 100, Reason::Emergency }));

    // on the melee under half HP: an emergency too
    v.mobs[0].target  = kMelee;
    v.members[3].hp   = 49;
    CHECK(callOf(v) == std::optional<Call>(Call{ 100, Reason::Emergency }));

    // the hold is the setting's
    v.members[3].hp = 100;
    v.holdHp        = 10;
    CHECK(callOf(v) == std::optional<Call>(Call{ 100, Reason::Upkeep }));
}

TEST_CASE("tank calls: the add -- a second mob on the party while hers is on her is not hers to Provoke", "[cardian][tactics][tank]")
{
    auto v  = party();
    v.fight = 100;
    v.mobs  = { { 100, 20, kTank, 3.0f }, { 101, 100, kHealer, 5.0f } };
    // her fight is held at the end; the add on the Healer is the player's
    // to direct (RESEARCH §17.10)
    CHECK_FALSE(callOf(v).has_value());

    // her fight on its clock comes first, add or no add
    v.mobs[0].hp = 80;
    CHECK(callOf(v) == std::optional<Call>(Call{ 100, Reason::Upkeep }));

    // her fight over, the add is the next pull
    v.fight.reset();
    v.mobs = { { 101, 100, kHealer, 5.0f } };
    CHECK(callOf(v) == std::optional<Call>(Call{ 101, Reason::Pull }));
}

TEST_CASE("tank calls: why Provoke is held, for the log", "[cardian][tactics][tank]")
{
    using cardian::tank::Held;
    auto v = party();
    v.provokeReady = false;
    CHECK(cardian::tank::decide(v).held == Held::NoProvoke);

    v = party();
    CHECK(cardian::tank::decide(v).held == Held::Nothing);
    v.mobs = { { 100, 100, kPuller, 30.0f } };
    CHECK(cardian::tank::decide(v).held == Held::OutOfRange);

    v.fight = 100;
    CHECK(cardian::tank::decide(v).held == Held::OutOfRange);
    v.mobs[0] = { 100, 10, kTank, 3.0f };
    CHECK(cardian::tank::decide(v).held == Held::OnHer);
    v.mobs[0].target = kMelee;
    CHECK(cardian::tank::decide(v).held == Held::EndOfFight);
    v.mobs[0].hp = 90;
    CHECK(cardian::tank::decide(v).call == std::optional<Call>(Call{ 100, Reason::Upkeep }));
}

TEST_CASE("tank calls: her fight must be in the picture to be judged; the tactician puts it there itself", "[cardian][tactics][tank]")
{
    auto v  = party();
    v.fight = 100;
    v.mobs  = {}; // the log has not opened it, and tactics.cpp's add(PFight) has not run: no call
    CHECK_FALSE(callOf(v).has_value());
    CHECK(cardian::tank::decide(v).held == cardian::tank::Held::Nothing);
    v.mobs = { { 100, 90, kTank, 2.0f } };
    CHECK(callOf(v) == std::optional<Call>(Call{ 100, Reason::Upkeep }));
}

TEST_CASE("tank calls: the emergency's edges -- herself, nobody, and exactly half", "[cardian][tactics][tank]")
{
    using cardian::tank::Held;
    auto v  = party();
    v.fight = 100;
    v.mobs  = { { 100, 10, kTank, 3.0f } };

    // the mob on her while she is under half HP is no emergency: she holds it
    v.members[0].hp = 30;
    CHECK_FALSE(callOf(v).has_value());
    CHECK(cardian::tank::decide(v).held == Held::OnHer);

    // on nobody of ours under the hold: held, as at any end of a fight
    v.members[0].hp  = 100;
    v.mobs[0].target = 0;
    CHECK_FALSE(callOf(v).has_value());
    CHECK(cardian::tank::decide(v).held == Held::EndOfFight);

    // a member at exactly half HP is fine; one below is not
    v.mobs[0].target = kMelee;
    v.members[3].hp  = 50;
    CHECK_FALSE(callOf(v).has_value());
    v.members[3].hp = 49;
    CHECK(callOf(v) == std::optional<Call>(Call{ 100, Reason::Emergency }));
}
