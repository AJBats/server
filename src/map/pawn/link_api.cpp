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

#include "link_api.h"

#include "cardian_link.h"
#include "pawn.h"
#include "pawn_items.h"

#include "entities/char_entity.h"
#include "enums/item_state.h"
#include "item_container.h"
#include "items/item.h"

namespace pawn::linkapi
{
    namespace
    {
        using namespace cardian::link;

        // One of her containers as it stands now
        auto inventoryOf(CCharEntity* PPawn, const uint8 location) -> cl_inventory
        {
            auto msg     = make<cl_inventory>();
            msg.cardian  = PPawn->id;
            msg.loc      = location;
            const auto* storage = PPawn->getStorage(location);
            if (storage == nullptr)
            {
                return msg;
            }
            msg.size = storage->GetSize();
            msg.free = storage->GetFreeSlotsCount();

            constexpr std::size_t capacity = sizeof(cl_inventory::items) / sizeof(cl_item);
            for (uint8 slot = 1; slot <= storage->GetSize() && msg.count < capacity; ++slot)
            {
                const CItem* PItem = storage->GetItem(slot);
                if (PItem == nullptr || PItem->getQuantity() == 0)
                {
                    continue;
                }
                auto& item = msg.items[msg.count++];
                item.slot  = slot;
                item.id    = PItem->getID();
                item.qty   = PItem->getQuantity();
                item.flags = static_cast<uint8_t>(PItem->state() == ItemState::Equipped ? CL_ITEM_EQUIPPED : 0);
            }
            return msg;
        }

        // A stack from his inventory to hers: her inventory as it now stands,
        // then the outcome
        void give(CCharEntity* PChar, const cl_give& ask, Reply& reply)
        {
            auto* PPawn = pawn::findManagedPawn(PChar, ask.cardian);
            if (PPawn == nullptr)
            {
                reply.finish(ask, CL_S_NO_SUCH_CARDIAN);
                return;
            }

            const auto status = pawn::items::giveToPawn(PChar, PPawn, ask.slot, ask.qty);
            if (status == CL_S_OK)
            {
                reply.more(inventoryOf(PPawn, LOC_INVENTORY));
            }
            reply.finish(ask, status);
        }
    } // namespace

    void registerHandlers()
    {
        handle<cl_give>(give);
    }
} // namespace pawn::linkapi
