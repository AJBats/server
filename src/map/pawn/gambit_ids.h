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

#include "ai/helpers/gambits_container.h"

#include <array>
#include <string_view>

// Cardian's numbers in a gambit row: the ids it adds beside upstream's
// gambits::G_* enums (as casts, so upstream's header stays untouched), and
// the behaviours and roles a behaviour row names. Every value here is
// written into saved rows (the grammar, gambit_text.h) and is frozen: a
// value is retired, never reused. Nothing here reads an entity or a spell,
// and nothing needs APP_SOURCES, so the pure headers and xi_test can
// include it; upstream's gambits_container.h, which the G_* enums live in,
// comes with it, and that header is not light.
namespace pawn
{
    // Cardian-only gambit reaction: flip a controller behaviour instead of
    // acting. select = the behaviour (Behavior below), arg = on/off. Applied
    // while the row's conditions hold, and it never consumes the think.
    constexpr auto G_REACTION_BEHAVIOR = static_cast<gambits::G_REACTION>(100);

    // Cardian-only gambit condition, reserved for the party strategy channel
    // (RESEARCH §8): holds while the party's strategy equals the argument.
    // No strategy exists yet, so a row with it never fires.
    constexpr auto G_CONDITION_STRATEGY = static_cast<gambits::G_CONDITION>(100);

    // Cardian-only gambit condition for the tactician (RESEARCH §14.12): the
    // row leaves the when to her judgement. Below her Support Mage row
    // (tactician_line.h) it holds, and her tactician decides; as an ordinary
    // condition it never holds, so a row carrying it never fires as an order.
    constexpr auto G_CONDITION_TACTICIANS_CHOICE = static_cast<gambits::G_CONDITION>(101);

    // Cardian-only gambit targets: a foe around the party that is not her
    // fight yet, for the rows that decide which fight she takes (Attack).
    // Upstream's targets name an ally, or the fight she is already in.
    constexpr auto G_TARGET_LEADERS_TARGET   = static_cast<gambits::G_TARGET>(100); // the party leader's battle target, while he is engaged
    constexpr auto G_TARGET_TARGETED_BY_ALLY = static_cast<gambits::G_TARGET>(101); // a mob another cardian of the party is fighting
    constexpr auto G_TARGET_TARGETING_ALLY   = static_cast<gambits::G_TARGET>(102); // a mob on her or on a party member
    constexpr auto G_TARGET_TARGETING_SELF   = static_cast<gambits::G_TARGET>(103); // a mob on her

    // The behaviours a row can switch -- engine tuning, not what the party
    // is doing right now (hunting is the party's strategy, another channel).
    // Values are frozen: they appear in the row grammar and are persisted
    // (M3.85); the gaps are retired values. Rows are the ONLY source of a
    // behaviour: an unchecked row is off, the first row, top down, to speak
    // for a behaviour wins, and a switch no row speaks for is off.
    enum class Behavior : uint16
    {
        AvoidAggro          = 1, // switch
        Formation           = 4, // a Slot
        RestWithPlayer      = 6, // switch: kneel when the player kneels
        HomePointWithPlayer = 7, // switch: a KO'd cardian home points when the player does
        // 8: retired Rest; resting is owned by the shared policy.
        BoostBeforeWs       = 9, // switch: a Monk's Boost goes out right before her weapon skill, nothing between (D5)
        // 10: retired RestInBattle. Never reuse persisted behavior IDs.
        Role                = 11, // a parameter: the role she plays (pawn::Role); the tactician's conveyor assigns her casts (RESEARCH §12.12 item 2)
        // 12: retired MeleeMage; an Attack row that claims the mob decides whether a
        // Support Mage fights it. The grammar refuses 12, so no row carries it.
    };
    constexpr uint16 BehaviorCount = 13; // one past the highest value ever given, retired ones included

    // The retired behaviour values: the grammar refuses a row that names
    // one, saved or imported, so an old meaning never comes back
    constexpr auto isRetiredBehavior(const uint32 behavior) -> bool
    {
        return behavior == 8 || behavior == 10 || behavior == 12;
    }

    // A switch row carries the value 1 and its checkbox is the switch; a
    // parameter row (the formation slot, the role) carries its value
    constexpr auto isSwitch(const Behavior b) -> bool
    {
        return b != Behavior::Formation && b != Behavior::Role;
    }

    // The roles a Role row can name (the argument of Behavior::Role). Values
    // are frozen like the behaviours: they appear in rows and are persisted
    enum class Role : uint16
    {
        None        = 0,
        SupportMage = 1,
        Tank        = 2, // stands in (RESEARCH §12.16): at a stake she tows the mob to it and holds its 3 o'clock; the rest of the role comes later
        MeleeDamage = 3, // the Damage role, standing in: a name and an editor entry; the rear seat, sneak attack and trick attack come later
    };
    constexpr auto roleName(const Role role) -> std::string_view
    {
        switch (role)
        {
            case Role::SupportMage:
                return "Support Mage";
            case Role::Tank:
                return "Tank";
            case Role::MeleeDamage:
                return "Damage";
            default:
                return "none";
        }
    }
    constexpr std::array<Role, 3> kRoles{ Role::SupportMage, Role::Tank, Role::MeleeDamage };

    // The persisted numbers, pinned: a renumbered value fails the build
    // before it can reread a saved row as something else
    static_assert(static_cast<uint16>(G_REACTION_BEHAVIOR) == 100);
    static_assert(static_cast<uint16>(G_CONDITION_STRATEGY) == 100);
    static_assert(static_cast<uint16>(G_CONDITION_TACTICIANS_CHOICE) == 101);
    static_assert(static_cast<uint16>(G_TARGET_LEADERS_TARGET) == 100);
    static_assert(static_cast<uint16>(G_TARGET_TARGETED_BY_ALLY) == 101);
    static_assert(static_cast<uint16>(G_TARGET_TARGETING_ALLY) == 102);
    static_assert(static_cast<uint16>(G_TARGET_TARGETING_SELF) == 103);

    static_assert(static_cast<uint16>(Behavior::AvoidAggro) == 1);
    static_assert(static_cast<uint16>(Behavior::Formation) == 4);
    static_assert(static_cast<uint16>(Behavior::RestWithPlayer) == 6);
    static_assert(static_cast<uint16>(Behavior::HomePointWithPlayer) == 7);
    static_assert(static_cast<uint16>(Behavior::BoostBeforeWs) == 9);
    static_assert(static_cast<uint16>(Behavior::Role) == 11);
    static_assert(BehaviorCount == 13);
    static_assert(isRetiredBehavior(8) && isRetiredBehavior(10) && isRetiredBehavior(12));
    static_assert(!isRetiredBehavior(static_cast<uint16>(Behavior::AvoidAggro)) && !isRetiredBehavior(static_cast<uint16>(Behavior::Formation)) &&
                  !isRetiredBehavior(static_cast<uint16>(Behavior::RestWithPlayer)) && !isRetiredBehavior(static_cast<uint16>(Behavior::HomePointWithPlayer)) &&
                  !isRetiredBehavior(static_cast<uint16>(Behavior::BoostBeforeWs)) && !isRetiredBehavior(static_cast<uint16>(Behavior::Role)));

    static_assert(static_cast<uint16>(Role::None) == 0);
    static_assert(static_cast<uint16>(Role::SupportMage) == 1);
    static_assert(static_cast<uint16>(Role::Tank) == 2);
    static_assert(static_cast<uint16>(Role::MeleeDamage) == 3);
} // namespace pawn
