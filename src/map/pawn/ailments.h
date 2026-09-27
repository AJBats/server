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

#include <algorithm>
#include <array>
#include <optional>

// Ailments and the spells that take them off: the Enfeeble status a row's
// condition names (gambit_ids.h G_STATUS_ENFEEBLE), and "-na (best)", which
// casts the -na for the worst ailment on someone. Status and spell ids are
// xi::StatusEffect's and spell.h's (pinned against both in
// cardian_ailments_tests.cpp); what is on someone and what she can cast are
// asked through callables, so xi_test pins the rules without an entity.
namespace cardian::ailments
{
    inline constexpr uint32 kNaFamily = 4;   // SPELLFAMILY_NA
    inline constexpr uint16 kErase    = 143; // Erase: any one erasable effect

    struct Cure
    {
        uint16 effect = 0;
        uint16 spell  = 0;
    };

    // What each -na takes off, worst first: -na (best) casts for the first
    // of these on someone that she can cast, and Erase only after all of
    // them, for an erasable effect (Slow, Bio, Weight, a stat down...)
    inline constexpr std::array<Cure, 11> kCures{ {
        { 15, 20 }, // Doom -> Cursna
        { 7, 18 },  // Petrification -> Stona
        { 6, 17 },  // Silence -> Silena
        { 4, 15 },  // Paralysis -> Paralyna
        { 9, 20 },  // Curse -> Cursna
        { 20, 20 }, // Curse II -> Cursna
        { 30, 20 }, // Bane -> Cursna
        { 31, 19 }, // Plague -> Viruna
        { 8, 19 },  // Disease -> Viruna
        { 5, 16 },  // Blindness -> Blindna
        { 3, 14 },  // Poison -> Poisona
    } };

    // A spell that takes an ailment off: one of the -na above, or Erase.
    // Esuna, a -na family spell on the caster herself, is none of them
    constexpr auto isRemoval(const uint32 spell) -> bool
    {
        return spell == kErase || std::ranges::any_of(kCures, [spell](const Cure& c)
                                                      {
                                                          return c.spell == spell;
                                                      });
    }

    // Whether someone carries an ailment of the Enfeeble group: one a -na
    // cures, or an erasable effect. has(effect) reads what is on her;
    // `erasable` whether an erasable effect Erase can take is on her
    template <typename Has>
    constexpr auto enfeebled(Has&& has, const bool erasable) -> bool
    {
        return erasable || std::ranges::any_of(kCures, [&has](const Cure& c)
                                               {
                                                   return has(c.effect);
                                               });
    }

    // Whether a removal spell would take something off someone: a -na one
    // of its ailments, Erase an erasable effect. False for any other spell
    template <typename Has>
    constexpr auto cures(const uint32 spell, Has&& has, const bool erasable) -> bool
    {
        if (spell == kErase)
        {
            return erasable;
        }
        return std::ranges::any_of(kCures, [spell, &has](const Cure& c)
                                   {
                                       return c.spell == spell && has(c.effect);
                                   });
    }

    // -na (best): the spell for the worst ailment on someone that she can
    // cast (can(spell): she knows it and can cast it now), Erase last;
    // nothing when nothing on her is hers to cure
    template <typename Has, typename Can>
    constexpr auto best(Has&& has, const bool erasable, Can&& can) -> std::optional<uint16>
    {
        for (const auto& c : kCures)
        {
            if (has(c.effect) && can(c.spell))
            {
                return c.spell;
            }
        }
        if (erasable && can(kErase))
        {
            return kErase;
        }
        return std::nullopt;
    }
} // namespace cardian::ailments
