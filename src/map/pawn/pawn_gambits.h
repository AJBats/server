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
#include "gambit_host.h"
#include "gambit_ids.h"
#include "gambit_layers.h"
#include "party_roles.h"
#include "pawn_spellbook.h"
#include "tactician_line.h"
#include "world.h"

#include "data/enums/job.h"

#include "common/cbasetypes.h"
#include "common/timer.h"
#include "common/types/hash_map.h"

#include "ai/helpers/gambits_container.h"

#include <functional>
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
class CSpell;

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

    // The row as the player reads it, in its two halves: when, "Party: HP <
    // 50%", and what, "Cure (best)" (the editor draws an arrow between them)
    struct GambitLabel
    {
        std::string head;
        std::string action;
    };
    auto labelGambit(const gambits::Gambit_t& gambit) -> GambitLabel;

    // A spell family as a label: "Cure", or "family 37" when unnamed
    auto familyName(uint32 family) -> std::string;

    // The catalogue the editor's pickers offer for one cardian: the
    // conditions, FFXII's way -- one clause each, one target and one
    // condition on its side's page, a number or a status it takes left to
    // the row ("Ally: HP < *%") -- the statuses a status condition names,
    // and the actions of her jobs at every level -- her spells, abilities
    // and weapon skills, plus the behaviours -- each marked whether she can
    // use it now. A character his own client drives (gambit_host.h
    // OwnClient) is offered what his hands do: no behaviour, and no
    // Tactician's choice, since he has no tactician.
    enum class Side : uint8
    {
        Self,
        Ally,
        Foe,
    };
    enum class Takes : uint8
    {
        Nothing,
        Number, // in its range, the label's '*'
        Status, // picked in the row's next cell
    };
    struct VocabCondition
    {
        gambits::G_TARGET    target;
        gambits::G_CONDITION condition;
        Takes                takes = Takes::Nothing;
        Side                 side  = Side::Self;
        uint16               min = 0, max = 0, step = 0, initial = 0; // Takes::Number: its range, and where a new row starts
        std::string          label;                                   // a '*' stands for the number
    };
    struct VocabStatus
    {
        uint16      id; // xi::StatusEffect
        std::string label;
    };
    enum class ActionGroup : uint8
    {
        Fight,
        Behaviours,
        Magic,
        Abilities,
        WeaponSkills,
        Ranged,
    };
    struct VocabAction
    {
        gambits::G_REACTION reaction;
        gambits::G_SELECT   select;
        uint32              arg = 0;
        std::string         label;
        ActionGroup         group;
        uint16              targets = 0;    // the valid-target mask (TARGET_*), so a command window knows which cursor to open
        uint16              mp      = 0;    // spells: the base MP cost, so a command window can grey what she cannot afford
        bool                usable  = true; // whether she can use it now (the pickers grey the rest)
    };
    struct Vocabulary
    {
        uint8                       mjob = 0; // her jobs and levels, which the actions follow
        uint8                       mlvl = 0;
        uint8                       sjob = 0;
        uint8                       slvl = 0;
        std::vector<VocabCondition> conditions;
        std::vector<VocabStatus>    statuses;
        std::vector<VocabAction>    actions;
    };
    auto vocabularyFor(CCharEntity* PPawn, bool ownClient = false) -> Vocabulary;

    // Another of the caster's party is already casting what would make this
    // cast redundant on this target: the same buff or debuff family no
    // stronger, a cure on someone above half HP, the same -na
    auto partyAlreadyCasting(CCharEntity* PCaster, CSpell* PSpell, const CBattleEntity* PTarget) -> bool;

    // Whoever a character's battle target hates most; nobody without one
    auto topEnmityOf(const CBattleEntity* PEntity) -> CBattleEntity*;

    // An ally of hers engaged on this foe: any character of her party in
    // her zone but herself, the player as much as a cardian
    auto allyOn(const CCharEntity* PSelf, const CBattleEntity* PFoe) -> const CCharEntity*;

    // A foe around a character's party as the engage door's finders see it
    // (engage_math.h Foe): whether the leader is engaged on it, an ally of
    // his, and whether it is on him or on his party. `heldOff` is the
    // asker's own hold-off on it
    auto foeFacts(const CCharEntity* PSelf, CBattleEntity* PFoe, const CCharEntity* PLeader, bool heldOff = false) -> cardian::engage::Foe;

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
    // A row carrying the tactician's mark (tactician_line.h) is her
    // tactician's tool (Admits and UseHateTool, and the engage door's melee
    // rows), never an order, and a mark on nothing it has a judgement for
    // is struck out; every other row is an order. The world's rows are all
    // orders.
    //
    // It runs for whoever hosts it (gambit_host.h): a cardian's controller,
    // or the controller of a character his own client drives. His rows are
    // his own alone -- no world layer, no party role's -- and his plain rows
    // run as orders: what his hands do. No tactician runs for him yet, so a
    // marked row of his, and a behaviour row, is struck out
    // (tactician_line.h ownClientStateOf).
    class CGambits
    {
    public:
        CGambits(CCharEntity* POwner, GambitHost* PHost);

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

        // Whether her list, her role's rows fitted in, offers her tactician
        // a tool: a marked row that runs and names something it has a
        // judgement for (tactician_line.h State::Tool). OffersSpells: the
        // spells it casts for the party -- cures, priced debuffs, ailments
        // -- which make her attend fights at cure range. Both are kept
        // between changes to her rows. OffersRest: her Rest row, the MP
        // pacing, with its conditions holding now (RESEARCH §17.13)
        auto OffersTools() const -> bool;
        auto OffersSpells() const -> bool;
        auto OffersRest() -> bool;
        // The nukes her tactician may cast for her -- learned, at her jobs
        // and level, whatever her MP and recasts say this instant -- while a
        // marked Damage spell (any) row of hers runs; none otherwise. What a
        // Burn on her mob is priced against (spell_bank.cpp burnPlans)
        auto OfferedNukes() -> std::vector<SpellID>;
        // Her marked row of a tool that goes out right before her weapon
        // skill -- Boost, or Sneak Attack from the mob's back -- on, its
        // gate holding now, in any layer that runs (a wild Monk's brains
        // carry a Boost row) (RESEARCH §17.13)
        auto OffersBeforeWs(cardian::tactician::Allowance tool) -> bool;
        // Her Sneak Attack goes naked, on a plain hit (RESEARCH §17.13 item
        // 5): her marked Sneak Attack row offers it and no weapon skill row
        // of hers that is on can take it. Read each think
        auto NakedSneak() const -> bool
        {
            return m_nakedSneak;
        }
        // The tank tactician's door (RESEARCH §17.11): the first enabled
        // marked row that lets her use this hate tool on this target now --
        // the row names the ability and the target, its retry has run, its
        // conditions hold -- used through it, and stamped. Nothing when it
        // was; else why it was not, for the log
        auto UseHateTool(uint16 ability, CBattleEntity* PTarget, const std::string& why) -> std::optional<std::string>;
        // Her rows as the editor shows them: her own and the rows her party
        // role lends, in the running order (gambit_layers.h place, with the
        // role's rows where Rebind keeps them), each with
        // whose it is, its number (hers: the one the edits name; a lent one:
        // 0, no edit names it) and its state where it sits
        struct ShownRow
        {
            const GambitRow*          row    = nullptr;
            cardian::layers::Origin   origin = cardian::layers::Origin::Own;
            std::size_t               index  = 0;
            cardian::tactician::State state  = cardian::tactician::State::Order;
            bool                      on     = true; // as it runs: a row the role pins runs on whatever its checkbox
        };
        auto Shown() -> std::vector<ShownRow>;
        // Whether her own row at a 1-based place is pinned by her party role
        // (the role's row stands in its place): the role owns its content,
        // so it is never switched, rewritten or deleted -- the edits refuse
        // such a row themselves, whoever asks -- and the player owns its
        // order, so it moves as any of hers
        auto Locked(std::size_t index) const -> bool;
        // The party role whose rows run with hers now; None for no role
        auto LentBy() const -> cardian::party::Role;

        // Her tools, for her tactician: the id of the first enabled marked
        // row that lets her cast this spell on this target now -- the spell
        // is the row's, the target is one the row names, its retry has run,
        // and its conditions hold, with no timer spent. Nothing when no row
        // does, or her master switch is off. Only her tactician reads it:
        // a plain row casts as the order it is
        auto Admits(uint16 spell, CBattleEntity* PTarget) -> std::optional<std::string>;
        // Whether any enabled marked row names this spell, whoever it is
        // for and whatever its conditions: what her rest pacing may count
        // on having
        auto AllowsSpell(uint16 spell) const -> bool;

        // The rows that decide which fight she takes (engage_math.h), read
        // by the engage door: the enabled Attack rows in the running order
        // (the world's layer first while she is in the wild), and none while
        // the master switch is off (engage_math.h doorReads). Each is
        // numbered within its own layer, so one of her own carries the
        // number the editor shows it under. The think never runs them. A row
        // the editor would refuse (pairingError; only a hand-edited saved
        // set can hold one) is passed over, and so is one struck out. A
        // marked one is her tactician's melee (`below`, the name from the
        // line's day): the door reads it only while her tactician lets her
        // melee (tactician_line.h meleeAllowed). The pointers hold until her
        // rows, or her world layer, next change, so a reader uses them
        // within the tick it asked in.
        struct EngageRow
        {
            std::size_t              index  = 0;     // 1-based within its layer
            bool                     world  = false; // a row of the world's layer
            bool                     below  = false; // marked: her tactician's melee
            const gambits::Gambit_t* gambit = nullptr;
            bool                     lent   = false; // a row her party role lends (gambit_layers.h)
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
        // What her party role's layer was compiled for: the role, her main
        // job and her sub job, since a seat lends the tools her two jobs can
        // ever use (role_bundles.h)
        struct RoleKey
        {
            cardian::party::Role role;
            xi::Job              job;
            xi::Job              sub;
            auto                 operator==(const RoleKey&) const -> bool = default;
        };

        // The layers that run for her now (gambit_layers.h): her world rows
        // while she is in the wild, compiled on first need and again whenever
        // their key changes (world.h brainKey), then her own with her party
        // role's rows fitted in, compiled again whenever her role, her main
        // job or her sub job changes -- never as she levels: a seat lends
        // what her jobs can ever reach (role_bundles.h)
        auto RunningLayers() -> cardian::layers::Layers<GambitRow>;
        void RebuildWorldLayer();
        void RebuildRoleLayer(RoleKey key);
        auto Candidates(gambits::G_TARGET selector) -> std::vector<CBattleEntity*>;
        // Whether a target is one a row's selector names: Candidates as a
        // question about one entity, over her whole alliance for the party
        // selectors, a mob for `Target`
        auto Names(gambits::G_TARGET selector, const CBattleEntity* PTarget) const -> bool;
        auto SelectTarget(const gambits::Gambit_t& gambit) -> CBattleEntity*;
        // What her rows call "the mob": her battle target, else the party's
        // fight she attends or walks in on (RESEARCH §12.15)
        auto FightTarget() -> CBattleEntity*;
        // pending: a timer already passed, not spent again. The tactician's
        // mark holds wherever it is read (tactician_line.h)
        auto CheckTrigger(CBattleEntity* PTrigger, const gambits::Gambit_t& gambit, std::size_t groupIndex, bool pending = false) -> bool;
        // The states of the running layers' rows, in forEachRow's order:
        // the world's first, then her own and the lent rows as fitted
        auto RunningStates(const cardian::layers::Layers<GambitRow>& layers) const -> std::vector<cardian::tactician::State>;
        // Whether a marked row that runs names a tool of the kind wanted
        auto OffersAny(const std::function<bool(cardian::tactician::Allowance)>& wanted) const -> bool;
        // The answers that move only with her rows, kept per generation of
        // them; every edit, load and layer rebuild calls RowsChanged
        struct Offered
        {
            uint32 generation = 0;
            bool   tools      = false;
            bool   spells     = false;
            bool   nukes      = false;
        };
        auto Offers() const -> const Offered&;
        void RowsChanged();
        // Where her role's rows stand among hers (gambit_layers.h bindLent):
        // Rebind keeps each lent row on the row of hers it already stands
        // in, by that row's identifier, and binds the rest afresh; every
        // change to her rows calls it (RowsChanged), and a new role, or a
        // row appended as her rows load (AddGambit), starts it fresh.
        // RoleBinds reads the bindings as her rows' places now;
        // Fitted is her rows and the role's as they run (gambit_layers.h
        // place)
        void Rebind();
        auto RoleBinds() const -> std::vector<std::optional<std::size_t>>;
        auto Fitted() const -> std::vector<cardian::layers::Placed<const GambitRow>>;
        auto ResolveSpell(const gambits::Action_t& action, CBattleEntity* PTarget) -> Maybe<SpellID>;
        // Behaviour rows (G_REACTION_BEHAVIOR only) flip controller switches
        // and never consume the think; engage rows (Attack) are the door's
        // (EngageRows) and never consume it either
        auto IsBehavior(const gambits::Gambit_t& gambit) const -> bool;
        void ApplyBehavior(const gambits::Gambit_t& gambit);
        // A marked self buff's call where it sits in her think: its when
        // (tactician_line.h buffNow), the ability hers and off its recast
        auto BuffNow(const gambits::Gambit_t& gambit, bool engaged) const -> bool;
        // A marked Berserk or Defender row, its gate holding: the other
        // stance buff taken off, unless the player ordered it
        void KeepStance(const gambits::Gambit_t& gambit);
        // The nukes she can cast now (spell_bank.h isNuke)
        auto NukeSpells() -> std::vector<SpellID>;
        // Whether her Sneak Attack goes naked, worked out (NakedSneak)
        auto NakedSneakNow() -> bool;
        // Her tactician's nuke at a marked Damage spell (any) row (RESEARCH
        // §17.13, the Black Mage), on the foe its gate found, while she is in
        // the fight: never while the foe is on her -- she holds until it is
        // on someone else -- and never one dealing nothing; else the nuke
        // that deals the most a second of hers (bank_math.h pickNuke), cast,
        // a dying mob finished by the quickest that covers it. Every reason
        // she holds in a fight is said once, as it changes. Whether it went
        // out
        auto CastNuke(CBattleEntity* PTarget, bool engaged, std::size_t index) -> bool;

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
        // Her spell rows feed her scope's conveyor: a tactician watches it,
        // and she is a cardian (a played character's casts are his own)
        auto Conveyed() const -> bool;

        CCharEntity*      POwner;
        GambitHost*       m_host;
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

        // Her party role's layer (role_bundles.h): its rows (ids "r1",
        // "r2"... numbered on across rebuilds), the identifier of the row of
        // hers each stands in ("" for none: it runs at the top), the key
        // they were compiled for, and its own TIMER clocks
        std::vector<GambitRow>                  m_roleRows;
        std::vector<std::string>                m_roleBinds;
        std::optional<RoleKey>                  m_roleKey;
        uint32                                  m_rowsGeneration = 0; // moves with every change to her rows or her layers
        mutable std::optional<Offered>          m_offers;
        uint32                                  m_nextRoleId = 0;
        HashMap<std::string, timer::time_point> m_roleTimers;
        // The tank tactician's mind as last said, why its last call could
        // not go through (said once), and whether it has her think (said as
        // it changes)
        std::string                             m_tankMind;
        std::string                             m_tankRefusal;
        bool                                    m_tankOnDuty = false;
        // Why her nukes hold, as last said (said as it changes)
        std::string                             m_nukeHold;
        // NakedSneak's answer, as of her last think (NakedSneakNow)
        bool                                    m_nakedSneak = false;
    };
} // namespace pawn
