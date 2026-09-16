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

#pragma once

#include "bank_math.h"
#include "fight_math.h"

#include "common/cbasetypes.h"
#include "data/enums/mod.h"
#include "data/enums/status_effect.h"
#include "spell.h"

#include <string>
#include <vector>

class CBattleEntity;
class CCharEntity;
class CMobEntity;

namespace pawn::tactics
{
    using cardian::tactics::Exchange;
    using cardian::tactics::FightRecord;
    using cardian::tactics::SpotAverages;

    // The MP bank (RESEARCH §12.6, §12.13), slice 2: every spell priced in
    // cure MP, printed only. It reads the fight log's figures and asks the
    // server's own formulas for the rest, sampled where they roll dice
    // (modules/cardian/lua/tactics_bank.lua), and decides nothing.

    // How a spell is priced: the mob's melee rounds it removes, the rounds
    // it makes miss, the defence it takes off the mob, the damage it ticks
    enum class Model : uint8
    {
        StoppedRounds,
        Misses,
        DefenceDown,
        Dot,
    };

    // A spell the bank prices, with what the server's tables say of it.
    // A fixed duration means the effect lands whenever the cast resolves
    // (only immunity says no) and holds that long; zero means the resist
    // roll decides both, fed the stat and the accuracy bonus
    struct Priced
    {
        SpellID          id;
        Model            model;
        xi::StatusEffect effect;
        uint16           tier; // the effect's tier, the key a learned percentage is filed under
        xi::Mod          stat;
        int32            bonusMacc;
        int32            fixedDuration;
        double           tick; // seconds between damage ticks, for the ones that tick
    };

    auto priced(SpellID id) -> const Priced*;

    namespace bank
    {
        // The samplers, modules/cardian/lua/tactics_bank.lua: a library the
        // pawn module loads at init (it overrides nothing, so init.txt's
        // module loader would call it malformed)
        void load();

        // What the log saw cast, as one line: a cure's tier table against
        // the gap it faced and the bank's pick (no record needed), a
        // debuff's price, or "unpriced" with its family. Nothing when there
        // is nothing to say
        auto castLine(FightRecord* r, const Exchange& x, CBattleEntity* PCaster, CBattleEntity* PTarget, CSpell* PSpell, int32 missing) -> std::string;

        // Every priced debuff one member could cast on the mob, as the fight opens
        auto priceMember(FightRecord& r, const SpotAverages& spot, const Exchange& x, const std::vector<CBattleEntity*>& members, CBattleEntity* PMember, CMobEntity* PMob) -> std::vector<std::string>;

        // A member's melee hit on the mob: what it would have been at the
        // mob's base defence, the extra booked per defence-down effect
        void defenceSplit(FightRecord& r, CBattleEntity* PMember, CMobEntity* PMob, int32 landed);

        // A damage-over-time tick the mob lost, booked per effect by its
        // share of the regen the mob is losing
        void dotSplit(FightRecord& r, CMobEntity* PMob, int32 landed);
    } // namespace bank
} // namespace pawn::tactics
