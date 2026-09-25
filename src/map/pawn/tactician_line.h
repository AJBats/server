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

#include "engage_math.h"
#include "gambit_ids.h"

#include "ai/helpers/gambits_container.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <optional>
#include <string_view>

// The tactician line (ROADMAP K, RESEARCH §14.12 decisions 7, 14 and
// 17-19). Her first Support Mage row splits her list: the rows above it are
// orders, as any row is; the rows below it are her tactician's allow-list,
// the spells it may cast and the fights it may melee, and they never run as
// orders. Where a row sits gives it its meaning, and a row with none where
// it sits is struck out: kept, shown, and doing nothing. The rules are pure,
// over a row and plain numbers, so xi_test pins them; the gambit engine
// (CGambits) asks.
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
    inline constexpr std::array<Spell, 8>  kPricedDebuffs{ {
        { 58, 14 },  // Paralyze
        { 56, 12 },  // Slow
        { 254, 72 }, // Blind
        { 23, 6 },   // Dia
        { 33, 8 },   // Diaga
        { 220, 62 }, // Poison
        { 225, 63 }, // Poisonga
        { 230, 64 }, // Bio
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

    // A row's meaning where it sits
    enum class State : uint8
    {
        Order,    // an order, as every row above the line is, and every row of a list with no line
        Line,     // her Support Mage row: the line itself
        Allows,   // below the line: something her tactician may use
        NotBelow, // below the line, and nothing her tactician uses: struck out
        Clock,    // below the line on a timer or a chance, which her judgement has no use for: struck out
        NoChoice, // Tactician's choice with no tactician above it: struck out
    };

    constexpr auto struck(const State state) -> bool
    {
        return state == State::NotBelow || state == State::Clock || state == State::NoChoice;
    }

    // The state as the editor reads it on a row's line (Link protocol 13):
    // the server names, the addon words
    constexpr auto token(const State state) -> std::string_view
    {
        switch (state)
        {
            case State::Order:
                return "o";
            case State::Line:
                return "t";
            case State::Allows:
                return "a";
            case State::NotBelow:
                return "x-below";
            case State::Clock:
                return "x-clock";
            case State::NoChoice:
                return "x-choice";
        }
        return "o";
    }

    // Her Support Mage row: a behaviour row that names the role
    inline auto isSupportMageRow(const gambits::Gambit_t& g) -> bool
    {
        const auto behaviour = [](const gambits::Action_t& a)
        {
            return a.reaction == pawn::G_REACTION_BEHAVIOR;
        };
        const auto role = [](const gambits::Action_t& a)
        {
            return static_cast<uint16>(a.select) == static_cast<uint16>(pawn::Behavior::Role) &&
                   a.select_arg == static_cast<uint32>(pawn::Role::SupportMage);
        };
        return !g.actions.empty() && std::ranges::all_of(g.actions, behaviour) && std::ranges::any_of(g.actions, role);
    }

    // The line: the 1-based place of her first Support Mage row, whatever
    // its checkbox or its condition (ROADMAP K call 3); none without one.
    // gambitOf(row) reads a row's gambit, so any list of rows can be asked
    template <typename Rows, typename GambitOf>
    auto lineOf(const Rows& rows, GambitOf&& gambitOf) -> std::optional<std::size_t>
    {
        std::size_t place = 0;
        for (const auto& row : rows)
        {
            ++place;
            if (isSupportMageRow(gambitOf(row)))
            {
                return place;
            }
        }
        return std::nullopt;
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
    // prices on the foe (`Target`), or the melee of a fight a Foe target
    // finds (decision 19)
    enum class Allowance : uint8
    {
        None,
        Cures,
        Debuff,
        Melee,
    };

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
                            (a.select == G_SELECT::HIGHEST && isPricedFamily(a.select_arg));
        if (debuff)
        {
            return g.target_selector == gambits::G_TARGET::TARGET ? Allowance::Debuff : Allowance::None;
        }
        return Allowance::None;
    }

    // A row's state at its 1-based place, given the line. Below the line a
    // row means one thing or nothing (decision 14): what it lets her
    // tactician do, or struck out. Tactician's choice needs a tactician
    // above it, so outside the line's rows it is struck out
    inline auto stateOf(const gambits::Gambit_t& g, const std::size_t place, const std::optional<std::size_t> line) -> State
    {
        if (line.has_value() && place == *line)
        {
            return State::Line;
        }
        if (line.has_value() && place > *line)
        {
            if (allowanceOf(g) == Allowance::None)
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

    // Whether a row below the line lets her tactician cast this spell:
    // Cure (best) any tier, a tier itself, a priced debuff by its id or by
    // its family
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
        const auto family = pricedFamilyOf(spell);
        return family.has_value() && *family == a.select_arg;
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
