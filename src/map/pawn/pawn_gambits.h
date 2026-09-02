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

#include <optional>

#include <cstddef>
#include <string>
#include <string_view>
#include <utility>
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

    // Cardian-only gambit condition, reserved for the party strategy channel
    // (RESEARCH §8): holds while the party's strategy equals the argument.
    // No strategy exists yet, so a row with it never fires.
    constexpr auto G_CONDITION_STRATEGY = static_cast<gambits::G_CONDITION>(100);

    // The behaviours a row can switch -- engine tuning, not what the party
    // is doing right now (hunting is the party's strategy, another channel).
    // Values are frozen: they appear in the row grammar and will be
    // persisted (M3.85); the gaps are retired values. Rows are the ONLY
    // source of a behaviour: an unchecked row is off, the first row, top
    // down, to speak for a behaviour wins, and a switch no row speaks for
    // is off.
    enum class Behavior : uint16
    {
        AvoidAggro          = 1, // switch
        Formation           = 4, // a Slot
        RestWithPlayer      = 6, // switch: kneel when the player kneels
        HomePointWithPlayer = 7, // switch: a KO'd cardian home points when the player does
    };
    constexpr uint16 BehaviorCount = 8; // one past the last value

    // A switch row carries the value 1 and its checkbox is the switch; a
    // parameter row (the formation slot) carries its value
    constexpr auto isSwitch(const Behavior b) -> bool
    {
        return b != Behavior::Formation;
    }

    // Every cardian starts with these rows (the row grammar, gambit_text.h):
    // avoid aggro on, rest with the player on. Profiles, when they come, are
    // rows in storage seeded the same way.
    auto defaultRows() -> const std::vector<std::pair<std::string, bool>>&;

    enum class Slot : uint16
    {
        Follow = 0, // the chain behind the player
        Lead   = 1, // ahead of the player: the hunter's place
    };

    // One row of a cardian's list: the engine's gambit plus the ON/OFF the
    // player sees in the editor (the trust struct is upstream's, untouched)
    struct GambitRow
    {
        gambits::Gambit_t gambit;
        bool              enabled = true;
    };

    // The row as the player reads it: "Party: HP < 50% -> Cure (best)"
    auto labelGambit(const gambits::Gambit_t& gambit) -> std::string;

    // The catalogue the editor's pickers offer for one cardian: targets,
    // conditions (thresholds pre-expanded, FFXII-style: "HP < 50%" and
    // "HP < 60%" are two entries), statuses (for "has X" / "no X"), and
    // the actions she can take right now -- her spells, abilities and
    // weapon skills, plus the behaviours. Keys are row-grammar fragments.
    struct VocabEntry
    {
        std::string key;   // "target", "cond:arg", "status id", or "reaction:select:arg"
        std::string label; // as the player reads it
        std::string group; // actions only: Behaviours / Magic / Abilities / WeaponSkills / Ranged
    };
    struct Vocabulary
    {
        std::vector<VocabEntry> targets;
        std::vector<VocabEntry> conditions;
        std::vector<VocabEntry> statuses;
        std::vector<VocabEntry> actions;
    };
    auto vocabularyFor(CCharEntity* PPawn) -> Vocabulary;

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

        auto AddGambit(gambits::Gambit_t gambit, bool enabled = true) -> std::string;
        void RemoveGambit(const std::string& id);
        void RemoveAllGambits();
        void SetTPSkillSettings(gambits::G_TP_TRIGGER trigger, gambits::G_SELECT select, uint16 value);

        // engaged == false runs the between-fights pass: no weapon skills,
        // no ranged attacks, no gambits carrying offensive reactions.
        void Tick(timer::time_point tick, bool engaged);

        // The behaviour pass alone, every tick, pathing or not: switches are
        // asserted only while their rows' conditions hold
        void TickBehaviors();

        // The console's way in: the first unconditional row for a behaviour
        // (appended at the bottom if none, where any conditional row above it
        // wins). A switch row is checked or unchecked; a parameter row takes
        // the value.
        void SetBehaviorRow(Behavior behavior, uint16 arg);

        // The editor's view and edits (indices are 1-based, as shown)
        auto Rows() const -> const std::vector<GambitRow>&
        {
            return m_gambits;
        }
        auto MasterOn() const -> bool
        {
            return m_masterOn;
        }
        void SetMaster(bool on);
        auto SetEnabled(std::size_t index, bool on) -> bool;
        auto Move(std::size_t from, std::size_t to) -> bool;
        auto Erase(std::size_t index) -> bool;
        auto Insert(std::size_t index, gambits::Gambit_t gambit) -> bool;
        auto Replace(std::size_t index, gambits::Gambit_t gambit) -> bool; // keeps the row's ON/OFF

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

        std::vector<GambitRow>             m_gambits;
        bool                               m_masterOn = true;
        std::vector<gambits::TrustSkill_t> m_tpSkills;
        gambits::G_TP_TRIGGER              m_tpTrigger = gambits::G_TP_TRIGGER::ASAP;
        gambits::G_SELECT                  m_tpSelect  = gambits::G_SELECT::HIGHEST;
        uint16                             m_tpValue   = 0;

        HashMap<std::string, timer::time_point> m_timerConditionLastTrigger;
    };
} // namespace pawn
