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

// The Magic a gambit editor offers (pawn::vocabularyFor), as a plain rule:
// only the spells she has learned, for her and for the player's own page
// alike. A spell of her jobs she has not learned is not offered; one she has
// learned and cannot cast now (her level short of it) is offered greyed. The
// three entries that pick a spell for her -- Enfeeble, Damage spell (any)
// and a family's "(best)" -- are offered only when she has learned a spell
// they would cast. A row that names a spell she has not learned still reads
// as the server words it: the catalogue feeds the pickers, not the rows.
#pragma once

#include <algorithm>
#include <cstdint>
#include <vector>

namespace cardian::gambitspells
{
    // One spell of her two jobs, at any level, as the catalogue weighs it
    struct Spell
    {
        uint16_t id       = 0;
        uint16_t family   = 0;     // its SPELLFAMILY, 0 for none
        bool     learned  = false; // in her spellbook
        bool     castable = false; // learned, and her jobs and levels cast it now (CSpellBook::Eligible)
        bool     nuke     = false; // a damage spell (spell_bank.h isNuke)
        bool     enfeeble = false; // one of Enfeeble's spells (tactician_line.h kEnfeebleOrder)
    };

    enum class Kind : uint8_t
    {
        Enfeeble,   // the first of her enfeebles the foe does not carry
        AnyNuke,    // Damage spell (any): her tactician's pick of her damage spells
        FamilyBest, // a family's "(best)": the strongest of it she can cast
        Specific,   // one spell
    };

    struct Entry
    {
        Kind     kind   = Kind::Specific;
        uint16_t arg    = 0;     // a FamilyBest's family, a Specific's spell
        bool     usable = false; // she can cast it now (for the three that pick, one of theirs)

        auto operator==(const Entry&) const -> bool = default;
    };

    // Her Magic in the catalogue's order, from every spell of her two jobs
    // in spell order: Enfeeble, then Damage spell (any), then each spell she
    // has learned, a family's "(best)" just before the first of it -- for a
    // family her jobs have more than one spell of, so the row moves up its
    // tiers as she learns them
    inline auto entries(const std::vector<Spell>& spells) -> std::vector<Entry>
    {
        std::vector<Entry> out;
        const auto         offered = [&](auto&& which, const Kind kind)
        {
            bool learned = false;
            bool usable  = false;
            for (const auto& s : spells)
            {
                if (which(s))
                {
                    learned = learned || s.learned;
                    usable  = usable || s.castable;
                }
            }
            if (learned)
            {
                out.push_back({ kind, 0, usable });
            }
        };
        offered([](const Spell& s)
                {
                    return s.enfeeble;
                },
                Kind::Enfeeble);
        offered([](const Spell& s)
                {
                    return s.nuke;
                },
                Kind::AnyNuke);

        std::vector<uint16_t> named;
        for (const auto& s : spells)
        {
            if (!s.learned)
            {
                continue;
            }
            const auto kin = std::ranges::count_if(spells, [&](const Spell& k)
                                                   {
                                                       return k.family == s.family;
                                                   });
            if (s.family != 0 && kin > 1 && std::ranges::find(named, s.family) == named.end())
            {
                named.push_back(s.family);
                const bool usable = std::ranges::any_of(spells, [&](const Spell& k)
                                                        {
                                                            return k.family == s.family && k.castable;
                                                        });
                out.push_back({ Kind::FamilyBest, s.family, usable });
            }
            out.push_back({ Kind::Specific, s.id, s.castable });
        }
        return out;
    }
} // namespace cardian::gambitspells
