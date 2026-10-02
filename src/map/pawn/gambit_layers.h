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

#include "tactician_line.h"

#include "common/cbasetypes.h"

#include <array>
#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// A character's rows come in layers (ROADMAP K5, RESEARCH §14.12 decision 8,
// RESEARCH §17). Her own rows -- the set she has, else her job's defaults --
// are the same list wherever she is, and the only one the editor edits or
// saves. A world body out in the wild also runs the world's rows
// (modules/cardian/world/brains.yaml): never saved, shown or sent, and run
// AHEAD of hers, so the world's competence goes first. A member of a
// player's party also runs the rows her party role lends her
// (role_bundles.h): never saved, shown in her editor read-only, and fitted
// onto her own list (the fit, below): the role's rows first, then hers,
// a lent row that means the same as one of hers standing in its place --
// and staying on that row of hers, wherever she moves it, while she holds
// the role (bindLent).
// Joining a player's party, or leaving it, changes which layers run and
// rewrites none of them, with one exception: rows the player edited on a
// guest are forgotten as she leaves, and her job's defaults seeded again
// (pawn.cpp, forgetGuestGambits). Every reader of her rows -- the think,
// the behaviours, the conveyor's requests, the engage door, the allow-list
// -- walks the same running order, so they all agree on which row comes
// first. Pure, so xi_test pins it (cardian_gambit_layers_tests.cpp).
namespace cardian::layers
{
    // The world layer's row ids carry this prefix, a role's rows this one;
    // her own rows' ids are plain numbers. The conveyor keeps a row's id
    // across ticks and a request names its row by it, so a lent row's id can
    // never be taken for one of hers, and a rebuilt layer numbers on from
    // the last id it gave, never reusing one.
    constexpr std::string_view kWorldPrefix = "w";
    constexpr std::string_view kRolePrefix  = "r";

    inline auto worldRowId(const uint32 n) -> std::string
    {
        return std::string(kWorldPrefix) + std::to_string(n);
    }

    inline auto roleRowId(const uint32 n) -> std::string
    {
        return std::string(kRolePrefix) + std::to_string(n);
    }

    constexpr auto isWorldRowId(const std::string_view id) -> bool
    {
        return id.starts_with(kWorldPrefix);
    }

    constexpr auto isRoleRowId(const std::string_view id) -> bool
    {
        return id.starts_with(kRolePrefix);
    }

    // Whose a row in the running order is: her own, lent by her party role,
    // or the role's standing in the place of a row of hers that means the
    // same (the role wins while she holds it; hers is kept underneath and
    // comes back when the role goes)
    enum class Origin : uint8
    {
        Own,
        Lent,
        Both,
    };

    // A row in the running order: the row itself, whose it is, its 1-based
    // place within its own layer (hers: the number the editor shows it
    // under; a lent row: its place in the bundle; the role's in her place:
    // her number, so an edit of its content that names it is refused, and
    // a move of it moves her row, the role's going with it), and whether it
    // runs: her own row's checkbox, and always for the role's
    template <typename Row>
    struct Placed
    {
        Row*        row    = nullptr;
        Origin      origin = Origin::Own;
        std::size_t index  = 0;
        bool        on     = true;
    };

    // The layers that run for her, in order: the world's first while she is
    // in the wild, then her own and the rows her role lends, fitted
    template <typename Row>
    struct Layers
    {
        std::span<Row>           world;
        std::vector<Placed<Row>> rows;
    };

    // --- the fit (RESEARCH §17.3) ---------------------------------------

    // The side a row's condition names, for the overlap rule
    enum class Side : uint8
    {
        Self,
        Ally,
        Foe,
    };

    inline auto sideOf(const gambits::Gambit_t& g) -> Side
    {
        if (g.target_selector == gambits::G_TARGET::SELF)
        {
            return Side::Self;
        }
        return engage::isFoeTarget(g.target_selector) ? Side::Foe : Side::Ally;
    }

    inline auto sameActions(const gambits::Gambit_t& a, const gambits::Gambit_t& b) -> bool
    {
        if (a.actions.size() != b.actions.size())
        {
            return false;
        }
        for (std::size_t i = 0; i < a.actions.size(); ++i)
        {
            const auto& x = a.actions[i];
            const auto& y = b.actions[i];
            if (x.reaction != y.reaction || x.select != y.select || x.select_arg != y.select_arg)
            {
                return false;
            }
        }
        return true;
    }

    // Two rows mean the same thing when they have the same action on the
    // same side, whatever their conditions or the mark (RESEARCH §17.2
    // decision 6): a lent row like that stands in hers. An Attack row is
    // the exception: its finder is its meaning (which fight she takes), so
    // only the same finder is the same row -- the Tank's "targeted by ally"
    // pull takes her "targeted by ally" row and leaves her "targeting
    // ally" order standing. Among several of hers that would do, one
    // naming the same target is the one
    inline auto sameRow(const gambits::Gambit_t& a, const gambits::Gambit_t& b) -> bool
    {
        return a.target_selector == b.target_selector && sameActions(a, b);
    }

    inline auto overlaps(const gambits::Gambit_t& a, const gambits::Gambit_t& b) -> bool
    {
        if (engage::isEngageRow(a) || engage::isEngageRow(b))
        {
            return sameRow(a, b);
        }
        return sideOf(a) == sideOf(b) && sameActions(a, b);
    }

    // Which row of hers each lent row stands in: its index in `own`, or
    // none. A lent row keeps the row of hers it already stands in --
    // kept[li], that row's index now, wherever she has carried it -- while
    // that row still means the same: a duplicate of it she adds, or carries
    // past it, never takes its place, so the role's row never jumps from
    // one of her rows to another under her edits (the user, 2026-10-01).
    // The rest take a row afresh: one naming the same target first, else
    // the first on the same side; never one another lent row holds. `kept`
    // may be shorter than `lent`, or empty: nothing kept, a fresh fit
    template <typename Row, typename GambitOf>
    auto bindLent(const std::span<Row> own, const std::span<Row> lent, GambitOf&& gambitOf, const std::span<const std::optional<std::size_t>> kept = {})
        -> std::vector<std::optional<std::size_t>>
    {
        std::vector<std::optional<std::size_t>> standsIn(lent.size());
        std::vector<bool>                       taken(own.size(), false);
        for (std::size_t li = 0; li < lent.size() && li < kept.size(); ++li)
        {
            const auto oi = kept[li];
            if (oi.has_value() && *oi < own.size() && !taken[*oi] && overlaps(gambitOf(own[*oi]), gambitOf(lent[li])))
            {
                standsIn[li] = *oi;
                taken[*oi]   = true;
            }
        }
        const auto takeOver = [&](const std::size_t li, auto&& same)
        {
            if (standsIn[li].has_value())
            {
                return;
            }
            for (std::size_t oi = 0; oi < own.size(); ++oi)
            {
                if (!taken[oi] && same(gambitOf(own[oi]), gambitOf(lent[li])))
                {
                    standsIn[li] = oi;
                    taken[oi]    = true;
                    return;
                }
            }
        };
        for (std::size_t li = 0; li < lent.size(); ++li)
        {
            takeOver(li, [](const gambits::Gambit_t& a, const gambits::Gambit_t& b) { return sameRow(a, b); });
        }
        for (std::size_t li = 0; li < lent.size(); ++li)
        {
            takeOver(li, [](const gambits::Gambit_t& a, const gambits::Gambit_t& b) { return overlaps(a, b); });
        }
        return standsIn;
    }

    // Her own rows and the rows her role lends, in the running order: the
    // role's rows first, in the bundle's order, then her own in hers -- a
    // role is the quick override (the user, 2026-09-30; RESEARCH §17.13),
    // so what it lends outranks what she has. A lent row bound to a row of
    // hers (`binds`, from bindLent above), whatever her checkbox or condition,
    // takes her row's place instead (Both): the role's row runs there, on,
    // pinned, while she holds the role, and hers is kept underneath and
    // comes back when the role goes. A bind out of her rows' range, or onto
    // a row another lent row holds, counts as none. enabledOf(row) reads a
    // row's checkbox
    template <typename Row, typename EnabledOf>
    auto place(const std::span<Row> own, const std::span<Row> lent, const std::span<const std::optional<std::size_t>> binds, EnabledOf&& enabledOf)
        -> std::vector<Placed<Row>>
    {
        std::vector<std::optional<std::size_t>> standsIn(own.size());
        std::vector<bool>                       lentOut(lent.size(), false);
        for (std::size_t li = 0; li < lent.size() && li < binds.size(); ++li)
        {
            if (binds[li].has_value() && *binds[li] < own.size() && !standsIn[*binds[li]].has_value())
            {
                standsIn[*binds[li]] = li;
                lentOut[li]          = true;
            }
        }

        std::vector<Placed<Row>> out;
        out.reserve(own.size() + lent.size());
        for (std::size_t li = 0; li < lent.size(); ++li)
        {
            if (!lentOut[li])
            {
                out.push_back({ &lent[li], Origin::Lent, li + 1, true });
            }
        }
        // Her row at oi, or the role's standing in its place under her number
        for (std::size_t oi = 0; oi < own.size(); ++oi)
        {
            if (standsIn[oi].has_value())
            {
                out.push_back({ &lent[*standsIn[oi]], Origin::Both, oi + 1, true });
            }
            else
            {
                out.push_back({ &own[oi], Origin::Own, oi + 1, static_cast<bool>(enabledOf(own[oi])) });
            }
        }
        return out;
    }

    // The fit with nothing kept: every lent row binds afresh (bindLent), then
    // takes its place. gambitOf(row) reads a row's gambit
    template <typename Row, typename GambitOf, typename EnabledOf>
    auto fit(const std::span<Row> own, const std::span<Row> lent, GambitOf&& gambitOf, EnabledOf&& enabledOf) -> std::vector<Placed<Row>>
    {
        const auto binds = bindLent<Row>(own, lent, gambitOf);
        return place<Row>(own, lent, binds, enabledOf);
    }

    // Her layers with no role lending her anything: her own rows alone,
    // behind the world's in the wild
    template <typename Row>
    auto layersFor(const bool inTheWild, const std::span<Row> world, const std::span<Row> own) -> Layers<Row>
    {
        Layers<Row> out{ inTheWild ? world : std::span<Row>{}, {} };
        out.rows.reserve(own.size());
        for (std::size_t i = 0; i < own.size(); ++i)
        {
            out.rows.push_back({ &own[i], Origin::Own, i + 1, own[i].enabled });
        }
        return out;
    }

    // Her layers with her role's rows fitted onto her own afresh, nothing
    // kept: for the tests. A live character's rows keep their bindings
    // across her edits (CGambits::RunningLayers lays them out with place)
    template <typename Row, typename GambitOf, typename EnabledOf>
    auto layersFor(const bool inTheWild, const std::span<Row> world, const std::span<Row> own, const std::span<Row> lent, GambitOf&& gambitOf, EnabledOf&& enabledOf) -> Layers<Row>
    {
        return { inTheWild ? world : std::span<Row>{}, fit(own, lent, gambitOf, enabledOf) };
    }

    // Every row in the running order: fn(row, place, on), place 1-based
    // across the layers (the conveyor's order among her rows), on whether
    // the row runs (Placed::on; a world row's checkbox). fn returns true to
    // stop there; whether it stopped
    template <typename Row, typename Fn>
    auto forEachRow(const Layers<Row>& layers, Fn&& fn) -> bool
    {
        std::size_t place = 0;
        for (auto& row : layers.world)
        {
            if (fn(row, ++place, static_cast<bool>(row.enabled)))
            {
                return true;
            }
        }
        for (const auto& p : layers.rows)
        {
            if (fn(*p.row, ++place, p.on))
            {
                return true;
            }
        }
        return false;
    }

    // A row by its id (idOf(row)): a world id in the world's layer, any
    // other among the fitted rows, hers and the role's alike (their ids
    // never collide: a prefix each). Nothing when the row is gone, or its
    // layer does not run now -- a request a world row made in the wild is
    // no longer hers to keep once she is with a player, nor one a lent row
    // made once the role is taken from her
    template <typename Row, typename IdOf>
    auto findRow(const Layers<Row>& layers, const std::string_view id, IdOf&& idOf) -> Row*
    {
        if (isWorldRowId(id))
        {
            for (auto& row : layers.world)
            {
                if (idOf(row) == id)
                {
                    return &row;
                }
            }
            return nullptr;
        }
        for (const auto& p : layers.rows)
        {
            if (idOf(*p.row) == id)
            {
                return p.row;
            }
        }
        return nullptr;
    }

    // A behaviour a row speaks for this tick. The rows speak in the running
    // order, and the first to speak for a behaviour wins, whichever layer it
    // is in: a later row leaves it be. False when the behaviour was already
    // spoken for, or is not one of the N
    template <std::size_t N>
    constexpr auto speak(std::array<std::optional<uint16>, N>& behaviors, const uint16 behavior, const uint16 arg) -> bool
    {
        if (behavior >= N || behaviors[behavior].has_value())
        {
            return false;
        }
        behaviors[behavior] = arg;
        return true;
    }

} // namespace cardian::layers
