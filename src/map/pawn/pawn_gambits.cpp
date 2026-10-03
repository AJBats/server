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

#include "pawn_gambits.h"
#include "engage_math.h"
#include "gambit_text.h"
#include "pawn.h"
#include "pawn_controller.h"
#include "party_roster.h"
#include "role_bundles.h"
#include "tactics.h"
#include "spell_bank.h"

#include "utils/battleutils.h"
#include "spell.h"
#include "weapon_skill.h"

#include <algorithm>
#include <array>
#include <magic_enum/magic_enum.hpp>

#include "common/logging.h"
#include "common/settings.h"
#include "common/utils.h"
#include "common/xirand.h"

#include "ability.h"
#include "ai/ai_container.h"
#include "ai/states/ability_state.h"
#include "ai/states/magic_state.h"
#include "ai/states/mobskill_state.h"
#include "ai/states/petskill_state.h"
#include "ai/states/range_state.h"
#include "ai/states/weaponskill_state.h"
#include "enmity_container.h"
#include "entities/char_entity.h"
#include "entities/mob_entity.h"
#include "party.h"
#include "recast_container.h"
#include "status_effect.h"
#include "status_effect_container.h"
#include "utils/battleutils.h"
#include "utils/charutils.h"
#include "weapon_skill.h"

#include <algorithm>
#include <chrono>
#include <iterator>
#include <list>
#include <set>

// Spell families run past magic_enum's default range (127): read them all,
// or familyName says "family 130"
template <>
struct magic_enum::customize::enum_range<SPELLFAMILY>
{
    static constexpr int min = 0;
    static constexpr int max = 255;
};

using namespace gambits;

namespace pawn
{
    namespace
    {
        const std::set<xi::Job> kMeleeJobs = {
            xi::Job::WAR,
            xi::Job::MNK,
            xi::Job::THF,
            xi::Job::PLD,
            xi::Job::DRK,
            xi::Job::BST,
            xi::Job::SAM,
            xi::Job::NIN,
            xi::Job::DRG,
            xi::Job::BLU,
            xi::Job::PUP,
            xi::Job::DNC,
            xi::Job::RUN,
        };

        const std::set<xi::Job> kCasterJobs = {
            xi::Job::WHM,
            xi::Job::BLM,
            xi::Job::RDM,
            xi::Job::BRD,
            xi::Job::SMN,
            xi::Job::BLU,
            xi::Job::SCH,
            xi::Job::GEO,
            xi::Job::RUN,
        };

        static_assert(cardian::tactician::kHateAbilities[0] == ABILITY_PROVOKE);
        static_assert(cardian::tactician::kBerserk == ABILITY_BERSERK && cardian::tactician::kDefender == ABILITY_DEFENDER &&
                      cardian::tactician::kAggressor == ABILITY_AGGRESSOR && cardian::tactician::kFocus == ABILITY_FOCUS &&
                      cardian::tactician::kDodge == ABILITY_DODGE && cardian::tactician::kBoost == ABILITY_BOOST &&
                      cardian::tactician::kSneakAttack == ABILITY_SNEAK_ATTACK);
        static_assert(pawn::bundles::kProvoke == ABILITY_PROVOKE && pawn::bundles::kSpellCure == static_cast<uint16>(SpellID::Cure) &&
                      pawn::bundles::kSpellPoisona == static_cast<uint16>(SpellID::Poisona));

        // The levels a member's two jobs reach on this server, for the tools
        // her seat lends (role_bundles.h): the server's cap, and the sub
        // job's level under it by the server's sub job rule
        auto seatCaps() -> pawn::bundles::Caps
        {
            const auto cap = settings::get<uint8>("main.MAX_LEVEL");
            return { cap, pawn::bundles::subLevelAt(cap, settings::get<uint8>("map.SUBJOB_RATIO")) };
        }

        // The level a job learns a spell at, from the game's tables; 0 for
        // never
        auto spellLevel(const uint16 id, const xi::Job job) -> uint8
        {
            auto*       PSpell = spell::GetSpell(static_cast<SpellID>(id));
            const uint8 level  = PSpell != nullptr ? PSpell->getJob(job) : 0;
            return level == 255 ? 0 : level;
        }

        // The level a job learns a seat tool's ability or spell at -- for
        // Enfeeble, the first debuff her tactician prices that it learns --
        // from the game's tables; 0 for never
        auto toolLevel(const pawn::bundles::Need need, const xi::Job job) -> uint8
        {
            if (need.kind == pawn::bundles::Need::Kind::Ability)
            {
                auto* PAbility = ability::GetAbility(need.id);
                return PAbility != nullptr && PAbility->getJob() == job ? PAbility->getLevel() : 0;
            }
            if (need.kind == pawn::bundles::Need::Kind::Spell)
            {
                return spellLevel(need.id, job);
            }
            if (need.kind == pawn::bundles::Need::Kind::Enfeeble)
            {
                uint8 first = 0;
                for (const auto& debuff : cardian::tactician::kPricedDebuffs)
                {
                    const uint8 level = spellLevel(debuff.id, job);
                    if (level > 0 && (first == 0 || level < first))
                    {
                        first = level;
                    }
                }
                return first;
            }
            return 0;
        }

        // What each self buff puts on her: up, her tactician has nothing to
        // add (tactician_line.h buffNow)
        auto buffEffect(const uint32 ability) -> std::optional<xi::StatusEffect>
        {
            switch (ability)
            {
                case ABILITY_BERSERK:
                    return xi::StatusEffect::Berserk;
                case ABILITY_DEFENDER:
                    return xi::StatusEffect::Defender;
                case ABILITY_AGGRESSOR:
                    return xi::StatusEffect::Aggressor;
                case ABILITY_FOCUS:
                    return xi::StatusEffect::Focus;
                case ABILITY_DODGE:
                    return xi::StatusEffect::Dodge;
                default:
                    return std::nullopt;
            }
        }
        static_assert(cardian::tactician::kTargetEnemy == TARGET_ENEMY);
        static_assert(cardian::tactician::kTargetFriendly == (TARGET_SELF | TARGET_PLAYER_PARTY | TARGET_PLAYER_ALLIANCE | TARGET_PLAYER | TARGET_PLAYER_DEAD |
                                                              TARGET_PLAYER_PARTY_PIANISSIMO | TARGET_PET | TARGET_PLAYER_PARTY_ENTRUST));

        auto titleCase(std::string_view raw) -> std::string; // below, with the row words

        // A spell family's target flags: its first spell's, read once --
        // the spell table never changes while the map runs
        auto familyFlags(const uint32 family) -> uint16
        {
            static const auto flags = []
            {
                std::array<uint16, 1024> out{};
                for (uint16 id = 1; id < MAX_SPELL_ID; ++id)
                {
                    auto*      PSpell = spell::GetSpell(static_cast<SpellID>(id));
                    const auto f      = PSpell != nullptr ? static_cast<uint32>(PSpell->getSpellFamily()) : 0;
                    if (f != 0 && f < out.size() && out[f] == 0)
                    {
                        out[f] = PSpell->getValidTarget();
                    }
                }
                return out;
            }();
            return family < flags.size() ? flags[family] : 0;
        }

        // What an action may be aimed at, as its spell, ability or kind says:
        // 0 when it names nothing fixed (a behaviour, Entrust, an item)
        auto actionFlags(const Action_t& a) -> uint16
        {
            switch (a.reaction)
            {
                case G_REACTION::ATTACK:
                case G_REACTION::RATTACK:
                case G_REACTION::WS:
                    return TARGET_ENEMY;
                case G_REACTION::MA:
                    switch (a.select)
                    {
                        case G_SELECT::SPECIFIC:
                        {
                            auto* PSpell = spell::GetSpell(static_cast<SpellID>(a.select_arg));
                            return PSpell != nullptr ? PSpell->getValidTarget() : 0;
                        }
                        case G_SELECT::HIGHEST:
                        case G_SELECT::LOWEST:
                            return familyFlags(a.select_arg);
                        case G_SELECT::RANDOM:
                        case G_SELECT::MB_ELEMENT:
                        case G_SELECT::BEST_AGAINST_TARGET:
                            return TARGET_ENEMY;
                        case G_SELECT::BEST_INDI:
                            return TARGET_SELF;
                        case pawn::G_SELECT_ENFEEBLE:
                            return TARGET_ENEMY;
                        default:
                            return 0;
                    }
                case G_REACTION::JA:
                {
                    auto* PAbility = a.select == G_SELECT::SPECIFIC ? ability::GetAbility(static_cast<uint16>(a.select_arg)) : nullptr;
                    return PAbility != nullptr ? PAbility->getValidTarget() : 0;
                }
                default:
                    return 0;
            }
        }

        // Whether every action of a row can be aimed at the side its
        // condition names (tactician_line.h fitsSide): a misfit is kept,
        // struck out, and does nothing. Attack is the door's, which reads a
        // Foe condition alone: on any other it never acts
        auto rowFits(const Gambit_t& g) -> bool
        {
            if (cardian::engage::isEngageRow(g) && !cardian::engage::isFoeTarget(g.target_selector))
            {
                return false;
            }
            return std::ranges::all_of(g.actions, [&g](const Action_t& a)
                                       {
                                           const auto flags      = actionFlags(a);
                                           const bool onHerFight = a.reaction == G_REACTION::WS || (a.reaction == G_REACTION::JA && (flags & TARGET_ENEMY) != 0);
                                           return cardian::tactician::fitsSide(g.target_selector, flags, onHerFight);
                                       });
        }

        auto resonanceOf(const CStatusEffect* PSCEffect) -> std::list<SKILLCHAIN_ELEMENT>
        {
            std::list<SKILLCHAIN_ELEMENT> resonance;
            if (const uint16 power = PSCEffect->GetPower())
            {
                resonance.emplace_back(static_cast<SKILLCHAIN_ELEMENT>(power & 0xF));
                resonance.emplace_back(static_cast<SKILLCHAIN_ELEMENT>(power >> 4 & 0xF));
                resonance.emplace_back(static_cast<SKILLCHAIN_ELEMENT>(power >> 8));
            }
            return resonance;
        }

        auto propertiesOf(const TrustSkill_t& skill) -> std::list<SKILLCHAIN_ELEMENT>
        {
            return {
                static_cast<SKILLCHAIN_ELEMENT>(skill.primary),
                static_cast<SKILLCHAIN_ELEMENT>(skill.secondary),
                static_cast<SKILLCHAIN_ELEMENT>(skill.tertiary),
            };
        }

        // A skillchain window a closer can still hit
        auto openWindow(const CBattleEntity* PTarget) -> CStatusEffect*
        {
            auto* PSCEffect = PTarget->StatusEffectContainer->GetStatusEffect(xi::StatusEffect::Skillchain);
            if (PSCEffect && PSCEffect->GetStartTime() + 3s < timer::now())
            {
                return PSCEffect;
            }
            return nullptr;
        }

        auto barSpellFor(const uint32 element) -> Maybe<SpellID>
        {
            switch (element)
            {
                case ELEMENT_FIRE:
                    return SpellID::Barfire;
                case ELEMENT_ICE:
                    return SpellID::Barblizzard;
                case ELEMENT_WIND:
                    return SpellID::Baraero;
                case ELEMENT_EARTH:
                    return SpellID::Barstone;
                case ELEMENT_THUNDER:
                    return SpellID::Barthunder;
                case ELEMENT_WATER:
                    return SpellID::Barwater;
                default:
                    return std::nullopt;
            }
        }

        auto elementOfCast(const CBattleEntity* PEntity) -> Maybe<uint16>
        {
            if (!PEntity->PAI->IsCurrentState<CMagicState>())
            {
                return std::nullopt;
            }
            return static_cast<CMagicState*>(PEntity->PAI->GetCurrentState())->GetSpell()->getElement();
        }

        auto isElemental(const uint16 element) -> bool
        {
            return element >= ELEMENT_FIRE && element <= ELEMENT_WATER;
        }

        // What is on someone, as ailments.h asks it
        auto effectsOn(CBattleEntity* PEntity)
        {
            return [PEntity](const uint16 effect)
            {
                return PEntity->StatusEffectContainer->HasStatusEffect(static_cast<xi::StatusEffect>(effect));
            };
        }

        // An erasable effect Erase can take off her: one that runs out, as
        // CStatusEffectContainer::EraseStatusEffect takes only those
        auto erasableOn(CBattleEntity* PEntity) -> bool
        {
            bool found = false;
            PEntity->StatusEffectContainer->ForEachEffect([&found](CStatusEffect& effect)
                                                          {
                                                              found = found || (effect.HasEffectFlag(xi::StatusEffectFlag::Erasable) && effect.GetDuration() > 0s && !effect.isDeleted());
                                                          });
            return found;
        }

        // The row's spell, when it takes ailments off: -na (best), a -na or
        // Erase. Only the first spell of a row can start a think (Execute)
        auto removalOf(const Gambit_t& g) -> const Action_t*
        {
            const auto spell = std::ranges::find(g.actions, G_REACTION::MA, &Action_t::reaction);
            if (spell == g.actions.end())
            {
                return nullptr;
            }
            return cardian::tactician::isRemovalAction(*spell) ? &*spell : nullptr;
        }

        // An enfeeble an Enfeeble order may cast on the foe: not on it
        // already, not nullified by what is on it, and not one it is immune
        // to (the bank's verdicts, the ones her tactician's pricing asks)
        auto enfeebleLands(CSpell* PSpell, CBattleEntity* PTarget) -> bool
        {
            return !pawn::tactics::bank::onAlready(PSpell, PTarget).has_value() && !pawn::tactics::bank::blockedOn(PSpell, PTarget) &&
                   !pawn::tactics::bank::immuneTo(PSpell, PTarget);
        }

        // A spell a row picked by what is on the target still answers it:
        // a -na or Erase still has something to take off, an enfeeble is not
        // on already and nothing another caster landed since blocks it. The
        // conveyor asks every tick before it casts a request a row fed, so
        // only what can change in a request's short life is asked; the row's
        // own pick weighed the rest (an immunity never changes)
        auto stillAnswers(const Action_t& action, const uint16 spellId, CBattleEntity* PTarget) -> bool
        {
            if (cardian::tactician::isRemovalAction(action))
            {
                return cardian::ailments::cures(spellId, effectsOn(PTarget), spellId == cardian::ailments::kErase && erasableOn(PTarget));
            }
            if (action.select == pawn::G_SELECT_ENFEEBLE)
            {
                auto* PSpell = spell::GetSpell(static_cast<SpellID>(spellId));
                return !pawn::tactics::bank::onAlready(PSpell, PTarget).has_value() && !pawn::tactics::bank::blockedOn(PSpell, PTarget);
            }
            return true;
        }
    } // namespace

    // A played character's gambits start off: his set is his to build
    CGambits::CGambits(CCharEntity* PChar, GambitHost* PHost)
    : POwner(PChar)
    , m_host(PHost)
    , m_spellBook(PChar)
    , m_masterOn(!PHost->OwnClient())
    {
    }

    auto CGambits::Conveyed() const -> bool
    {
        return !m_host->OwnClient() && pawn::tactics::has(POwner);
    }

    auto CGambits::AddGambit(Gambit_t gambit, const bool enabled) -> std::string
    {
        gambit.identifier = fmt::format("{}", ++m_nextId);
        gambit.last_used  = {};
        m_gambits.push_back(GambitRow{ std::move(gambit), enabled });
        // An append is how her rows load, one at a time: her role's rows bind
        // afresh over the list so far, so a load ends as a fresh fit would
        // (a row naming the same target first), not on whichever match came
        // in first. The editor's edits keep the bindings
        m_roleBinds.clear();
        RowsChanged();
        return m_gambits.back().gambit.identifier;
    }

    void CGambits::RemoveGambit(const std::string& id)
    {
        std::erase_if(m_gambits, [&id](const GambitRow& row)
                      {
                          return row.gambit.identifier == id;
                      });

        const auto prefix = fmt::format("{}:", id);
        std::erase_if(m_timerConditionLastTrigger, [&](const auto& kv)
                      {
                          return kv.first.rfind(prefix, 0) == 0;
                      });
        RowsChanged();
    }

    void CGambits::RemoveAllGambits()
    {
        // Her own rows and their clocks; the world's layer is its own
        m_gambits.clear();
        m_timerConditionLastTrigger.clear();
        RowsChanged();
    }

    namespace
    {
        auto gambitOfRow(const GambitRow& row) -> const Gambit_t&
        {
            return row.gambit;
        }

        auto enabledOfRow(const GambitRow& row) -> bool
        {
            return row.enabled;
        }
    } // namespace

    auto CGambits::RunningLayers() -> cardian::layers::Layers<GambitRow>
    {
        // The world's layer runs for one of the world's own out in the wild,
        // and for nobody else: with a player, or held by his contract, she
        // runs her own rows alone
        const bool wild = m_host->IsWorld() && pawn::world::inTheWild(POwner->id);
        if (wild && (!m_worldKey.has_value() || *m_worldKey != pawn::world::brainKey(POwner)))
        {
            RebuildWorldLayer();
        }
        // Her party role's rows run with a player and never in the wild. Her
        // sub job as she has set it, a zone's restriction of it aside: the
        // rows stay put, and a restricted tool is never usable anyway. A
        // played character's seat lends him nothing yet: a bundle is marked
        // rows, and no tactician runs for him
        const auto role = wild || m_host->OwnClient() ? cardian::party::Role::None : pawn::roster::roleOf(POwner);
        const RoleKey key{ role, POwner->GetMJob(), POwner->GetSJob(true) };
        if (!m_roleKey.has_value() || *m_roleKey != key)
        {
            RebuildRoleLayer(key);
        }
        const auto binds = RoleBinds();
        return { wild ? std::span<GambitRow>(m_worldRows) : std::span<GambitRow>{},
                 cardian::layers::place<GambitRow>(m_gambits, m_roleRows, binds, enabledOfRow) };
    }

    void CGambits::RebuildRoleLayer(const RoleKey key)
    {
        // A new role binds afresh: the first of her rows that means the same
        // as each of its rows, in her order as it stands now
        const bool had  = !m_roleRows.empty();
        const auto role = key.role;
        m_roleKey       = key;
        m_roleRows.clear();
        m_roleBinds.clear();
        m_roleTimers.clear();
        for (const auto& [spec, enabled] : pawn::bundles::bundleFor(role, key.job, key.sub, seatCaps(), toolLevel))
        {
            if (auto row = pawn::text::parseRow(spec); row.has_value())
            {
                row->identifier = cardian::layers::roleRowId(++m_nextRoleId);
                row->last_used  = {};
                m_roleRows.push_back(GambitRow{ std::move(*row), enabled });
            }
            else
            {
                ShowErrorFmt("pawn: malformed role row '{}' for {}", spec, POwner->getName());
            }
        }
        RowsChanged();
        if (!m_roleRows.empty())
        {
            ShowInfoFmt("pawn: {} runs the {} role's {} rows with her own", POwner->getName(), cardian::party::roleName(role), m_roleRows.size());
        }
        else if (had)
        {
            ShowInfoFmt("pawn: {} runs her own rows alone again", POwner->getName());
        }
    }

    void CGambits::RebuildWorldLayer()
    {
        // The rows first: the first reader of all reads the file, which moves
        // the generation the key records
        const auto specs = pawn::world::brainRows(POwner);
        m_worldKey       = pawn::world::brainKey(POwner);
        m_worldRows.clear();
        m_worldTimers.clear();
        RowsChanged();
        for (const auto& spec : specs)
        {
            if (auto row = pawn::text::parseRow(spec); row.has_value())
            {
                row->identifier = cardian::layers::worldRowId(++m_nextWorldId);
                row->last_used  = {};
                m_worldRows.push_back(GambitRow{ std::move(*row), true });
            }
            else
            {
                ShowErrorFmt("pawn: malformed world row '{}' for {}", spec, POwner->getName());
            }
        }
        // Her own rows may not be loaded yet (her first tick reads the layer
        // before it loads them), so the line counts the world's alone
        ShowInfoFmt("pawn: {} runs the world's layer ({} rows: the world's, the {}'s, the {}'s) ahead of her own", POwner->getName(), m_worldRows.size(),
                    magic_enum::enum_name(POwner->GetMJob()), m_worldKey->role);
    }

    void CGambits::Tick(const timer::time_point tick, const bool engaged)
    {
        TracyZoneScoped;

        // Between fights her nuke rows are never asked (offensive rows wait
        // for an engagement), so why she held is forgotten here: the next
        // fight's first hold is said even if it is the same
        if (!engaged)
        {
            m_nukeHold.clear();
        }

        // The pacer (CPawnController::ReadyToAct): her think waits until the
        // server would take a new action from her, and thinks the first tick
        // it would -- an ability's animation does not hold her once it has
        // landed; a spell, a weapon skill, a shot, an item, a stun do
        if (!m_host->ReadyToAct())
        {
            return;
        }

        // The player's order from the command window is her next action:
        // nothing of the engine's goes ahead of it
        if (m_host->HasQueuedOrder())
        {
            return;
        }

        // The master switch: nothing of her own. A weapon skill, like
        // everything else, is a row's doing -- there is no TP trigger
        // behind the list
        if (!m_masterOn)
        {
            return;
        }

        // Retreat: nothing of her own until it is lifted, cures included --
        // she runs. The player's own orders are his (FireQueuedOrder)
        if (m_host->IsRetreating())
        {
            return;
        }

        // Her scope's conveyor (RESEARCH §12.12 item 2): where a tactician
        // watches, spell rows feed it and it says who casts; where none
        // does, rows cast as they always have. With her gambits on, a
        // marked row under a conveyor is her tactician running
        // (CPawnController::TacticianRuns); the tank tactician is the Tank
        // seat's (RESEARCH §17.13: the seat is what she is for). A played
        // character's rows cast for him: the conveyor watches his casts,
        // never assigns them
        const bool conveyor = Conveyed();
        const bool spells   = conveyor && pawn::tactics::offersSpells(POwner);
        const bool tank     = conveyor && pawn::roster::roleOf(POwner) == cardian::party::Role::Tank;
        if (tank != m_tankOnDuty)
        {
            m_tankOnDuty = tank;
            ShowInfoFmt("tactics: {}'s tank tactician {}", POwner->getName(), tank ? "takes her think" : "stands down");
        }

        // The tactician's own needs for the spells she offers, on her
        // staggered think (below; the pricing is not every tick's), fed
        // before her assignment is read, so a need that think finds -- a
        // fight's first debuff (her Burn ahead of her first nuke), a cure --
        // is hers that think, not the next
        const auto positionOffset = std::chrono::milliseconds(m_host->PartyPosition() * 100);
        const bool thinkDue       = tick + positionOffset >= m_lastAction;
        if (spells && thinkDue)
        {
            pawn::tactics::roleThink(POwner, engaged);
        }

        // The shared party tick measures and assigns emergency aid before
        // ordinary needs. Each mage reads that same decision here.
        if (conveyor && CastAssignment(engaged))
        {
            return;
        }

        // The tank tactician's call (RESEARCH §17.11): Provoke on the pull,
        // on its clock in the fight, held for the next pull at the end.
        // Ahead of the stagger, as the emergency cure is: the pull waits
        // for nobody. Asked only while she can act at all (the emergency
        // cure's own gate): a call the server would refuse -- mid-item,
        // stunned, asleep -- is not made, so nothing is pushed at her
        if (tank && m_host->RestAllowsAction() && m_host->ReadyToAct())
        {
            if (const auto call = pawn::tactics::tankCall(POwner, engaged); call.has_value())
            {
                // Its mind, said as it changes -- not the clock ticking,
                // which is every fight's rhythm (quiet)
                if (!call->quiet && call->mind != m_tankMind)
                {
                    m_tankMind = call->mind;
                    ShowInfoFmt("tactics: {}'s tank tactician: {}", POwner->getName(), call->mind);
                }
                if (call->mob.has_value())
                {
                    auto*      PMob    = pawn::tactics::entity(POwner, *call->mob);
                    const auto refused = PMob != nullptr ? UseHateTool(ABILITY_PROVOKE, PMob, call->why) : std::optional<std::string>("the mob is gone");
                    if (!refused.has_value())
                    {
                        m_tankRefusal.clear();
                        return;
                    }
                    // Said once, not every tick: the same refusal of the
                    // same call
                    const auto line = fmt::format("cannot Provoke {} ({}): {}", PMob != nullptr ? PMob->getName() : "?", call->why, *refused);
                    if (m_tankRefusal != line)
                    {
                        m_tankRefusal = line;
                        ShowInfoFmt("tactics: {} {}", POwner->getName(), line);
                    }
                }
                else
                {
                    m_tankRefusal.clear();
                }
            }
        }

        // Stagger pawns so a party doesn't think in lockstep
        if (!thinkDue)
        {
            return;
        }

        m_spellBook.Refresh();
        RefreshWeaponSkills();
        m_nakedSneak = NakedSneakNow();

        m_lastAction = tick + std::chrono::milliseconds(xirand::GetRandomNumber(2000, 3000));

        // Her rows in the running order (the world's first in the wild),
        // top down; the first to act ends the think. An order acts, and so
        // does a marked -na or Erase row while her tactician runs, which
        // has no judgement of its own for them yet, and a marked self buff
        // whose when says now (BuffNow); every other marked row is her
        // tactician's to read, and a struck-out row does nothing
        // (tactician_line.h actsAlone)
        const auto layers = RunningLayers();
        const auto states = RunningStates(layers);
        cardian::layers::forEachRow(layers, [&](GambitRow& row, const std::size_t index, const bool on)
                                    {
                                        auto&      gambit = row.gambit;
                                        const auto state  = states[index - 1];
                                        if (on && state == cardian::tactician::State::Tool)
                                        {
                                            KeepStance(gambit);
                                        }
                                        const bool buff = on && state == cardian::tactician::State::Tool && BuffNow(gambit, engaged);
                                        // A marked Damage spell (any) row, her nukes, is her tactician's to cast where it sits (CastNuke)
                                        const bool nuke = on && state == cardian::tactician::State::Tool && cardian::tactician::allowanceOf(gambit) == cardian::tactician::Allowance::Nuke;
                                        if (!on || !(cardian::tactician::actsAlone(state, gambit, spells || tank) || buff || nuke) || IsBehavior(gambit) ||
                                            cardian::engage::isEngageRow(gambit) || tick < gambit.last_used + std::chrono::seconds(gambit.retry_delay))
                                        {
                                            return false;
                                        }

                                        if (!engaged && IsOffensive(gambit))
                                        {
                                            return false;
                                        }

                                        CBattleEntity* PTarget = SelectTarget(gambit);
                                        if (PTarget == nullptr)
                                        {
                                            return false;
                                        }

                                        if (!(nuke ? CastNuke(PTarget, engaged, index) : Execute(gambit, PTarget, engaged, index)))
                                        {
                                            return false;
                                        }
                                        if (buff)
                                        {
                                            auto* PAbility = ability::GetAbility(static_cast<uint16>(gambit.actions.front().select_arg));
                                            ShowInfoFmt("tactics: {} uses {} (row {}: fighting, not up, ready)", POwner->getName(),
                                                        PAbility != nullptr ? titleCase(PAbility->getName()) : "?", index);
                                        }
                                        if (gambit.retry_delay != 0)
                                        {
                                            gambit.last_used = tick;
                                        }
                                        return true;
                                    });
    }

    auto CGambits::CastAssignment(const bool engaged) -> bool
    {
        const auto a = pawn::tactics::assignment(POwner, engaged);
        if (a.has_value() && a->emergency && (!m_host->RestAllowsAction() || m_host->Acting() ||
            !m_host->CanAct() || !pawn::tactics::bank::usable(POwner, a->spell)))
        {
            return true; // reserve this slot without sending a failed cast every tick
        }
        return a.has_value() && (CastAssigned(a->spell, a->target, a->why) || a->emergency);
    }

    auto CGambits::CastAssigned(const SpellID spellId, const uint32 target, const std::string& why) -> bool
    {
        auto* PTarget = pawn::tactics::entity(POwner, target);
        if (PTarget == nullptr || !m_host->CastAssigned(PTarget->entityId(), spellId))
        {
            return false;
        }
        auto* PSpell = spell::GetSpell(spellId);
        ShowInfoFmt("tactics: {} casts {} on {} ({})", POwner->getName(), PSpell != nullptr ? PSpell->getName() : "?", PTarget->getName(), why);
        return true;
    }

    namespace
    {
        auto rowIdOf(const GambitRow& row) -> const std::string&
        {
            return row.gambit.identifier;
        }
    } // namespace

    void CGambits::StampRetry(const std::string& id, const timer::time_point at)
    {
        if (auto* row = cardian::layers::findRow(RunningLayers(), id, rowIdOf); row != nullptr && row->gambit.retry_delay != 0)
        {
            row->gambit.last_used = at;
        }
    }

    auto CGambits::RequestValid(const std::string& rowId, const uint32 target, const uint16 spellId) -> bool
    {
        if (!m_masterOn)
        {
            return false;
        }
        // A request a row fed is one that acts alone (tactician_line.h
        // actsAlone): a row since marked for the tactician, or struck out,
        // no longer asks
        const auto layers = RunningLayers();
        const auto* row   = cardian::layers::findRow(layers, rowId, rowIdOf);
        if (row == nullptr || IsBehavior(row->gambit))
        {
            return false;
        }
        // Its state where it sits in the running order, and whether it runs
        const auto                               states = RunningStates(layers);
        std::optional<cardian::tactician::State> found;
        bool                                     runs = false;
        cardian::layers::forEachRow(layers, [&](const GambitRow& r, const std::size_t place, const bool on)
                                    {
                                        if (&r == row)
                                        {
                                            found = states[place - 1];
                                            runs  = on;
                                        }
                                        return found.has_value();
                                    });
        if (!runs)
        {
            return false;
        }
        const auto state = found.value_or(cardian::tactician::State::Order);
        if (!cardian::tactician::actsAlone(state, row->gambit, m_host->TacticianRuns()))
        {
            return false;
        }
        const auto& g = row->gambit;
        const auto action = std::find_if(g.actions.begin(), g.actions.end(), [](const auto& a) { return a.reaction == G_REACTION::MA; });
        if (action == g.actions.end() ||
            (action->select == G_SELECT::SPECIFIC && action->select_arg != spellId))
        {
            return false;
        }
        for (auto* candidate : Candidates(g.target_selector))
        {
            auto* castTarget = candidate;
            if (g.target_selector == G_TARGET::TRIGGER_SELF_ACTION_TARGET)
            {
                castTarget = FightTarget();
            }
            else if (g.target_selector == G_TARGET::TRIGGER_TARGET_ACTION_SELF)
            {
                castTarget = POwner;
            }
            if (action->select == G_SELECT::ENTRUSTED)
            {
                castTarget = m_host->GetLivePlayer();
            }
            if (const auto* spell = spellId != 0 ? spell::GetSpell(static_cast<SpellID>(spellId)) : nullptr;
                spell != nullptr && spell->getValidTarget() == TARGET_SELF)
            {
                castTarget = POwner;
            }
            if (castTarget == nullptr || castTarget->id != target)
            {
                continue;
            }
            bool matches = true;
            for (std::size_t group = 0; group < g.predicate_groups.size(); ++group)
            {
                if (!CheckTrigger(candidate, g, group, true))
                {
                    matches = false;
                    break;
                }
            }
            if (matches && stillAnswers(*action, spellId, castTarget))
            {
                return true;
            }
        }
        return false;
    }

    auto CGambits::Candidates(const G_TARGET selector) -> std::vector<CBattleEntity*>
    {
        std::vector<CBattleEntity*> out;

        auto partyAlive = [this](CBattleEntity* PMember)
        {
            // Selection describes the row's target, not its casting range.
            // The requested action handles reach and walking into range.
            return PMember != nullptr && PMember->isAlive() && PMember->loc.zone == POwner->loc.zone;
        };

        auto collect = [&](auto&& accept)
        {
            POwner->ForParty([&](CBattleEntity* PMember)
                             {
                                 if (partyAlive(PMember) && accept(PMember))
                                 {
                                     out.push_back(PMember);
                                 }
                             });

            std::stable_sort(out.begin(), out.end(), [](CBattleEntity* a, CBattleEntity* b)
                             {
                                 return a->GetHPP() < b->GetHPP();
                             });
        };

        switch (selector)
        {
            case G_TARGET::SELF:
            case G_TARGET::TRIGGER_SELF_ACTION_TARGET:
            {
                out.push_back(POwner);
                break;
            }
            case G_TARGET::TARGET:
            case G_TARGET::TRIGGER_TARGET_ACTION_SELF:
            {
                if (auto* PMob = FightTarget())
                {
                    out.push_back(PMob);
                }
                break;
            }
            case G_TARGET::MASTER:
            {
                if (auto* PPlayer = m_host->GetLivePlayer())
                {
                    out.push_back(PPlayer);
                }
                break;
            }
            case G_TARGET::PARTY:
            {
                collect([](const CBattleEntity*)
                        {
                            return true;
                        });
                break;
            }
            case G_TARGET::PARTY_DEAD:
            {
                POwner->ForParty([&](CBattleEntity* PMember)
                                 {
                                     if (PMember != nullptr && PMember->isDead() && PMember->loc.zone == POwner->loc.zone)
                                     {
                                         out.push_back(PMember);
                                     }
                                 });
                break;
            }
            case G_TARGET::TANK:
            {
                collect([](const CBattleEntity* PMember)
                        {
                            return PMember->GetMJob() == xi::Job::PLD || PMember->GetMJob() == xi::Job::RUN;
                        });
                break;
            }
            case G_TARGET::MELEE:
            {
                collect([](const CBattleEntity* PMember)
                        {
                            return kMeleeJobs.contains(PMember->GetMJob());
                        });
                break;
            }
            case G_TARGET::RANGED:
            {
                collect([](const CBattleEntity* PMember)
                        {
                            return PMember->GetMJob() == xi::Job::RNG || PMember->GetMJob() == xi::Job::COR;
                        });
                break;
            }
            case G_TARGET::CASTER:
            {
                collect([](const CBattleEntity* PMember)
                        {
                            return kCasterJobs.contains(PMember->GetMJob());
                        });
                break;
            }
            case G_TARGET::TOP_ENMITY:
            {
                if (const auto* PTop = m_host->GetTopEnmity())
                {
                    collect([PTop](const CBattleEntity* PMember)
                            {
                                return PMember == PTop;
                            });
                }
                break;
            }
            default:
            {
                // A finder with any action but Attack (the door's) names her
                // fight, when her fight is of its kind. CURILLA is a trust NPC
                // special; PARTY_MULTI is unimplemented upstream too
                if (const auto finder = cardian::engage::finderOf(selector); finder.has_value())
                {
                    if (auto* PMob = FightTarget(); m_host->FoeOfKind(*finder, PMob))
                    {
                        out.push_back(PMob);
                    }
                }
                break;
            }
        }

        return out;
    }

    auto CGambits::Names(const G_TARGET selector, const CBattleEntity* PTarget) const -> bool
    {
        if (PTarget == nullptr || PTarget->isDead())
        {
            return false;
        }
        // Her tactician asks only about its scope, her alliance, so a party
        // selector names any character alive in it
        const bool member = PTarget->objtype == TYPE_PC;
        switch (selector)
        {
            case G_TARGET::SELF:
                return PTarget == POwner;
            case G_TARGET::TARGET:
                return PTarget->objtype == TYPE_MOB;
            case G_TARGET::MASTER:
                return PTarget == m_host->GetLivePlayer();
            case G_TARGET::PARTY:
                return member;
            case G_TARGET::TANK:
                return member && (PTarget->GetMJob() == xi::Job::PLD || PTarget->GetMJob() == xi::Job::RUN);
            case G_TARGET::MELEE:
                return member && kMeleeJobs.contains(PTarget->GetMJob());
            case G_TARGET::RANGED:
                return member && (PTarget->GetMJob() == xi::Job::RNG || PTarget->GetMJob() == xi::Job::COR);
            case G_TARGET::CASTER:
                return member && kCasterJobs.contains(PTarget->GetMJob());
            case G_TARGET::TOP_ENMITY:
                return member && PTarget == m_host->GetTopEnmity();
            default:
            {
                // A finder names a mob of its kind (FoeOfKind reads, never writes)
                const auto finder = cardian::engage::finderOf(selector);
                return finder.has_value() && m_host->FoeOfKind(*finder, const_cast<CBattleEntity*>(PTarget));
            }
        }
    }

    auto CGambits::SelectTarget(const Gambit_t& gambit) -> CBattleEntity*
    {
        // A -na or Erase row picks someone it has a cure for: under a
        // condition many hold (a marked row, Enfeeble), the most hurt
        // may carry nothing she can take off while another does, or another
        // mage may be taking it off already. It is asked of whom the spell
        // lands on, and before the row's conditions, so a candidate passed
        // over never spends the row's timer
        const auto* removal = removalOf(gambit);
        for (auto* PCandidate : Candidates(gambit.target_selector))
        {
            CBattleEntity* PActionTarget = PCandidate;
            switch (gambit.target_selector)
            {
                case G_TARGET::TRIGGER_SELF_ACTION_TARGET:
                    PActionTarget = FightTarget();
                    break;
                case G_TARGET::TRIGGER_TARGET_ACTION_SELF:
                    PActionTarget = POwner;
                    break;
                default:
                    break;
            }

            if (removal != nullptr)
            {
                const auto spell = ResolveSpell(*removal, PActionTarget);
                if (!spell.has_value() || pawn::tactics::othersCasting(POwner, spell::GetSpell(*spell), PActionTarget))
                {
                    continue;
                }
            }

            bool matches = true;
            for (std::size_t groupIndex = 0; groupIndex < gambit.predicate_groups.size(); ++groupIndex)
            {
                if (!CheckTrigger(PCandidate, gambit, groupIndex))
                {
                    matches = false;
                    break;
                }
            }

            if (matches)
            {
                return PActionTarget;
            }
        }
        return nullptr;
    }

    void CGambits::Prompt()
    {
        m_lastAction = timer::time_point::min();
    }

    auto CGambits::FightTarget() -> CBattleEntity*
    {
        if (auto* PMob = POwner->GetBattleTarget(); PMob != nullptr)
        {
            return PMob;
        }
        return m_host->PartyFightTarget();
    }

    namespace
    {
        // "Sleep" is asleep: Sleep II and Lullaby answer the same row -- the
        // three a wake-up removes together (CLuaBaseEntity::wakeUp, which a
        // Cure calls) -- so a brain needs one row for all (the user,
        // 2026-09-14). Nightmare is Sleep itself, a tier above. Enfeeble is
        // a group: any ailment a -na cures, or an effect Erase takes
        // (ailments.h)
        auto hasStatus(CBattleEntity* PEntity, const uint32 arg) -> bool
        {
            if (arg == pawn::G_STATUS_ENFEEBLE)
            {
                return cardian::ailments::enfeebled(effectsOn(PEntity), erasableOn(PEntity));
            }
            const auto effect = static_cast<xi::StatusEffect>(arg);
            if (effect == xi::StatusEffect::SleepI)
            {
                return PEntity->StatusEffectContainer->HasStatusEffect({ xi::StatusEffect::SleepI, xi::StatusEffect::SleepIi, xi::StatusEffect::Lullaby });
            }
            return PEntity->StatusEffectContainer->HasStatusEffect(effect);
        }
    } // namespace

    auto CGambits::CheckTrigger(CBattleEntity* PTrigger, const Gambit_t& gambit, const std::size_t groupIndex, const bool pending) -> bool
    {
        TracyZoneScoped;

        const auto&       group = gambit.predicate_groups[groupIndex];
        std::vector<bool> results;
        results.reserve(group.predicates.size());

        for (std::size_t predicateIndex = 0; predicateIndex < group.predicates.size(); ++predicateIndex)
        {
            const auto& predicate = group.predicates[predicateIndex];
            const auto  arg       = predicate.condition_arg;

            switch (predicate.condition)
            {
                case G_CONDITION::ALWAYS:
                    results.push_back(true);
                    break;
                case G_CONDITION::HPP_LT:
                    results.push_back(PTrigger->GetHPP() < arg);
                    break;
                case G_CONDITION::HPP_GTE:
                    results.push_back(PTrigger->GetHPP() >= arg);
                    break;
                case G_CONDITION::MPP_LT:
                    results.push_back(PTrigger->GetMPP() < arg);
                    break;
                case G_CONDITION::MPP_GTE:
                    results.push_back(PTrigger->GetMPP() >= arg);
                    break;
                case G_CONDITION::TP_LT:
                    results.push_back(PTrigger->health.tp < static_cast<int16>(arg));
                    break;
                case G_CONDITION::TP_GTE:
                    results.push_back(PTrigger->health.tp >= static_cast<int16>(arg));
                    break;
                case G_CONDITION::LVL_LT:
                    results.push_back(PTrigger->GetMLevel() < arg);
                    break;
                case G_CONDITION::LVL_GTE:
                    results.push_back(PTrigger->GetMLevel() >= arg);
                    break;
                case G_CONDITION::STATUS:
                    results.push_back(hasStatus(PTrigger, arg));
                    break;
                case G_CONDITION::NOT_STATUS:
                    results.push_back(!hasStatus(PTrigger, arg));
                    break;
                case G_CONDITION::STATUS_FLAG:
                    results.push_back(PTrigger->StatusEffectContainer->HasStatusEffectByFlag(static_cast<xi::StatusEffectFlag>(arg)));
                    break;
                case G_CONDITION::TIMER:
                {
                    if (pending)
                    {
                        results.push_back(true); // this request already passed its timer
                        break;
                    }
                    if (arg == 0)
                    {
                        results.push_back(true);
                        break;
                    }

                    const auto key      = fmt::format("{}:{}:{}", gambit.identifier, groupIndex, predicateIndex);
                    const auto interval = std::chrono::seconds(arg);
                    const auto now      = timer::now();

                    // A world row keeps its clock in the world layer's map, a lent row in the role layer's
                    auto& timers        = cardian::layers::isWorldRowId(gambit.identifier) ? m_worldTimers
                                          : cardian::layers::isRoleRowId(gambit.identifier) ? m_roleTimers
                                                                                            : m_timerConditionLastTrigger;
                    auto [it, inserted] = timers.try_emplace(key, now);
                    if (inserted)
                    {
                        results.push_back(true);
                    }
                    else if (now - it->second >= interval)
                    {
                        it->second = now;
                        results.push_back(true);
                    }
                    else
                    {
                        results.push_back(false);
                    }
                    break;
                }
                case G_CONDITION::JA_ON_COOLDOWN:
                {
                    // HasRecast reads the clock (an ability's entry outlives its
                    // recast), and judges a charge ability by its recast time,
                    // as upstream's own Ability does
                    const auto* PAbility = ability::GetAbility(static_cast<uint16>(arg));
                    results.push_back(PAbility != nullptr && POwner->PRecastContainer->HasRecast(RECAST_ABILITY, PAbility->getRecastId(), PAbility->getRecastTime()));
                    break;
                }
                case G_CONDITION::HAS_RUNES:
                    results.push_back(!PTrigger->StatusEffectContainer->GetAllRuneEffects().empty());
                    break;
                case G_CONDITION::NO_MAX_RUNE:
                {
                    std::size_t maxRunes = 1;
                    if (POwner->GetMJob() == xi::Job::RUN)
                    {
                        maxRunes = POwner->GetMLevel() >= 65 ? 3 : (POwner->GetMLevel() >= 35 ? 2 : 1);
                    }
                    results.push_back(PTrigger->StatusEffectContainer->GetAllRuneEffects().size() < maxRunes);
                    break;
                }
                case G_CONDITION::NO_SAMBA:
                    results.push_back(!PTrigger->StatusEffectContainer->HasStatusEffect(xi::StatusEffect::DrainSamba) &&
                                      !PTrigger->StatusEffectContainer->HasStatusEffect(xi::StatusEffect::HasteSamba));
                    break;
                case G_CONDITION::NO_STORM:
                    // clang-format off
                    results.push_back(!PTrigger->StatusEffectContainer->HasStatusEffect({
                        xi::StatusEffect::Firestorm,
                        xi::StatusEffect::Hailstorm,
                        xi::StatusEffect::Windstorm,
                        xi::StatusEffect::Sandstorm,
                        xi::StatusEffect::Thunderstorm,
                        xi::StatusEffect::Rainstorm,
                        xi::StatusEffect::Aurorastorm,
                        xi::StatusEffect::Voidstorm,
                        xi::StatusEffect::FirestormIi,
                        xi::StatusEffect::HailstormIi,
                        xi::StatusEffect::WindstormIi,
                        xi::StatusEffect::SandstormIi,
                        xi::StatusEffect::ThunderstormIi,
                        xi::StatusEffect::RainstormIi,
                        xi::StatusEffect::AurorastormIi,
                        xi::StatusEffect::VoidstormIi,
                    }));
                    // clang-format on
                    break;
                case G_CONDITION::PT_HAS_TANK:
                    results.push_back(PartyHasTank());
                    break;
                case G_CONDITION::NOT_PT_HAS_TANK:
                    results.push_back(!PartyHasTank());
                    break;
                case G_CONDITION::HAS_TOP_ENMITY:
                case G_CONDITION::NOT_HAS_TOP_ENMITY:
                {
                    // On a foe, `Foe: targeting self` / `not targeting self`:
                    // whether the engaged mob's target is her, as the finder
                    // reads it. On her own side, whether the member the row
                    // reads (herself, or an ally) tops her fight's enmity.
                    // Nothing when nobody is its target
                    const CBattleEntity* PHated = nullptr;
                    if (PTrigger->objtype == TYPE_MOB)
                    {
                        PHated = PTrigger->PAI->IsEngaged() ? PTrigger->GetBattleTarget() : nullptr;
                    }
                    else
                    {
                        PHated = m_host->GetTopEnmity();
                    }
                    const auto* PWho = PTrigger->objtype == TYPE_MOB ? static_cast<const CBattleEntity*>(POwner) : PTrigger;
                    const bool  her  = PHated != nullptr && PHated->targid == PWho->targid;
                    results.push_back(PHated != nullptr && (predicate.condition == G_CONDITION::HAS_TOP_ENMITY) == her);
                    break;
                }
                case G_CONDITION::SC_AVAILABLE:
                {
                    const auto* PSCEffect = openWindow(PTrigger);
                    results.push_back(PSCEffect != nullptr && PSCEffect->GetTier() == 0);
                    break;
                }
                case G_CONDITION::NOT_SC_AVAILABLE:
                    results.push_back(PTrigger->StatusEffectContainer->GetStatusEffect(xi::StatusEffect::Skillchain) == nullptr);
                    break;
                case G_CONDITION::MB_AVAILABLE:
                {
                    const auto* PSCEffect = openWindow(PTrigger);
                    results.push_back(PSCEffect != nullptr && PSCEffect->GetTier() > 0);
                    break;
                }
                case G_CONDITION::LUNGE_MB_AVAILABLE:
                {
                    bool        useLunge  = false;
                    const auto* PSCEffect = openWindow(PTrigger);
                    if (PSCEffect != nullptr && PSCEffect->GetTier() > 0)
                    {
                        const auto sc = static_cast<SKILLCHAIN_ELEMENT>(PSCEffect->GetPower());
                        if (sc != SC_NONE && battleutils::GetSkillchainTier(sc) >= 3)
                        {
                            if (sc == SC_LIGHT || sc == SC_LIGHT_II)
                            {
                                useLunge = POwner->StatusEffectContainer->HasStatusEffect({ xi::StatusEffect::Lux, xi::StatusEffect::Ignis, xi::StatusEffect::Flabra, xi::StatusEffect::Sulpor });
                            }
                            else if (sc == SC_DARKNESS || sc == SC_DARKNESS_II)
                            {
                                useLunge = POwner->StatusEffectContainer->HasStatusEffect({ xi::StatusEffect::Tenebrae, xi::StatusEffect::Tellus, xi::StatusEffect::Unda, xi::StatusEffect::Gelus });
                            }
                        }
                    }
                    results.push_back(useLunge);
                    break;
                }
                case G_CONDITION::READYING_WS:
                    results.push_back(PTrigger->PAI->IsCurrentState<CWeaponSkillState>());
                    break;
                case G_CONDITION::READYING_MS:
                    results.push_back(PTrigger->PAI->IsCurrentState<CMobSkillState>());
                    break;
                case G_CONDITION::READYING_JA:
                    results.push_back(PTrigger->PAI->IsCurrentState<CAbilityState>());
                    break;
                case G_CONDITION::CASTING_MA:
                    results.push_back(PTrigger->PAI->IsCurrentState<CMagicState>());
                    break;
                case G_CONDITION::CASTING_DEBUFF:
                {
                    bool isDebuff = false;
                    if (PTrigger->PAI->IsCurrentState<CMagicState>())
                    {
                        isDebuff = static_cast<CMagicState*>(PTrigger->PAI->GetCurrentState())->GetSpell()->isDebuff();
                    }
                    results.push_back(isDebuff);
                    break;
                }
                case G_CONDITION::CASTING_ELE_MA_AOE:
                {
                    bool isAOE = false;
                    if (PTrigger->PAI->IsCurrentState<CMagicState>())
                    {
                        const auto* PSpell = static_cast<CMagicState*>(PTrigger->PAI->GetCurrentState())->GetSpell();
                        isAOE              = isElemental(PSpell->getElement()) && PSpell->getAOE() == SPELLAOE_RADIAL;
                    }
                    results.push_back(isAOE);
                    break;
                }
                case G_CONDITION::CASTING_ELEMENT_MA:
                {
                    const auto element = elementOfCast(PTrigger);
                    results.push_back(element.has_value() && isElemental(*element));
                    break;
                }
                case G_CONDITION::CAST_ELE_MA_SELF:
                {
                    bool onSelf = false;
                    if (PTrigger->PAI->IsCurrentState<CMagicState>())
                    {
                        auto*       MState  = static_cast<CMagicState*>(PTrigger->PAI->GetCurrentState());
                        const auto* MTarget = MState->target().resolve();
                        onSelf              = MTarget != nullptr && MTarget->id == POwner->id && isElemental(MState->GetSpell()->getElement());
                    }
                    results.push_back(onSelf);
                    break;
                }
                case G_CONDITION::NEED_ELE_BAREFFECT:
                {
                    bool needBar = false;
                    if (const auto element = elementOfCast(PTrigger); element.has_value())
                    {
                        uint16 castElement = *element;
                        switch (castElement)
                        {
                            case ELEMENT_FIRE:
                                needBar = !POwner->StatusEffectContainer->HasStatusEffect(xi::StatusEffect::Barfire);
                                break;
                            case ELEMENT_ICE:
                                needBar = !POwner->StatusEffectContainer->HasStatusEffect(xi::StatusEffect::Barblizzard);
                                break;
                            case ELEMENT_WIND:
                                needBar = !POwner->StatusEffectContainer->HasStatusEffect(xi::StatusEffect::Baraero);
                                break;
                            case ELEMENT_EARTH:
                                needBar = !POwner->StatusEffectContainer->HasStatusEffect(xi::StatusEffect::Barstone);
                                break;
                            case ELEMENT_THUNDER:
                                needBar = !POwner->StatusEffectContainer->HasStatusEffect(xi::StatusEffect::Barthunder);
                                break;
                            case ELEMENT_WATER:
                                needBar = !POwner->StatusEffectContainer->HasStatusEffect(xi::StatusEffect::Barwater);
                                break;
                            default:
                                castElement = static_cast<uint16>(battleutils::GetDayElement());
                                break;
                        }
                        POwner->SetLocalVar("[Gambit]CastElement", castElement);
                    }
                    results.push_back(needBar);
                    break;
                }
                case G_CONDITION::IS_ECOSYSTEM:
                    results.push_back(PTrigger->m_EcoSystem == static_cast<xi::Ecosystem>(arg));
                    break;
                case G_CONDITION::RANDOM:
                    results.push_back(xirand::GetRandomNumber<uint16>(100) < static_cast<int16>(arg));
                    break;
                case G_CONDITION::HP_MISSING:
                    results.push_back((PTrigger->health.maxhp - PTrigger->health.hp) >= static_cast<int16>(arg));
                    break;
                case G_CONDITION::SUB_ANIMATION:
                    results.push_back(PTrigger->animationsub == arg);
                    break;
                case pawn::G_CONDITION_STRATEGY:
                    results.push_back(pawn::partyStrategy(POwner) == arg);
                    break;
                case pawn::G_CONDITION_TACTICIANS_CHOICE:
                    // The tactician's mark says who decides, not when: it
                    // holds wherever it is read, and a marked row never acts
                    // on its own because the think passes it by
                    // (tactician_line.h actsAlone) -- a Cure left to her
                    // judgement is never an "always cure"
                    results.push_back(true);
                    break;
                default:
                    // VAL_URIEL_CHECK and anything newer: trust-NPC specific
                    results.push_back(false);
                    break;
            }
        }

        switch (group.logic)
        {
            case G_LOGIC::AND:
                return std::ranges::all_of(results, [](const bool r)
                                           {
                                               return r;
                                           });
            case G_LOGIC::OR:
                return std::ranges::any_of(results, [](const bool r)
                                           {
                                               return r;
                                           });
            default:
                return false;
        }
    }

    auto CGambits::ResolveSpell(const Action_t& action, CBattleEntity* PTarget) -> Maybe<SpellID>
    {
        switch (action.select)
        {
            case G_SELECT::SPECIFIC:
            {
                // A -na or Erase goes only on someone it would take something off
                if (cardian::ailments::isRemoval(action.select_arg) &&
                    (PTarget == nullptr || !cardian::ailments::cures(action.select_arg, effectsOn(PTarget), erasableOn(PTarget))))
                {
                    return std::nullopt;
                }
                return m_spellBook.GetAvailable(static_cast<SpellID>(action.select_arg));
            }
            case G_SELECT::HIGHEST:
            {
                // -na (best): the -na for the worst ailment on the target
                // that she can cast now, Erase last (ailments.h); "can cast"
                // at the spell's real MP cost, as the conveyor asks before it
                // casts (bank::usable)
                if (action.select_arg == cardian::ailments::kNaFamily)
                {
                    if (PTarget == nullptr)
                    {
                        return std::nullopt;
                    }
                    const auto spell = cardian::ailments::best(effectsOn(PTarget), erasableOn(PTarget), [this](const uint16 id)
                                                               {
                                                                   return pawn::tactics::bank::usable(POwner, static_cast<SpellID>(id));
                                                               });
                    return spell.has_value() ? Maybe<SpellID>(static_cast<SpellID>(*spell)) : std::nullopt;
                }
                return m_spellBook.GetBestAvailable(static_cast<SPELLFAMILY>(action.select_arg));
            }
            case pawn::G_SELECT_ENFEEBLE:
            {
                // Enfeeble as an order: the first of her tactician's single-
                // target enfeebles she can cast now (bank::usable, as the
                // conveyor asks) that would land on the foe
                // (tactician_line.h kEnfeebleOrder, enfeebleLands)
                if (PTarget == nullptr)
                {
                    return std::nullopt;
                }
                const auto spell = cardian::tactician::firstEnfeeble([this, PTarget](const uint16 id)
                                                                     {
                                                                         return pawn::tactics::bank::usable(POwner, static_cast<SpellID>(id)) &&
                                                                                enfeebleLands(spell::GetSpell(static_cast<SpellID>(id)), PTarget);
                                                                     });
                return spell.has_value() ? Maybe<SpellID>(static_cast<SpellID>(*spell)) : std::nullopt;
            }
            case G_SELECT::RANDOM:
                return m_spellBook.GetRandomDamageSpell();
            case G_SELECT::BEST_INDI:
                return m_spellBook.GetBestIndiSpell(m_host->GetLivePlayer());
            case G_SELECT::ENTRUSTED:
                return m_spellBook.GetBestEntrustedSpell(m_host->GetLivePlayer());
            case G_SELECT::BEST_AGAINST_TARGET:
                return m_spellBook.GetBestAgainstTargetWeakness(PTarget, static_cast<SpellID>(action.select_arg));
            case G_SELECT::EN_MOB_WEAKNESS:
                return m_spellBook.EnSpellAgainstTargetWeakness(POwner->GetBattleTarget());
            case G_SELECT::STORM_MOB_WEAKNESS:
                return m_spellBook.StormDayAgainstTargetWeakness(PTarget);
            case G_SELECT::HELIX_MOB_WEAKNESS:
                return m_spellBook.HelixAgainstTargetWeakness(PTarget);
            case G_SELECT::STORM_DAY:
                return m_spellBook.GetStormDay();
            case G_SELECT::HELIX_DAY:
                return m_spellBook.GetHelixDay();
            case G_SELECT::DEF_BAR_ELEMENT:
            {
                const auto element = POwner->GetLocalVar("[Gambit]CastElement");
                const auto bar     = element != 0 ? barSpellFor(element) : Maybe<SpellID>(SpellID::Barfire);
                return bar.has_value() ? m_spellBook.GetAvailable(*bar) : std::nullopt;
            }
            case G_SELECT::MB_ELEMENT:
            {
                const auto* PSCEffect = PTarget->StatusEffectContainer->GetStatusEffect(xi::StatusEffect::Skillchain, 0);
                if (PSCEffect == nullptr)
                {
                    return std::nullopt;
                }

                // Highest-tier known nuke of an element that bursts the chain
                Maybe<SpellID> choice;
                const auto&    nukes = m_spellBook.DamageSpells();
                for (const auto resonance : resonanceOf(PSCEffect))
                {
                    for (const auto chainElement : battleutils::GetSkillchainMagicElement(resonance))
                    {
                        for (auto it = nukes.rbegin(); it != nukes.rend(); ++it)
                        {
                            if (spell::GetSpell(*it)->getElement() == chainElement && m_spellBook.GetAvailable(*it).has_value())
                            {
                                choice = *it;
                                break;
                            }
                        }
                    }
                }
                return choice;
            }
            default:
                return std::nullopt;
        }
    }

    void CGambits::TickBehaviors()
    {
        // All behaviour rows, every tick: a switch is asserted only while its
        // row's conditions hold, so the controller falls back to its base the
        // moment they stop. Never takes the think away from action rows.
        // The rows speak in the running order, the world's first in the
        // wild, and the first to speak for a behaviour wins (gambit_layers.h
        // speak, in SetGambitBehavior)
        m_host->ClearGambitBehaviors();
        if (!m_masterOn)
        {
            return;
        }
        // A behaviour row is an order; one marked for the tactician has no
        // judgement behind it, struck out and silent
        const auto layers = RunningLayers();
        const auto states = RunningStates(layers);
        cardian::layers::forEachRow(layers, [&](const GambitRow& row, const std::size_t place, const bool on)
                                    {
                                        const auto state = states[place - 1];
                                        if (on && state == cardian::tactician::State::Order &&
                                            IsBehavior(row.gambit) && SelectTarget(row.gambit) != nullptr)
                                        {
                                            ApplyBehavior(row.gambit);
                                        }
                                        return false;
                                    });
    }

    auto CGambits::EngageRows() -> std::vector<EngageRow>
    {
        std::vector<EngageRow> out;
        if (!m_masterOn)
        {
            return out;
        }
        // The world's rows first, then her own and the lent ones as fitted.
        // An order is read; a marked Attack row, the fight her tactician may
        // melee on (tactician_line.h Allowance::Melee), is read as its
        const auto  layers   = RunningLayers();
        const auto  states   = RunningStates(layers);
        std::size_t place    = 0;
        const auto  consider = [&](const GambitRow& row, const bool on, const bool world, const bool lent, const std::size_t index)
        {
            const auto state  = states[place++];
            const bool marked = state == cardian::tactician::State::Tool;
            if ((state == cardian::tactician::State::Order || marked) && cardian::engage::doorReads(m_masterOn, on, row.gambit))
            {
                out.push_back({ index, world, marked, &row.gambit, lent });
            }
        };
        for (const auto& row : layers.world)
        {
            consider(row, row.enabled, true, false, place + 1);
        }
        for (const auto& p : layers.rows)
        {
            consider(*p.row, p.on, false, p.origin == cardian::layers::Origin::Lent, p.index);
        }
        return out;
    }

    auto CGambits::RunningStates(const cardian::layers::Layers<GambitRow>& layers) const -> std::vector<cardian::tactician::State>
    {
        std::vector<cardian::tactician::State> out;
        out.reserve(layers.world.size() + layers.rows.size());
        for (const auto& row : layers.world)
        {
            out.push_back(cardian::tactician::stateOf(row.gambit, rowFits(row.gambit)));
        }
        // A played character's plain rows are orders; his marked rows, with no
        // tactician for him yet, and the ones only a cardian runs are struck out
        for (const auto& p : layers.rows)
        {
            out.push_back(m_host->OwnClient() ? cardian::tactician::ownClientStateOf(p.row->gambit, rowFits(p.row->gambit))
                                              : cardian::tactician::stateOf(p.row->gambit, rowFits(p.row->gambit)));
        }
        return out;
    }

    auto CGambits::Shown() -> std::vector<ShownRow>
    {
        const auto            layers    = RunningLayers();
        const auto            states    = RunningStates(layers);
        const std::size_t     worldRows = layers.world.size();
        std::vector<ShownRow> out;
        out.reserve(layers.rows.size());
        for (std::size_t i = 0; i < layers.rows.size(); ++i)
        {
            const auto& p = layers.rows[i];
            out.push_back({ p.row, p.origin, p.origin == cardian::layers::Origin::Lent ? 0 : p.index, states[worldRows + i], p.on });
        }
        return out;
    }

    auto CGambits::LentBy() const -> cardian::party::Role
    {
        return m_roleKey.has_value() ? m_roleKey->role : cardian::party::Role::None;
    }

    auto CGambits::Locked(const std::size_t index) const -> bool
    {
        const auto rows = Fitted();
        return std::ranges::any_of(rows, [index](const cardian::layers::Placed<const GambitRow>& p)
                                   {
                                       return p.origin == cardian::layers::Origin::Both && p.index == index;
                                   });
    }

    auto CGambits::OffersAny(const std::function<bool(cardian::tactician::Allowance)>& wanted) const -> bool
    {
        // Her own and the lent rows as fitted: a marked row that runs and
        // names a tool the tactician has a judgement for, of the kind asked.
        // A played character's marked rows offer nothing yet: no tactician
        // runs for him (ownClientStateOf strikes them)
        if (m_host->OwnClient())
        {
            return false;
        }
        const auto rows = Fitted();
        return std::ranges::any_of(rows, [&](const cardian::layers::Placed<const GambitRow>& p)
                                   {
                                       const auto& g = p.row->gambit;
                                       return p.on && cardian::tactician::stateOf(g, rowFits(g)) == cardian::tactician::State::Tool && wanted(cardian::tactician::allowanceOf(g));
                                   });
    }

    void CGambits::RowsChanged()
    {
        ++m_rowsGeneration;
        m_offers.reset();
        Rebind();
    }

    void CGambits::Rebind()
    {
        const auto binds = cardian::layers::bindLent<const GambitRow>(std::span<const GambitRow>(m_gambits), std::span<const GambitRow>(m_roleRows), gambitOfRow, RoleBinds());
        m_roleBinds.assign(m_roleRows.size(), std::string());
        for (std::size_t li = 0; li < binds.size(); ++li)
        {
            if (binds[li].has_value())
            {
                m_roleBinds[li] = m_gambits[*binds[li]].gambit.identifier;
            }
        }
    }

    auto CGambits::RoleBinds() const -> std::vector<std::optional<std::size_t>>
    {
        std::vector<std::optional<std::size_t>> out(m_roleRows.size());
        for (std::size_t li = 0; li < out.size() && li < m_roleBinds.size(); ++li)
        {
            if (m_roleBinds[li].empty())
            {
                continue;
            }
            for (std::size_t oi = 0; oi < m_gambits.size(); ++oi)
            {
                if (m_gambits[oi].gambit.identifier == m_roleBinds[li])
                {
                    out[li] = oi;
                    break;
                }
            }
        }
        return out;
    }

    auto CGambits::Fitted() const -> std::vector<cardian::layers::Placed<const GambitRow>>
    {
        const auto binds = RoleBinds();
        return cardian::layers::place<const GambitRow>(std::span<const GambitRow>(m_gambits), std::span<const GambitRow>(m_roleRows), binds, enabledOfRow);
    }

    auto CGambits::Offers() const -> const Offered&
    {
        // Asked many times a tick, by every member's scope (tactics
        // scopeOf) and by the engage door, and the answer moves only when
        // her rows do: laid out once per generation of them
        if (!m_offers.has_value() || m_offers->generation != m_rowsGeneration)
        {
            m_offers = Offered{ m_rowsGeneration, OffersAny([](cardian::tactician::Allowance) { return true; }), OffersAny(cardian::tactician::isSpellTool),
                                OffersAny([](const cardian::tactician::Allowance a) { return a == cardian::tactician::Allowance::Nuke; }) };
        }
        return *m_offers;
    }

    auto CGambits::OffersTools() const -> bool
    {
        return Offers().tools;
    }

    auto CGambits::OffersSpells() const -> bool
    {
        return Offers().spells;
    }

    auto CGambits::OffersRest() -> bool
    {
        // Her marked Rest row, with the player's gate on it holding now
        // (RESEARCH §17.13: the row's other conditions are his, AND-ed with
        // the tactician's judgement), so `* Self: MP < 30% -> Rest` paces
        // her under 30% and not above. Read on her, as a Self row is
        const auto rows = Fitted();
        return std::ranges::any_of(rows, [&](const cardian::layers::Placed<const GambitRow>& p)
                                   {
                                       const auto& g = p.row->gambit;
                                       if (!p.on || cardian::tactician::stateOf(g, rowFits(g)) != cardian::tactician::State::Tool || cardian::tactician::allowanceOf(g) != cardian::tactician::Allowance::Rest)
                                       {
                                           return false;
                                       }
                                       for (std::size_t group = 0; group < g.predicate_groups.size(); ++group)
                                       {
                                           if (!CheckTrigger(POwner, g, group, true))
                                           {
                                               return false;
                                           }
                                       }
                                       return true;
                                   });
    }

    auto CGambits::OffersBeforeWs(const cardian::tactician::Allowance tool) -> bool
    {
        // Her marked Boost or Sneak Attack row, on, the player's gate on it
        // holding now: it goes out right before her weapon skill and
        // nothing between (CPawnController::WeaponSkill). Read over the
        // running layers, the world's included: a wild Monk's brains carry
        // a Boost row (brains.yaml)
        if (!m_masterOn)
        {
            return false;
        }
        const auto layers = RunningLayers();
        const auto states = RunningStates(layers);
        return cardian::layers::forEachRow(layers, [&](GambitRow& row, const std::size_t place, const bool on)
                                           {
                                               const auto& g = row.gambit;
                                               if (!on || states[place - 1] != cardian::tactician::State::Tool || cardian::tactician::allowanceOf(g) != tool)
                                               {
                                                   return false;
                                               }
                                               for (std::size_t group = 0; group < g.predicate_groups.size(); ++group)
                                               {
                                                   if (!CheckTrigger(POwner, g, group, true))
                                                   {
                                                       return false;
                                                   }
                                               }
                                               return true;
                                           });
    }

    auto CGambits::NakedSneakNow() -> bool
    {
        // A Thief, main or sub, with her marked Sneak Attack row, and no
        // weapon skill row of hers that would fire taking it: one naming a
        // weapon skill that cannot is respected as written, and Weapon skill
        // (best) or (any) takes it while one of hers can (RESEARCH §17.13
        // item 5; tactician_line.h wsRowTakesSneak)
        if ((POwner->GetMJob() != xi::Job::THF && POwner->GetSJob() != xi::Job::THF) || !OffersBeforeWs(cardian::tactician::Allowance::SneakAttack))
        {
            return false;
        }
        const bool anyTakes = std::ranges::any_of(m_tpSkills, [](const TrustSkill_t& s)
                                                  {
                                                      return CPawnController::TakesSneakAttack(static_cast<uint16>(s.skill_id));
                                                  });
        const auto takesIfHers = [this](const uint16 wsid)
        {
            return charutils::hasWeaponSkill(POwner, wsid) && charutils::canUseWeaponSkill(POwner, wsid) && CPawnController::TakesSneakAttack(wsid);
        };
        const auto layers = RunningLayers();
        const auto states = RunningStates(layers);
        const bool pairs  = cardian::layers::forEachRow(layers, [&](GambitRow& row, const std::size_t place, const bool on)
                                                       {
                                                           return on && cardian::tactician::wsRowTakesSneak(row.gambit, states[place - 1], anyTakes, takesIfHers);
                                                       });
        return !pairs;
    }

    auto CGambits::BuffNow(const Gambit_t& g, const bool engaged) const -> bool
    {
        // A marked self buff where it sits in her think: its when
        // (tactician_line.h buffNow, her seat read off the party's roles),
        // and the ability hers and usable now -- asked here, so a buff she
        // cannot use passes the think on instead of trying and failing at
        // the server, which pushes her a battle message every think.
        // Fighting is her weapon drawn: the think is also run engaged as
        // she walks in on a fight and while a mage attends one from the
        // perimeter, and a buff is not spent on either
        if (cardian::tactician::allowanceOf(g) != cardian::tactician::Allowance::Buff)
        {
            return false;
        }
        const auto ability = g.actions.front().select_arg;
        const auto effect  = buffEffect(ability);
        const bool up      = effect.has_value() && POwner->StatusEffectContainer->HasStatusEffect(*effect);
        const bool tank    = pawn::roster::roleOf(POwner) == cardian::party::Role::Tank;
        if (!cardian::tactician::buffNow(ability, engaged && POwner->PAI->IsEngaged(), up, tank))
        {
            return false;
        }
        // The player's own Berserk or Defender holds her stance while it
        // lasts: the other one is not put up over it (KeepStance)
        if (cardian::tactician::isStanceAbility(ability))
        {
            const uint16 other = cardian::tactician::otherStance(static_cast<uint16>(ability));
            if (m_host->PlayersBuff(other, *buffEffect(other)))
            {
                return false;
            }
        }
        // The pacer lets her act now (CPawnController::ReadyToAct), and
        // nothing on her shuts her job abilities out
        if (!m_host->RestAllowsAction() || !m_host->ReadyToAct() || m_host->AbilitiesShutOut())
        {
            return false;
        }
        const auto* PAbility = ability::GetAbility(static_cast<uint16>(ability));
        return PAbility != nullptr && charutils::hasAbility(POwner, PAbility->getID()) &&
               !POwner->PRecastContainer->HasRecast(RECAST_ABILITY, PAbility->getRecastId(), std::chrono::seconds(0));
    }

    void CGambits::KeepStance(const Gambit_t& g)
    {
        // A marked Berserk or Defender row of hers, its gate holding, keeps
        // her in the stance her seat calls for (tactician_line.h
        // wrongStance): the other buff, up, is taken off -- both up, their
        // numbers cancel. Never the player's own, his order's or a plain row
        // of his (CPawnController::PlayersBuff): his wins while it lasts
        if (cardian::tactician::allowanceOf(g) != cardian::tactician::Allowance::Buff || !cardian::tactician::isStanceAbility(g.actions.front().select_arg))
        {
            return;
        }
        for (std::size_t group = 0; group < g.predicate_groups.size(); ++group)
        {
            if (!CheckTrigger(POwner, g, group, true))
            {
                return;
            }
        }
        const bool   tank   = pawn::roster::roleOf(POwner) == cardian::party::Role::Tank;
        const uint16 wrong  = cardian::tactician::wrongStance(tank);
        const auto   effect = *buffEffect(wrong);
        if (!POwner->StatusEffectContainer->HasStatusEffect(effect) || m_host->PlayersBuff(wrong, effect))
        {
            return;
        }
        POwner->StatusEffectContainer->DelStatusEffect(effect);
        ShowInfoFmt("tactics: {} drops {} ({})", POwner->getName(), wrong == cardian::tactician::kBerserk ? "Berserk" : "Defender",
                    tank ? "seated Tank: Defender's stance" : "not the Tank: Berserk's stance");
    }

    auto CGambits::OfferedNukes() -> std::vector<SpellID>
    {
        // What she can cast at all -- learned, her jobs and level allowing --
        // not what her MP and recasts allow this instant: a Burn's plan
        // spans its whole window, and her MP is counted there
        std::vector<SpellID> out;
        if (!Offers().nukes)
        {
            return out;
        }
        for (const auto id : m_spellBook.DamageSpells())
        {
            if (auto* PSpell = spell::GetSpell(id); pawn::tactics::bank::isNuke(PSpell) && CSpellBook::Eligible(POwner, PSpell))
            {
                out.push_back(id);
            }
        }
        return out;
    }

    auto CGambits::NukeSpells() -> std::vector<SpellID>
    {
        std::vector<SpellID> out;
        for (const auto id : m_spellBook.DamageSpells())
        {
            if (pawn::tactics::bank::isNuke(spell::GetSpell(id)) && pawn::tactics::bank::usable(POwner, id))
            {
                out.push_back(id);
            }
        }
        return out;
    }

    auto CGambits::CastNuke(CBattleEntity* PTarget, const bool engaged, const std::size_t index) -> bool
    {
        // Why her nukes hold, said as it changes, not every think
        const auto hold = [this](std::string why)
        {
            if (why == m_nukeHold)
            {
                return;
            }
            m_nukeHold = std::move(why);
            if (!m_nukeHold.empty())
            {
                ShowInfoFmt("tactics: {} holds her nukes ({})", POwner->getName(), m_nukeHold);
            }
        };
        // Every reason she does not nuke while she is in a fight is said;
        // out of one (between fights) there is nothing to say
        if (!engaged || PTarget == nullptr || PTarget->isDead())
        {
            hold("");
            return false;
        }
        if (!m_host->RestAllowsAction())
        {
            hold("she is resting or getting up");
            return false;
        }
        // The moment the mob turns on her she stops, until it is on someone
        // else: a Black Mage can be two-shot (the user, 2026-10-02)
        if (PTarget->GetBattleTarget() == POwner)
        {
            hold(fmt::format("{} is on her", PTarget->getName()));
            return false;
        }
        if (POwner->StatusEffectContainer->HasPreventActionEffect() || POwner->StatusEffectContainer->HasStatusEffect({ xi::StatusEffect::Silence, xi::StatusEffect::Mute }))
        {
            hold("she cannot cast");
            return false;
        }
        const auto spells = NukeSpells();
        if (spells.empty())
        {
            // A nuke she could cast once her MP or its recast allows, or
            // none at her level and jobs at all (a low White Mage the Damage
            // seat lent the row)
            const bool any = std::ranges::any_of(m_spellBook.DamageSpells(), [this](const SpellID id)
                                                 {
                                                     auto* PSpell = spell::GetSpell(id);
                                                     return pawn::tactics::bank::isNuke(PSpell) && CSpellBook::Eligible(POwner, PSpell);
                                                 });
            hold(any ? "none she can cast now: her MP, or their recasts" : "no nuke of hers at her level and jobs yet");
            return false;
        }
        if (!pawn::tactics::has(POwner))
        {
            hold("no tactician watches her party");
            return false;
        }
        // Timed, for her nuke line: what pricing costs as the party grows
        const auto started = std::chrono::steady_clock::now();
        const auto pricing = pawn::tactics::nukePrices(POwner, PTarget, spells);
        const auto spent   = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
        if (!pricing.has_value())
        {
            hold(fmt::format("no fight of the party's on {} to price against yet", PTarget->getName()));
            return false;
        }
        if (pricing->prices.empty())
        {
            hold("the formula could not price her nukes");
            return false;
        }
        const auto* pick = cardian::tactics::pickNuke(pricing->prices);
        if (pick == nullptr)
        {
            hold(fmt::format("none of her nukes would hurt {}", PTarget->getName()));
            return false;
        }
        auto* PSpell = spell::GetSpell(static_cast<SpellID>(pick->id));
        if (distance(POwner->loc.p, PTarget->loc.p) > pawn::tactics::bank::castRange(POwner, PSpell, PTarget))
        {
            hold(fmt::format("{} is out of her reach", PTarget->getName()));
            return false;
        }
        if (!m_host->CastAssigned(PTarget->entityId(), static_cast<SpellID>(pick->id)))
        {
            hold(fmt::format("the server would not start her {}", pick->spell));
            return false;
        }
        hold("");

        // Where she stands against whoever holds the mob: logged for tuning,
        // never a gate (she pushes as hard as she can; the stop is the mob
        // turning on her). Read, not GetHighestEnmity, which prunes as it reads
        std::string enmity;
        if (auto* PMob = dynamic_cast<CMobEntity*>(PTarget); PMob != nullptr && PMob->PEnmityContainer != nullptr)
        {
            const int32          mine    = PMob->PEnmityContainer->GetCE(POwner) + PMob->PEnmityContainer->GetVE(POwner);
            const CBattleEntity* PHolder = nullptr;
            int32                theirs  = 0;
            for (const auto& [id, entry] : *PMob->PEnmityContainer->GetEnmityList())
            {
                if (entry.active && entry.PEnmityOwner != nullptr && entry.PEnmityOwner != POwner && entry.CE + entry.VE > theirs)
                {
                    PHolder = entry.PEnmityOwner;
                    theirs  = entry.CE + entry.VE;
                }
            }
            if (PHolder != nullptr)
            {
                enmity = fmt::format("; enmity {}% of {}'s", mine * 100 / theirs, PHolder->getName());
            }
        }
        const auto cost = pricing->seeded > 0 ? fmt::format("; {} seeded in {:.2f} ms", pricing->seeded, spent) : fmt::format("; priced from kept seeds in {:.2f} ms", spent);
        ShowInfoFmt("tactics: {} nukes {} (row {}): {}{}{}", POwner->getName(), PTarget->getName(), index, pick->line(), enmity, cost);
        return true;
    }

    auto CGambits::Admits(const uint16 spell, CBattleEntity* PTarget) -> std::optional<std::string>
    {
        if (!m_masterOn || PTarget == nullptr)
        {
            return std::nullopt;
        }
        // Her own and the lent rows as fitted: a marked row is a tool the
        // tactician may use, hers or the role's alike
        const auto        layers    = RunningLayers();
        const auto        states    = RunningStates(layers);
        const std::size_t worldRows = layers.world.size();
        const auto        now       = timer::now();
        for (std::size_t i = 0; i < layers.rows.size(); ++i)
        {
            const auto& row = *layers.rows[i].row;
            const auto& g   = row.gambit;
            if (!layers.rows[i].on || states[worldRows + i] != cardian::tactician::State::Tool ||
                !cardian::tactician::allowsSpell(g, spell) || !Names(g.target_selector, PTarget) ||
                now < g.last_used + std::chrono::seconds(g.retry_delay))
            {
                continue;
            }
            bool holds = true;
            for (std::size_t group = 0; holds && group < g.predicate_groups.size(); ++group)
            {
                holds = CheckTrigger(PTarget, g, group, true);
            }
            if (holds)
            {
                return g.identifier;
            }
        }
        return std::nullopt;
    }

    auto CGambits::UseHateTool(const uint16 ability, CBattleEntity* PTarget, const std::string& why) -> std::optional<std::string>
    {
        if (!m_masterOn || PTarget == nullptr)
        {
            return "her gambits are off";
        }
        auto* PAbility = ability::GetAbility(ability);
        if (PAbility == nullptr || !charutils::hasAbility(POwner, ability))
        {
            return "not hers to use";
        }
        if (POwner->PRecastContainer->HasRecast(RECAST_ABILITY, PAbility->getRecastId(), PAbility->getRecastTime()))
        {
            return "on its clock";
        }
        // The marked row that lets her: the tool's, naming the target, its
        // retry run and its conditions holding. Used through it, so its
        // retry stamp and its number in the log are the row's, as a cast's
        // are: its place in the list as the editor shows it, lent rows
        // counted
        const auto        layers    = RunningLayers();
        const auto        states    = RunningStates(layers);
        const std::size_t worldRows = layers.world.size();
        const auto        now       = timer::now();
        std::string       refused   = "no marked row lets her";
        for (std::size_t i = 0; i < layers.rows.size(); ++i)
        {
            auto&       row = *layers.rows[i].row;
            const auto& g   = row.gambit;
            if (!layers.rows[i].on || states[worldRows + i] != cardian::tactician::State::Tool || !cardian::tactician::allowsAbility(g, ability))
            {
                continue;
            }
            const auto rowName = fmt::format("row {}", worldRows + i + 1);
            if (!Names(g.target_selector, PTarget))
            {
                refused = rowName + " does not name the target";
                continue;
            }
            if (now < g.last_used + std::chrono::seconds(g.retry_delay))
            {
                refused = rowName + " is on its retry";
                continue;
            }
            bool holds = true;
            for (std::size_t group = 0; holds && group < g.predicate_groups.size(); ++group)
            {
                holds = CheckTrigger(PTarget, g, group, true);
            }
            if (!holds)
            {
                refused = rowName + "'s condition does not hold";
                continue;
            }
            if (!m_host->Ability(PTarget->entityId(), ability))
            {
                return "the server refused it";
            }
            ShowInfoFmt("tactics: {} uses {} on {} ({}, {})", POwner->getName(), titleCase(PAbility->getName()), PTarget->getName(), why, rowName);
            if (g.retry_delay != 0)
            {
                row.gambit.last_used = now;
            }
            return std::nullopt;
        }
        return refused;
    }

    auto CGambits::AllowsSpell(const uint16 spell) const -> bool
    {
        // A played character's marked rows offer no tactician anything yet
        if (m_host->OwnClient())
        {
            return false;
        }
        const auto rows = Fitted();
        return std::ranges::any_of(rows, [&](const cardian::layers::Placed<const GambitRow>& p)
                                   {
                                       const auto& g = p.row->gambit;
                                       return p.on && cardian::tactician::stateOf(g, rowFits(g)) == cardian::tactician::State::Tool && cardian::tactician::allowsSpell(g, spell);
                                   });
    }

    auto CGambits::EngageConditionsHold(const Gambit_t& gambit, CBattleEntity* PFoe) -> bool
    {
        if (PFoe == nullptr)
        {
            return false;
        }
        // An engage row carries no TIMER or RANDOM (pairingError), so reading
        // it every roam tick changes nothing
        for (std::size_t group = 0; group < gambit.predicate_groups.size(); ++group)
        {
            if (!CheckTrigger(PFoe, gambit, group))
            {
                return false;
            }
        }
        return true;
    }

    auto CGambits::IsBehavior(const Gambit_t& gambit) const -> bool
    {
        return !gambit.actions.empty() &&
               std::all_of(gambit.actions.begin(), gambit.actions.end(), [](const Action_t& action)
                           {
                               return action.reaction == G_REACTION_BEHAVIOR;
                           });
    }

    void CGambits::ApplyBehavior(const Gambit_t& gambit)
    {
        for (const auto& action : gambit.actions)
        {
            m_host->SetGambitBehavior(static_cast<uint16>(action.select), static_cast<uint16>(action.select_arg));
        }
    }

    void CGambits::SetBehaviorRow(const pawn::Behavior behavior, const uint16 arg)
    {
        // One name per behaviour value, "?" for the gaps and the retired
        // values; the assert keeps the list in step with the enum
        static constexpr auto names = std::to_array<std::string_view>({ "?", "avoid aggro", "?", "?", "formation", "?", "rest with player", "home point with player", "?", "?", "?", "?", "?", "avoid links", "rest" });
        static_assert(names.size() == pawn::BehaviorCount);
        const auto name = names[std::min<std::size_t>(static_cast<std::size_t>(behavior), names.size() - 1)];
        const bool sw   = pawn::isSwitch(behavior);

        // A retired behaviour is no row's, as the grammar has it
        if (pawn::isRetiredBehavior(static_cast<uint16>(behavior)))
        {
            ShowWarningFmt("pawn: {} gambit row: behaviour {} is retired", POwner->getName(), static_cast<uint16>(behavior));
            return;
        }

        const auto unconditional = [&](const GambitRow& row)
        {
            const auto& g = row.gambit;
            return IsBehavior(g) && g.actions.size() == 1 && static_cast<pawn::Behavior>(g.actions[0].select) == behavior &&
                   g.predicate_groups.size() == 1 && g.predicate_groups[0].predicates.size() == 1 &&
                   g.predicate_groups[0].predicates[0].condition == G_CONDITION::ALWAYS;
        };
        // Her row for it is set where it stands; a new one ends her list
        if (const auto it = std::find_if(m_gambits.begin(), m_gambits.end(), unconditional); it != m_gambits.end())
        {
            it->gambit.actions[0].select_arg = sw ? 1 : arg;
            it->enabled                      = sw ? arg != 0 : true;
            RowsChanged();
        }
        else
        {
            Gambit_t row;
            row.target_selector = G_TARGET::SELF;
            row.predicate_groups.emplace_back(G_LOGIC::AND, std::vector<Predicate_t>{ Predicate_t(G_CONDITION::ALWAYS, 0) });
            row.actions.emplace_back(G_REACTION_BEHAVIOR, static_cast<G_SELECT>(behavior), sw ? 1 : arg);
            AddGambit(std::move(row), sw ? arg != 0 : true);
        }
        if (sw)
        {
            ShowInfoFmt("pawn: {} gambit row: {} {}", POwner->getName(), name, arg != 0 ? "checked" : "unchecked");
        }
        else
        {
            ShowInfoFmt("pawn: {} gambit row: {} = {}", POwner->getName(), name, arg);
        }
    }

    void CGambits::SetMaster(const bool on)
    {
        if (m_masterOn != on)
        {
            ShowInfoFmt("pawn: {} gambits {}", POwner->getName(), on ? "on" : "off");
        }
        m_masterOn = on;
    }

    auto CGambits::SetEnabled(const std::size_t index, const bool on) -> bool
    {
        if (index == 0 || index > m_gambits.size() || Locked(index))
        {
            return false;
        }
        m_gambits[index - 1].enabled = on;
        RowsChanged();
        return true;
    }

    auto CGambits::Move(const std::size_t from, const std::size_t to) -> bool
    {
        if (from == 0 || to == 0 || from > m_gambits.size() || to > m_gambits.size())
        {
            return false;
        }
        if (from != to)
        {
            GambitRow row = std::move(m_gambits[from - 1]);
            m_gambits.erase(m_gambits.begin() + static_cast<std::ptrdiff_t>(from - 1));
            m_gambits.insert(m_gambits.begin() + static_cast<std::ptrdiff_t>(to - 1), std::move(row));
            RowsChanged();
        }
        return true;
    }

    auto CGambits::Erase(const std::size_t index) -> bool
    {
        if (index == 0 || index > m_gambits.size() || Locked(index))
        {
            return false;
        }
        m_gambits.erase(m_gambits.begin() + static_cast<std::ptrdiff_t>(index - 1));
        RowsChanged();
        return true;
    }

    auto CGambits::Insert(const std::size_t index, Gambit_t gambit) -> bool
    {
        if (index == 0 || index > m_gambits.size() + 1)
        {
            return false;
        }
        gambit.identifier = fmt::format("{}", ++m_nextId);
        gambit.last_used  = {};
        m_gambits.insert(m_gambits.begin() + static_cast<std::ptrdiff_t>(index - 1), GambitRow{ std::move(gambit), true });
        RowsChanged();
        return true;
    }

    auto CGambits::Replace(const std::size_t index, Gambit_t gambit) -> bool
    {
        if (index == 0 || index > m_gambits.size() || Locked(index))
        {
            return false;
        }
        gambit.identifier          = fmt::format("{}", ++m_nextId);
        gambit.last_used           = {};
        m_gambits[index - 1].gambit = std::move(gambit);
        RowsChanged();
        return true;
    }

    namespace
    {
        // "cure_iii" -> "Cure III", "provoke" -> "Provoke"
        auto titleCase(std::string_view raw) -> std::string
        {
            std::string out;
            std::string word;
            const auto  flush = [&]()
            {
                if (word.empty())
                {
                    return;
                }
                const bool numeral = std::all_of(word.begin(), word.end(), [](const char c)
                                                 {
                                                     return c == 'i' || c == 'v' || c == 'x' || c == 'I' || c == 'V' || c == 'X';
                                                 });
                for (std::size_t i = 0; i < word.size(); ++i)
                {
                    const auto c = static_cast<unsigned char>(word[i]);
                    out += static_cast<char>((numeral || i == 0) ? std::toupper(c) : std::tolower(c));
                }
                word.clear();
            };
            for (const char c : raw)
            {
                if (c == '_' || c == ' ')
                {
                    flush();
                    out += ' ';
                }
                else
                {
                    word += c;
                }
            }
            flush();
            return out;
        }

        // A row's target in the words of the gambit review (RESEARCH §14.13),
        // FFXII's: a side -- Self, Ally or Foe -- then one clause. The four
        // finders are Foe clauses of their own, and upstream's trust targets,
        // which the pickers no longer offer, keep a clause naming them so an
        // older row still reads
        auto targetName(const std::size_t target) -> std::string_view
        {
            static constexpr std::array<std::string_view, 14> names{ "Self", "Ally", "Foe", "Ally: the player", "Ally: tank", "Ally: melee", "Ally: ranged", "Ally: caster", "Ally: hate holder", "Ally: Curilla", "Ally: status = KO", "Ally", "Self", "Foe" };
            // Cardian's foes around the party (gambit_ids.h)
            switch (target)
            {
                case static_cast<std::size_t>(pawn::G_TARGET_LEADERS_TARGET):
                    return "Foe: party leader's target";
                case static_cast<std::size_t>(pawn::G_TARGET_TARGETED_BY_ALLY):
                    return "Foe: targeted by ally";
                case static_cast<std::size_t>(pawn::G_TARGET_TARGETING_ALLY):
                    return "Foe: targeting ally";
                case static_cast<std::size_t>(pawn::G_TARGET_TARGETING_SELF):
                    return "Foe: targeting self";
                default:
                    return target < names.size() ? names[target] : std::string_view("?");
            }
        }

        // A target that is a side alone, whose clause is the condition's:
        // Self, Ally, Foe (the old Target), and upstream's aliases of them
        auto isSide(const std::size_t target) -> bool
        {
            switch (static_cast<G_TARGET>(target))
            {
                case G_TARGET::SELF:
                case G_TARGET::PARTY:
                case G_TARGET::TARGET:
                case G_TARGET::PARTY_MULTI:
                case G_TARGET::TRIGGER_SELF_ACTION_TARGET:
                case G_TARGET::TRIGGER_TARGET_ACTION_SELF:
                    return true;
                default:
                    return false;
            }
        }

        // A Foe target: its condition's clause is about the foe
        auto onFoe(const std::size_t target) -> bool
        {
            return cardian::engage::isFoeTarget(static_cast<G_TARGET>(target)) || target == static_cast<std::size_t>(G_TARGET::TRIGGER_TARGET_ACTION_SELF);
        }

        auto statusName(const uint32 id) -> std::string
        {
            if (id == pawn::G_STATUS_ENFEEBLE)
            {
                return "Enfeeble";
            }
            return id == static_cast<uint16>(xi::StatusEffect::Ko) ? std::string("KO") : titleCase(effects::GetEffectName(static_cast<uint16>(id)));
        }

        // A condition's clause, its value given as text: the number or the
        // status on a row, '*' for the number in a picker entry, nothing for
        // the status a picker entry leaves to the row's next cell.
        // Comparisons are FFXII's signs. On a foe, holding hate reads from
        // the foe's side: whether its target is her
        auto conditionWords(const G_CONDITION condition, const std::string& value, const bool foe) -> std::string
        {
            const auto with = [&value](const std::string_view words)
            {
                return value.empty() ? std::string(words) : fmt::format("{} {}", words, value);
            };
            switch (condition)
            {
                case G_CONDITION::ALWAYS:
                    return "any";
                case G_CONDITION::HPP_LT:
                    return fmt::format("HP < {}%", value);
                case G_CONDITION::HPP_GTE:
                    return fmt::format("HP ≥ {}%", value);
                case G_CONDITION::MPP_LT:
                    return fmt::format("MP < {}%", value);
                case G_CONDITION::MPP_GTE:
                    return fmt::format("MP ≥ {}%", value);
                case G_CONDITION::TP_LT:
                    return fmt::format("TP < {}", value);
                case G_CONDITION::TP_GTE:
                    return fmt::format("TP ≥ {}", value);
                case G_CONDITION::LVL_LT:
                    return fmt::format("level < {}", value);
                case G_CONDITION::LVL_GTE:
                    return fmt::format("level ≥ {}", value);
                case G_CONDITION::STATUS:
                    return with("status =");
                case G_CONDITION::NOT_STATUS:
                    return with("status ≠");
                case G_CONDITION::STATUS_FLAG:
                    return with("status flag");
                case G_CONDITION::HAS_TOP_ENMITY:
                    return foe ? "targeting self" : "holds hate";
                case G_CONDITION::NOT_HAS_TOP_ENMITY:
                    return foe ? "not targeting self" : "doesn't hold hate";
                case G_CONDITION::SC_AVAILABLE:
                    return "skillchain open";
                case G_CONDITION::NOT_SC_AVAILABLE:
                    return "no skillchain open";
                case G_CONDITION::MB_AVAILABLE:
                    return "magic burst open";
                case G_CONDITION::READYING_WS:
                    return "readying a weapon skill";
                case G_CONDITION::READYING_MS:
                    return "readying a mob skill";
                case G_CONDITION::READYING_JA:
                    return "readying an ability";
                case G_CONDITION::CASTING_MA:
                    return "casting";
                case G_CONDITION::CASTING_DEBUFF:
                    return "casting a debuff";
                case G_CONDITION::RANDOM:
                    return fmt::format("{}% of the time", value);
                case G_CONDITION::NO_SAMBA:
                    return "no samba up";
                case G_CONDITION::NO_STORM:
                    return "no storm up";
                case G_CONDITION::PT_HAS_TANK:
                    return "tank in party";
                case G_CONDITION::NOT_PT_HAS_TANK:
                    return "no tank in party";
                case G_CONDITION::IS_ECOSYSTEM:
                    return with("ecosystem");
                case G_CONDITION::HP_MISSING:
                    return fmt::format("missing {} HP", value);
                case G_CONDITION::JA_ON_COOLDOWN:
                    return fmt::format("{} on cooldown", value);
                case G_CONDITION::TIMER:
                    return fmt::format("every {}s", value);
                case pawn::G_CONDITION_STRATEGY:
                    return with("strategy");
                case pawn::G_CONDITION_TACTICIANS_CHOICE:
                    return "tactician's choice";
                default:
                    return fmt::format("condition {}:{}", static_cast<uint16>(condition), value);
            }
        }

        // A condition's clause on a row, its value read from the row
        auto conditionText(const Predicate_t& p, const bool foe) -> std::string
        {
            const auto arg = p.condition_arg;
            switch (p.condition)
            {
                case G_CONDITION::STATUS:
                case G_CONDITION::NOT_STATUS:
                    return conditionWords(p.condition, statusName(arg), foe);
                case G_CONDITION::JA_ON_COOLDOWN:
                {
                    auto* PAbility = ability::GetAbility(static_cast<uint16>(arg));
                    return conditionWords(p.condition, PAbility != nullptr ? titleCase(PAbility->getName()) : std::to_string(arg), foe);
                }
                default:
                    return conditionWords(p.condition, std::to_string(arg), foe);
            }
        }

        // A row's head: its target, then its conditions ("" for none). A
        // side alone takes the conditions as its clause ("Ally: HP < 50%";
        // none is "Ally: any", and "Self" alone); a target with a clause of
        // its own takes them after it ("Foe: party leader's target, HP ≥ 50%")
        auto headText(const std::size_t target, const std::string& conditions) -> std::string
        {
            const auto name = targetName(target);
            const bool self = target == static_cast<std::size_t>(G_TARGET::SELF) || target == static_cast<std::size_t>(G_TARGET::TRIGGER_SELF_ACTION_TARGET);
            if (conditions.empty())
            {
                return isSide(target) && !self ? fmt::format("{}: any", name) : std::string(name);
            }
            return isSide(target) ? fmt::format("{}: {}", name, conditions) : fmt::format("{}, {}", name, conditions);
        }

        // A switch row reads as its name; only an explicit "off" (a
        // conditional override) says so
        auto behaviorText(const Action_t& a) -> std::string
        {
            const auto  behavior = static_cast<pawn::Behavior>(a.select);
            const char* off      = a.select_arg == 0 ? ": off" : "";
            switch (behavior)
            {
                case pawn::Behavior::AvoidAggro:
                    return fmt::format("Avoid aggro{}", off);
                case pawn::Behavior::AvoidLinks:
                    return fmt::format("Avoid links{}", off);
                case pawn::Behavior::Formation:
                    return fmt::format("Formation: {}", cardian::formation::slotName(static_cast<pawn::Slot>(a.select_arg)));
                case pawn::Behavior::RestWithPlayer:
                    return fmt::format("Rest with the player{}", off);
                case pawn::Behavior::HomePointWithPlayer:
                    return fmt::format("Home point with the player{}", off);
                case pawn::Behavior::Rest:
                    return fmt::format("Rest{}", off);
                default:
                    return fmt::format("behaviour {} = {}", static_cast<uint16>(a.select), a.select_arg);
            }
        }

        auto actionText(const Action_t& a) -> std::string
        {
            if (a.reaction == pawn::G_REACTION_BEHAVIOR)
            {
                return behaviorText(a);
            }
            switch (a.reaction)
            {
                case G_REACTION::MA:
                {
                    switch (a.select)
                    {
                        case G_SELECT::SPECIFIC:
                        {
                            auto* PSpell = spell::GetSpell(static_cast<SpellID>(a.select_arg));
                            return PSpell != nullptr ? titleCase(PSpell->getName()) : fmt::format("spell {}", a.select_arg);
                        }
                        case G_SELECT::HIGHEST:
                            return familyName(a.select_arg) + " (best)";
                        case G_SELECT::LOWEST:
                            return familyName(a.select_arg) + " (lowest)";
                        case G_SELECT::RANDOM:
                            return "Damage spell (any)"; // any of her damage spells (spell_bank.h isNuke); marked, her tactician's pick
                        case G_SELECT::MB_ELEMENT:
                            return "Magic burst";
                        case G_SELECT::ENTRUSTED:
                            return "Entrust " + familyName(a.select_arg);
                        case G_SELECT::BEST_INDI:
                            return "Indi (best)";
                        case G_SELECT::BEST_AGAINST_TARGET:
                            return familyName(a.select_arg) + " (best against target)";
                        case pawn::G_SELECT_ENFEEBLE:
                            return "Enfeeble";
                        default:
                            return fmt::format("magic ({}:{})", static_cast<uint16>(a.select), a.select_arg);
                    }
                }
                case G_REACTION::JA:
                {
                    if (a.select == G_SELECT::SPECIFIC)
                    {
                        auto* PAbility = ability::GetAbility(static_cast<uint16>(a.select_arg));
                        return PAbility != nullptr ? titleCase(PAbility->getName()) : fmt::format("ability {}", a.select_arg);
                    }
                    return fmt::format("ability ({}:{})", static_cast<uint16>(a.select), a.select_arg);
                }
                case G_REACTION::WS:
                {
                    if (a.select == G_SELECT::SPECIFIC)
                    {
                        auto* PSkill = battleutils::GetWeaponSkill(static_cast<uint16>(a.select_arg));
                        return PSkill != nullptr ? titleCase(PSkill->getName()) : fmt::format("weapon skill {}", a.select_arg);
                    }
                    return a.select == G_SELECT::RANDOM ? "Weapon skill (any)" : "Weapon skill (best)";
                }
                case G_REACTION::RATTACK:
                    return "Ranged Attack";
                case G_REACTION::ATTACK:
                    return "Attack";
                default:
                    return fmt::format("action {}:{}:{}", static_cast<uint16>(a.reaction), static_cast<uint16>(a.select), a.select_arg);
            }
        }
    } // namespace

    auto familyName(const uint32 family) -> std::string
    {
        // The families whose enum name reads badly as words (one is
        // misspelt upstream)
        switch (static_cast<SPELLFAMILY>(family))
        {
            case SPELLFAMILY_NA:
                return "-na";
            case SPELLFAMILY_JA:
                return "-ja";
            case SPELLFMAILY_TELEPORT:
                return "Teleport";
            case SPELLFAMILY_ELE_BAR:
                return "Bar-element";
            case SPELLFAMILY_ELE_BAR_RA:
                return "Bar-element-ra";
            case SPELLFAMILY_STATUS_BAR:
                return "Bar-status";
            case SPELLFAMILY_STATUS_BAR_RA:
                return "Bar-status-ra";
            case SPELLFAMILY_ELE_DOT:
                return "Elemental DoT";
            default:
                break;
        }
        auto name = std::string(magic_enum::enum_name(static_cast<SPELLFAMILY>(family)));
        if (name.rfind("SPELLFAMILY_", 0) == 0)
        {
            name.erase(0, 12);
        }
        return name.empty() ? fmt::format("family {}", family) : titleCase(name);
    }

    auto labelGambit(const Gambit_t& g) -> GambitLabel
    {
        // "Always" says nothing beside another condition, and alone is the
        // side's "any"; an OR group that holds it is always true, so says
        // nothing either. The tactician's mark is no clause either: the
        // editor shows it as a star beside the row's switch (RESEARCH §17.13)
        const auto silent = [](const Predicate_t& p)
        {
            return p.condition == G_CONDITION::ALWAYS || p.condition == pawn::G_CONDITION_TACTICIANS_CHOICE;
        };
        const auto  target = static_cast<std::size_t>(g.target_selector);
        std::string conditions;
        for (const auto& group : g.predicate_groups)
        {
            if (group.logic == G_LOGIC::OR && std::ranges::any_of(group.predicates, silent))
            {
                continue;
            }
            bool first = true;
            for (const auto& predicate : group.predicates)
            {
                if (silent(predicate))
                {
                    continue;
                }
                if (!conditions.empty())
                {
                    conditions += (!first && group.logic == G_LOGIC::OR) ? " or " : ", ";
                }
                conditions += conditionText(predicate, onFoe(target));
                first = false;
            }
        }
        GambitLabel out{ headText(target, conditions), "" };
        for (std::size_t i = 0; i < g.actions.size(); ++i)
        {
            if (i != 0)
            {
                out.action += " + ";
            }
            out.action += actionText(g.actions[i]);
        }
        if (g.retry_delay != 0)
        {
            out.action += fmt::format(" (every {}s)", g.retry_delay);
        }
        return out;
    }

    auto CGambits::Execute(const Gambit_t& gambit, CBattleEntity* PTarget, const bool engaged, const std::size_t index) -> bool
    {
        bool spellSeen = false;

        for (const auto& action : gambit.actions)
        {
            bool executed = false;

            switch (action.reaction)
            {
                case G_REACTION::RATTACK:
                {
                    if (engaged && m_host->RangedAttack(PTarget->entityId()))
                    {
                        Debug("ranged attack", 0, PTarget);
                        executed = true;
                    }
                    break;
                }
                case G_REACTION::MA:
                {
                    // Only the first spell of an action list can start this think
                    if (spellSeen)
                    {
                        break;
                    }
                    spellSeen = true;

                    // Entrust goes on the player; every other cast target is
                    // the gambit's (self-target spells are redirected by the
                    // controller)
                    CBattleEntity* PCastTarget = action.select == G_SELECT::ENTRUSTED ? m_host->GetLivePlayer() : PTarget;
                    if (PCastTarget == nullptr)
                    {
                        break;
                    }

                    // A spell on a mob is the fight's: nothing offensive while
                    // she is not in it -- drawn, or attending a mob that is
                    // engaged -- the conveyor's own hand-out rule (a first
                    // cast on a mob nobody has struck is a pull)
                    if (!engaged && PCastTarget->objtype == TYPE_MOB)
                    {
                        break;
                    }

                    // Where a tactician watches, the row feeds the conveyor
                    // instead of casting (a "best cure" leaves the tier to
                    // the bank); when the cast comes straight back as hers
                    // it is her action this think, as it always was, and
                    // when it does not the row's next action gets its turn
                    if (Conveyed())
                    {
                        CSpell* PSpell = nullptr;
                        if (!(action.select == G_SELECT::HIGHEST && action.select_arg == SPELLFAMILY_CURE))
                        {
                            const auto spellId = ResolveSpell(action, PTarget);
                            PSpell             = spellId.has_value() ? spell::GetSpell(*spellId) : nullptr;
                            if (PSpell == nullptr)
                            {
                                break;
                            }
                        }
                        const auto fed = pawn::tactics::feed(POwner, PSpell, PCastTarget, static_cast<uint32>(index), gambit.identifier);
                        if (fed.has_value() && fed->mine)
                        {
                            executed = CastAssigned(fed->spell, fed->target, fed->why);
                        }
                        break;
                    }

                    const auto spellId = ResolveSpell(action, PTarget);
                    if (!spellId.has_value())
                    {
                        break;
                    }
                    // A debuff already on the target, or nullified by what is
                    // on it, is not cast again, whoever landed it: the
                    // conveyor's rule (tactics::feed), here for the casts it
                    // does not hand out -- a played character's among them
                    if (auto* PSpell = spell::GetSpell(*spellId); PSpell != nullptr &&
                        (pawn::tactics::bank::onAlready(PSpell, PCastTarget).has_value() || pawn::tactics::bank::blockedOn(PSpell, PCastTarget)))
                    {
                        break;
                    }
                    if (m_host->Cast(PCastTarget->entityId(), *spellId))
                    {
                        Debug("cast", static_cast<uint32>(*spellId), PCastTarget);
                        executed = true;
                    }
                    break;
                }
                case G_REACTION::JA:
                {
                    executed = ExecuteAbility(action, PTarget, engaged);
                    // A plain row's Berserk or Defender is the player's
                    // command, as his order is: the tactician's stance leaves
                    // it be (dumb gambits overrule, the user, 2026-10-02)
                    if (executed && action.select == G_SELECT::SPECIFIC && !cardian::tactician::isMarked(gambit) &&
                        cardian::tactician::isStanceAbility(action.select_arg))
                    {
                        m_host->NoteOrderedStance(static_cast<uint16>(action.select_arg));
                    }
                    break;
                }
                case G_REACTION::WS:
                {
                    executed = ExecuteWeaponSkill(action, engaged);
                    break;
                }
                default:
                {
                    // ATTACK is the engage door's (EngageRows), and the think
                    // skips a row that carries it; MS and ANIM_STRING have no
                    // character equivalent
                    break;
                }
            }

            if (executed)
            {
                return true;
            }
        }

        return false;
    }

    auto CGambits::ExecuteAbility(const Action_t& action, CBattleEntity* PTarget, const bool engaged) -> bool
    {
        CAbility*    PAbility = nullptr;
        const auto   mLevel   = POwner->GetMLevel();
        const int16  tp       = POwner->health.tp;

        switch (action.select)
        {
            case G_SELECT::SPECIFIC:
            {
                PAbility = ability::GetAbility(static_cast<uint16>(action.select_arg));
                break;
            }
            case G_SELECT::HIGHEST_WALTZ:
            {
                static constexpr std::pair<ABILITY, uint16> kWaltzes[] = {
                    { ABILITY_CURING_WALTZ_V, 800 },
                    { ABILITY_CURING_WALTZ_IV, 650 },
                    { ABILITY_CURING_WALTZ_III, 500 },
                    { ABILITY_CURING_WALTZ_II, 350 },
                    { ABILITY_CURING_WALTZ, 200 },
                };
                for (const auto& [waltz, cost] : kWaltzes)
                {
                    auto* PWaltz = ability::GetAbility(waltz);
                    if (PWaltz != nullptr && mLevel >= PWaltz->getLevel() && tp >= cost && charutils::hasAbility(POwner, waltz))
                    {
                        PAbility = PWaltz;
                        break;
                    }
                }
                break;
            }
            case G_SELECT::BEST_SAMBA:
            {
                uint16 cost = 0;
                if (mLevel > 65)
                {
                    PAbility = ability::GetAbility(PartyHasHealer() ? ABILITY_HASTE_SAMBA : ABILITY_DRAIN_SAMBA_III);
                    cost     = PartyHasHealer() ? 350 : 400;
                }
                else if (mLevel > 45)
                {
                    PAbility = ability::GetAbility(PartyHasHealer() ? ABILITY_HASTE_SAMBA : ABILITY_DRAIN_SAMBA_II);
                    cost     = PartyHasHealer() ? 350 : 250;
                }
                else if (mLevel > 35)
                {
                    PAbility = ability::GetAbility(ABILITY_DRAIN_SAMBA_II);
                    cost     = 250;
                }
                else if (mLevel >= 5)
                {
                    PAbility = ability::GetAbility(ABILITY_DRAIN_SAMBA);
                    cost     = 100;
                }
                if (tp < cost)
                {
                    PAbility = nullptr;
                }
                break;
            }
            case G_SELECT::RUNE_DAY:
            {
                uint32 element = POwner->GetLocalVar("[Gambit]CastElement");
                if (element == 0)
                {
                    element = battleutils::GetDayElement();
                }

                ABILITY rune = ABILITY_IGNIS;
                switch (element)
                {
                    case ELEMENT_FIRE:
                        rune = ABILITY_UNDA;
                        break;
                    case ELEMENT_ICE:
                        rune = ABILITY_IGNIS;
                        break;
                    case ELEMENT_WIND:
                        rune = ABILITY_GELUS;
                        break;
                    case ELEMENT_EARTH:
                        rune = ABILITY_FLABRA;
                        break;
                    case ELEMENT_THUNDER:
                        rune = ABILITY_TELLUS;
                        break;
                    case ELEMENT_WATER:
                        rune = ABILITY_SULPOR;
                        break;
                    case ELEMENT_LIGHT:
                        rune = ABILITY_TENEBRAE;
                        break;
                    case ELEMENT_DARK:
                        rune = ABILITY_LUX;
                        break;
                    default:
                        break;
                }
                PAbility = ability::GetAbility(rune);
                break;
            }
            default:
                break;
        }

        if (PAbility == nullptr || !charutils::hasAbility(POwner, PAbility->getID()))
        {
            return false;
        }

        // Enemy abilities go on the battle target, party abilities on the
        // gambit's target when it is friendly, everything else on the pawn
        CBattleEntity* PJATarget = POwner;
        const auto     valid     = PAbility->getValidTarget();
        if (valid & TARGET_ENEMY)
        {
            if (!engaged)
            {
                return false;
            }
            PJATarget = FightTarget();
            if (PJATarget == nullptr)
            {
                return false;
            }
        }
        else if ((valid & (TARGET_PLAYER_PARTY | TARGET_PLAYER)) && PTarget->allegiance == POwner->allegiance)
        {
            PJATarget = PTarget;
        }

        if (PJATarget == nullptr || !m_host->Ability(PJATarget->entityId(), PAbility->getID()))
        {
            return false;
        }

        Debug("ability", PAbility->getID(), PJATarget);
        return true;
    }

    auto CGambits::ExecuteWeaponSkill(const Action_t& action, const bool engaged) -> bool
    {
        if (!engaged || POwner->health.tp < 1000)
        {
            return false;
        }

        uint16 wsid = 0;
        switch (action.select)
        {
            case G_SELECT::SPECIFIC:
            {
                wsid = static_cast<uint16>(action.select_arg);
                if (!charutils::hasWeaponSkill(POwner, wsid) || !charutils::canUseWeaponSkill(POwner, wsid))
                {
                    return false;
                }
                break;
            }
            case G_SELECT::HIGHEST:
            {
                if (m_tpSkills.empty())
                {
                    return false;
                }
                wsid = static_cast<uint16>(m_tpSkills.back().skill_id);
                // Her best is hers to choose: with Sneak Attack able to go
                // now, the best that takes it (RESEARCH §17.13 item 5); any
                // other would spend it for nothing
                if (!CPawnController::TakesSneakAttack(wsid) && m_host->SneakAttackNow(POwner->GetBattleTarget()))
                {
                    for (auto it = m_tpSkills.rbegin(); it != m_tpSkills.rend(); ++it)
                    {
                        if (CPawnController::TakesSneakAttack(static_cast<uint16>(it->skill_id)))
                        {
                            wsid = static_cast<uint16>(it->skill_id);
                            break;
                        }
                    }
                }
                break;
            }
            case G_SELECT::RANDOM:
            {
                if (m_tpSkills.empty())
                {
                    return false;
                }
                wsid = static_cast<uint16>(xirand::GetRandomElement(m_tpSkills).skill_id);
                // Any of hers, and with Sneak Attack able to go now any that takes it
                if (!CPawnController::TakesSneakAttack(wsid) && m_host->SneakAttackNow(POwner->GetBattleTarget()))
                {
                    std::vector<TrustSkill_t> physical;
                    std::ranges::copy_if(m_tpSkills, std::back_inserter(physical), [](const TrustSkill_t& s)
                                         {
                                             return CPawnController::TakesSneakAttack(static_cast<uint16>(s.skill_id));
                                         });
                    if (!physical.empty())
                    {
                        wsid = static_cast<uint16>(xirand::GetRandomElement(physical).skill_id);
                    }
                }
                break;
            }
            default:
                return false;
        }

        CBattleEntity* PTarget = battleutils::isValidSelfTargetWeaponskill(wsid) ? POwner : POwner->GetBattleTarget();
        if (PTarget == nullptr || !m_host->WeaponSkill(PTarget->entityId(), wsid))
        {
            return false;
        }

        Debug("weapon skill", wsid, PTarget);
        return true;
    }

    void CGambits::RefreshWeaponSkills()
    {
        std::vector<TrustSkill_t> skills;
        for (uint16 id = 1; id < MAX_WEAPONSKILL_ID; ++id)
        {
            if (!charutils::hasWeaponSkill(POwner, id))
            {
                continue;
            }

            const CWeaponSkill* PWeaponSkill = battleutils::GetWeaponSkill(id);
            if (PWeaponSkill == nullptr || !charutils::canUseWeaponSkill(POwner, id))
            {
                continue;
            }

            skills.emplace_back(G_REACTION::WS,
                                id,
                                PWeaponSkill->getPrimarySkillchain(),
                                PWeaponSkill->getSecondarySkillchain(),
                                PWeaponSkill->getTertiarySkillchain(),
                                battleutils::isValidSelfTargetWeaponskill(id) ? TARGET_SELF : TARGET_ENEMY);
        }

        const bool same = skills.size() == m_tpSkills.size() &&
                          std::equal(skills.begin(), skills.end(), m_tpSkills.begin(), [](const TrustSkill_t& a, const TrustSkill_t& b)
                                     {
                                         return a.skill_id == b.skill_id;
                                     });
        if (!same)
        {
            m_tpSkills = std::move(skills);
        }
    }

    auto CGambits::PartyHasHealer() const -> bool
    {
        bool hasHealer = false;
        POwner->ForParty([&](const CBattleEntity* PMember)
                         {
                             const auto job = PMember->GetMJob();
                             if (job == xi::Job::WHM || job == xi::Job::RDM || job == xi::Job::PLD || job == xi::Job::SCH)
                             {
                                 hasHealer = true;
                             }
                         });
        return hasHealer;
    }

    auto CGambits::PartyHasTank() const -> bool
    {
        bool hasTank = false;
        POwner->ForParty([&](const CBattleEntity* PMember)
                         {
                             const auto job = PMember->GetMJob();
                             if (job == xi::Job::NIN || job == xi::Job::PLD || job == xi::Job::RUN)
                             {
                                 hasTank = true;
                             }
                         });
        return hasTank;
    }

    auto CGambits::IsOffensive(const Gambit_t& gambit) const -> bool
    {
        return std::ranges::any_of(gambit.actions, [](const Action_t& action)
                                   {
                                       return action.reaction == G_REACTION::RATTACK || action.reaction == G_REACTION::WS || action.reaction == G_REACTION::MS;
                                   });
    }

    void CGambits::Debug(const std::string_view what, const uint32 id, const CBattleEntity* PTarget) const
    {
        if (settings::get<bool>("pawn.GAMBIT_DEBUG"))
        {
            // The distance too: the server refuses an ability past its range
            // inside the state, after this line, and says nothing to us
            ShowInfoFmt("pawn: {} {} {} -> {}{}", POwner->getName(), what, id, PTarget != nullptr ? PTarget->getName() : "-",
                        PTarget != nullptr && PTarget != POwner ? fmt::format(" ({:.1f} y)", distance(POwner->loc.p, PTarget->loc.p)) : "");
        }
    }
    auto jobAbilities(CCharEntity* PChar) -> std::vector<CAbility*>
    {
        std::vector<CAbility*> out;
        if (PChar == nullptr)
        {
            return out;
        }
        for (const auto job : { PChar->GetMJob(), PChar->GetSJob() })
        {
            if (job == xi::Job::NONE)
            {
                continue;
            }
            for (auto* PAbility : ability::GetAbilities(job))
            {
                // Job lists also contain pet abilities outside the character bitfield.
                if (PAbility != nullptr && PAbility->getID() < sizeof(PChar->m_Abilities) * 8 &&
                    std::find(out.begin(), out.end(), PAbility) == out.end())
                {
                    out.push_back(PAbility);
                }
            }
        }
        return out;
    }

    auto abilitiesFor(CCharEntity* PChar) -> std::vector<CAbility*>
    {
        auto out = jobAbilities(PChar);
        std::erase_if(out, [PChar](CAbility* PAbility)
                      {
                          return !charutils::hasAbility(PChar, PAbility->getID());
                      });
        return out;
    }

    auto vocabularyFor(CCharEntity* PPawn, const bool ownClient) -> Vocabulary
    {
        Vocabulary v;
        if (PPawn == nullptr)
        {
            return v;
        }

        // The conditions, FFXII's way (the gambit review, RESEARCH §14.13):
        // each entry one clause that names its side and when, on the page
        // of that side, stored as one target and one condition. An entry
        // that takes a number carries '*' for it in its label, and its range;
        // a status entry takes the status picked in the row's next cell.
        // Upstream's trust targets (tank, melee, caster...) are two
        // conditions in one and are not offered; a row that holds one still
        // reads (headText). The tactician's mark is not offered either: it
        // is set and taken off from the row's menu, never picked as a
        // condition (RESEARCH §17.13)
        struct Range
        {
            uint16 min = 0, max = 0, step = 0, initial = 0;
        };
        struct Clause
        {
            Side        side;
            G_TARGET    target;
            G_CONDITION condition;
            Takes       takes = Takes::Nothing;
            Range       range = {};
        };
        const auto self    = G_TARGET::SELF;
        const auto ally    = G_TARGET::PARTY;
        const auto foe     = G_TARGET::TARGET;
        const auto hpBelow = Range{ 10, 90, 10, 50 };
        const auto hpAbove = Range{ 10, 100, 10, 75 };
        const auto mpBelow = Range{ 10, 90, 10, 30 };
        const auto tpAbove = Range{ 500, 3000, 500, 1000 };
        const auto clauses = std::to_array<Clause>({
            { Side::Self, self, G_CONDITION::ALWAYS },
            { Side::Self, self, G_CONDITION::HPP_LT, Takes::Number, hpBelow },
            { Side::Self, self, G_CONDITION::HPP_GTE, Takes::Number, hpAbove },
            { Side::Self, self, G_CONDITION::MPP_LT, Takes::Number, mpBelow },
            { Side::Self, self, G_CONDITION::TP_GTE, Takes::Number, tpAbove },
            { Side::Self, self, G_CONDITION::HAS_TOP_ENMITY },
            { Side::Self, self, G_CONDITION::NOT_HAS_TOP_ENMITY },
            { Side::Self, self, G_CONDITION::PT_HAS_TANK },
            { Side::Self, self, G_CONDITION::NOT_PT_HAS_TANK },
            { Side::Self, self, G_CONDITION::STATUS, Takes::Status },
            { Side::Self, self, G_CONDITION::NOT_STATUS, Takes::Status },
            { Side::Ally, ally, G_CONDITION::ALWAYS },
            { Side::Ally, ally, G_CONDITION::HPP_LT, Takes::Number, hpBelow },
            { Side::Ally, ally, G_CONDITION::HPP_GTE, Takes::Number, hpAbove },
            { Side::Ally, ally, G_CONDITION::MPP_LT, Takes::Number, mpBelow },
            { Side::Ally, ally, G_CONDITION::TP_GTE, Takes::Number, tpAbove },
            { Side::Ally, ally, G_CONDITION::STATUS, Takes::Status },
            { Side::Ally, ally, G_CONDITION::NOT_STATUS, Takes::Status },
            { Side::Ally, G_TARGET::PARTY_DEAD, G_CONDITION::ALWAYS },
            { Side::Foe, foe, G_CONDITION::ALWAYS },
            { Side::Foe, pawn::G_TARGET_LEADERS_TARGET, G_CONDITION::ALWAYS },
            { Side::Foe, pawn::G_TARGET_TARGETED_BY_ALLY, G_CONDITION::ALWAYS },
            { Side::Foe, pawn::G_TARGET_TARGETING_ALLY, G_CONDITION::ALWAYS },
            { Side::Foe, pawn::G_TARGET_TARGETING_SELF, G_CONDITION::ALWAYS },
            { Side::Foe, foe, G_CONDITION::NOT_HAS_TOP_ENMITY },
            { Side::Foe, foe, G_CONDITION::HPP_LT, Takes::Number, hpBelow },
            { Side::Foe, foe, G_CONDITION::HPP_GTE, Takes::Number, hpAbove },
            { Side::Foe, foe, G_CONDITION::TP_GTE, Takes::Number, tpAbove },
            { Side::Foe, foe, G_CONDITION::SC_AVAILABLE },
            { Side::Foe, foe, G_CONDITION::MB_AVAILABLE },
            { Side::Foe, foe, G_CONDITION::STATUS, Takes::Status },
            { Side::Foe, foe, G_CONDITION::NOT_STATUS, Takes::Status },
        });
        for (const auto& c : clauses)
        {
            // Tactician's choice leaves the when to a tactician, and a
            // played character has none
            if (ownClient && c.condition == pawn::G_CONDITION_TACTICIANS_CHOICE)
            {
                continue;
            }
            const auto target = static_cast<std::size_t>(c.target);
            const auto value  = c.takes == Takes::Number ? std::string("*") : std::string();
            const auto words  = c.condition == G_CONDITION::ALWAYS ? std::string() : conditionWords(c.condition, value, onFoe(target));
            v.conditions.push_back({ c.target, c.condition, c.takes, c.side, c.range.min, c.range.max, c.range.step, c.range.initial, headText(target, words) });
        }

        // The statuses "status =" and "status ≠" can name: Enfeeble first,
        // the group (ailments.h), then the ones a party fights and buffs
        // with (KO is the ally's own entry). Ids are xi::StatusEffect.
        v.statuses.push_back({ static_cast<uint16>(pawn::G_STATUS_ENFEEBLE), statusName(pawn::G_STATUS_ENFEEBLE) });
        for (const uint16 id : { 1u, 2u, 3u, 4u, 5u, 6u, 7u, 8u, 9u, 10u, 11u, 12u, 13u, 15u, 16u, 28u, 31u,
                                 33u, 36u, 37u, 40u, 41u, 42u, 43u, 56u, 57u, 58u, 66u, 68u, 158u })
        {
            v.statuses.push_back({ id, statusName(id) });
        }

        // Attack: which fight she takes, with a Foe target (engage_math.h)
        const auto behaviour = [](const pawn::Behavior b)
        {
            return static_cast<G_SELECT>(b);
        };
        v.actions = {
            { G_REACTION::ATTACK, G_SELECT::HIGHEST, 0, "Attack", ActionGroup::Fight, TARGET_ENEMY },
        };
        // The behaviours move a cardian or speak to her tactician: a played
        // character's client moves him, and he has no tactician
        if (!ownClient)
        {
            v.actions.push_back({ G_REACTION_BEHAVIOR, behaviour(pawn::Behavior::AvoidAggro), 1, "Avoid aggro", ActionGroup::Behaviours });
            v.actions.push_back({ G_REACTION_BEHAVIOR, behaviour(pawn::Behavior::AvoidLinks), 1, "Avoid links", ActionGroup::Behaviours });
            v.actions.push_back({ G_REACTION_BEHAVIOR, behaviour(pawn::Behavior::Rest), 1, "Rest", ActionGroup::Behaviours });
            v.actions.push_back({ G_REACTION_BEHAVIOR, behaviour(pawn::Behavior::RestWithPlayer), 1, "Rest with the player", ActionGroup::Behaviours });
            v.actions.push_back({ G_REACTION_BEHAVIOR, behaviour(pawn::Behavior::HomePointWithPlayer), 1, "Home point with the player", ActionGroup::Behaviours });
        }

        // The actions of her main job and her support job, at every level, in
        // a fixed order, each marked whether she can use it now (RESEARCH
        // §14.13): what she has not learned yet reads greyed. The command
        // window lists only what she can use.
        v.mjob = static_cast<uint8>(PPawn->GetMJob());
        v.mlvl = PPawn->GetMLevel();
        v.sjob = static_cast<uint8>(PPawn->GetSJob());
        v.slvl = PPawn->GetSLevel();
        const auto ofHerJobs = [PPawn](auto* PAction)
        {
            for (const auto job : { PPawn->GetMJob(), PPawn->GetSJob() })
            {
                const auto level = job != xi::Job::NONE ? PAction->getJob(job) : 0;
                if (level != 0 && level != 255)
                {
                    return true;
                }
            }
            return false;
        };

        // Enfeeble heads her Magic when her jobs cast any of its enfeebles
        // (tactician_line.h kEnfeebleOrder), usable once she can cast one
        {
            bool ofHers = false;
            bool usable = false;
            for (const auto id : cardian::tactician::kEnfeebleOrder)
            {
                auto* PSpell = spell::GetSpell(static_cast<SpellID>(id));
                ofHers       = ofHers || (PSpell != nullptr && ofHerJobs(PSpell));
                usable       = usable || (PSpell != nullptr && CSpellBook::Eligible(PPawn, PSpell));
            }
            if (ofHers)
            {
                VocabAction enfeeble{ G_REACTION::MA, pawn::G_SELECT_ENFEEBLE, 0, "Enfeeble", ActionGroup::Magic, TARGET_ENEMY };
                enfeeble.usable = usable;
                v.actions.push_back(std::move(enfeeble));
            }
        }

        // Damage spell (any) follows when her jobs cast any damage spell
        // (spell_bank.h isNuke), usable once she can cast one
        {
            bool ofHers = false;
            bool usable = false;
            for (uint16 id = 1; id < MAX_SPELL_ID && !usable; ++id)
            {
                auto* PSpell = spell::GetSpell(static_cast<SpellID>(id));
                if (PSpell == nullptr || !pawn::tactics::bank::isNuke(PSpell) || !ofHerJobs(PSpell))
                {
                    continue;
                }
                ofHers = true;
                usable = CSpellBook::Eligible(PPawn, PSpell);
            }
            if (ofHers)
            {
                VocabAction nuke{ G_REACTION::MA, G_SELECT::RANDOM, 0, "Damage spell (any)", ActionGroup::Magic, TARGET_ENEMY };
                nuke.usable = usable;
                v.actions.push_back(std::move(nuke));
            }
        }

        // Magic in spell order, a family's "(best)" just before its first
        // spell when she has more than one spell of it to choose among
        std::vector<CSpell*> spells;
        std::vector<SPELLFAMILY> hers;
        for (uint16 id = 1; id < MAX_SPELL_ID; ++id)
        {
            auto* PSpell = spell::GetSpell(static_cast<SpellID>(id));
            if (PSpell == nullptr || PSpell->getSpellGroup() == SPELLGROUP_TRUST || PSpell->getName().empty() || !ofHerJobs(PSpell))
            {
                continue;
            }
            spells.push_back(PSpell);
            if (PSpell->getSpellFamily() != SPELLFAMILY_NONE && CSpellBook::Eligible(PPawn, PSpell))
            {
                hers.push_back(PSpell->getSpellFamily());
            }
        }
        std::vector<SPELLFAMILY> named;
        for (auto* PSpell : spells)
        {
            const auto family = PSpell->getSpellFamily();
            const auto kin    = std::ranges::count_if(spells, [family](CSpell* PKin)
                                                      {
                                                          return PKin->getSpellFamily() == family;
                                                      });
            if (family != SPELLFAMILY_NONE && kin > 1 && std::ranges::find(named, family) == named.end())
            {
                named.push_back(family);
                VocabAction best{ G_REACTION::MA, G_SELECT::HIGHEST, static_cast<uint32>(family), familyName(static_cast<uint32>(family)) + " (best)", ActionGroup::Magic };
                best.usable = std::ranges::find(hers, family) != hers.end();
                v.actions.push_back(std::move(best));
            }
            const auto id = static_cast<uint16>(PSpell->getID());
            VocabAction spell{ G_REACTION::MA, G_SELECT::SPECIFIC, id, titleCase(PSpell->getName()), ActionGroup::Magic, PSpell->getValidTarget(), PSpell->getMPCost() };
            spell.usable = CSpellBook::Eligible(PPawn, PSpell);
            v.actions.push_back(std::move(spell));
        }

        // Abilities, her main job's then her support job's, as each job's
        // list orders them
        for (auto* PAbility : jobAbilities(PPawn))
        {
            VocabAction ability{ G_REACTION::JA, G_SELECT::SPECIFIC, PAbility->getID(), titleCase(PAbility->getName()), ActionGroup::Abilities, PAbility->getValidTarget() };
            ability.usable = charutils::hasAbility(PPawn, PAbility->getID());
            v.actions.push_back(std::move(ability));
        }

        // A level-up rebuilds her learned-weapon-skill bitfield, and a
        // skill-up adds a weapon skill only when the skill lands exactly on
        // its unlock value: a rise past it is missed, and a player picks the
        // skill up on the next zone or gear change. A cardian does neither,
        // so the list is rebuilt here, where it is about to be read.
        charutils::BuildingCharWeaponSkills(PPawn);

        v.actions.push_back({ G_REACTION::WS, G_SELECT::HIGHEST, 0, "Weapon skill (best)", ActionGroup::WeaponSkills });
        v.actions.push_back({ G_REACTION::WS, G_SELECT::RANDOM, 0, "Weapon skill (any)", ActionGroup::WeaponSkills });
        for (uint16 id = 1; id < MAX_WEAPONSKILL_ID; ++id)
        {
            auto* PWeaponSkill = battleutils::GetWeaponSkill(id);
            if (PWeaponSkill == nullptr || PWeaponSkill->getName().empty() || !ofHerJobs(PWeaponSkill))
            {
                continue;
            }
            VocabAction skill{ G_REACTION::WS, G_SELECT::SPECIFIC, id, titleCase(PWeaponSkill->getName()), ActionGroup::WeaponSkills, TARGET_ENEMY };
            skill.usable = charutils::hasWeaponSkill(PPawn, id) && charutils::canUseWeaponSkill(PPawn, id);
            v.actions.push_back(std::move(skill));
        }

        v.actions.push_back({ G_REACTION::RATTACK, G_SELECT::HIGHEST, 0, "Ranged Attack", ActionGroup::Ranged, TARGET_ENEMY });
        return v;
    }

    auto partyAlreadyCasting(CCharEntity* PCaster, CSpell* PSpell, const CBattleEntity* PTarget) -> bool
    {
        bool redundant = false;
        PCaster->ForParty([&](const CBattleEntity* PMember)
                          {
                              if (redundant || PMember == PCaster || !PMember->PAI->IsCurrentState<CMagicState>())
                              {
                                  return;
                              }

                              auto*       MState  = static_cast<CMagicState*>(PMember->PAI->GetCurrentState());
                              auto*       MSpell  = MState->GetSpell();
                              const auto* MTarget = MState->target().resolve();
                              if (MSpell == nullptr || PTarget == nullptr || MTarget != PTarget)
                              {
                                  return;
                              }

                              const bool sameFamily   = PSpell->getSpellFamily() == MSpell->getSpellFamily();
                              const bool weakerOrSame = PSpell->getID() <= MSpell->getID();

                              if ((PSpell->isBuff() || PSpell->isDebuff()) && sameFamily && weakerOrSame)
                              {
                                  redundant = true;
                              }
                              else if (PSpell->isCure() && MSpell->isCure() && PTarget->GetHPP() > 50)
                              {
                                  redundant = true;
                              }
                              else if (PSpell->isNa() && MSpell->isNa() && sameFamily && PSpell->getID() == MSpell->getID())
                              {
                                  redundant = true;
                              }
                          });
        return redundant;
    }

    auto topEnmityOf(const CBattleEntity* PEntity) -> CBattleEntity*
    {
        if (const auto* PMob = dynamic_cast<CMobEntity*>(PEntity->GetBattleTarget()))
        {
            return PMob->PEnmityContainer->GetHighestEnmity();
        }
        return nullptr;
    }

    auto allyOn(const CCharEntity* PSelf, const CBattleEntity* PFoe) -> const CCharEntity*
    {
        if (PSelf->PParty == nullptr)
        {
            return nullptr;
        }
        for (auto* PMember : PSelf->PParty->members)
        {
            const auto* PChar = dynamic_cast<const CCharEntity*>(PMember);
            if (PChar != nullptr && PChar != PSelf && PChar->loc.zone == PSelf->loc.zone && PChar->PAI->IsEngaged() &&
                PChar->GetBattleTarget() == PFoe)
            {
                return PChar;
            }
        }
        return nullptr;
    }

    auto foeFacts(const CCharEntity* PSelf, CBattleEntity* PFoe, const CCharEntity* PLeader, const bool heldOff) -> cardian::engage::Foe
    {
        cardian::engage::Foe f;
        if (PFoe == nullptr)
        {
            return f;
        }
        auto* PMob      = dynamic_cast<CMobEntity*>(PFoe);
        f.leadersTarget = PMob != nullptr && PLeader != nullptr && PLeader->PAI->IsEngaged() && PLeader->GetBattleTarget() == PFoe;
        f.allysFight    = allyOn(PSelf, PFoe) != nullptr;
        if (PMob != nullptr && PMob->PAI->IsEngaged())
        {
            // The departing player's old aggro is not a new fight here
            auto*       PVictim = PMob->GetBattleTarget();
            const auto* PChar   = dynamic_cast<const CCharEntity*>(PVictim);
            if (PVictim != nullptr && (PChar == nullptr || !PChar->requestedZoneChange))
            {
                f.onSelf  = PVictim == PSelf;
                f.onParty = f.onSelf || (PSelf->PParty != nullptr && PVictim->PParty == PSelf->PParty);
            }
        }
        // Underground with no fight on, it is not a fight yet: the party waits,
        // weapons away, and takes it when it surfaces (the combat tick lets such
        // a target go)
        f.underground = PMob != nullptr && pawn::isUnderground(PMob) && !PMob->PAI->IsEngaged();
        f.heldOff     = heldOff;
        return f;
    }

auto isMeleeJob(const xi::Job job) -> bool
{
    return kMeleeJobs.contains(job);
}
} // namespace pawn
