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
#include <chrono>
#include <cstddef>
#include <optional>
#include <string_view>

// The tactician's mark (RESEARCH §17.13; before it the tactician line,
// ROADMAP K, RESEARCH §14.12 decisions 7, 14 and 17-19, §17.11). A row
// carrying Tactician's choice (gambit_ids.h) is the tactician's: a tool it
// may use -- a Cure, a priced debuff, a -na, the melee of a fight, a hate
// tool -- for whom the row's target and other conditions allow, when the
// tactician's judgement says. Every other row is an order, as FFXII's are.
// Position means nothing: a marked row and an order sit anywhere, and the
// list's order is their priority. A marked row never runs as an order --
// but for her -na and Erase rows, which run as written while her tactician
// runs, until it has a judgement of its own for ailments (actsAlone), and
// her self buffs, which act where they sit in the think when their own
// when says now (buffNow). A
// mark on a tool the tactician has no judgement for is struck out: kept,
// shown, and doing nothing. The rules are pure, over a row, so xi_test
// pins them; the gambit engine (CGambits) asks.
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
    // target enfeebles her tactician prices. On a marked row her tactician
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

    // A row's meaning. The values are the Link's (CL_GS_*, pinned in
    // link_api.cpp); 1 and 5 were the line's and are retired with it
    enum class State : uint8
    {
        Order       = 0, // an order: a plain row, as every row in a list with no mark
        Tool        = 2, // marked: a tool her tactician may use
        NoJudgement = 3, // marked, and nothing her tactician has a judgement for: struck out
        Clock       = 4, // marked, on a timer or a chance, which her judgement has no use for: struck out
        Misfit      = 6, // an action that cannot be aimed at the side its condition names: struck out, marked or not
        Client      = 7, // a behaviour row in the list of a character his own client drives: only a cardian runs one, struck out
    };

    constexpr auto struck(const State state) -> bool
    {
        return state == State::NoJudgement || state == State::Clock || state == State::Misfit || state == State::Client;
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

    // Whether a row is the tactician's: it carries the mark
    inline auto isMarked(const gambits::Gambit_t& g) -> bool
    {
        return carries(g, pawn::G_CONDITION_TACTICIANS_CHOICE);
    }

    // What the tank's tactician uses for hate (RESEARCH §17.11): Provoke,
    // by its ability id (ability.h, asserted in pawn_gambits.cpp). Flash,
    // Shield Bash and the rest are later rows, each named
    inline constexpr std::array<uint16, 1> kHateAbilities{ 35 }; // Provoke

    constexpr auto isHateAbility(const uint32 ability) -> bool
    {
        return std::ranges::find(kHateAbilities, ability) != kHateAbilities.end();
    }

    // The self buffs her tactician uses on their own clocks while she
    // fights, and Boost and Sneak Attack, which wait for her weapon skill
    // (RESEARCH §17.13, the tool table), by ability id (ability.h, asserted
    // in pawn_gambits.cpp)
    inline constexpr uint16                kBerserk     = 31;
    inline constexpr uint16                kDefender    = 33;
    inline constexpr uint16                kAggressor   = 34;
    inline constexpr uint16                kFocus       = 36;
    inline constexpr uint16                kDodge       = 37;
    inline constexpr uint16                kBoost       = 39;
    inline constexpr uint16                kSneakAttack = 44;
    inline constexpr std::array<uint16, 5> kBuffAbilities{ kBerserk, kDefender, kAggressor, kFocus, kDodge };

    constexpr auto isBuffAbility(const uint32 ability) -> bool
    {
        return std::ranges::find(kBuffAbilities, ability) != kBuffAbilities.end();
    }

    // Berserk and Defender are a Warrior's stance: both may be up at once,
    // and then their numbers cancel, so her tactician keeps the one her
    // seat calls for and takes the other off (the user, 2026-10-02). Seated
    // Tank, Defender's stance; any other seat, Berserk's
    constexpr auto isStanceAbility(const uint32 ability) -> bool
    {
        return ability == kBerserk || ability == kDefender;
    }

    // The stance buff her seat does not want: the one to take off
    constexpr auto wrongStance(const bool tankSeat) -> uint16
    {
        return tankSeat ? kBerserk : kDefender;
    }

    // Berserk for Defender, Defender for Berserk
    constexpr auto otherStance(const uint16 stance) -> uint16
    {
        return stance == kBerserk ? kDefender : kBerserk;
    }

    // The player's own Berserk or Defender -- his order, or a plain row of
    // his firing it -- is never taken off by her tactician, for the one use
    // it put up: the effect that went up within this long of it firing
    // (an ability lands a tick or two after it starts), not a later one
    inline constexpr auto kOrderLands = std::chrono::seconds(5);

    template <typename Rep, typename Period>
    constexpr auto isOrderedUse(const std::chrono::duration<Rep, Period> effectStartAfterOrder) -> bool
    {
        return effectStartAfterOrder >= effectStartAfterOrder.zero() && effectStartAfterOrder <= kOrderLands;
    }

    // A self buff's when: she is fighting, it is not on her already, and
    // her seat allows it -- Defender is the Tank seat's, Berserk is never
    // the Tank seat's (wrongStance), Aggressor, Focus and Dodge are any
    // seat's. Whether the ability is hers and off its recast is the
    // caller's to ask
    constexpr auto buffNow(const uint32 ability, const bool engaged, const bool up, const bool tankSeat) -> bool
    {
        if (!engaged || up || !isBuffAbility(ability))
        {
            return false;
        }
        return ability != wrongStance(tankSeat);
    }

    // Who a marked Cure may be for: someone on the party's side
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

    // What a marked row lets her tactician do -- the tools it has a
    // judgement for: one action, and that one hers -- a Cure for someone on
    // the party's side, a debuff she prices on a Foe row's foe (Enfeeble:
    // the single-target ones), the melee of a fight a Foe row finds
    // (decision 19), a -na or Erase for someone on the party's side, a
    // hate tool on a Foe row's foe, her rest (a Self -> Rest row: the MP
    // pacing, RESEARCH §17.13), a self buff on its clock (a Self row:
    // Berserk, Defender, Aggressor, Focus, Dodge), Boost before her weapon
    // skill (Self), Sneak Attack before it from the mob's back (Self), or a
    // damage spell on a Foe row's foe (Damage spell (any): the Black Mage's
    // nukes). None: a mark with no judgement behind it
    enum class Allowance : uint8
    {
        None,
        Cures,
        Debuff,
        Melee,
        Ailments,
        Hate,
        Rest,
        Buff,
        Boost,
        Nuke,
        SneakAttack,
    };

    // The tools her tactician casts in the party's fights: cures, priced
    // debuffs, ailments and damage spells. A member with a marked row of
    // one of these attends fights at cure range (RESEARCH §17.13); the first
    // three feed the conveyor, a damage spell acts in her think
    // (CGambits::CastNuke)
    constexpr auto isSpellTool(const Allowance allowance) -> bool
    {
        return allowance == Allowance::Cures || allowance == Allowance::Debuff || allowance == Allowance::Ailments || allowance == Allowance::Nuke;
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
            if (a.select != G_SELECT::SPECIFIC)
            {
                return Allowance::None;
            }
            if (isHateAbility(a.select_arg))
            {
                return engage::isFoeTarget(g.target_selector) ? Allowance::Hate : Allowance::None;
            }
            const bool self = g.target_selector == gambits::G_TARGET::SELF;
            if (a.select_arg == kBoost)
            {
                return self ? Allowance::Boost : Allowance::None;
            }
            if (a.select_arg == kSneakAttack)
            {
                return self ? Allowance::SneakAttack : Allowance::None;
            }
            return isBuffAbility(a.select_arg) && self ? Allowance::Buff : Allowance::None;
        }
        if (a.reaction == pawn::G_REACTION_BEHAVIOR)
        {
            const bool rest = static_cast<uint16>(a.select) == static_cast<uint16>(pawn::Behavior::Rest);
            return rest && g.target_selector == gambits::G_TARGET::SELF ? Allowance::Rest : Allowance::None;
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
        // Damage spell (any): marked, her tactician's nukes (RESEARCH §17.13,
        // the Black Mage) -- when, and which of her damage spells
        if (a.select == G_SELECT::RANDOM)
        {
            return engage::isFoeTarget(g.target_selector) ? Allowance::Nuke : Allowance::None;
        }
        return Allowance::None;
    }

    // A row's state, wherever it sits. A marked row means one thing or
    // nothing (decision 14): the tool it lets her tactician use, or struck
    // out -- a mark on nothing the tactician has a judgement for, or on a
    // timer or a chance, which its judgement has no use for. A plain row is
    // an order. `fits`: whether every action fits the row's side (fitsSide,
    // read off the spell and ability tables by the caller); a misfit is
    // struck out, marked or not
    inline auto stateOf(const gambits::Gambit_t& g, const bool fits = true) -> State
    {
        if (!fits)
        {
            return State::Misfit;
        }
        if (!isMarked(g))
        {
            return State::Order;
        }
        if (allowanceOf(g) == Allowance::None)
        {
            return State::NoJudgement;
        }
        if (carries(g, gambits::G_CONDITION::TIMER) || carries(g, gambits::G_CONDITION::RANDOM))
        {
            return State::Clock;
        }
        return State::Tool;
    }

    // A row's state in the list of a character his own client drives
    // (gambit_host.h OwnClient): a cardian's (stateOf), but for a behaviour
    // row, which moves a body or speaks to her tactician about moving, and
    // does nothing for him
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
        return stateOf(g, fits);
    }

    // Whether a marked row lets her tactician cast this spell: Cure (best)
    // any tier, a tier itself, a priced debuff by its id or by its family,
    // and Enfeeble the single-target priced debuffs
    inline auto allowsSpell(const gambits::Gambit_t& g, const uint32 spell) -> bool
    {
        const auto kind = allowanceOf(g);
        if (!isMarked(g) || (kind != Allowance::Cures && kind != Allowance::Debuff))
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

    // Whether a marked row lets her tactician use this hate tool: the row
    // names it
    inline auto allowsAbility(const gambits::Gambit_t& g, const uint32 ability) -> bool
    {
        return isMarked(g) && allowanceOf(g) == Allowance::Hate && g.actions.front().select_arg == ability;
    }

    // Whether a row acts on its own, as her think runs it: an order, and,
    // while her tactician runs, a marked -na or Erase row. Her tactician
    // has no judgement of its own for ailments yet, so such a row runs as
    // written, the mark holding on it: under -na (best) she takes an
    // ailment off whoever carries one she can cure. With her tactician not
    // running it waits, as her marked Cure rows do
    inline auto actsAlone(const State state, const gambits::Gambit_t& g, const bool runs) -> bool
    {
        return state == State::Order || (runs && state == State::Tool && allowanceOf(g) == Allowance::Ailments);
    }

    // Her tactician's melee (decision 19): a fight a marked Attack row
    // claims is hers to draw and close on while her tactician runs, her
    // recovery is not due, and she is not down resting. Standing again from
    // a rest is being ready to melee again; nothing else holds her out
    constexpr auto meleeAllowed(const bool runs, const bool recoveryDue, const bool resting) -> bool
    {
        return runs && !recoveryDue && !resting;
    }

    // She leaves a fight to rest when it is her tactician's melee -- a
    // marked Attack row claims the mob and no order does -- her recovery is
    // due, and the player did not order this fight himself (decision 16)
    constexpr auto leavesToRest(const bool runs, const bool recoveryDue, const bool claimedByOrder, const bool claimedByMark, const bool playersOrder) -> bool
    {
        return runs && recoveryDue && claimedByMark && !claimedByOrder && !playersOrder;
    }

    // Whether a weapon skill takes Sneak Attack, read off its own script
    // (scripts/actions/weaponskills/<name>.lua): only the server's physical
    // weapon skill path applies it. The magical path (Gust Slash), the
    // ranged one (Sidewinder) and the scripts of their own (Energy Steal,
    // Energy Drain, Spirits Within) ignore it -- and spend it all the same,
    // since it goes with the next weapon skill or swing (RESEARCH §17.13
    // item 5)
    constexpr auto scriptTakesSneakAttack(const std::string_view source) -> bool
    {
        return source.find("doPhysicalWeaponskill") != std::string_view::npos;
    }

    // Whether a weapon skill row of hers takes Sneak Attack as it fires: only
    // an order fires (a marked weapon skill row has no judgement behind it);
    // one naming a skill does when the skill is hers and takes it; Weapon
    // skill (best) or (any) does while one of hers takes it. No row that
    // does, and her Sneak Attack goes naked (CGambits::NakedSneakNow)
    template <typename Takes>
    auto wsRowTakesSneak(const gambits::Gambit_t& g, const State state, const bool anyOfHersTakes, Takes&& takesIfHers) -> bool
    {
        if (state != State::Order)
        {
            return false;
        }
        return std::ranges::any_of(g.actions, [&](const gambits::Action_t& a)
                                   {
                                       if (a.reaction != gambits::G_REACTION::WS)
                                       {
                                           return false;
                                       }
                                       if (a.select == gambits::G_SELECT::SPECIFIC)
                                       {
                                           return takesIfHers(static_cast<uint16>(a.select_arg));
                                       }
                                       return (a.select == gambits::G_SELECT::HIGHEST || a.select == gambits::G_SELECT::RANDOM) && anyOfHersTakes;
                                   });
    }
} // namespace cardian::tactician
