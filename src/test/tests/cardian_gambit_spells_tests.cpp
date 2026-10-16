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

// The Magic a gambit editor offers (pawn/gambit_spells.h): only the spells
// she has learned, the three entries that pick a spell for her only when
// she has learned one they would cast -- for a cardian's page and the
// player's own alike, since both read pawn::vocabularyFor.

#include <catch2/catch_test_macros.hpp>

#include "map/pawn/gambit_spells.h"

#include <vector>

using namespace cardian::gambitspells;

namespace
{
    constexpr uint16_t kCureFamily  = 1;
    constexpr uint16_t kStoneFamily = 2;

    // A White Mage / Black Mage at 12 / 6: Cure and Cure II of her main (II
    // learned, beyond her level), Dia (an enfeeble), Stone and Stone II of her
    // support (Stone learned), Protect (no family to pick, not learned)
    auto mage() -> std::vector<Spell>
    {
        return {
            { .id = 1, .family = kCureFamily, .learned = true, .castable = true },
            { .id = 2, .family = kCureFamily, .learned = true, .castable = false },
            { .id = 23, .learned = true, .castable = true, .enfeeble = true },
            { .id = 43, .learned = false },
            { .id = 159, .family = kStoneFamily, .learned = true, .castable = true, .nuke = true },
            { .id = 160, .family = kStoneFamily, .learned = false, .nuke = true },
        };
    }
} // namespace

TEST_CASE("Gambit spells: only what she has learned, in spell order, each family's (best) before its first", "[cardian][gambit][vocab]")
{
    const std::vector<Entry> want{
        { Kind::Enfeeble, 0, true },
        { Kind::AnyNuke, 0, true },
        { Kind::FamilyBest, kCureFamily, true },
        { Kind::Specific, 1, true },
        { Kind::Specific, 2, false }, // learned ahead of her level: offered, greyed
        { Kind::Specific, 23, true },
        { Kind::FamilyBest, kStoneFamily, true },
        { Kind::Specific, 159, true },
    };
    CHECK(entries(mage()) == want);
}

TEST_CASE("Gambit spells: a spell of her jobs she has not learned is not offered", "[cardian][gambit][vocab]")
{
    for (const auto& e : entries(mage()))
    {
        CHECK_FALSE((e.kind == Kind::Specific && (e.arg == 43 || e.arg == 160)));
    }
}

TEST_CASE("Gambit spells: the entries that pick wait for a spell she has learned", "[cardian][gambit][vocab]")
{
    // her jobs cast an enfeeble and a damage spell, and she has learned neither
    const std::vector<Spell> none{
        { .id = 23, .learned = false, .enfeeble = true },
        { .id = 159, .family = kStoneFamily, .learned = false, .nuke = true },
        { .id = 160, .family = kStoneFamily, .learned = false, .nuke = true },
    };
    CHECK(entries(none).empty());

    // learned but beyond her level: offered, greyed
    const std::vector<Spell> ahead{
        { .id = 23, .learned = true, .castable = false, .enfeeble = true },
        { .id = 159, .family = kStoneFamily, .learned = true, .castable = false, .nuke = true },
        { .id = 160, .family = kStoneFamily, .learned = false, .nuke = true },
    };
    const std::vector<Entry> greyed{
        { Kind::Enfeeble, 0, false },
        { Kind::AnyNuke, 0, false },
        { Kind::Specific, 23, false },
        { Kind::FamilyBest, kStoneFamily, false },
        { Kind::Specific, 159, false },
    };
    CHECK(entries(ahead) == greyed);
}

TEST_CASE("Gambit spells: a family with one spell of her jobs has no (best)", "[cardian][gambit][vocab]")
{
    const std::vector<Spell> one{
        { .id = 1, .family = kCureFamily, .learned = true, .castable = true },
    };
    CHECK(entries(one) == std::vector<Entry>{ { Kind::Specific, 1, true } });
}

TEST_CASE("Gambit spells: a family's (best) stands where her first learned spell of it does", "[cardian][gambit][vocab]")
{
    // Cure unlearned, Cure II learned: the (best) heads Cure II
    const std::vector<Spell> spells{
        { .id = 1, .family = kCureFamily, .learned = false },
        { .id = 2, .family = kCureFamily, .learned = true, .castable = true },
    };
    const std::vector<Entry> want{
        { Kind::FamilyBest, kCureFamily, true },
        { Kind::Specific, 2, true },
    };
    CHECK(entries(spells) == want);
}
