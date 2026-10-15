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

#include "junk.h"

#include "junk_math.h"

#include "common/database.h"
#include "common/logging.h"
#include "enums/item_flag.h"

#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <unordered_set>

namespace pawn::junk
{
    namespace
    {
        const std::filesystem::path kExceptionPath = std::filesystem::path("modules/cardian") / "junk.yaml";

        std::unordered_set<uint16> junkItems;

        // A file of the rule's, parsed; nothing, and the map log told why,
        // when it cannot be read or does not parse
        template <typename File>
        auto readFile(const std::filesystem::path& path) -> std::optional<File>
        {
            std::ifstream in(path, std::ios::binary);
            if (!in)
            {
                ShowErrorFmt("junk: {} cannot be read; nothing is junk", path.string());
                return std::nullopt;
            }
            const std::string text{ std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>() };
            std::string       error;
            auto              file = cardian::junk::parse<File>(text, error);
            if (!file.has_value())
            {
                ShowErrorFmt("junk: {}: {}; nothing is junk", path.string(), error);
            }
            return file;
        }

        auto flagged(const uint32 flags, const ItemFlag flag) -> bool
        {
            return (flags & static_cast<uint32>(flag)) != 0;
        }
    } // namespace

    void load()
    {
        junkItems.clear();
        const auto exceptions = readFile<cardian::junk::files::ExceptionFile>(kExceptionPath);
        if (!exceptions.has_value())
        {
            return;
        }

        cardian::junk::Sources sources;
        sources.keep.insert(exceptions->keep.begin(), exceptions->keep.end());
        sources.junk.insert(exceptions->junk.begin(), exceptions->junk.end());

        // the synthesis recipes, desynthesis left out: what they use -- the
        // crystal too, so the crystals are junk -- and what they make
        const auto recipes = db::preparedStmt("SELECT Crystal, HQCrystal, Ingredient1, Ingredient2, Ingredient3, Ingredient4, Ingredient5, Ingredient6, "
                                              "Ingredient7, Ingredient8, Result, ResultHQ1, ResultHQ2, ResultHQ3 FROM synth_recipes WHERE Desynth = 0");
        while (recipes && recipes->next())
        {
            for (const auto* column : { "Crystal", "HQCrystal" })
            {
                if (const auto id = recipes->get<uint16>(column); id != 0)
                {
                    sources.used.insert(id);
                }
            }
            for (int i = 1; i <= 8; ++i)
            {
                if (const auto id = recipes->get<uint16>(fmt::format("Ingredient{}", i)); id != 0)
                {
                    sources.used.insert(id);
                }
            }
            for (const auto* column : { "Result", "ResultHQ1", "ResultHQ2", "ResultHQ3" })
            {
                if (const auto id = recipes->get<uint16>(column); id != 0)
                {
                    sources.made.insert(id);
                }
            }
        }

        const auto items = db::preparedStmt("SELECT b.itemid, b.flags, b.BaseSell, e.itemId IS NOT NULL AS gear FROM item_basic b "
                                            "LEFT JOIN item_equipment e ON e.itemId = b.itemid");
        while (items && items->next())
        {
            const auto flags = items->get<uint32>("flags");
            const cardian::junk::Item item{ .id         = items->get<uint16>("itemid"),
                                            .equipment  = items->get<uint8>("gear") != 0,
                                            .ex         = flagged(flags, ItemFlag::Exclusive),
                                            .rare       = flagged(flags, ItemFlag::Rare),
                                            .vendorBuys = !flagged(flags, ItemFlag::NoSale) && items->get<uint32>("BaseSell") > 0 };
            if (cardian::junk::isJunk(item, sources))
            {
                junkItems.insert(item.id);
            }
        }
        ShowInfoFmt("junk: {} items are junk ({} used by recipes, {} kept and {} named junk by hand)", junkItems.size(), sources.used.size(),
                    sources.keep.size(), sources.junk.size());
    }

    auto isJunk(const uint16 itemId) -> bool
    {
        return junkItems.contains(itemId);
    }

    auto count() -> std::size_t
    {
        return junkItems.size();
    }
} // namespace pawn::junk
