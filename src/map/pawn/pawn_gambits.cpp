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

#include <algorithm>
#include <array>

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

    auto CGambits::AddGambit(Gambit_t gambit) -> std::string
    {
        gambit.identifier = fmt::format("{}", ++m_nextId);
        gambit.last_used  = {};
        m_gambits.emplace_back(std::move(gambit));
        return m_gambits.back().identifier;
    }

    void CGambits::RemoveGambit(const std::string& id)
    {
        std::erase_if(m_gambits, [&id](const Gambit_t& gambit)
                      {
                          return gambit.identifier == id;
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

        if (engaged && POwner->health.tp >= 1000 && TryWeaponSkill())
        {
            return;
        }

        for (auto& gambit : m_gambits)
        {
            if (IsBehavior(gambit) || tick < gambit.last_used + std::chrono::seconds(gambit.retry_delay))
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
        for (const auto& gambit : m_gambits)
        {
            if (IsBehavior(gambit) && SelectTarget(gambit) != nullptr)
            {
                ApplyBehavior(gambit);
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
        static constexpr std::array<std::string_view, pawn::BehaviorCount> names{ "?", "avoid aggro", "hunt", "hunt band", "formation", "clean pulls", "rest with player", "home point with player" };
        const auto                                                         name = names[std::min<std::size_t>(static_cast<std::size_t>(behavior), names.size() - 1)];

        const auto unconditional = [&](const Gambit_t& g)
        {
            return IsBehavior(g) && g.actions.size() == 1 && static_cast<pawn::Behavior>(g.actions[0].select) == behavior &&
                   g.predicate_groups.size() == 1 && g.predicate_groups[0].predicates.size() == 1 &&
                   g.predicate_groups[0].predicates[0].condition == G_CONDITION::ALWAYS;
        };
        if (const auto it = std::find_if(m_gambits.begin(), m_gambits.end(), unconditional); it != m_gambits.end())
        {
            it->actions[0].select_arg = arg;
        }
        else
        {
            Gambit_t row;
            row.target_selector = G_TARGET::SELF;
            row.predicate_groups.emplace_back(G_LOGIC::AND, std::vector<Predicate_t>{ Predicate_t(G_CONDITION::ALWAYS, 0) });
            row.actions.emplace_back(G_REACTION_BEHAVIOR, static_cast<G_SELECT>(behavior), arg);
            AddGambit(std::move(row));
        }
        ShowInfoFmt("pawn: {} gambit row: {} = {}", POwner->getName(), name, arg);
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
} // namespace pawn
