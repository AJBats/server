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

#include "formation_math.h"
#include "gambit_defaults.h"
#include "gambit_ids.h"
#include "gambit_layers.h"
#include "pawn_spellbook.h"
#include "tactician_line.h"
#include "world.h"

#include "data/enums/job.h"

#include "common/cbasetypes.h"
#include "common/timer.h"
#include "common/types/hash_map.h"

#include "ai/helpers/gambits_container.h"

#include <optional>

#include <array>
#include <cstddef>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

class CAbility;
class CBattleEntity;
class CCharEntity;
class CPawnController;

namespace pawn
{
    // A character's first rows are her job's defaults, defaultRowsFor
    // (gambit_defaults.h), seeded once by loadBrain (pawn.h). A world body
    // in the wild also runs the world's layer ahead of them (CGambits,
    // gambit_layers.h)

    // The formation slots live with the ring's arithmetic (formation_math.h)
    using Slot = cardian::formation::Slot;

    // A melee job: the gambit engine's MELEE group, and seated on the
    // flanks first
    auto isMeleeJob(xi::Job job) -> bool;

    // One row of a cardian's list: the engine's gambit plus the ON/OFF the
    // player sees in the editor (the trust struct is upstream's, untouched)
    struct GambitRow
    {
        gambits::Gambit_t gambit;
        bool              enabled = true;
    };

    // The row as the player reads it: "Party: HP < 50% -> Cure (best)"
    auto labelGambit(const gambits::Gambit_t& gambit) -> std::string;

    // A spell family as a label: "Cure", or "family 37" when unnamed
    auto familyName(uint32 family) -> std::string;

    // The catalogue the editor's pickers offer for one cardian: the
    // conditions, FFXII's way -- one clause each, naming its side, a number
    // it takes left to the row ("Ally: HP < *%") -- the statuses a status
    // condition names, and the actions she can take right now -- her
    // spells, abilities and weapon skills, plus the behaviours. Keys are
    // row-grammar fragments.
    struct VocabEntry
    {
        std::string key;   // "target|cond:arg" ('*' for the number, 's' for the status), "status id", or "reaction:select:arg"
        std::string label; // as the player reads it; a '*' stands for the number
        std::string group; // actions: Behaviours / Magic / Abilities / WeaponSkills / Ranged; numeric conditions: "min,max,step,default"
        uint16      targets = 0; // actions: the valid-target mask (TARGET_*), so a command window knows which cursor to open
        uint16      mp      = 0; // spells: the base MP cost, so a command window can grey what she cannot afford
        std::string page;        // conditions: the side's page, self / ally / foe
        bool        usable = true; // actions: whether she can use it now (the pickers grey the rest)
    };
    struct Vocabulary
    {
        uint8                   mjob = 0; // her jobs and levels, which the actions follow
        uint8                   mlvl = 0;
        uint8                   sjob = 0;
        uint8                   slvl = 0;
        std::vector<VocabEntry> conditions;
        std::vector<VocabEntry> statuses;
        std::vector<VocabEntry> actions;
    };
    auto vocabularyFor(CCharEntity* PPawn) -> Vocabulary;

    // Her main and support job's abilities at every level (a pet's command,
    // outside the character bitfield, left out), and of those the ones she
    // has now: the catalogue lists the first, the command window and the
    // cooldowns the second.
    auto jobAbilities(CCharEntity* PChar) -> std::vector<CAbility*>;
    auto abilitiesFor(CCharEntity* PChar) -> std::vector<CAbility*>;

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
    //
    // Her rows come in two layers (gambit_layers.h): her own, the only list
    // the editor reads or writes (Rows and the edits below) and saves; and,
    // while she is a world body out in the wild (world.h inTheWild), the
    // world's, compiled from brains.yaml, never saved, shown or sent, and
    // run ahead of hers by the think, the behaviours, the conveyor's
    // requests and the engage door alike. Both follow her master switch.
    //
    // Her own first Support Mage row is the tactician line
    // (tactician_line.h): the rows below it are her tactician's allow-list
    // (Admits, and the engage door's melee rows), never orders, and a row
    // with no meaning where it sits is struck out. The world's layer has no
    // line: its rows are orders.
    class CGambits
    {
    public:
        CGambits(CCharEntity* PPawn, CPawnController* PController);

        auto AddGambit(gambits::Gambit_t gambit, bool enabled = true) -> std::string;
        void RemoveGambit(const std::string& id);
        void RemoveAllGambits();

        // engaged == false runs the between-fights pass: no weapon skills,
        // no ranged attacks, no gambits carrying offensive reactions.
        void Tick(timer::time_point tick, bool engaged);

        // A row's need was met, by her or by another (the conveyor's word):
        // its retry clock starts now
        void StampRetry(const std::string& id, timer::time_point at);
        // Her next think is the next tick: a new fight is a new think
        void Prompt();
        // Recheck a queued spell without consuming timer predicates again.
        auto RequestValid(const std::string& rowId, uint32 target, uint16 spell) -> bool;

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

        // The tactician line: the 1-based place of her first Support Mage
        // row among her own rows, whatever its checkbox; none without one
        auto Line() const -> std::optional<std::size_t>;
        // What her own row at a 1-based place means where it sits, as the
        // editor draws it
        auto StateOf(std::size_t index) const -> cardian::tactician::State;

        // Her allow-list, for her tactician: the id of the first enabled row
        // below the line that lets her cast this spell on this target now --
        // the spell is the row's, the target is one the row names, its retry
        // has run, and its conditions hold, Tactician's choice among them,
        // with no timer spent. Nothing when no row does, or her master switch
        // is off. Only her tactician reads it: the rows above the line cast
        // as the orders they are
        auto Admits(uint16 spell, CBattleEntity* PTarget) -> std::optional<std::string>;
        // Whether any enabled row below the line names this spell, whoever
        // it is for and whatever its conditions: what her rest pacing may
        // count on having
        auto AllowsSpell(uint16 spell) const -> bool;

        // The rows that decide which fight she takes (engage_math.h), read
        // by the engage door: the enabled Attack rows in the running order
        // (the world's layer first while she is in the wild), and none while
        // the master switch is off (engage_math.h doorReads). Each is
        // numbered within its own layer, so one of her own carries the
        // number the editor shows it under. The think never runs them. A row
        // the editor would refuse (pairingError; only a hand-edited saved
        // set can hold one) is passed over, and so is one struck out where
        // it sits. One below the line is her tactician's melee (`below`):
        // the door reads it only while her tactician lets her melee
        // (tactician_line.h meleeAllowed). The pointers hold until her
        // rows, or her world layer, next change, so a reader uses them
        // within the tick it asked in.
        struct EngageRow
        {
            std::size_t              index  = 0;     // 1-based within its layer
            bool                     world  = false; // a row of the world's layer
            bool                     below  = false; // below her tactician line
            const gambits::Gambit_t* gambit = nullptr;
        };
        auto EngageRows() -> std::vector<EngageRow>;
        // Whether an engage row's conditions hold, every one read on the
        // foe the row names
        auto EngageConditionsHold(const gambits::Gambit_t& gambit, CBattleEntity* PFoe) -> bool;

        auto SpellBook() -> CSpellBook&
        {
            return m_spellBook;
        }

    private:
        // The layers that run for her now (gambit_layers.h): her world rows
        // while she is in the wild, compiled on first need and again whenever
        // their key changes (world.h brainKey), then her own
        auto RunningLayers() -> cardian::layers::Layers<GambitRow>;
        void RebuildWorldLayer();
        auto Candidates(gambits::G_TARGET selector) -> std::vector<CBattleEntity*>;
        // Whether a target is one a row's selector names: Candidates as a
        // question about one entity, over her whole alliance for the party
        // selectors, a mob for `Target`
        auto Names(gambits::G_TARGET selector, const CBattleEntity* PTarget) const -> bool;
        auto SelectTarget(const gambits::Gambit_t& gambit) -> CBattleEntity*;
        // What her rows call "the mob": her battle target, else the party's
        // fight she attends or walks in on (RESEARCH §12.15)
        auto FightTarget() -> CBattleEntity*;
        // pending: a timer already passed, not spent again. gate: the row is
        // an allow-list entry, so Tactician's choice holds
        auto CheckTrigger(CBattleEntity* PTrigger, const gambits::Gambit_t& gambit, std::size_t groupIndex, bool pending = false, bool gate = false) -> bool;
        // The states of the running layers' rows, in forEachRow's order:
        // the world's first (orders, no line), then her own
        auto RunningStates(const cardian::layers::Layers<GambitRow>& layers) const -> std::vector<cardian::tactician::State>;
        auto ResolveSpell(const gambits::Action_t& action, CBattleEntity* PTarget) -> Maybe<SpellID>;
        // Behaviour rows (G_REACTION_BEHAVIOR only) flip controller switches
        // and never consume the think; engage rows (Attack) are the door's
        // (EngageRows) and never consume it either
        auto IsBehavior(const gambits::Gambit_t& gambit) const -> bool;
        void ApplyBehavior(const gambits::Gambit_t& gambit);

        // A row's actions in order until one fires; `index` is the row's
        // 1-based place, the conveyor's order among her rows
        auto Execute(const gambits::Gambit_t& gambit, CBattleEntity* PTarget, bool engaged, std::size_t index) -> bool;
        // The conveyor's side of a think (RESEARCH §12.12 item 2): her
        // standing assignment cast; a cast the conveyor assigned, logged
        auto CastAssignment(bool engaged) -> bool;
        auto CastAssigned(SpellID spellId, uint32 target, const std::string& why) -> bool;
        auto ExecuteAbility(const gambits::Action_t& action, CBattleEntity* PTarget, bool engaged) -> bool;
        auto ExecuteWeaponSkill(const gambits::Action_t& action, bool engaged) -> bool;
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

        HashMap<std::string, timer::time_point> m_timerConditionLastTrigger;

        // The world's layer: its rows (ids "w1", "w2"... numbered on across
        // rebuilds), the key they were compiled for, and its own TIMER
        // clocks, so a reload of her own rows leaves the world's running
        std::vector<GambitRow>                  m_worldRows;
        std::optional<pawn::world::BrainKey>    m_worldKey;
        uint32                                  m_nextWorldId = 0;
        HashMap<std::string, timer::time_point> m_worldTimers;
    };
} // namespace pawn
