// Cardian: the spell a scroll teaches, read by the auction house's learnable
// mark (auction.cpp) and by a cardian of the player's own learning the
// scrolls in her bag (pawn_scrolls.cpp).
#pragma once

#include "enums/item_flag.h"
#include "items/item.h"
#include "spell.h"

#include <optional>

namespace pawn::scrolls
{
    // A scroll's item flag says it is one, and its subid is the spell it
    // teaches -- but for Haste II's, which names Atomos (its script teaches
    // Haste II); three scrolls have no script to use them by: Comet, Meteor,
    // Breakga. None for anything else
    inline auto spellOf(const CItem* PItem) -> std::optional<SpellID>
    {
        constexpr uint16 kHasteII = 4692;
        const uint16     itemId   = PItem->getID();
        if (itemId == 4827 || itemId == 4851 || itemId == 4889 || !PItem->hasFlag(ItemFlag::Scroll) || PItem->getSubID() == 0)
        {
            return std::nullopt;
        }
        return itemId == kHasteII ? SpellID::Haste_II : static_cast<SpellID>(PItem->getSubID());
    }
} // namespace pawn::scrolls
