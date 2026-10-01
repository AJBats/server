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

    // The tactician's mark (RESEARCH §17.13): a row carrying this condition
    // is the tactician's -- the row's other conditions and its target say
    // whom it may be for, and the tactician decides the when. The row never
    // fires as an order, and the tactician reaches its tool through it
    // (tactician_line.h). The editor shows the mark, not a clause.
    constexpr auto G_CONDITION_TACTICIANS_CHOICE = static_cast<gambits::G_CONDITION>(101);

    // Cardian-only gambit targets: a foe around the party that is not her
    // fight yet, for the rows that decide which fight she takes (Attack).
    // Upstream's targets name an ally, or the fight she is already in.
    constexpr auto G_TARGET_LEADERS_TARGET   = static_cast<gambits::G_TARGET>(100); // the party leader's battle target, while he is engaged
    constexpr auto G_TARGET_TARGETED_BY_ALLY = static_cast<gambits::G_TARGET>(101); // a mob an ally of hers -- any of the party but herself, the player included -- is fighting
    constexpr auto G_TARGET_TARGETING_ALLY   = static_cast<gambits::G_TARGET>(102); // a mob on her or on a party member
    constexpr auto G_TARGET_TARGETING_SELF   = static_cast<gambits::G_TARGET>(103); // a mob on her

    // Cardian-only spell select: Enfeeble, the single-target enfeebles her
    // tactician prices (tactician_line.h kEnfeebleOrder). On a marked row
    // it lets her tactician cast any of them; as an order it casts the
    // first she can that the foe does not carry yet. Never a -ga spell.
    constexpr auto G_SELECT_ENFEEBLE = static_cast<gambits::G_SELECT>(100);

    // Cardian-only status a status condition can name: Enfeeble, a group,
    // any ailment a -na spell cures or anything Erase takes off
    // (ailments.h). Far past every status id upstream numbers, so no real
    // status is ever read as it.
    constexpr uint32 G_STATUS_ENFEEBLE = 10000;

    // The behaviours a row can switch -- engine tuning, not what the party
    // is doing right now (hunting is the party's strategy, another channel).
    // Values are frozen: they appear in the row grammar and are persisted
    // (M3.85); the gaps are retired values. Rows are the ONLY source of a
    // behaviour: an unchecked row is off, the first row, top down, to speak
    // for a behaviour wins, and a switch no row speaks for is off.
    enum class Behavior : uint16
    {
        AvoidAggro          = 1, // switch: keep out of the detection circles of every idle mob that would go for her
        Formation           = 4, // a Slot
        RestWithPlayer      = 6, // switch: kneel when the player kneels
        HomePointWithPlayer = 7, // switch: a KO'd cardian home points when the player does
        // 8: retired Rest; resting is owned by the shared policy.
        BoostBeforeWs       = 9, // switch: a Monk's Boost goes out right before her weapon skill, nothing between (D5)
        // 10: retired RestInBattle. Never reuse persisted behavior IDs.
        // 11: retired Role, the tactician line (Support Mage 1, Tank 2, and
        // Damage 3 before it): what she is for is her party role, and what
        // the tactician may use is her marked rows (RESEARCH §17.13).
        // 12: retired MeleeMage; an Attack row that claims the mob decides whether a
        // mage fights it. The grammar refuses 12, so no row carries it.
        AvoidLinks          = 13, // switch: keep clear of the idle kin of every mob fighting her, whatever AvoidAggro says
        Rest                = 14, // switch: when it holds and no fight is on, her own rest order, down until full; marked, the tactician's MP pacing (RESEARCH §17.13)
    };
    constexpr uint16 BehaviorCount = 15; // one past the highest value ever given, retired ones included

    // The retired behaviour values: the grammar refuses a row that names
    // one, saved or imported, so an old meaning never comes back
    constexpr auto isRetiredBehavior(const uint32 behavior) -> bool
    {
        return behavior == 8 || behavior == 10 || behavior == 11 || behavior == 12;
    }

    // A switch row carries the value 1 and its checkbox is the switch; a
    // parameter row (the formation slot) carries its value
    constexpr auto isSwitch(const Behavior b) -> bool
    {
        return b != Behavior::Formation;
    }

    // The persisted numbers, pinned: a renumbered value fails the build
    // before it can reread a saved row as something else
    static_assert(static_cast<uint16>(G_REACTION_BEHAVIOR) == 100);
    static_assert(static_cast<uint16>(G_CONDITION_STRATEGY) == 100);
    static_assert(static_cast<uint16>(G_CONDITION_TACTICIANS_CHOICE) == 101);
    static_assert(static_cast<uint16>(G_TARGET_LEADERS_TARGET) == 100);
    static_assert(static_cast<uint16>(G_TARGET_TARGETED_BY_ALLY) == 101);
    static_assert(static_cast<uint16>(G_TARGET_TARGETING_ALLY) == 102);
    static_assert(static_cast<uint16>(G_TARGET_TARGETING_SELF) == 103);
    static_assert(static_cast<uint16>(G_SELECT_ENFEEBLE) == 100);
    static_assert(G_STATUS_ENFEEBLE == 10000);

    static_assert(static_cast<uint16>(Behavior::AvoidAggro) == 1);
    static_assert(static_cast<uint16>(Behavior::Formation) == 4);
    static_assert(static_cast<uint16>(Behavior::RestWithPlayer) == 6);
    static_assert(static_cast<uint16>(Behavior::HomePointWithPlayer) == 7);
    static_assert(static_cast<uint16>(Behavior::BoostBeforeWs) == 9);
    static_assert(static_cast<uint16>(Behavior::AvoidLinks) == 13);
    static_assert(static_cast<uint16>(Behavior::Rest) == 14);
    static_assert(BehaviorCount == 15);
    static_assert(isRetiredBehavior(8) && isRetiredBehavior(10) && isRetiredBehavior(11) && isRetiredBehavior(12));
    static_assert(!isRetiredBehavior(static_cast<uint16>(Behavior::AvoidAggro)) && !isRetiredBehavior(static_cast<uint16>(Behavior::Formation)) &&
                  !isRetiredBehavior(static_cast<uint16>(Behavior::RestWithPlayer)) && !isRetiredBehavior(static_cast<uint16>(Behavior::HomePointWithPlayer)) &&
                  !isRetiredBehavior(static_cast<uint16>(Behavior::BoostBeforeWs)) && !isRetiredBehavior(static_cast<uint16>(Behavior::AvoidLinks)) &&
                  !isRetiredBehavior(static_cast<uint16>(Behavior::Rest)));
} // namespace pawn
