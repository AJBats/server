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

#include "pawn_spellbook.h"

#include "common/cbasetypes.h"
#include "common/timer.h"
#include "common/types/hash_map.h"

#include "ai/helpers/gambits_container.h"

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

class CBattleEntity;
class CCharEntity;
class CPawnController;

namespace pawn
{
    // Cardian-only gambit reaction: flip a controller behaviour instead of
    // acting. select = the behaviour (Behavior below), arg = on/off. Applied
    // while the row's conditions hold, and it never consumes the think.
    constexpr auto G_REACTION_BEHAVIOR = static_cast<gambits::G_REACTION>(100);

    enum class Behavior : uint16
    {
        AvoidAggro = 1,
    };

    // The pawn gambit interpreter: CGambitsContainer's decision loop rebuilt
    // for a character owner. It speaks the trust vocabulary (gambits::G_*,
    // the same Lua table shapes) so brains transfer verbatim, but the party
    // is the pawn's own CParty, spells come from the pawn's spell book, and
    // weapon skills from what the character has learned and can use with its
    // current weapon. Nothing is filtered at add time: a gambit for a spell
    // or ability the pawn lacks never fires, and starts firing the day the
    // pawn learns it -- the same gambit list serves level 1 and level 75.
    //
    // Deliberate departures from the trust engine:
    //  - party-scoped selectors consider the most hurt member first
    //  - one action per think (a second cast can never start anyway)
    //  - G_REACTION::WS is a real reaction (SPECIFIC / HIGHEST / RANDOM)
    //  - an out-of-combat pass runs support gambits between fights
    //  - JA_ON_COOLDOWN consults the pawn's recast container
    //  - trust-NPC specials (mob skills, animation strings, Curilla,
    //    Uriel, Ayame/August) are not carried over
    class CGambits
    {
    public:
        CGambits(CCharEntity* PPawn, CPawnController* PController);

        auto AddGambit(gambits::Gambit_t gambit) -> std::string;
        void RemoveGambit(const std::string& id);
        void RemoveAllGambits();
        void SetTPSkillSettings(gambits::G_TP_TRIGGER trigger, gambits::G_SELECT select, uint16 value);

        // engaged == false runs the between-fights pass: no weapon skills,
        // no ranged attacks, no gambits carrying offensive reactions.
        void Tick(timer::time_point tick, bool engaged);

        // The behaviour pass alone, every tick, pathing or not: switches are
        // asserted only while their rows' conditions hold
        void TickBehaviors();

        auto Size() const -> std::size_t
        {
            return m_gambits.size();
        }

        auto SpellBook() -> CSpellBook&
        {
            return m_spellBook;
        }

    private:
        auto Candidates(gambits::G_TARGET selector) -> std::vector<CBattleEntity*>;
        auto SelectTarget(const gambits::Gambit_t& gambit) -> CBattleEntity*;
        auto CheckTrigger(CBattleEntity* PTrigger, const gambits::Gambit_t& gambit, std::size_t groupIndex) -> bool;
        auto ResolveSpell(const gambits::Action_t& action, CBattleEntity* PTarget) -> Maybe<SpellID>;
        // Behaviour rows (G_REACTION_BEHAVIOR only) flip controller switches
        // and never consume the think
        auto IsBehavior(const gambits::Gambit_t& gambit) const -> bool;
        void ApplyBehavior(const gambits::Gambit_t& gambit);

        auto Execute(const gambits::Gambit_t& gambit, CBattleEntity* PTarget, bool engaged) -> bool;
        auto ExecuteAbility(const gambits::Action_t& action, CBattleEntity* PTarget, bool engaged) -> bool;
        auto ExecuteWeaponSkill(const gambits::Action_t& action, bool engaged) -> bool;
        auto TryWeaponSkill() -> bool;
        void RefreshWeaponSkills();
        auto PartyHasHealer() const -> bool;
        auto PartyHasTank() const -> bool;
        auto IsOffensive(const gambits::Gambit_t& gambit) const -> bool;
        void Debug(std::string_view what, uint32 id, const CBattleEntity* PTarget) const;

        CCharEntity*      POwner;
        CPawnController*  m_PController;
        CSpellBook        m_spellBook;
        timer::time_point m_lastAction;
        uint32            m_nextId = 0;

        std::vector<gambits::Gambit_t>     m_gambits;
        std::vector<gambits::TrustSkill_t> m_tpSkills;
        gambits::G_TP_TRIGGER              m_tpTrigger = gambits::G_TP_TRIGGER::ASAP;
        gambits::G_SELECT                  m_tpSelect  = gambits::G_SELECT::HIGHEST;
        uint16                             m_tpValue   = 0;

        HashMap<std::string, timer::time_point> m_timerConditionLastTrigger;
    };
} // namespace pawn
