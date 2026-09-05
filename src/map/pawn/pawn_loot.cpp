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

#include "pawn_loot.h"
#include "pawn.h"

#include "common/logging.h"
#include "common/xirand.h"

#include "entities/char_entity.h"
#include "enums/item_flag.h"
#include "item_container.h"
#include "items/item.h"
#include "treasure_pool.h"
#include "utils/charutils.h"
#include "utils/itemutils.h"
#include "zone.h"

#include <string>
#include <vector>

namespace pawn::loot
{
    namespace
    {
        // The pool's own tests for a lot, applied first so a lot is never
        // refused
        bool canHold(CCharEntity* PChar, const CItem* PItem)
        {
            if (PChar->getStorage(LOC_INVENTORY)->GetFreeSlotsCount() == 0)
            {
                return false;
            }
            return !(PItem->hasFlag(ItemFlag::Rare) && charutils::HasItem(PChar, PItem->getID()));
        }

        // What she holds of it; the taker is whoever holds more afterwards
        uint32 quantityOf(CCharEntity* PChar, const uint16 itemID)
        {
            uint32      total     = 0;
            const auto* container = PChar->getStorage(LOC_INVENTORY);
            for (uint8 slot = 0; slot <= container->GetSize(); ++slot)
            {
                const CItem* PItem = container->GetItem(slot);
                if (PItem != nullptr && PItem->getID() == itemID)
                {
                    total += PItem->getQuantity();
                }
            }
            return total;
        }
    } // namespace

    void handOff(CCharEntity* PPawn)
    {
        CTreasurePool* PPool = PPawn->PTreasurePool;
        if (PPool == nullptr || PPool->itemCount() == 0 || PPool->getPoolType() == TreasurePoolType::Zone)
        {
            return;
        }

        // A real member keeps the pool as it is
        std::vector<CCharEntity*> staying;
        for (auto* PMember : PPool->getMembers())
        {
            if (!isPawn(PMember))
            {
                return;
            }
            staying.push_back(PMember);
        }
        if (staying.empty())
        {
            return;
        }

        const std::string zone  = PPawn->loc.zone != nullptr ? PPawn->loc.zone->getName() : "?";
        const auto&       items = PPool->getItems(); // a slot resolves inside the pool as its last member acts

        for (uint8 slot = 0; slot < items.size(); ++slot)
        {
            const uint16 itemID = items[slot].ID;
            if (itemID == 0)
            {
                continue;
            }
            const CItem* PItem = xi::items::lookup(itemID);
            if (PItem == nullptr)
            {
                continue;
            }
            const std::string name = PItem->getName();

            std::vector<uint32> before;
            for (auto* PCardian : staying)
            {
                before.push_back(quantityOf(PCardian, itemID));
            }

            // Random lots, so the pool's highest-lot rule picks uniformly
            // among those who can hold it. A lot the pool refuses anyway
            // becomes a pass, so the slot still resolves
            for (auto* PCardian : staying)
            {
                if (PPool->hasLottedItem(PCardian, slot))
                {
                    continue;
                }
                if (!canHold(PCardian, PItem))
                {
                    PPool->passItem(PCardian, slot);
                    continue;
                }
                PPool->lotItem(PCardian, slot, xirand::GetRandomNumber<uint16>(1, 1000));
                if (items[slot].ID != 0 && !PPool->hasLottedItem(PCardian, slot))
                {
                    PPool->passItem(PCardian, slot);
                }
            }

            if (items[slot].ID != 0)
            {
                ShowWarningFmt("pawn: loot left in {}: {} did not resolve, still in the pool", zone, name);
                continue;
            }

            std::string taker = "lost, no one staying can hold it";
            for (size_t i = 0; i < staying.size(); ++i)
            {
                if (quantityOf(staying[i], itemID) > before[i])
                {
                    taker = staying[i]->getName();
                    break;
                }
            }
            ShowInfoFmt("pawn: loot left in {}: {} -> {}", zone, name, taker);
        }
    }
} // namespace pawn::loot
