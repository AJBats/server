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

#include "ailments.h"
#include "engage_math.h"
#include "gambit_ids.h"

#include "ai/helpers/gambits_container.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <optional>
#include <string_view>

// The tactician line (ROADMAP K, RESEARCH §14.12 decisions 7, 14 and
// 17-19; §17.11). Her first Role row that says Support Mage or Tank splits
// her list: the rows above it are orders, as any row is; the rows below it
// are her tactician's allow-list, and the line's role says which tactician
// -- a Support Mage's: the spells it may cast and the fights it may melee; a
// tank's: the fights it may melee and the hate tools it may use. They never
// run as orders -- but for her -na and Erase rows under a Support Mage line,
// which run as written while her tactician runs, until it has a judgement
// of its own for ailments (actsAlone). Where a row sits gives it its
// meaning, and a row with none where it sits is struck out: kept, shown,
// and doing nothing. The rules are pure, over a row and plain numbers, so
// xi_test pins them; the gambit engine (CGambits) asks.
namespace cardian::tactician
{
    // What her tactician casts, by spell id and spell family (spell.h's
    // numbers, asserted where spell_bank.cpp builds its lists from these):
    // the Cure tiers, and the debuffs the bank prices, in kPriced's order
    struct Spell
    {
        uint16 id     = 0;
        uint32 family = 0;
    };
    inline constexpr uint32                kCureFamily = 1; // SPELLFAMILY_CURE
    inline constexpr std::array<uint16, 6> kCureTiers{ 1, 2, 3, 4, 5, 6 }; // Cure to Cure VI
    inline constexpr std::array<Spell, 14> kPricedDebuffs{ {
        { 58, 14 },  // Paralyze
        { 56, 12 },  // Slow
        { 254, 72 }, // Blind
        { 23, 6 },   // Dia
        { 33, 8 },   // Diaga
        { 220, 62 }, // Poison
        { 225, 63 }, // Poisonga
        { 230, 64 }, // Bio
        { 235, 65 }, // Burn
        { 236, 65 }, // Frost
        { 237, 65 }, // Choke
        { 238, 65 }, // Rasp
        { 239, 65 }, // Shock
        { 240, 65 }, // Drown
    } };

    constexpr auto isCureTier(const uint32 spell) -> bool
    {
        return std::ranges::find(kCureTiers, spell) != kCureTiers.end();
    }

    // The family of a debuff the bank prices; none for any other spell
    constexpr auto pricedFamilyOf(const uint32 spell) -> std::optional<uint32>
    {
        for (const auto& p : kPricedDebuffs)
        {
            if (p.id == spell)
            {
                return p.family;
            }
        }
        return std::nullopt;
    }

    constexpr auto isPricedFamily(const uint32 family) -> bool
    {
        return std::ranges::any_of(kPricedDebuffs, [family](const Spell& p)
                                   {
                                       return p.family == family;
                                   });
    }

    // What Enfeeble casts (gambit_ids.h G_SELECT_ENFEEBLE): the single-
    // target enfeebles her tactician prices. Below the line her tactician
    // may cast any of them; as an order she casts the first of them she can
    // that the foe does not carry yet. The -ga spells are never Enfeeble's:
    // one reaches every mob around the foe, so they take a row that names them
    inline constexpr std::array<uint16, 12> kEnfeebleOrder{ 58, 56, 254, 23, 230, 220,   // Paralyze, Slow, Blind, Dia, Bio, Poison,
                                                            235, 236, 237, 238, 239, 240 }; // Burn, Frost, Choke, Rasp, Shock, Drown
    static_assert(std::ranges::all_of(kEnfeebleOrder, [](const uint16 spell) { return pricedFamilyOf(spell).has_value(); }));

    constexpr auto isEnfeebleSpell(const uint32 spell) -> bool
    {
        return std::ranges::find(kEnfeebleOrder, spell) != kEnfeebleOrder.end();
    }

    // castable(spell): she can cast it now and the foe does not carry it
    template <typename Castable>
    constexpr auto firstEnfeeble(Castable&& castable) -> std::optional<uint16>
    {
        for (const auto spell : kEnfeebleOrder)
        {
            if (castable(spell))
            {
                return spell;
            }
        }
        return std::nullopt;
    }

    // A row's meaning where it sits
    enum class State : uint8
    {
        Order,    // an order, as every row above the line is, and every row of a list with no line
        Line,     // her Support Mage or Tank row: the line itself
        Allows,   // below the line: something her tactician may use
        NotBelow, // below the line, and nothing her tactician uses: struck out
        Clock,    // below the line on a timer or a chance, which her judgement has no use for: struck out
        NoChoice, // Tactician's choice with no tactician above it: struck out
        Misfit,   // an action that cannot be aimed at the side its condition names: struck out, wherever it sits
        Client,   // a behaviour row in the list of a character his own client drives: only a cardian runs one, struck out
    };

    constexpr auto struck(const State state) -> bool
    {
        return state == State::NotBelow || state == State::Clock || state == State::NoChoice || state == State::Misfit || state == State::Client;
    }

    // An action's target flags, as upstream's TARGETTYPE writes them
    // (battle_entity.h; pinned against it in pawn_gambits.cpp): the enemy,
    // and every flag on the party's side
    constexpr uint16 kTargetEnemy    = 0x0004;
    constexpr uint16 kTargetFriendly = 0x0001 | 0x0002 | 0x0008 | 0x0010 | 0x0020 | 0x0080 | 0x0100 | 0x0200;

    // Whether an action with these target flags can be aimed at the side
    // a row's condition names (RESEARCH §14.13): a Foe condition wants an
    // action for an enemy, a Self or Ally one an action for her side. A row
    // that fails is kept, struck out, and does nothing -- FFXII takes any
    // row. No flags (a behaviour, Entrust) is nothing to judge. Upstream's
    // trigger targets split the two: 12 reads her and acts on the foe, 13
    // reads the foe and acts on her. `onHerFight`: a weapon skill or an
    // ability aimed at the enemy, which lands on her fight whatever the row
    // names; under a Self condition that is the row's meaning, as in
    // upstream's trust brains (Self: TP >= 1000 -> a weapon skill)
    constexpr auto fitsSide(const gambits::G_TARGET target, const uint16 flags, const bool onHerFight = false) -> bool
    {
        if (flags == 0)
        {
            return true;
        }
        if (onHerFight && target == gambits::G_TARGET::SELF && (flags & kTargetEnemy) != 0)
        {
            return true;
        }
        const bool onFoe = target == gambits::G_TARGET::TRIGGER_SELF_ACTION_TARGET ||
                           (target != gambits::G_TARGET::TRIGGER_TARGET_ACTION_SELF && engage::isFoeTarget(target));
        return (flags & (onFoe ? kTargetEnemy : kTargetFriendly)) != 0;
    }

    // The state as the editor reads it on a row's line (Link protocol 13):
    // the server names, the addon words
    // The role a behaviour row names that makes it a line: Support Mage or
    // Tank (RESEARCH §17.11: the Tank row is a line as Support Mage's is).
    // A Damage row is a role and no line: nothing judges under it yet
    inline auto lineRoleOf(const gambits::Gambit_t& g) -> std::optional<pawn::Role>
    {
        const auto behaviour = [](const gambits::Action_t& a)
        {
            return a.reaction == pawn::G_REACTION_BEHAVIOR;
        };
        if (g.actions.empty() || !std::ranges::all_of(g.actions, behaviour))
        {
            return std::nullopt;
        }
        for (const auto& a : g.actions)
        {
            if (static_cast<uint16>(a.select) == static_cast<uint16>(pawn::Behavior::Role) &&
                (a.select_arg == static_cast<uint32>(pawn::Role::SupportMage) || a.select_arg == static_cast<uint32>(pawn::Role::Tank)))
            {
                return static_cast<pawn::Role>(a.select_arg);
            }
        }
        return std::nullopt;
    }

    inline auto isLineRow(const gambits::Gambit_t& g) -> bool
    {
        return lineRoleOf(g).has_value();
    }

    // The line: the 1-based place of her first line row, whatever its
    // checkbox or its condition (ROADMAP K call 3), and whose tactician it
    // is. One line a list: a second line row below it is a behaviour row
    // below the line, struck out (RESEARCH §17.10)
    struct Line
    {
        std::size_t place = 0;
        pawn::Role  role  = pawn::Role::SupportMage;

        auto operator==(const Line&) const -> bool = default;
    };

    // gambitOf(row) reads a row's gambit, so any list of rows can be asked
    template <typename Rows, typename GambitOf>
    auto lineOf(const Rows& rows, GambitOf&& gambitOf) -> std::optional<Line>
    {
        std::size_t place = 0;
        for (const auto& row : rows)
        {
            ++place;
            if (const auto role = lineRoleOf(gambitOf(row)); role.has_value())
            {
                return Line{ place, *role };
            }
        }
        return std::nullopt;
    }

    // What the tank's tactician uses for hate (RESEARCH §17.11): Provoke,
    // by its ability id (ability.h, asserted in pawn_gambits.cpp). Flash,
    // Shield Bash and the rest are later rows, each named
    inline constexpr std::array<uint16, 1> kHateAbilities{ 35 }; // Provoke

    constexpr auto isHateAbility(const uint32 ability) -> bool
    {
        return std::ranges::find(kHateAbilities, ability) != kHateAbilities.end();
    }

    inline auto carries(const gambits::Gambit_t& g, const gambits::G_CONDITION condition) -> bool
    {
        return std::ranges::any_of(g.predicate_groups, [condition](const gambits::PredicateGroup_t& group)
                                   {
                                       return std::ranges::any_of(group.predicates, [condition](const gambits::Predicate_t& p)
                                                                  {
                                                                      return p.condition == condition;
                                                                  });
                                   });
    }

    // Who a Cure below the line may be for: someone on the party's side
    constexpr auto curesTarget(const gambits::G_TARGET target) -> bool
    {
        using gambits::G_TARGET;
        switch (target)
        {
            case G_TARGET::SELF:
            case G_TARGET::PARTY:
            case G_TARGET::MASTER:
            case G_TARGET::TANK:
            case G_TARGET::MELEE:
            case G_TARGET::RANGED:
            case G_TARGET::CASTER:
            case G_TARGET::TOP_ENMITY:
                return true;
            default:
                return false;
        }
    }

    // What a row below the line lets her tactician do: one action, and
    // that one hers -- a Cure for someone on the party's side, a debuff she
    // prices on a Foe row's foe (Enfeeble: the single-target ones), the
    // melee of a fight a Foe row finds (decision 19), a -na or Erase for
    // someone on the party's side, or a hate tool on a Foe row's foe
    enum class Allowance : uint8
    {
        None,
        Cures,
        Debuff,
        Melee,
        Ailments,
        Hate,
    };

    // Which allowances a line's tactician reads: a Support Mage's casts and
    // melees; a tank's melees and holds hate. A row below a line whose
    // tactician does not read it is struck out there (NotBelow)
    constexpr auto allowedUnder(const pawn::Role line, const Allowance allowance) -> bool
    {
        switch (allowance)
        {
            case Allowance::Cures:
            case Allowance::Debuff:
            case Allowance::Ailments:
                return line == pawn::Role::SupportMage;
            case Allowance::Melee:
                return true;
            case Allowance::Hate:
                return line == pawn::Role::Tank;
            default:
                return false;
        }
    }

    // An action that takes ailments off: -na (best), a -na or Erase
    inline auto isRemovalAction(const gambits::Action_t& a) -> bool
    {
        using gambits::G_SELECT;
        return (a.select == G_SELECT::HIGHEST && a.select_arg == ailments::kNaFamily) || (a.select == G_SELECT::SPECIFIC && ailments::isRemoval(a.select_arg));
    }

    inline auto allowanceOf(const gambits::Gambit_t& g) -> Allowance
    {
        using gambits::G_REACTION;
        using gambits::G_SELECT;
        if (g.actions.size() != 1)
        {
            return Allowance::None;
        }
        const auto& a = g.actions.front();
        if (a.reaction == G_REACTION::ATTACK)
        {
            return engage::isFoeTarget(g.target_selector) ? Allowance::Melee : Allowance::None;
        }
        if (a.reaction == G_REACTION::JA)
        {
            const bool hate = a.select == G_SELECT::SPECIFIC && isHateAbility(a.select_arg);
            return hate && engage::isFoeTarget(g.target_selector) ? Allowance::Hate : Allowance::None;
        }
        if (a.reaction != G_REACTION::MA)
        {
            return Allowance::None;
        }
        const bool cure = (a.select == G_SELECT::HIGHEST && a.select_arg == kCureFamily) || (a.select == G_SELECT::SPECIFIC && isCureTier(a.select_arg));
        if (cure)
        {
            return curesTarget(g.target_selector) ? Allowance::Cures : Allowance::None;
        }
        const bool debuff = (a.select == G_SELECT::SPECIFIC && pricedFamilyOf(a.select_arg).has_value()) ||
                            (a.select == G_SELECT::HIGHEST && isPricedFamily(a.select_arg)) || a.select == pawn::G_SELECT_ENFEEBLE;
        if (debuff)
        {
            return engage::isFoeTarget(g.target_selector) ? Allowance::Debuff : Allowance::None;
        }
        if (isRemovalAction(a))
        {
            return curesTarget(g.target_selector) ? Allowance::Ailments : Allowance::None;
        }
        return Allowance::None;
    }

    // A row's state at its 1-based place, given the line. Below the line a
    // row means one thing or nothing (decision 14): what it lets her
    // tactician do, or struck out -- and the line's tactician must read it
    // (allowedUnder). Tactician's choice needs a tactician above it, so
    // outside the line's rows it is struck out
    // `fits`: whether every action fits the row's side (fitsSide, read off
    // the spell and ability tables by the caller); a misfit is struck out
    // wherever it sits
    inline auto stateOf(const gambits::Gambit_t& g, const std::size_t place, const std::optional<Line> line, const bool fits = true) -> State
    {
        if (!fits)
        {
            return State::Misfit;
        }
        if (line.has_value() && place == line->place)
        {
            return State::Line;
        }
        if (line.has_value() && place > line->place)
        {
            if (!allowedUnder(line->role, allowanceOf(g)))
            {
                return State::NotBelow;
            }
            if (carries(g, gambits::G_CONDITION::TIMER) || carries(g, gambits::G_CONDITION::RANDOM))
            {
                return State::Clock;
            }
            return State::Allows;
        }
        return carries(g, pawn::G_CONDITION_TACTICIANS_CHOICE) ? State::NoChoice : State::Order;
    }

    // A row's state in the list of a character his own client drives
    // (gambit_host.h OwnClient). He has no tactician, so his list has no
    // line and every row is an order -- but a behaviour row (a role, a line
    // row, Avoid aggro...) moves a cardian or speaks to her tactician, and
    // does nothing for him; Tactician's choice has nobody to choose
    inline auto ownClientStateOf(const gambits::Gambit_t& g, const bool fits = true) -> State
    {
        if (!fits)
        {
            return State::Misfit;
        }
        const bool behaviour = !g.actions.empty() && std::ranges::all_of(g.actions, [](const gambits::Action_t& a)
                                                                         {
                                                                             return a.reaction == pawn::G_REACTION_BEHAVIOR;
                                                                         });
        if (behaviour)
        {
            return State::Client;
        }
        return carries(g, pawn::G_CONDITION_TACTICIANS_CHOICE) ? State::NoChoice : State::Order;
    }

    // Whether a row below the line lets her tactician cast this spell:
    // Cure (best) any tier, a tier itself, a priced debuff by its id or by
    // its family, and Enfeeble the single-target priced debuffs
    inline auto allowsSpell(const gambits::Gambit_t& g, const uint32 spell) -> bool
    {
        const auto kind = allowanceOf(g);
        if (kind != Allowance::Cures && kind != Allowance::Debuff)
        {
            return false;
        }
        const auto& a = g.actions.front();
        if (a.select == gambits::G_SELECT::SPECIFIC)
        {
            return a.select_arg == spell;
        }
        if (kind == Allowance::Cures)
        {
            return isCureTier(spell);
        }
        if (a.select == pawn::G_SELECT_ENFEEBLE)
        {
            return isEnfeebleSpell(spell);
        }
        const auto family = pricedFamilyOf(spell);
        return family.has_value() && *family == a.select_arg;
    }

    // Whether a row below a Tank line lets her tactician use this hate
    // tool: the row names it
    inline auto allowsAbility(const gambits::Gambit_t& g, const uint32 ability) -> bool
    {
        return allowanceOf(g) == Allowance::Hate && g.actions.front().select_arg == ability;
    }

    // Whether a row acts on its own, as her think runs it: an order, and,
    // below the line while her tactician runs, a -na or Erase row. Her
    // tactician has no judgement of its own for ailments yet, so such a row
    // runs as written, Tactician's choice holding on it as on every row
    // below the line: under -na (best) she takes an ailment off whoever
    // carries one she can cure. With her tactician not running it waits, as
    // her Cure rows below the line do
    inline auto actsAlone(const State state, const gambits::Gambit_t& g, const bool runs) -> bool
    {
        return state == State::Order || (runs && state == State::Allows && allowanceOf(g) == Allowance::Ailments);
    }

    // Her tactician's melee (decision 19): a fight a row below the line
    // claims is hers to draw and close on while her tactician runs, her
    // recovery is not due, and she is not down resting. Standing again from
    // a rest is being ready to melee again; nothing else holds her out
    constexpr auto meleeAllowed(const bool runs, const bool recoveryDue, const bool resting) -> bool
    {
        return runs && !recoveryDue && !resting;
    }

    // She leaves a fight to rest when it is her tactician's melee -- a row
    // below the line claims the mob and none above does -- her recovery is
    // due, and the player did not order this fight himself (decision 16)
    constexpr auto leavesToRest(const bool runs, const bool recoveryDue, const bool claimedAbove, const bool claimedBelow, const bool playersOrder) -> bool
    {
        return runs && recoveryDue && claimedBelow && !claimedAbove && !playersOrder;
    }
} // namespace cardian::tactician
