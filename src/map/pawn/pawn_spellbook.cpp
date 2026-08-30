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

#include "pawn_spellbook.h"

#include "common/xirand.h"

#include "entities/char_entity.h"
#include "recast_container.h"
#include "status_effect_container.h"
#include "utils/battleutils.h"

#include <algorithm>
#include <iterator>

namespace pawn
{
    namespace
    {
        constexpr std::size_t kSpellListSize = 1024;

        // Index = element (ELEMENT_FIRE .. ELEMENT_DARK), 1-based like the enum
        constexpr SPELLFAMILY kNukeFamilies[8]  = { SPELLFAMILY_FIRE, SPELLFAMILY_BLIZZARD, SPELLFAMILY_AERO, SPELLFAMILY_STONE, SPELLFAMILY_THUNDER, SPELLFAMILY_WATER, SPELLFAMILY_BANISH, SPELLFAMILY_DRAIN };
        constexpr SPELLFAMILY kEnFamilies[8]    = { SPELLFAMILY_ENFIRE, SPELLFAMILY_ENBLIZZARD, SPELLFAMILY_ENAERO, SPELLFAMILY_ENSTONE, SPELLFAMILY_ENTHUNDER, SPELLFAMILY_ENWATER, SPELLFAMILY_NONE, SPELLFAMILY_NONE };
        constexpr SPELLFAMILY kStormFamilies[8] = { SPELLFAMILY_FIRESTORM, SPELLFAMILY_HAILSTORM, SPELLFAMILY_WINDSTORM, SPELLFAMILY_SANDSTORM, SPELLFAMILY_THUNDERSTORM, SPELLFAMILY_RAINSTORM, SPELLFAMILY_AURORASTORM, SPELLFAMILY_VOIDSTORM };
        constexpr SPELLFAMILY kHelixFamilies[8] = { SPELLFAMILY_PYROHELIX, SPELLFAMILY_CRYOHELIX, SPELLFAMILY_ANEMOHELIX, SPELLFAMILY_GEOHELIX, SPELLFAMILY_IONOHELIX, SPELLFAMILY_HYDROHELIX, SPELLFAMILY_LUMINOHELIX, SPELLFAMILY_NOCTOHELIX };
    } // namespace

    CSpellBook::CSpellBook(CCharEntity* PChar)
    : m_PChar(PChar)
    {
    }

    void CSpellBook::Refresh()
    {
        const uint64 signature = Signature();
        if (signature != m_signature)
        {
            m_signature = signature;
            Rebuild();
        }
    }

    auto CSpellBook::Signature() const -> uint64
    {
        uint64 known = 0;
        for (std::size_t id = 0; id < kSpellListSize; ++id)
        {
            if (m_PChar->m_SpellList[id])
            {
                ++known;
            }
        }

        return (static_cast<uint64>(m_PChar->GetMJob()) << 56) |
               (static_cast<uint64>(m_PChar->GetSJob()) << 48) |
               (static_cast<uint64>(m_PChar->GetMLevel()) << 40) |
               (static_cast<uint64>(m_PChar->GetSLevel()) << 32) |
               known;
    }

    void CSpellBook::Rebuild()
    {
        m_known.clear();
        m_ga.clear();
        m_damage.clear();
        m_buff.clear();
        m_debuff.clear();
        m_heal.clear();
        m_na.clear();
        m_raise.clear();
        m_severe.clear();

        for (std::size_t id = 0; id < kSpellListSize; ++id)
        {
            if (!m_PChar->m_SpellList[id])
            {
                continue;
            }

            const auto spellId = static_cast<SpellID>(id);
            CSpell*    spell   = spell::GetSpell(spellId);
            if (spell == nullptr || spell->getSpellGroup() == SPELLGROUP_TRUST || !spell::CanUseSpell(m_PChar, spell))
            {
                continue;
            }

            m_known.emplace_back(spellId);

            // Same bucketing as CMobSpellContainer::AddSpell, so family and
            // category queries answer the way trust brains expect
            if (spell->getAOE() > 0 && spell->canTargetEnemy())
            {
                m_ga.emplace_back(spellId);
            }
            else if (spell->isSevere())
            {
                m_severe.emplace_back(spellId);
            }
            else if (spell->canTargetEnemy())
            {
                m_damage.emplace_back(spellId);
            }
            else if (spell->isDebuff())
            {
                m_debuff.emplace_back(spellId);
            }
            else if (spell->isNa())
            {
                m_na.emplace_back(spellId);
            }
            else if (spell->isRaise())
            {
                m_raise.emplace_back(spellId);
            }
            else if (spell->isHeal())
            {
                m_heal.emplace_back(spellId);
            }
            else if (spell->isBuff())
            {
                m_buff.emplace_back(spellId);
            }
        }
    }

    auto CSpellBook::IsUsable(const SpellID spellId) const -> bool
    {
        const CSpell* spell = spell::GetSpell(spellId);
        if (spell == nullptr)
        {
            return false;
        }

        const auto skill    = spell->getSkillType();
        const bool enoughMP = spell->getMPCost() <= m_PChar->health.mp ||
                              skill == xi::SkillType::Ninjutsu ||
                              skill == xi::SkillType::Singing ||
                              skill == xi::SkillType::WindInstrument ||
                              skill == xi::SkillType::StringInstrument ||
                              skill == xi::SkillType::Geomancy ||
                              m_PChar->StatusEffectContainer->HasStatusEffect(xi::StatusEffect::Manafont);

        return enoughMP && !m_PChar->PRecastContainer->Has(RECAST_MAGIC, static_cast<Recast>(spellId));
    }

    auto CSpellBook::GetAvailable(const SpellID spellId) const -> Maybe<SpellID>
    {
        if (!std::binary_search(m_known.begin(), m_known.end(), spellId) || !IsUsable(spellId))
        {
            return std::nullopt;
        }
        return spellId;
    }

    auto CSpellBook::GetBestAvailable(const SPELLFAMILY family) const -> Maybe<SpellID>
    {
        Maybe<SpellID> best;

        auto search = [&](const std::vector<SpellID>& list)
        {
            for (const auto id : list)
            {
                CSpell* spell = spell::GetSpell(id);
                if ((family == SPELLFAMILY_NONE || spell->getSpellFamily() == family) && IsUsable(id))
                {
                    // Lists ascend by id; the highest id in a family is its strongest tier
                    if (!best.has_value() || id > *best)
                    {
                        best = id;
                    }
                }
            }
        };

        if (family == SPELLFAMILY_NONE)
        {
            search(m_damage);
        }
        else
        {
            search(m_ga);
            search(m_damage);
            search(m_buff);
            search(m_debuff);
            search(m_heal);
            search(m_na);
            search(m_raise);
        }

        return best;
    }

    auto CSpellBook::WeakestElement(CBattleEntity* PTarget) const -> std::size_t
    {
        const int16 resistances[8] = {
            PTarget->getMod(xi::Mod::FIRE_RES_RANK),
            PTarget->getMod(xi::Mod::ICE_RES_RANK),
            PTarget->getMod(xi::Mod::WIND_RES_RANK),
            PTarget->getMod(xi::Mod::EARTH_RES_RANK),
            PTarget->getMod(xi::Mod::THUNDER_RES_RANK),
            PTarget->getMod(xi::Mod::WATER_RES_RANK),
            PTarget->getMod(xi::Mod::LIGHT_RES_RANK),
            PTarget->getMod(xi::Mod::DARK_RES_RANK),
        };

        // 1-based to line up with ELEMENT_FIRE .. ELEMENT_DARK
        return static_cast<std::size_t>(std::distance(std::begin(resistances), std::min_element(std::begin(resistances), std::end(resistances)))) + 1;
    }

    auto CSpellBook::BestOfElementFamilies(const std::size_t element, const SPELLFAMILY (&families)[8]) const -> Maybe<SpellID>
    {
        if (element < 1 || element > 8 || families[element - 1] == SPELLFAMILY_NONE)
        {
            return std::nullopt;
        }
        return GetBestAvailable(families[element - 1]);
    }

    auto CSpellBook::GetBestAgainstTargetWeakness(CBattleEntity* PTarget, const SpellID preferred) const -> Maybe<SpellID>
    {
        if (PTarget == nullptr)
        {
            return GetBestAvailable(SPELLFAMILY_NONE);
        }

        const auto weakness = WeakestElement(PTarget);

        if (const CSpell* spell = spell::GetSpell(preferred); spell != nullptr && spell->getElement() == weakness)
        {
            if (const auto available = GetAvailable(preferred); available.has_value())
            {
                return available;
            }
        }

        const auto choice = BestOfElementFamilies(weakness, kNukeFamilies);
        return choice.has_value() ? choice : GetBestAvailable(SPELLFAMILY_NONE);
    }

    auto CSpellBook::EnSpellAgainstTargetWeakness(CBattleEntity* PTarget) const -> Maybe<SpellID>
    {
        if (PTarget == nullptr)
        {
            return std::nullopt;
        }
        return BestOfElementFamilies(WeakestElement(PTarget), kEnFamilies);
    }

    auto CSpellBook::StormDayAgainstTargetWeakness(CBattleEntity* PTarget) const -> Maybe<SpellID>
    {
        if (PTarget == nullptr)
        {
            return std::nullopt;
        }
        return BestOfElementFamilies(WeakestElement(PTarget), kStormFamilies);
    }

    auto CSpellBook::HelixAgainstTargetWeakness(CBattleEntity* PTarget) const -> Maybe<SpellID>
    {
        if (PTarget == nullptr)
        {
            return std::nullopt;
        }
        return BestOfElementFamilies(WeakestElement(PTarget), kHelixFamilies);
    }

    auto CSpellBook::GetStormDay() const -> Maybe<SpellID>
    {
        return BestOfElementFamilies(static_cast<std::size_t>(battleutils::GetDayElement()), kStormFamilies);
    }

    auto CSpellBook::GetHelixDay() const -> Maybe<SpellID>
    {
        return BestOfElementFamilies(static_cast<std::size_t>(battleutils::GetDayElement()), kHelixFamilies);
    }

    auto CSpellBook::GetBestIndiSpell(CBattleEntity* PFor) const -> Maybe<SpellID>
    {
        if (PFor == nullptr)
        {
            return std::nullopt;
        }

        // Mirrors CMobSpellContainer::GetBestIndiSpell: pick for the job of
        // the party member the bubble is meant to help
        CBattleEntity* PForTarget    = PFor->GetBattleTarget();
        bool           accBuffNeeded = false;
        bool           maccNeeded    = false;

        if (PForTarget != nullptr)
        {
            accBuffNeeded = battleutils::GetHitRate(PFor, PForTarget) < 65;

            const auto mInt        = PFor->getMod(xi::Mod::INT);
            const auto tInt        = PForTarget->getMod(xi::Mod::INT);
            const auto intDiff     = mInt - tInt + 10;
            auto       maccFromInt = static_cast<float>(mInt);
            if (mInt > tInt + 10)
            {
                maccFromInt = tInt + ((mInt - intDiff) * 0.5f);
            }
            const auto  totalMacc    = PFor->GetSkill(xi::SkillType::ElementalMagic) + maccFromInt + PFor->getMod(xi::Mod::MACC);
            const float magicHitRate = (totalMacc - PForTarget->getMod(xi::Mod::MEVA)) / 10.0f;
            maccNeeded               = magicHitRate < 10;
        }

        Maybe<SpellID> choice;
        Maybe<SpellID> subChoice = SpellID::Indi_Regen;

        switch (PFor->GetMJob())
        {
            case xi::Job::WAR:
            case xi::Job::MNK:
            case xi::Job::THF:
            case xi::Job::DRK:
            case xi::Job::BST:
            case xi::Job::RNG:
            case xi::Job::SAM:
            case xi::Job::DRG:
            case xi::Job::BLU:
            case xi::Job::COR:
            case xi::Job::PUP:
            case xi::Job::DNC:
                choice    = accBuffNeeded ? SpellID::Indi_Precision : SpellID::Indi_Fury;
                subChoice = SpellID::Indi_Regen;
                break;
            case xi::Job::WHM:
            case xi::Job::BRD:
            case xi::Job::SMN:
            case xi::Job::GEO:
                choice    = SpellID::Indi_Refresh;
                subChoice = SpellID::Indi_Refresh;
                break;
            case xi::Job::BLM:
            case xi::Job::RDM:
            case xi::Job::SCH:
                choice    = maccNeeded ? SpellID::Indi_Focus : SpellID::Indi_Acumen;
                subChoice = SpellID::Indi_Refresh;
                break;
            case xi::Job::PLD:
            case xi::Job::RUN:
            case xi::Job::NIN:
                choice    = SpellID::Indi_Haste;
                subChoice = SpellID::Indi_Regen;
                break;
            default:
                break;
        }

        if (PFor->GetMLevel() < 20)
        {
            choice = std::nullopt;
        }
        else if (PFor->GetMLevel() < 93)
        {
            choice = subChoice;
            if (subChoice == SpellID::Indi_Refresh && PFor->GetMLevel() < 30)
            {
                choice = SpellID::Indi_Regen;
            }
        }

        return choice.has_value() ? GetAvailable(*choice) : std::nullopt;
    }

    auto CSpellBook::GetBestEntrustedSpell(CBattleEntity* PFor) const -> Maybe<SpellID>
    {
        if (PFor == nullptr)
        {
            return std::nullopt;
        }

        Maybe<SpellID> choice;
        switch (PFor->GetMJob())
        {
            case xi::Job::WAR:
            case xi::Job::MNK:
            case xi::Job::THF:
            case xi::Job::DRK:
            case xi::Job::BST:
            case xi::Job::RNG:
            case xi::Job::SAM:
            case xi::Job::DRG:
            case xi::Job::BLU:
            case xi::Job::COR:
            case xi::Job::PUP:
            case xi::Job::DNC:
                choice = SpellID::Indi_Frailty;
                break;
            case xi::Job::WHM:
            case xi::Job::BRD:
            case xi::Job::SMN:
                choice = SpellID::Indi_Acumen;
                break;
            case xi::Job::BLM:
            case xi::Job::RDM:
            case xi::Job::SCH:
            case xi::Job::PLD:
            case xi::Job::RUN:
                choice = SpellID::Indi_Refresh;
                break;
            case xi::Job::NIN:
                choice = SpellID::Indi_Regen;
                break;
            default:
                break;
        }

        return choice.has_value() ? GetAvailable(*choice) : std::nullopt;
    }

    auto CSpellBook::GetRandomDamageSpell() const -> Maybe<SpellID>
    {
        std::vector<SpellID> usable;
        for (const auto id : m_damage)
        {
            if (IsUsable(id))
            {
                usable.emplace_back(id);
            }
        }
        if (usable.empty())
        {
            return std::nullopt;
        }
        return xirand::GetRandomElement(usable);
    }
} // namespace pawn
