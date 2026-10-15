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

#include <glaze/yaml.hpp>

#include <optional>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

// The junk rule (OPEN_ISSUES #472), one for every character -- the player's
// word for it is haul: what Deposit Haul banks, and a junk setting of Sell
// or Bank acts on. An item is junk when it is a raw material -- a recipe uses
// it and none makes it -- that cannot be equipped (gear that recipes use is
// never junk), is neither Ex nor Rare, and a vendor buys. A common material
// a quest asks for is junk all the same (the user, 2026-10-15). The hand-kept
// exceptions (junk.yaml) say otherwise either way, but never make gear, an
// Ex or a Rare item junk. Pure: the map's reading of its database and the
// file is junk.cpp
namespace cardian::junk
{
    namespace files
    {
        // modules/cardian/junk.yaml
        struct ExceptionFile
        {
            std::vector<uint16> keep; // never junk, whatever the rule says
            std::vector<uint16> junk; // junk, whatever the rule says
        };
    } // namespace files

    // What the rule asks of one item, from item_basic and item_equipment
    struct Item
    {
        uint16 id         = 0;
        bool   equipment  = false;
        bool   ex         = false;
        bool   rare       = false;
        bool   vendorBuys = false; // not NOSALE, and a sell price above nothing
    };

    // What the rule asks of the recipes and the hand-kept file
    struct Sources
    {
        std::unordered_set<uint16> used; // an ingredient of a synthesis recipe, or its crystal
        std::unordered_set<uint16> made; // what a synthesis recipe makes, normal or high quality
        std::unordered_set<uint16> keep; // the hand list's never
        std::unordered_set<uint16> junk; // the hand list's always
    };

    inline auto isJunk(const Item& item, const Sources& s) -> bool
    {
        if (item.equipment || item.ex || item.rare || s.keep.contains(item.id))
        {
            return false;
        }
        if (s.junk.contains(item.id))
        {
            return true;
        }
        return item.vendorBuys && s.used.contains(item.id) && !s.made.contains(item.id);
    }

    template <typename File>
    auto parse(const std::string_view text, std::string& error) -> std::optional<File>
    {
        constexpr glz::opts kYaml{ .error_on_unknown_keys = true, .error_on_missing_keys = false };
        File                file{};
        if (const auto failed = glz::read_yaml<kYaml>(file, text); failed)
        {
            error = glz::format_error(failed, text);
            return std::nullopt;
        }
        return file;
    }
} // namespace cardian::junk
