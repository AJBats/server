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

// Ailments (ailments.h): the Enfeeble status group, which -na takes which
// ailment off, the order -na (best) weighs them in, and Erase after all of
// them. What is on someone and what she can cast are plain sets here.

#include <catch2/catch_test_macros.hpp>

#include "data/enums/status_effect.h"
#include "map/pawn/ailments.h"
#include "map/pawn/gambit_ids.h"
#include "map/spell.h"

#include <optional>
#include <set>
#include <utility>
#include <vector>

using cardian::ailments::best;
using cardian::ailments::cures;
using cardian::ailments::enfeebled;
using cardian::ailments::isRemoval;
using cardian::ailments::kCures;
using cardian::ailments::kErase;
using xi::StatusEffect;

namespace
{
    auto on(std::set<StatusEffect> effects)
    {
        return [effects = std::move(effects)](const uint16 effect)
        {
            return effects.contains(static_cast<StatusEffect>(effect));
        };
    }

    auto knows(std::set<SpellID> spells)
    {
        return [spells = std::move(spells)](const uint16 spell)
        {
            return spells.contains(static_cast<SpellID>(spell));
        };
    }

    auto id(const SpellID spell) -> uint16
    {
        return static_cast<uint16>(spell);
    }

    const std::set<SpellID> kWhiteMage{ SpellID::Poisona, SpellID::Paralyna, SpellID::Blindna, SpellID::Silena, SpellID::Stona, SpellID::Viruna, SpellID::Cursna, SpellID::Erase };
} // namespace

TEST_CASE("ailments: the table is xi::StatusEffect's and spell.h's numbers, worst first", "[cardian][gambits][ailments]")
{
    const std::vector<std::pair<StatusEffect, SpellID>> want{
        { StatusEffect::Doom, SpellID::Cursna },
        { StatusEffect::Petrification, SpellID::Stona },
        { StatusEffect::Silence, SpellID::Silena },
        { StatusEffect::Paralysis, SpellID::Paralyna },
        { StatusEffect::CurseI, SpellID::Cursna },
        { StatusEffect::CurseIi, SpellID::Cursna },
        { StatusEffect::Bane, SpellID::Cursna },
        { StatusEffect::Plague, SpellID::Viruna },
        { StatusEffect::Disease, SpellID::Viruna },
        { StatusEffect::Blindness, SpellID::Blindna },
        { StatusEffect::Poison, SpellID::Poisona },
    };
    REQUIRE(want.size() == kCures.size());
    for (std::size_t i = 0; i < want.size(); ++i)
    {
        INFO("entry " << i);
        CHECK(kCures[i].effect == static_cast<uint16>(want[i].first));
        CHECK(kCures[i].spell == id(want[i].second));
    }
    CHECK(kErase == id(SpellID::Erase));
    CHECK(cardian::ailments::kNaFamily == static_cast<uint32>(SPELLFAMILY_NA));

    // The Enfeeble status is no status upstream numbers
    CHECK(pawn::G_STATUS_ENFEEBLE > static_cast<uint32>(StatusEffect::TrustAuraMagicAttack));
}

TEST_CASE("ailments: the removal spells are the seven -na and Erase, never Esuna", "[cardian][gambits][ailments]")
{
    for (const auto spell : kWhiteMage)
    {
        CHECK(isRemoval(id(spell)));
    }
    CHECK_FALSE(isRemoval(id(SpellID::Esuna)));
    CHECK_FALSE(isRemoval(id(SpellID::Cure)));
    CHECK_FALSE(isRemoval(id(SpellID::Poison)));
}

TEST_CASE("ailments: Enfeeble holds for any ailment a -na cures, or an erasable effect", "[cardian][gambits][ailments]")
{
    CHECK_FALSE(enfeebled(on({}), false));
    CHECK(enfeebled(on({ StatusEffect::Poison }), false));
    CHECK(enfeebled(on({ StatusEffect::Doom }), false));
    CHECK(enfeebled(on({ StatusEffect::Bane }), false));
    CHECK(enfeebled(on({}), true)); // Slow, Bio, a stat down: Erase's

    // Sleep is not in it: a Cure wakes her, no -na does
    CHECK_FALSE(enfeebled(on({ StatusEffect::SleepI }), false));
    CHECK_FALSE(enfeebled(on({ StatusEffect::Protect, StatusEffect::Haste }), false));
}

TEST_CASE("ailments: -na (best) casts for the worst ailment she can cure, Erase last", "[cardian][gambits][ailments]")
{
    const auto all = knows(kWhiteMage);

    CHECK(best(on({ StatusEffect::Poison }), false, all) == std::optional<uint16>(id(SpellID::Poisona)));
    CHECK(best(on({ StatusEffect::Poison, StatusEffect::Paralysis }), false, all) == std::optional<uint16>(id(SpellID::Paralyna)));
    CHECK(best(on({ StatusEffect::Blindness, StatusEffect::Doom, StatusEffect::Silence }), false, all) == std::optional<uint16>(id(SpellID::Cursna)));
    CHECK(best(on({ StatusEffect::Plague }), false, all) == std::optional<uint16>(id(SpellID::Viruna)));
    CHECK(best(on({ StatusEffect::CurseIi }), false, all) == std::optional<uint16>(id(SpellID::Cursna)));

    // Erase only once no -na ailment is hers to cure
    CHECK(best(on({ StatusEffect::Poison }), true, all) == std::optional<uint16>(id(SpellID::Poisona)));
    CHECK(best(on({}), true, all) == std::optional<uint16>(id(SpellID::Erase)));

    // Nothing on her, nothing to cast
    CHECK_FALSE(best(on({}), false, all).has_value());
    CHECK_FALSE(best(on({ StatusEffect::SleepI }), false, all).has_value());
}

TEST_CASE("ailments: -na (best) passes over what she cannot cast to the next ailment", "[cardian][gambits][ailments]")
{
    // A low White Mage: no Cursna yet, so Doom waits and the poison goes
    const auto low = knows({ SpellID::Poisona, SpellID::Paralyna, SpellID::Blindna });
    CHECK(best(on({ StatusEffect::Doom, StatusEffect::Poison }), false, low) == std::optional<uint16>(id(SpellID::Poisona)));
    CHECK_FALSE(best(on({ StatusEffect::Doom }), false, low).has_value());
    CHECK_FALSE(best(on({}), true, low).has_value()); // no Erase either

    // Paralyna on recast (the caller's can() says no): the blindness goes
    const auto recast = knows({ SpellID::Poisona, SpellID::Blindna });
    CHECK(best(on({ StatusEffect::Paralysis, StatusEffect::Blindness }), false, recast) == std::optional<uint16>(id(SpellID::Blindna)));
}

TEST_CASE("ailments: a removal spell cures only what it takes off", "[cardian][gambits][ailments]")
{
    CHECK(cures(id(SpellID::Poisona), on({ StatusEffect::Poison }), false));
    CHECK_FALSE(cures(id(SpellID::Poisona), on({ StatusEffect::Paralysis }), false));
    CHECK(cures(id(SpellID::Viruna), on({ StatusEffect::Disease }), false));
    CHECK(cures(id(SpellID::Cursna), on({ StatusEffect::Doom }), false));
    CHECK(cures(id(SpellID::Erase), on({}), true));
    CHECK_FALSE(cures(id(SpellID::Erase), on({ StatusEffect::Poison }), false));
    CHECK_FALSE(cures(id(SpellID::Cure), on({ StatusEffect::Poison }), true));
}
