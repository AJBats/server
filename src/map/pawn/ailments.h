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
// casts the -na for the worst ailment on someone -- and wakes a sleeper
// with her cheapest Cure, which no -na does (the user, 2026-10-18). Status
// and spell ids are xi::StatusEffect's and spell.h's (pinned against both in
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

    // Asleep -- Sleep, Sleep II, Lullaby -- which a Cure wakes and no -na
    // does: -na (best) casts the cheapest Cure she can for it, ranked after
    // Doom and Petrification (kWakeAfter, its place in kCures). Not an
    // ailment of the Enfeeble group, and a Cure is no removal spell: a row
    // naming a Cure goes on as a Cure
    inline constexpr std::array<uint16, 3> kAsleep{ 2, 19, 193 };   // Sleep, Sleep II, Lullaby
    inline constexpr std::array<uint16, 4> kWakeCures{ 1, 2, 3, 4 }; // Cure to Cure IV, cheapest first
    inline constexpr std::size_t           kWakeAfter = 2;
    static_assert(kCures[kWakeAfter].effect == 6, "the wake goes after Doom and Petrification, ahead of Silence");

    constexpr auto isSleep(const uint16 effect) -> bool
    {
        return std::ranges::any_of(kAsleep, [effect](const uint16 e)
                                   {
                                       return e == effect;
                                   });
    }

    template <typename Has>
    constexpr auto asleep(Has&& has) -> bool
    {
        return std::ranges::any_of(kAsleep, [&has](const uint16 effect)
                                   {
                                       return has(effect);
                                   });
    }

    constexpr auto isWakeCure(const uint32 spell) -> bool
    {
        return std::ranges::any_of(kWakeCures, [spell](const uint16 cure)
                                   {
                                       return cure == spell;
                                   });
    }

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

    // Whether a spell -na (best) picks would take something off someone: a
    // -na one of its ailments, Erase an erasable effect, a Cure a sleep.
    // False for any other spell
    template <typename Has>
    constexpr auto cures(const uint32 spell, Has&& has, const bool erasable) -> bool
    {
        if (spell == kErase)
        {
            return erasable;
        }
        if (isWakeCure(spell))
        {
            return asleep(has);
        }
        return std::ranges::any_of(kCures, [spell, &has](const Cure& c)
                                   {
                                       return c.spell == spell && has(c.effect);
                                   });
    }

    // -na (best): the spell for the worst ailment on someone that she can
    // cast (can(spell): she knows it and can cast it now) -- her cheapest
    // Cure for a sleep, in its place -- Erase last; nothing when nothing on
    // her is hers to cure
    template <typename Has, typename Can>
    constexpr auto best(Has&& has, const bool erasable, Can&& can) -> std::optional<uint16>
    {
        for (std::size_t i = 0; i < kCures.size(); ++i)
        {
            if (i == kWakeAfter && asleep(has))
            {
                for (const auto cure : kWakeCures)
                {
                    if (can(cure))
                    {
                        return cure;
                    }
                }
            }
            if (has(kCures[i].effect) && can(kCures[i].spell))
            {
                return kCures[i].spell;
            }
        }
        if (erasable && can(kErase))
        {
            return kErase;
        }
        return std::nullopt;
    }
} // namespace cardian::ailments
