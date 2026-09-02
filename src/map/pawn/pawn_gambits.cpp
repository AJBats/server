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
#include "pawn.h"
#include "pawn_controller.h"

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
#include "entities/char_entity.h"
#include "recast_container.h"
#include "status_effect.h"
#include "status_effect_container.h"
#include "utils/battleutils.h"
#include "utils/charutils.h"
#include "weapon_skill.h"

#include <algorithm>
#include <list>
#include <set>

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
    } // namespace

    CGambits::CGambits(CCharEntity* PPawn, CPawnController* PController)
    : POwner(PPawn)
    , m_PController(PController)
    , m_spellBook(PPawn)
    {
    }

    auto defaultRows() -> const std::vector<std::pair<std::string, bool>>&
    {
        static const std::vector<std::pair<std::string, bool>> rows{
            { "0|0:0|100:1:1|0", true }, // Self -> Avoid aggro
            { "0|0:0|100:6:1|0", true }, // Self -> Rest with the player
        };
        return rows;
    }

    auto CGambits::AddGambit(Gambit_t gambit, const bool enabled) -> std::string
    {
        gambit.identifier = fmt::format("{}", ++m_nextId);
        gambit.last_used  = {};
        m_gambits.push_back(GambitRow{ std::move(gambit), enabled });
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
    }

    void CGambits::RemoveAllGambits()
    {
        m_gambits.clear();
        m_timerConditionLastTrigger.clear();
    }

    void CGambits::SetTPSkillSettings(const G_TP_TRIGGER trigger, const G_SELECT select, const uint16 value)
    {
        m_tpTrigger = trigger;
        m_tpSelect  = select;
        m_tpValue   = value;
    }

    void CGambits::Tick(const timer::time_point tick, const bool engaged)
    {
        TracyZoneScoped;

        // Stagger pawns so a party doesn't think in lockstep
        const auto positionOffset = std::chrono::milliseconds(m_PController->GetPawnPartyPosition() * 100);
        if (tick + positionOffset < m_lastAction)
        {
            return;
        }

        if (POwner->PAI->IsCurrentState<CAbilityState>() || POwner->PAI->IsCurrentState<CRangeState>() ||
            POwner->PAI->IsCurrentState<CMagicState>() || POwner->PAI->IsCurrentState<CWeaponSkillState>() ||
            POwner->PAI->IsCurrentState<CMobSkillState>() || POwner->PAI->IsCurrentState<CPetSkillState>())
        {
            return;
        }

        m_spellBook.Refresh();
        RefreshWeaponSkills();

        m_lastAction = tick + std::chrono::milliseconds(xirand::GetRandomNumber(2000, 3000));

        // The master switch: nothing of her own -- no weapon skill, no row
        if (!m_masterOn)
        {
            return;
        }

        if (engaged && POwner->health.tp >= 1000 && TryWeaponSkill())
        {
            return;
        }

        for (auto& row : m_gambits)
        {
            auto& gambit = row.gambit;
            if (!row.enabled || IsBehavior(gambit) || tick < gambit.last_used + std::chrono::seconds(gambit.retry_delay))
            {
                continue;
            }

            if (!engaged && IsOffensive(gambit))
            {
                continue;
            }

            CBattleEntity* PTarget = SelectTarget(gambit);
            if (PTarget == nullptr)
            {
                continue;
            }

            if (Execute(gambit, PTarget, engaged))
            {
                if (gambit.retry_delay != 0)
                {
                    gambit.last_used = tick;
                }
                break;
            }
        }
    }

    auto CGambits::Candidates(const G_TARGET selector) -> std::vector<CBattleEntity*>
    {
        std::vector<CBattleEntity*> out;

        auto nearbyAlive = [this](CBattleEntity* PMember)
        {
            return PMember != nullptr && PMember->isAlive() && PMember->loc.zone == POwner->loc.zone &&
                   distance(POwner->loc.p, PMember->loc.p) <= 15.0f;
        };

        auto collect = [&](auto&& accept)
        {
            POwner->ForParty([&](CBattleEntity* PMember)
                             {
                                 if (nearbyAlive(PMember) && accept(PMember))
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
                if (auto* PMob = POwner->GetBattleTarget())
                {
                    out.push_back(PMob);
                }
                break;
            }
            case G_TARGET::MASTER:
            {
                if (auto* PPlayer = m_PController->GetLivePlayer())
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
                                     if (PMember != nullptr && PMember->isDead() && PMember->loc.zone == POwner->loc.zone &&
                                         distance(POwner->loc.p, PMember->loc.p) <= 20.0f)
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
                if (const auto* PTop = m_PController->GetTopEnmity())
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
                // CURILLA is a trust NPC special; PARTY_MULTI is unimplemented upstream too
                break;
            }
        }

        return out;
    }

    auto CGambits::SelectTarget(const Gambit_t& gambit) -> CBattleEntity*
    {
        for (auto* PCandidate : Candidates(gambit.target_selector))
        {
            bool matches = true;
            for (std::size_t groupIndex = 0; groupIndex < gambit.predicate_groups.size(); ++groupIndex)
            {
                if (!CheckTrigger(PCandidate, gambit, groupIndex))
                {
                    matches = false;
                    break;
                }
            }

            if (!matches)
            {
                continue;
            }

            switch (gambit.target_selector)
            {
                case G_TARGET::TRIGGER_SELF_ACTION_TARGET:
                    return POwner->GetBattleTarget();
                case G_TARGET::TRIGGER_TARGET_ACTION_SELF:
                    return POwner;
                default:
                    return PCandidate;
            }
        }
        return nullptr;
    }

    auto CGambits::CheckTrigger(CBattleEntity* PTrigger, const Gambit_t& gambit, const std::size_t groupIndex) -> bool
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
                    results.push_back(PTrigger->StatusEffectContainer->HasStatusEffect(static_cast<xi::StatusEffect>(arg)));
                    break;
                case G_CONDITION::NOT_STATUS:
                    results.push_back(!PTrigger->StatusEffectContainer->HasStatusEffect(static_cast<xi::StatusEffect>(arg)));
                    break;
                case G_CONDITION::STATUS_FLAG:
                    results.push_back(PTrigger->StatusEffectContainer->HasStatusEffectByFlag(static_cast<xi::StatusEffectFlag>(arg)));
                    break;
                case G_CONDITION::TIMER:
                {
                    if (arg == 0)
                    {
                        results.push_back(true);
                        break;
                    }

                    const auto key      = fmt::format("{}:{}:{}", gambit.identifier, groupIndex, predicateIndex);
                    const auto interval = std::chrono::seconds(arg);
                    const auto now      = timer::now();

                    auto [it, inserted] = m_timerConditionLastTrigger.try_emplace(key, now);
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
                    const auto* PAbility = ability::GetAbility(static_cast<uint16>(arg));
                    results.push_back(PAbility != nullptr && POwner->PRecastContainer->Has(RECAST_ABILITY, PAbility->getRecastId()));
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
                {
                    const auto* PTop = m_PController->GetTopEnmity();
                    results.push_back(PTop != nullptr && PTop->targid == POwner->targid);
                    break;
                }
                case G_CONDITION::NOT_HAS_TOP_ENMITY:
                {
                    const auto* PTop = m_PController->GetTopEnmity();
                    results.push_back(PTop != nullptr && PTop->targid != POwner->targid);
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
                return m_spellBook.GetAvailable(static_cast<SpellID>(action.select_arg));
            case G_SELECT::HIGHEST:
                return m_spellBook.GetBestAvailable(static_cast<SPELLFAMILY>(action.select_arg));
            case G_SELECT::RANDOM:
                return m_spellBook.GetRandomDamageSpell();
            case G_SELECT::BEST_INDI:
                return m_spellBook.GetBestIndiSpell(m_PController->GetLivePlayer());
            case G_SELECT::ENTRUSTED:
                return m_spellBook.GetBestEntrustedSpell(m_PController->GetLivePlayer());
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
        m_PController->ClearGambitBehaviors();
        if (!m_masterOn)
        {
            return;
        }
        for (const auto& row : m_gambits)
        {
            if (row.enabled && IsBehavior(row.gambit) && SelectTarget(row.gambit) != nullptr)
            {
                ApplyBehavior(row.gambit);
            }
        }
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
            m_PController->SetGambitBehavior(static_cast<uint16>(action.select), static_cast<uint16>(action.select_arg));
        }
    }

    void CGambits::SetBehaviorRow(const pawn::Behavior behavior, const uint16 arg)
    {
        static constexpr std::array<std::string_view, pawn::BehaviorCount> names{ "?", "avoid aggro", "?", "?", "formation", "?", "rest with player", "home point with player" };
        const auto                                                         name = names[std::min<std::size_t>(static_cast<std::size_t>(behavior), names.size() - 1)];
        const bool                                                         sw   = pawn::isSwitch(behavior);

        const auto unconditional = [&](const GambitRow& row)
        {
            const auto& g = row.gambit;
            return IsBehavior(g) && g.actions.size() == 1 && static_cast<pawn::Behavior>(g.actions[0].select) == behavior &&
                   g.predicate_groups.size() == 1 && g.predicate_groups[0].predicates.size() == 1 &&
                   g.predicate_groups[0].predicates[0].condition == G_CONDITION::ALWAYS;
        };
        if (const auto it = std::find_if(m_gambits.begin(), m_gambits.end(), unconditional); it != m_gambits.end())
        {
            it->gambit.actions[0].select_arg = sw ? 1 : arg;
            it->enabled                      = sw ? arg != 0 : true;
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
        if (index == 0 || index > m_gambits.size())
        {
            return false;
        }
        m_gambits[index - 1].enabled = on;
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
        }
        return true;
    }

    auto CGambits::Erase(const std::size_t index) -> bool
    {
        if (index == 0 || index > m_gambits.size())
        {
            return false;
        }
        m_gambits.erase(m_gambits.begin() + static_cast<std::ptrdiff_t>(index - 1));
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
        return true;
    }

    auto CGambits::Replace(const std::size_t index, Gambit_t gambit) -> bool
    {
        if (index == 0 || index > m_gambits.size())
        {
            return false;
        }
        gambit.identifier          = fmt::format("{}", ++m_nextId);
        gambit.last_used           = {};
        m_gambits[index - 1].gambit = std::move(gambit);
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

        auto familyName(const uint32 family) -> std::string
        {
            auto name = std::string(magic_enum::enum_name(static_cast<SPELLFAMILY>(family)));
            if (name.rfind("SPELLFAMILY_", 0) == 0)
            {
                name.erase(0, 12);
            }
            return name.empty() ? fmt::format("family {}", family) : titleCase(name);
        }

        // The target names, by G_TARGET; the vocabulary and the row labels
        // read from the same table so a picker never renames a row.
        auto targetName(const std::size_t target) -> std::string_view
        {
            static constexpr std::array<std::string_view, 14> names{ "Self", "Party member", "Target", "The player", "Tank", "Melee", "Ranged", "Casters", "Top enmity", "Curilla", "Dead ally", "Party member", "Self", "Target" };
            return target < names.size() ? names[target] : std::string_view("?");
        }

        auto conditionText(const Predicate_t& p) -> std::string
        {
            const auto arg = p.condition_arg;
            switch (p.condition)
            {
                case G_CONDITION::ALWAYS:
                    return "always";
                case G_CONDITION::HPP_LT:
                    return fmt::format("HP below {}%", arg);
                case G_CONDITION::HPP_GTE:
                    return fmt::format("HP at least {}%", arg);
                case G_CONDITION::MPP_LT:
                    return fmt::format("MP below {}%", arg);
                case G_CONDITION::MPP_GTE:
                    return fmt::format("MP at least {}%", arg);
                case G_CONDITION::TP_LT:
                    return fmt::format("TP below {}", arg);
                case G_CONDITION::TP_GTE:
                    return fmt::format("TP at least {}", arg);
                case G_CONDITION::LVL_LT:
                    return fmt::format("level < {}", arg);
                case G_CONDITION::LVL_GTE:
                    return fmt::format("level >= {}", arg);
                case G_CONDITION::STATUS:
                    return fmt::format("has {}", titleCase(effects::GetEffectName(static_cast<uint16>(arg))));
                case G_CONDITION::NOT_STATUS:
                    return fmt::format("no {}", titleCase(effects::GetEffectName(static_cast<uint16>(arg))));
                case G_CONDITION::STATUS_FLAG:
                    return fmt::format("status flag {}", arg);
                case G_CONDITION::HAS_TOP_ENMITY:
                    return "holds hate";
                case G_CONDITION::NOT_HAS_TOP_ENMITY:
                    return "does not hold hate";
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
                    return fmt::format("{}% of the time", arg);
                case G_CONDITION::NO_SAMBA:
                    return "no samba up";
                case G_CONDITION::NO_STORM:
                    return "no storm up";
                case G_CONDITION::PT_HAS_TANK:
                    return "party has a tank";
                case G_CONDITION::NOT_PT_HAS_TANK:
                    return "no tank in the party";
                case G_CONDITION::IS_ECOSYSTEM:
                    return fmt::format("ecosystem {}", arg);
                case G_CONDITION::HP_MISSING:
                    return fmt::format("missing {} HP", arg);
                case G_CONDITION::JA_ON_COOLDOWN:
                    return fmt::format("{} on cooldown", ability::GetAbility(static_cast<uint16>(arg)) != nullptr ? ability::GetAbility(static_cast<uint16>(arg))->getName() : std::to_string(arg));
                case G_CONDITION::TIMER:
                    return fmt::format("every {}s", arg);
                case pawn::G_CONDITION_STRATEGY:
                    return fmt::format("strategy {}", arg);
                default:
                    return fmt::format("condition {}:{}", static_cast<uint16>(p.condition), arg);
            }
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
                case pawn::Behavior::Formation:
                    return fmt::format("Formation: {}", static_cast<pawn::Slot>(a.select_arg) == pawn::Slot::Lead ? "lead" : "follow");
                case pawn::Behavior::RestWithPlayer:
                    return fmt::format("Rest with the player{}", off);
                case pawn::Behavior::HomePointWithPlayer:
                    return fmt::format("Home point with the player{}", off);
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
                            return familyName(a.select_arg) + " (random)";
                        case G_SELECT::MB_ELEMENT:
                            return "Magic burst";
                        case G_SELECT::ENTRUSTED:
                            return "Entrust " + familyName(a.select_arg);
                        case G_SELECT::BEST_INDI:
                            return "Best indi";
                        case G_SELECT::BEST_AGAINST_TARGET:
                            return familyName(a.select_arg) + " (best against target)";
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
                    return a.select == G_SELECT::RANDOM ? "Any weapon skill" : "Best weapon skill";
                }
                case G_REACTION::RATTACK:
                    return "Ranged attack";
                case G_REACTION::ATTACK:
                    return "Attack";
                default:
                    return fmt::format("action {}:{}:{}", static_cast<uint16>(a.reaction), static_cast<uint16>(a.select), a.select_arg);
            }
        }
    } // namespace

    auto labelGambit(const Gambit_t& g) -> std::string
    {
        std::string out(targetName(static_cast<std::size_t>(g.target_selector)));
        std::string conditions;
        for (const auto& group : g.predicate_groups)
        {
            for (std::size_t i = 0; i < group.predicates.size(); ++i)
            {
                if (!conditions.empty())
                {
                    conditions += (i != 0 && group.logic == G_LOGIC::OR) ? " or " : ", ";
                }
                conditions += conditionText(group.predicates[i]);
            }
        }
        if (conditions != "always")
        {
            out += ": " + conditions;
        }
        out += " -> ";
        for (std::size_t i = 0; i < g.actions.size(); ++i)
        {
            if (i != 0)
            {
                out += " + ";
            }
            out += actionText(g.actions[i]);
        }
        if (g.retry_delay != 0)
        {
            out += fmt::format(" (every {}s)", g.retry_delay);
        }
        return out;
    }

    auto CGambits::Execute(const Gambit_t& gambit, CBattleEntity* PTarget, const bool engaged) -> bool
    {
        bool spellSeen = false;

        for (const auto& action : gambit.actions)
        {
            bool executed = false;

            switch (action.reaction)
            {
                case G_REACTION::RATTACK:
                {
                    if (engaged && m_PController->RangedAttack(PTarget->entityId()))
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

                    const auto spellId = ResolveSpell(action, PTarget);
                    if (!spellId.has_value())
                    {
                        break;
                    }

                    // Entrust goes on the player; every other cast target is
                    // the gambit's (self-target spells are redirected by the
                    // controller)
                    CBattleEntity* PCastTarget = action.select == G_SELECT::ENTRUSTED ? m_PController->GetLivePlayer() : PTarget;
                    if (PCastTarget != nullptr && m_PController->Cast(PCastTarget->entityId(), *spellId))
                    {
                        Debug("cast", static_cast<uint32>(*spellId), PCastTarget);
                        executed = true;
                    }
                    break;
                }
                case G_REACTION::JA:
                {
                    executed = ExecuteAbility(action, PTarget, engaged);
                    break;
                }
                case G_REACTION::WS:
                {
                    executed = ExecuteWeaponSkill(action, engaged);
                    break;
                }
                default:
                {
                    // ATTACK, MS, ANIM_STRING: no character equivalent
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
            PJATarget = POwner->GetBattleTarget();
        }
        else if ((valid & (TARGET_PLAYER_PARTY | TARGET_PLAYER)) && PTarget->allegiance == POwner->allegiance)
        {
            PJATarget = PTarget;
        }

        if (PJATarget == nullptr || !m_PController->Ability(PJATarget->entityId(), PAbility->getID()))
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
                break;
            }
            case G_SELECT::RANDOM:
            {
                if (m_tpSkills.empty())
                {
                    return false;
                }
                wsid = static_cast<uint16>(xirand::GetRandomElement(m_tpSkills).skill_id);
                break;
            }
            default:
                return false;
        }

        CBattleEntity* PTarget = battleutils::isValidSelfTargetWeaponskill(wsid) ? POwner : POwner->GetBattleTarget();
        if (PTarget == nullptr || !m_PController->WeaponSkill(PTarget->entityId(), wsid))
        {
            return false;
        }

        Debug("weapon skill", wsid, PTarget);
        return true;
    }

    auto CGambits::TryWeaponSkill() -> bool
    {
        TracyZoneScoped;

        CBattleEntity* PTarget = POwner->GetBattleTarget();
        if (PTarget == nullptr || m_tpSkills.empty())
        {
            return false;
        }

        const int16 tp = POwner->health.tp;

        auto triggered = [&]() -> bool
        {
            if (tp >= 3000)
            {
                return true;
            }

            switch (m_tpTrigger)
            {
                case G_TP_TRIGGER::ASAP:
                    return true;
                case G_TP_TRIGGER::RANDOM:
                {
                    const auto threshold = std::max<uint16>(m_tpValue, 1000);
                    return tp >= threshold && xirand::GetRandomNumber<uint16>(10000) < threshold;
                }
                case G_TP_TRIGGER::OPENER:
                {
                    const auto threshold = std::max<uint16>(m_tpValue, 1000);
                    bool       partnerReady = false;
                    POwner->ForParty([&](const CBattleEntity* PMember)
                                     {
                                         if (PMember != POwner && PMember->health.tp >= threshold)
                                         {
                                             partnerReady = true;
                                         }
                                     });
                    return partnerReady;
                }
                case G_TP_TRIGGER::CLOSER:
                {
                    const auto* PSCEffect = openWindow(PTarget);
                    return PSCEffect != nullptr && PSCEffect->GetTier() == 0;
                }
                case G_TP_TRIGGER::CLOSER_UNTIL_TP:
                {
                    if (tp >= std::max<uint16>(m_tpValue, 1500))
                    {
                        return true;
                    }
                    const auto* PSCEffect = openWindow(PTarget);
                    return PSCEffect != nullptr && PSCEffect->GetTier() == 0;
                }
                default:
                    return false;
            }
        };

        if (!triggered())
        {
            return false;
        }

        Maybe<TrustSkill_t> chosen;

        if (const auto* PSCEffect = PTarget->StatusEffectContainer->GetStatusEffect(xi::StatusEffect::Skillchain))
        {
            // A chain is open: close it with the best possible skillchain, or
            // hold TP rather than break it
            SKILLCHAIN_ELEMENT best      = SC_NONE;
            const auto         resonance = resonanceOf(PSCEffect);
            for (const auto& skill : m_tpSkills)
            {
                if (const auto possible = battleutils::FormSkillchain(resonance, propertiesOf(skill)); possible != SC_NONE && possible >= best)
                {
                    chosen = skill;
                    best   = possible;
                }
            }
        }
        else if (m_tpSelect == G_SELECT::RANDOM)
        {
            chosen = xirand::GetRandomElement(m_tpSkills);
        }
        else
        {
            chosen = m_tpSkills.back();
        }

        if (!chosen.has_value())
        {
            return false;
        }

        CBattleEntity* PWSTarget = (chosen->valid_targets & TARGET_SELF) ? POwner : PTarget;
        const auto     wsid      = static_cast<uint16>(chosen->skill_id);
        if (!m_PController->WeaponSkill(PWSTarget->entityId(), wsid))
        {
            return false;
        }

        Debug("weapon skill", wsid, PWSTarget);
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
            ShowInfoFmt("pawn: {} {} {} -> {}", POwner->getName(), what, id, PTarget != nullptr ? PTarget->getName() : "-");
        }
    }
    auto vocabularyFor(CCharEntity* PPawn) -> Vocabulary
    {
        Vocabulary v;
        if (PPawn == nullptr)
        {
            return v;
        }

        for (const std::size_t id : { 0u, 1u, 2u, 3u, 4u, 5u, 6u, 7u, 8u, 10u })
        {
            v.targets.push_back({ fmt::format("{}", id), std::string(targetName(id)), "" });
        }

        // Numeric conditions are one entry per comparator: the key and label
        // carry a '*' where the number goes, and group carries its range as
        // min,max,step,default. The number itself is picked on the row.
        v.conditions.push_back({ "0:0", "Always", "" });
        v.conditions.push_back({ "1:*", "HP below *%", "10,90,10,50" });
        v.conditions.push_back({ "2:*", "HP at least *%", "10,100,10,75" });
        v.conditions.push_back({ "3:*", "MP below *%", "10,90,10,30" });
        v.conditions.push_back({ "6:*", "TP at least *", "500,3000,500,1000" });
        v.conditions.push_back({ "12:0", "Holds hate", "" });
        v.conditions.push_back({ "13:0", "Does not hold hate", "" });
        v.conditions.push_back({ "25:0", "Party has a tank", "" });
        v.conditions.push_back({ "26:0", "No tank in the party", "" });
        v.conditions.push_back({ "14:0", "Skillchain open", "" });
        v.conditions.push_back({ "16:0", "Magic burst open", "" });

        // The statuses "has X" / "no X" can name: the ones a party fights
        // and buffs with. Ids are xi::StatusEffect.
        for (const uint16 id : { 0u, 1u, 2u, 3u, 4u, 5u, 6u, 7u, 8u, 9u, 10u, 11u, 12u, 13u, 15u, 16u, 28u, 31u,
                                 33u, 36u, 37u, 40u, 41u, 42u, 43u, 56u, 57u, 58u, 66u, 68u, 158u })
        {
            v.statuses.push_back({ fmt::format("{}", id), titleCase(effects::GetEffectName(id)), "" });
        }

        v.actions = {
            { "100:1:1", "Avoid aggro", "Behaviours" },
            { "100:6:1", "Rest with the player", "Behaviours" },
            { "100:7:1", "Home point with the player", "Behaviours" },
            { "100:4:1", "Formation: lead", "Behaviours" },
            { "100:4:0", "Formation: follow", "Behaviours" },
        };

        // Magic she knows and can cast now, plus "best of the family" for
        // every family she has a spell in
        std::vector<SPELLFAMILY> families;
        for (uint16 id = 1; id < MAX_SPELL_ID; ++id)
        {
            auto* PSpell = spell::GetSpell(static_cast<SpellID>(id));
            if (PSpell == nullptr || !charutils::hasSpell(PPawn, id) || !spell::CanUseSpell(PPawn, PSpell))
            {
                continue;
            }
            v.actions.push_back({ fmt::format("2:2:{}", id), titleCase(PSpell->getName()), "Magic" });
            if (const auto family = PSpell->getSpellFamily(); family != SPELLFAMILY_NONE && std::find(families.begin(), families.end(), family) == families.end())
            {
                families.push_back(family);
            }
        }
        for (const auto family : families)
        {
            v.actions.push_back({ fmt::format("2:0:{}", static_cast<uint32>(family)), familyName(static_cast<uint32>(family)) + " (best)", "Magic" });
        }

        // Job abilities she has (the ability table is built per job and level)
        for (const auto job : { PPawn->GetMJob(), PPawn->GetSJob() })
        {
            for (auto* PAbility : ability::GetAbilities(job))
            {
                if (PAbility != nullptr && charutils::hasAbility(PPawn, PAbility->getID()))
                {
                    const auto key = fmt::format("3:2:{}", PAbility->getID());
                    if (std::none_of(v.actions.begin(), v.actions.end(), [&](const VocabEntry& e) { return e.key == key; }))
                    {
                        v.actions.push_back({ key, titleCase(PAbility->getName()), "Abilities" });
                    }
                }
            }
        }

        v.actions.push_back({ "4:0:0", "Best weapon skill", "WeaponSkills" });
        v.actions.push_back({ "4:3:0", "Any weapon skill", "WeaponSkills" });
        for (uint16 id = 1; id < MAX_WEAPONSKILL_ID; ++id)
        {
            auto* PWeaponSkill = battleutils::GetWeaponSkill(id);
            if (PWeaponSkill != nullptr && charutils::hasWeaponSkill(PPawn, id) && charutils::canUseWeaponSkill(PPawn, id))
            {
                v.actions.push_back({ fmt::format("4:2:{}", id), titleCase(PWeaponSkill->getName()), "WeaponSkills" });
            }
        }

        v.actions.push_back({ "1:0:0", "Ranged attack", "Ranged" });
        return v;
    }
} // namespace pawn
