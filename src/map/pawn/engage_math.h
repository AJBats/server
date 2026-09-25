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

#include "gambit_ids.h"

#include "ai/helpers/gambits_container.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <optional>
#include <span>
#include <string_view>

// The rows that decide which fight a cardian takes (ROADMAP K): an engage
// row names a foe around the party (gambit_ids.h, targets 100-103) and
// Attack. The door reads them; the think never runs one. The rules here
// are pure functions over a row and over plain descriptions of the foes,
// so xi_test pins them; the controller's door (CPawnController) gathers
// the foes and asks.
namespace cardian::engage
{
    // A foe around the party that is not her fight yet: the targets only an
    // engage row names
    constexpr auto isFoeTarget(const gambits::G_TARGET target) -> bool
    {
        const auto id = static_cast<uint16>(target);
        return id >= static_cast<uint16>(pawn::G_TARGET_LEADERS_TARGET) && id <= static_cast<uint16>(pawn::G_TARGET_TARGETING_SELF);
    }

    // An engage row: one whose action is Attack
    inline auto isEngageRow(const gambits::Gambit_t& g) -> bool
    {
        return std::ranges::any_of(g.actions, [](const gambits::Action_t& a)
                                   {
                                       return a.reaction == gambits::G_REACTION::ATTACK;
                                   });
    }

    // Why the editor refuses a row, "" when it may stand. A foe target goes
    // with Attack alone, and Attack with a foe target alone: a foe target
    // on any other action, or Attack on an ally or her own fight, would
    // never act. An engage row is read on every roam tick, so a condition
    // that keeps a clock (TIMER) or rolls a die (RANDOM) would run down or
    // reroll there; it is refused on one.
    inline auto pairingError(const gambits::Gambit_t& g) -> std::string_view
    {
        const bool foe    = isFoeTarget(g.target_selector);
        const bool attack = isEngageRow(g);
        if (foe && !std::ranges::all_of(g.actions, [](const gambits::Action_t& a) { return a.reaction == gambits::G_REACTION::ATTACK; }))
        {
            return "a Foe target goes with Attack only";
        }
        if (attack && !foe)
        {
            return "Attack needs a Foe target";
        }
        if (attack)
        {
            for (const auto& group : g.predicate_groups)
            {
                for (const auto& p : group.predicates)
                {
                    if (p.condition == gambits::G_CONDITION::TIMER || p.condition == gambits::G_CONDITION::RANDOM)
                    {
                        return "an Attack row cannot wait on a timer or a chance";
                    }
                }
            }
        }
        return "";
    }

    // The rows the door reads: an enabled engage row the editor would let
    // stand, and none while her master switch is off. A row that fails the
    // pairing (only a hand-edited saved set holds one) is passed over
    inline auto doorReads(const bool master, const bool enabled, const gambits::Gambit_t& g) -> bool
    {
        return master && enabled && isEngageRow(g) && pairingError(g).empty();
    }

    // ------------------------------------------------------------------
    // The foes around the party, and the finders that name them
    // ------------------------------------------------------------------

    // One finder per foe target. Each says which foes are its kind
    // (accepts), which is how a row claims a mob she is already at, and
    // picks, for a new fight, the first foe of its kind in the order the
    // foes were gathered (firstFound)
    enum class Finder : uint8
    {
        LeadersTarget, // the party leader's battle target, while he is engaged
        AllysFight,    // a mob another cardian of her party is engaged on
        OnAlly,        // an engaged mob whose target is her or a member of her party
        OnSelf,        // an engaged mob whose target is her
    };

    // The party's fight is scanned in this order: the leader's weapon
    // drawn commits the party first, another cardian's fight (a hunter's
    // pull) next, then a mob that has come for one of us
    constexpr std::array<Finder, 4> kScanOrder{ Finder::LeadersTarget, Finder::AllysFight, Finder::OnAlly, Finder::OnSelf };

    // The finder a row's target names; none for a target that is not a foe
    constexpr auto finderOf(const gambits::G_TARGET target) -> std::optional<Finder>
    {
        switch (static_cast<uint16>(target))
        {
            case static_cast<uint16>(pawn::G_TARGET_LEADERS_TARGET):
                return Finder::LeadersTarget;
            case static_cast<uint16>(pawn::G_TARGET_TARGETED_BY_ALLY):
                return Finder::AllysFight;
            case static_cast<uint16>(pawn::G_TARGET_TARGETING_ALLY):
                return Finder::OnAlly;
            case static_cast<uint16>(pawn::G_TARGET_TARGETING_SELF):
                return Finder::OnSelf;
            default:
                return std::nullopt;
        }
    }

    // A foe around the party as the finders see it: what it is to the
    // party this tick. The controller fills it from the entities; nothing
    // here reads one. When it gathers the foes for a new fight it takes
    // only live ones within the leash of the party's place
    struct Foe
    {
        bool leadersTarget = false; // the leader's battle target, while he is engaged
        bool allysFight    = false; // another cardian of her party is engaged on it
        bool onParty       = false; // engaged, and its target is her or a member of her party
        bool onSelf        = false; // engaged, and its target is her
        bool underground   = false; // below ground with no fight on
        bool heldOff       = false; // her door refused it a moment ago (the hold-off)
    };

    constexpr auto accepts(const Finder finder, const Foe& foe) -> bool
    {
        switch (finder)
        {
            case Finder::LeadersTarget:
                return foe.leadersTarget;
            case Finder::AllysFight:
                return foe.allysFight;
            case Finder::OnAlly:
                return foe.onParty || foe.onSelf;
            case Finder::OnSelf:
                return foe.onSelf;
        }
        return false;
    }

    // Whether a foe counts when a fight is chosen: one below ground with no
    // fight on, or one her door holds off, is absent, so the next foe of
    // its kind, and then the next row, get their turn
    constexpr auto foeCounts(const Foe& foe) -> bool
    {
        return !foe.underground && !foe.heldOff;
    }

    // A finder's pick for a new fight: the first foe, in the order the
    // foes were gathered, that it accepts, that counts, and that `holds`
    // passes (holds(foe index): a row's conditions read on that foe)
    template <typename Holds>
    constexpr auto firstFound(const Finder finder, const std::span<const Foe> foes, Holds&& holds) -> std::optional<std::size_t>
    {
        for (std::size_t i = 0; i < foes.size(); ++i)
        {
            if (accepts(finder, foes[i]) && foeCounts(foes[i]) && holds(i))
            {
                return i;
            }
        }
        return std::nullopt;
    }

    constexpr auto firstFound(const Finder finder, const std::span<const Foe> foes) -> std::optional<std::size_t>
    {
        return firstFound(finder, foes, [](std::size_t)
                          {
                              return true;
                          });
    }

    // A foe picked, and the finder that picked it (for the why line)
    struct Pick
    {
        Finder      finder = Finder::LeadersTarget;
        std::size_t foe    = 0;
    };

    // The party's fight, whatever her rows say: what a Support Mage
    // attends, and what her rest watches for. Nothing while she retreats;
    // else each finder in scan order, its first-found pick, so the rows and
    // this scan agree on which foes count
    constexpr auto partyFight(const bool retreating, const std::span<const Foe> foes) -> std::optional<Pick>
    {
        if (retreating)
        {
            return std::nullopt;
        }
        for (const auto finder : kScanOrder)
        {
            if (const auto foe = firstFound(finder, foes); foe.has_value())
            {
                return Pick{ finder, *foe };
            }
        }
        return std::nullopt;
    }

    // ------------------------------------------------------------------
    // Her rows, and the door's answer
    // ------------------------------------------------------------------

    // An engage row as the door reads it: its number (the caller's, handed
    // back by a pick or a claim to say which row it was), its target and
    // its checkbox
    struct Row
    {
        std::size_t       index   = 0;
        gambits::G_TARGET target  = pawn::G_TARGET_LEADERS_TARGET;
        bool              enabled = true;
    };

    // The row that took a foe, its finder and the foe
    struct RowPick
    {
        std::size_t row    = 0; // the row's number (Row::index)
        Finder      finder = Finder::LeadersTarget;
        std::size_t foe    = 0;
    };

    // Her rows' choice of a new fight. Nothing while she retreats or while
    // her master switch is off. Otherwise her enabled rows, top down: each
    // asks its finder for the first foe that counts and on which the row's
    // conditions hold (holds(row position in `rows`, foe index)); the first
    // row with one wins. Every enabled Attack row is an order here
    template <typename Holds>
    constexpr auto chooseRow(const bool master, const bool retreating, const std::span<const Row> rows, const std::span<const Foe> foes, Holds&& holds)
        -> std::optional<RowPick>
    {
        if (!master || retreating)
        {
            return std::nullopt;
        }
        for (std::size_t r = 0; r < rows.size(); ++r)
        {
            const auto finder = finderOf(rows[r].target);
            if (!rows[r].enabled || !finder.has_value())
            {
                continue;
            }
            const auto foe = firstFound(*finder, foes, [&](const std::size_t i)
                                        {
                                            return holds(r, i);
                                        });
            if (foe.has_value())
            {
                return RowPick{ rows[r].index, *finder, *foe };
            }
        }
        return std::nullopt;
    }

    // The row that claims a mob, and its finder
    struct Claim
    {
        std::size_t row    = 0; // the row's number (Row::index)
        Finder      finder = Finder::LeadersTarget;
    };

    // The first of her rows that claims this foe: an enabled row, master
    // on, whose finder accepts it and whose conditions hold on it
    // (holds(row position in `rows`)). Where the foe stands and whether it
    // counts for a new fight do not matter: a claim decides how she fights
    // a mob she is already at or sent to, not which fight she takes
    template <typename Holds>
    constexpr auto claimingRow(const bool master, const std::span<const Row> rows, const Foe& foe, Holds&& holds) -> std::optional<Claim>
    {
        if (!master)
        {
            return std::nullopt;
        }
        for (std::size_t r = 0; r < rows.size(); ++r)
        {
            const auto finder = finderOf(rows[r].target);
            if (rows[r].enabled && finder.has_value() && accepts(*finder, foe) && holds(r))
            {
                return Claim{ rows[r].index, *finder };
            }
        }
        return std::nullopt;
    }

    // She attends the fight on a mob from the perimeter instead of
    // fighting it: she holds the Support Mage role and no row of hers
    // claims the mob. A row that claims it makes her fight it
    constexpr auto attendsFight(const bool supportMage, const bool claimed) -> bool
    {
        return supportMage && !claimed;
    }

    // The hunt (the Orders page's Pull: the party's strategy, never gated
    // by her gambits). A Support Mage whose rows take no fight at all never
    // pulls, since she attends. One enabled Attack row makes her a fighter,
    // and a pull she chose is hers to fight, never an attend
    constexpr auto huntsForParty(const bool supportMage, const bool anyEngageRow) -> bool
    {
        return !supportMage || anyEngageRow;
    }

    // How she takes the fight the door answers with
    enum class How : uint8
    {
        Draw,   // she draws on it, or walks in first
        Attend, // she attends it from the perimeter, weapon away
    };

    // The door's answer. A row's pick is drawn on. With none, a Support
    // Mage with somewhere to keep cure range to (an anchor or a stake)
    // attends the party's fight, so by default she attends without
    // engaging monsters; with nowhere, she takes only what her rows take,
    // which by default is nothing, not even a mob on her. A camp member out
    // in the wild (campMember) draws on the party's fight: a camp takes its
    // fights together, the world's own engagement as its leader's join is,
    // so her own rows -- whatever they have been edited to -- never leave
    // her camp idle. Otherwise the door has nothing: with her gambits off
    // she takes no fight of her own
    constexpr auto doorAnswer(const bool rowPicked, const bool partyFight, const bool supportMage, const bool hasPlace, const bool campMember = false)
        -> std::optional<How>
    {
        if (rowPicked)
        {
            return How::Draw;
        }
        if (supportMage && hasPlace && partyFight)
        {
            return How::Attend;
        }
        if (campMember && partyFight)
        {
            return How::Draw;
        }
        return std::nullopt;
    }

    // The beat before she acts on the door's answer: a draw waits her
    // reaction beat, an attendance none, since she is not walking in
    constexpr auto waitsBeat(const How how) -> bool
    {
        return how == How::Draw;
    }

    // What becomes of an attendance she has committed to: the mob engaged,
    // or the player's order
    enum class Kept : uint8
    {
        Attend, // she still attends it
        Draw,   // a row now claims that same mob: she fights it (the melee-mage upgrade)
        Stop,   // she stops attending: her gambits are off, or no row speaks for her role
    };

    constexpr auto keptAttendance(const bool supportMage, const bool claimed) -> Kept
    {
        if (claimed)
        {
            return Kept::Draw;
        }
        return supportMage ? Kept::Attend : Kept::Stop;
    }

    // Holding for the player's strike, while he is engaged
    enum class HoldStep : uint8
    {
        Keep,      // his target is the one she holds on
        Follow,    // she draws on his new target and holds there
        StandDown, // she sheathes, and the door takes her next fight
    };

    // His target moved: she follows it when her rows take the new one now
    // and she may draw on it outright; otherwise she stands down, since a
    // hold is not a begun fight and she never holds on a mob he has left.
    // A follow whose draw then fails stands her down too
    constexpr auto holdStep(const bool targetMoved, const bool rowsTakeIt, const bool mayDraw) -> HoldStep
    {
        if (!targetMoved)
        {
            return HoldStep::Keep;
        }
        return rowsTakeIt && mayDraw ? HoldStep::Follow : HoldStep::StandDown;
    }
} // namespace cardian::engage
