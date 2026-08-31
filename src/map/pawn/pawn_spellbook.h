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

#include "common/cbasetypes.h"
#include "common/types/maybe.h"

#include "spell.h"

#include <cstddef>
#include <vector>

class CBattleEntity;
class CCharEntity;

namespace pawn
{
    // The character-data counterpart of CMobSpellContainer: the spells this
    // pawn has learned and can cast at its current job and level, classified
    // the way the gambit interpreter expects, with the same availability
    // (MP, recast) and selection queries. Refresh() re-derives the book when
    // the character's job, level or learned-spell count changes, so a pawn
    // that learns Cure III starts casting it on the next think.
    class CSpellBook
    {
    public:
        explicit CSpellBook(CCharEntity* PChar);

        void Refresh();

        auto GetAvailable(SpellID spellId) const -> Maybe<SpellID>;
        auto GetBestAvailable(SPELLFAMILY family) const -> Maybe<SpellID>;
        auto GetBestAgainstTargetWeakness(CBattleEntity* PTarget, SpellID preferred) const -> Maybe<SpellID>;
        auto EnSpellAgainstTargetWeakness(CBattleEntity* PTarget) const -> Maybe<SpellID>;
        auto StormDayAgainstTargetWeakness(CBattleEntity* PTarget) const -> Maybe<SpellID>;
        auto HelixAgainstTargetWeakness(CBattleEntity* PTarget) const -> Maybe<SpellID>;
        auto GetStormDay() const -> Maybe<SpellID>;
        auto GetHelixDay() const -> Maybe<SpellID>;
        auto GetBestIndiSpell(CBattleEntity* PFor) const -> Maybe<SpellID>;
        auto GetBestEntrustedSpell(CBattleEntity* PFor) const -> Maybe<SpellID>;
        auto GetRandomDamageSpell() const -> Maybe<SpellID>;

        // Single-target damage spells, ascending id (magic-burst selection)
        auto DamageSpells() const -> const std::vector<SpellID>&
        {
            return m_damage;
        }

        auto Size() const -> std::size_t
        {
            return m_known.size();
        }

    private:
        auto IsUsable(SpellID spellId) const -> bool;
        auto Signature() const -> uint64;
        auto WeakestElement(CBattleEntity* PTarget) const -> std::size_t;
        auto BestOfElementFamilies(std::size_t element, const SPELLFAMILY (&families)[8]) const -> Maybe<SpellID>;
        void Rebuild();

        CCharEntity* m_PChar;
        uint64       m_signature = 0;

        std::vector<SpellID> m_known;
        std::vector<SpellID> m_ga;
        std::vector<SpellID> m_damage;
        std::vector<SpellID> m_buff;
        std::vector<SpellID> m_debuff;
        std::vector<SpellID> m_heal;
        std::vector<SpellID> m_na;
        std::vector<SpellID> m_raise;
        std::vector<SpellID> m_severe;
    };
} // namespace pawn
