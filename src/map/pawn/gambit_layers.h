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
// onto her own list by part (the fit, below), so a role's orders join her
// orders and a role's allow-list rows join hers below her tactician line.
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
    // her number, so the edits that name it are refused), and whether it
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

    // Two rows mean the same thing where they sit when they have the same
    // action on the same side, whatever their conditions (RESEARCH §17.2
    // decision 6): a lent row like that is left out, and hers stands
    inline auto overlaps(const gambits::Gambit_t& a, const gambits::Gambit_t& b) -> bool
    {
        if (sideOf(a) != sideOf(b) || a.actions.size() != b.actions.size())
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

    // Her own rows and the rows her role lends, in the running order:
    //  1. her own orders (her rows above her line, or all of them when she
    //     has none);
    //  2. the role's orders (the bundle's rows ahead of its line row, or
    //     all of them when it has none);
    //  3. the line: her own line row when she has one, checked or not,
    //     else the role's when the bundle brings one;
    //  4. her own rows below her line;
    //  5. the role's rows below its line.
    // A lent row that overlaps a row of hers in the same part, whatever her
    // checkbox or condition, takes her row's place (Both): the role's row
    // runs there, on, pinned, while she holds the role, and hers is kept
    // underneath and comes back when the role goes (the user, 2026-09-30:
    // a role is a quick override; a player who wants his own tuning takes
    // the role off). Her own line row's place is her line whenever she has
    // one, checked or not -- and a lent line of another role stands in it
    // too (a Tank line lent to a Support Mage: her tactician is the tank's
    // while she holds the role, and her rows its tactician does not read
    // are struck out there; RESEARCH §17.10). gambitOf(row) reads a row's
    // gambit, enabledOf(row) its checkbox
    template <typename Row, typename GambitOf, typename EnabledOf>
    auto fit(const std::span<Row> own, const std::span<Row> lent, GambitOf&& gambitOf, EnabledOf&& enabledOf) -> std::vector<Placed<Row>>
    {
        const auto lineIn = [&](const std::span<Row> rows) -> std::optional<std::size_t>
        {
            for (std::size_t i = 0; i < rows.size(); ++i)
            {
                if (tactician::isLineRow(gambitOf(rows[i])))
                {
                    return i;
                }
            }
            return std::nullopt;
        };
        const auto ownLine   = lineIn(own);
        const auto lentLine  = lineIn(lent);
        const bool lentLeads = lentLine.has_value() && !ownLine.has_value();

        // Each list's parts: [0, above) its orders; the line; then its rows
        // below
        const std::size_t ownAbove     = ownLine.has_value() ? *ownLine : own.size();
        const std::size_t ownBelowFrom = ownLine.has_value() ? *ownLine + 1 : own.size();
        const std::size_t lentAbove    = lentLine.has_value() ? *lentLine : lent.size();
        const std::size_t lentBelowFrom = lentLine.has_value() ? *lentLine + 1 : lent.size();

        // standsIn[oi]: the lent row that takes her row oi's place, if any
        std::vector<std::optional<std::size_t>> standsIn(own.size());
        std::vector<bool>                       lentOut(lent.size(), false);
        const auto                              overlapped = [&](const std::size_t li, const std::size_t from, const std::size_t to)
        {
            for (std::size_t oi = from; oi < to; ++oi)
            {
                if (!standsIn[oi].has_value() && overlaps(gambitOf(own[oi]), gambitOf(lent[li])))
                {
                    standsIn[oi] = li;
                    lentOut[li]  = true;
                    return;
                }
            }
        };
        for (std::size_t li = 0; li < lentAbove; ++li)
        {
            overlapped(li, 0, ownAbove);
        }
        for (std::size_t li = lentBelowFrom; li < lent.size(); ++li)
        {
            overlapped(li, ownBelowFrom, own.size());
        }
        if (lentLine.has_value() && !lentLeads)
        {
            standsIn[*ownLine] = *lentLine;
            lentOut[*lentLine] = true;
        }

        std::vector<Placed<Row>> out;
        out.reserve(own.size() + lent.size());
        // Her row at oi, or the role's standing in its place under her number
        const auto hers = [&](const std::size_t oi)
        {
            if (standsIn[oi].has_value())
            {
                out.push_back({ &lent[*standsIn[oi]], Origin::Both, oi + 1, true });
            }
            else
            {
                out.push_back({ &own[oi], Origin::Own, oi + 1, static_cast<bool>(enabledOf(own[oi])) });
            }
        };
        const auto theirs = [&](const std::size_t li)
        {
            out.push_back({ &lent[li], Origin::Lent, li + 1, true });
        };
        for (std::size_t oi = 0; oi < ownAbove; ++oi)
        {
            hers(oi);
        }
        for (std::size_t li = 0; li < lentAbove; ++li)
        {
            if (!lentOut[li])
            {
                theirs(li);
            }
        }
        if (lentLeads)
        {
            theirs(*lentLine);
        }
        else if (ownLine.has_value())
        {
            hers(*ownLine);
        }
        for (std::size_t oi = ownBelowFrom; oi < own.size(); ++oi)
        {
            hers(oi);
        }
        for (std::size_t li = lentBelowFrom; li < lent.size(); ++li)
        {
            if (!lentOut[li])
            {
                theirs(li);
            }
        }
        return out;
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

    // Her layers with her role's rows fitted onto her own
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

    // The roles a character holds this tick, as a set: every Role row that
    // speaks adds hers, where `speak` keeps only the first (RESEARCH
    // §17.4). With the Damage value retired every role is a line, and one
    // line a list, so the set holds one role today; it stays a set for
    // the day two speak. Bit r for pawn::Role r
    constexpr auto holdRole(uint32& held, const uint16 role) -> void
    {
        if (role < 32)
        {
            held |= (1u << role);
        }
    }

    constexpr auto holdsRole(const uint32 held, const uint16 role) -> bool
    {
        return role < 32 && (held & (1u << role)) != 0;
    }
} // namespace cardian::layers
