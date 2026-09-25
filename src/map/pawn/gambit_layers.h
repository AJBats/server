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

#include <array>
#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <string_view>

// A character's rows come in two layers (ROADMAP K5, RESEARCH §14.12
// decision 8). Her own rows -- the set she has, else her job's defaults --
// are the same list wherever she is, and the only one the editor shows,
// edits or saves. A world body out in the wild also runs the world's rows
// (modules/cardian/world/brains.yaml): never saved, shown or sent, and run
// AHEAD of hers, so the world's competence goes first. Joining a player's
// party, or leaving it, changes whether the world's layer runs and rewrites
// neither layer, with one exception: rows the player edited on a guest are
// forgotten as she leaves, and her job's defaults seeded again (pawn.cpp,
// forgetGuestGambits). Every reader of her rows -- the think, the behaviours,
// the conveyor's requests, the engage door -- walks the same running order,
// so they all agree on which row comes first. Pure, so xi_test pins it
// (cardian_gambit_layers_tests.cpp).
namespace cardian::layers
{
    // The world layer's row ids carry this prefix; her own rows' ids are
    // plain numbers. The conveyor keeps a row's id across ticks and a
    // request names its row by it, so a world row's id can never be taken
    // for one of hers, and a rebuilt world layer numbers on from the last
    // id it gave, never reusing one.
    constexpr std::string_view kWorldPrefix = "w";

    inline auto worldRowId(const uint32 n) -> std::string
    {
        return std::string(kWorldPrefix) + std::to_string(n);
    }

    constexpr auto isWorldRowId(const std::string_view id) -> bool
    {
        return id.starts_with(kWorldPrefix);
    }

    // The layers that run for her, in order: the world's first while she is
    // in the wild, then her own; her own alone anywhere else
    template <typename Row>
    struct Layers
    {
        std::span<Row> world;
        std::span<Row> own;
    };

    template <typename Row>
    constexpr auto layersFor(const bool inTheWild, const std::span<Row> world, const std::span<Row> own) -> Layers<Row>
    {
        return { inTheWild ? world : std::span<Row>{}, own };
    }

    // Every row in the running order: fn(row, place), place 1-based across
    // both layers (the conveyor's order among her rows). fn returns true to
    // stop there; whether it stopped
    template <typename Row, typename Fn>
    constexpr auto forEachRow(const Layers<Row>& layers, Fn&& fn) -> bool
    {
        std::size_t place = 0;
        for (auto& row : layers.world)
        {
            if (fn(row, ++place))
            {
                return true;
            }
        }
        for (auto& row : layers.own)
        {
            if (fn(row, ++place))
            {
                return true;
            }
        }
        return false;
    }

    // A row by its id (idOf(row)): a world id in the world's layer, any other
    // in her own. Nothing when the row is gone, or its layer does not run
    // now -- a request a world row made in the wild is no longer hers to
    // keep once she is with a player
    template <typename Row, typename IdOf>
    constexpr auto findRow(const Layers<Row>& layers, const std::string_view id, IdOf&& idOf) -> Row*
    {
        for (auto& row : isWorldRowId(id) ? layers.world : layers.own)
        {
            if (idOf(row) == id)
            {
                return &row;
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
