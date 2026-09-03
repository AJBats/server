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

#include "pawn_items.h"
#include "pawn.h"

#include "common/logging.h"

#include "ai/ai_container.h"
#include "entities/char_entity.h"
#include "entities/entity_id.h"
#include "enums/item_state.h"
#include "item_container.h"
#include "items/item.h"
#include "items/item_equipment.h"
#include "items/transaction.h"
#include "items/transactions/item_claim.h"
#include "lua/luautils.h"
#include "utils/charutils.h"
#include "utils/itemutils.h"

#include <fmt/format.h>

namespace
{
    // A one-stack move between two live characters: claim the source stack,
    // give a clone to the receiver, take from the sender, commit. Any
    // refusal rolls the whole move back through the layer's undo log.
    class CardianTransfer final : public Transaction
    {
    public:
        ~CardianTransfer() override
        {
            this->rollbackIfOpen();
        }

        uint8  landedSlot   = 0;
        uint16 landedItemId = 0;

        auto move(CCharEntity* PSender, CCharEntity* PReceiver, const uint8 slot, const uint32 qty) -> std::string
        {
            auto* storage = PSender->getStorage(LOC_INVENTORY);
            CItem* PItem  = storage != nullptr ? storage->GetItem(slot) : nullptr;

            if (PItem == nullptr || PItem->getQuantity() == 0)
            {
                return "no item in that slot";
            }
            if (PItem->isType(ITEM_CURRENCY))
            {
                return "gil cannot be transferred";
            }
            this->landedItemId = PItem->getID();
            if (PItem->state() == ItemState::Equipped)
            {
                return "item is equipped";
            }
            if (qty == 0 || qty > PItem->getQuantity())
            {
                return "bad quantity";
            }
            if (!this->claim(PSender, PItem).isSet())
            {
                return "item is busy";
            }

            auto stack = xi::items::clone(*PItem);
            if (!stack)
            {
                return "item cannot move";
            }
            stack->setQuantity(qty);

            // Receiver first: an out-of-space refusal is the common failure,
            // and this order leaves nothing to undo when it happens
            const auto landed = this->give(PReceiver, LOC_INVENTORY, std::move(stack));
            if (!landed.has_value())
            {
                this->rollback();
                return "no space";
            }
            this->landedSlot = *landed;
            if (!this->take(PSender, LOC_INVENTORY, slot, qty))
            {
                this->rollback();
                return "item slipped away";
            }
            if (!this->commit())
            {
                this->rollback();
                return "transfer refused";
            }
            return {};
        }

    protected:
        // give/take above already applied and recorded the work
        auto doCommit() -> bool override
        {
            return true;
        }

        void doRollback() override
        {
        }
    };

    // Payload fragments sized for one GP_SERV_COMMAND_CHAT_STD each (Mes is
    // 150 bytes and the command layer prepends "#cd xx.y <name> ")
    constexpr size_t kChunkLimit = 110;

    void packEntry(std::vector<std::string>& chunks, const std::string& entry)
    {
        if (chunks.empty() || chunks.back().size() + entry.size() + 1 > kChunkLimit)
        {
            chunks.emplace_back(entry);
            return;
        }
        chunks.back() += "," + entry;
    }
} // namespace

namespace pawn::items
{
    auto giveToPawn(CCharEntity* PPlayer, CCharEntity* PPawn, const uint8 slot, const uint32 qty, uint8* landedSlot) -> std::string
    {
        CardianTransfer transfer;

        auto result = transfer.move(PPlayer, PPawn, slot, qty);
        if (!result.empty())
        {
            return result;
        }

        // Her bag is kept stacked: the given stack may have merged into an
        // earlier one, so the landed slot is wherever that item is now
        tidyStacks(PPawn);
        if (landedSlot != nullptr)
        {
            *landedSlot = transfer.landedSlot;
            if (const auto* storage = PPawn->getStorage(LOC_INVENTORY); storage != nullptr)
            {
                const CItem* PLanded = storage->GetItem(transfer.landedSlot);
                if (PLanded == nullptr || PLanded->getID() != transfer.landedItemId)
                {
                    for (uint8 s = 1; s <= storage->GetSize(); ++s)
                    {
                        if (const CItem* PItem = storage->GetItem(s); PItem != nullptr && PItem->getID() == transfer.landedItemId)
                        {
                            *landedSlot = s;
                            break;
                        }
                    }
                }
            }
        }
        return result;
    }

    auto tidyStacks(CCharEntity* PPawn) -> uint8
    {
        CItemContainer* PContainer = PPawn->getStorage(LOC_INVENTORY);
        if (PContainer == nullptr)
        {
            return 0;
        }

        uint8       merges = 0;
        const uint8 size   = PContainer->GetSize();
        for (uint8 slotId = 1; slotId <= size; ++slotId)
        {
            const CItem* PItem = PContainer->GetItem(slotId);
            if (PItem == nullptr || PItem->isBusy() || PItem->getQuantity() >= PItem->getStackSize())
            {
                continue;
            }
            for (uint8 slotId2 = slotId + 1; slotId2 <= size; ++slotId2)
            {
                const CItem* PItem2 = PContainer->GetItem(slotId2);
                if (PItem2 == nullptr || PItem2->getID() != PItem->getID() || PItem2->isBusy() || PItem2->getQuantity() >= PItem2->getStackSize())
                {
                    continue;
                }

                const uint32 totalQty = PItem->getQuantity() + PItem2->getQuantity();
                const uint32 moveQty  = totalQty >= PItem->getStackSize() ? PItem->getStackSize() - PItem->getQuantity() : PItem2->getQuantity();
                if (moveQty == 0)
                {
                    continue;
                }

                // One transaction per pair, so a stack merged away is released
                // before the next pass
                const auto containerId = static_cast<uint8>(PContainer->GetID());
                auto       transaction = ItemClaimTransaction::start(PPawn);
                if (!transaction || !transaction->claimSlot(containerId, slotId) || !transaction->claimSlot(containerId, slotId2))
                {
                    continue;
                }
                if (!transaction->moveBetween(containerId, slotId2, containerId, slotId, moveQty) || !transaction->commit())
                {
                    ShowErrorFmt("pawn: {} could not merge stacks in slots {} and {}", PPawn->getName(), slotId, slotId2);
                    continue;
                }
                ++merges;

                // The destination as it stands after the commit, not the
                // pointer from before it
                PItem = PContainer->GetItem(slotId);
                if (PItem == nullptr || PItem->getQuantity() >= PItem->getStackSize())
                {
                    break;
                }
            }
        }
        return merges;
    }

    auto takeFromPawn(CCharEntity* PPlayer, CCharEntity* PPawn, const uint8 slot, const uint32 qty) -> std::string
    {
        return CardianTransfer().move(PPawn, PPlayer, slot, qty);
    }

    auto equip(CCharEntity* PPawn, const uint8 invSlot, const uint8 equipSlot) -> std::string
    {
        // Inventory slot 0 is the gil slot; to EquipItem it means "unequip"
        if (equipSlot >= SLOT_LINK1 || invSlot == 0)
        {
            return "bad slot";
        }

        const auto* storage = PPawn->getStorage(LOC_INVENTORY);
        const auto* PItem   = storage != nullptr ? dynamic_cast<CItemEquipment*>(storage->GetItem(invSlot)) : nullptr;
        if (PItem == nullptr)
        {
            return "not equipment";
        }

        charutils::EquipItem(PPawn, invSlot, equipSlot, LOC_INVENTORY);
        if (PPawn->getEquip(static_cast<SLOTTYPE>(equipSlot)) != PItem)
        {
            return "cannot equip";
        }

        luautils::CheckForGearSet(PPawn);
        PPawn->UpdateHealth();
        PPawn->retriggerLatents = true;
        return {};
    }

    auto unequip(CCharEntity* PPawn, const uint8 equipSlot) -> std::string
    {
        if (equipSlot >= SLOT_LINK1)
        {
            return "bad slot";
        }
        if (PPawn->getEquip(static_cast<SLOTTYPE>(equipSlot)) == nullptr)
        {
            return "nothing equipped";
        }

        charutils::EquipItem(PPawn, 0, equipSlot, LOC_INVENTORY);
        if (PPawn->getEquip(static_cast<SLOTTYPE>(equipSlot)) != nullptr)
        {
            return "cannot remove";
        }

        luautils::CheckForGearSet(PPawn);
        PPawn->UpdateHealth();
        PPawn->retriggerLatents = true;
        return {};
    }

    auto useItem(CCharEntity* PPawn, const uint8 slot) -> std::string
    {
        const auto* storage = PPawn->getStorage(LOC_INVENTORY);
        const CItem* PItem  = storage != nullptr ? storage->GetItem(slot) : nullptr;

        if (PItem == nullptr || PItem->getQuantity() == 0)
        {
            return "no item in that slot";
        }
        if (!PItem->isType(ITEM_USABLE))
        {
            return "item cannot be used";
        }
        if (PItem->isBusy())
        {
            return "item is busy";
        }

        if (!PPawn->PAI->UseItem(EntityId(PPawn), LOC_INVENTORY, slot))
        {
            return "cannot use right now";
        }
        return {};
    }

    auto dropItem(CCharEntity* PPawn, const uint8 slot, const uint32 qty) -> std::string
    {
        const auto* storage = PPawn->getStorage(LOC_INVENTORY);
        const CItem* PItem  = storage != nullptr ? storage->GetItem(slot) : nullptr;

        if (PItem == nullptr || PItem->getQuantity() == 0)
        {
            return "no item in that slot";
        }
        if (PItem->isType(ITEM_CURRENCY))
        {
            return "gil cannot be dropped";
        }
        if (PItem->isBusy())
        {
            return "item is busy";
        }
        if (qty == 0 || qty > PItem->getQuantity())
        {
            return "bad quantity";
        }

        const uint32 before = PItem->getQuantity();
        charutils::DropItem(PPawn, LOC_INVENTORY, slot, static_cast<int32>(qty), PItem->getID());

        const CItem* PAfter = storage->GetItem(slot);
        if (PAfter != nullptr && PAfter->getQuantity() == before)
        {
            return "cannot drop";
        }
        return {};
    }

    auto inventoryChunks(CCharEntity* PPawn) -> std::vector<std::string>
    {
        std::vector<std::string> chunks;

        const auto* storage = PPawn->getStorage(LOC_INVENTORY);
        if (storage == nullptr)
        {
            return chunks;
        }

        for (uint8 slot = 1; slot <= storage->GetSize(); ++slot)
        {
            const CItem* PItem = storage->GetItem(slot);
            if (PItem == nullptr || PItem->getQuantity() == 0)
            {
                continue;
            }

            auto entry = fmt::format("{}:{}:{}", slot, PItem->getID(), PItem->getQuantity());
            if (PItem->state() == ItemState::Equipped)
            {
                entry += ":E";
            }
            packEntry(chunks, entry);
        }
        return chunks;
    }

    auto equipChunks(CCharEntity* PPawn) -> std::vector<std::string>
    {
        std::vector<std::string> chunks;

        for (uint8 equipSlot = SLOT_MAIN; equipSlot < SLOT_LINK1; ++equipSlot)
        {
            const auto* PItem = PPawn->getEquip(static_cast<SLOTTYPE>(equipSlot));
            if (PItem == nullptr)
            {
                continue;
            }
            packEntry(chunks, fmt::format("{}:{}:{}", equipSlot, PItem->getID(), PItem->getSlotID()));
        }
        return chunks;
    }
} // namespace pawn::items
