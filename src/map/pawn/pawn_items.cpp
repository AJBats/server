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
#include "cardian_link_messages.h"
#include "pawn.h"
#include "pawn_controller.h"

#include "common/database.h"
#include "common/logging.h"
#include "common/settings.h"
#include "common/utils.h"

#include "ai/ai_container.h"
#include "ai/states/item_state.h"
#include "entities/char_entity.h"
#include "entities/entity_id.h"
#include "enums/item_state.h"
#include "item_container.h"
#include "items/item.h"
#include "items/item_equipment.h"
#include "items/transaction.h"
#include "items/transactions/item_claim.h"
#include "lua/luautils.h"
#include "pause/pause.h"
#include "recast_container.h"
#include "utils/charutils.h"
#include "utils/itemutils.h"

#include <algorithm>
#include <array>
#include <fmt/format.h>
#include <stdexcept>
#include <tuple>

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

        auto move(CCharEntity* PSender, CCharEntity* PReceiver, const uint8 slot, const uint32 qty) -> uint16
        {
            auto* storage = PSender->getStorage(LOC_INVENTORY);
            CItem* PItem  = storage != nullptr ? storage->GetItem(slot) : nullptr;

            if (PItem == nullptr || PItem->getQuantity() == 0)
            {
                return CL_S_NO_ITEM;
            }
            if (PItem->isType(ITEM_CURRENCY))
            {
                return CL_S_GIL_NOT_AN_ITEM;
            }
            this->landedItemId = PItem->getID();
            if (PItem->state() == ItemState::Equipped)
            {
                return CL_S_ITEM_EQUIPPED;
            }
            if (qty == 0 || qty > PItem->getQuantity())
            {
                return CL_S_BAD_QUANTITY;
            }
            if (!this->claim(PSender, PItem).isSet())
            {
                return CL_S_ITEM_BUSY;
            }

            auto stack = xi::items::clone(*PItem);
            if (!stack)
            {
                return CL_S_ITEM_CANNOT_MOVE;
            }
            stack->setQuantity(qty);

            // Receiver first: an out-of-space refusal is the common failure,
            // and this order leaves nothing to undo when it happens
            const auto landed = this->give(PReceiver, LOC_INVENTORY, std::move(stack));
            if (!landed.has_value())
            {
                this->rollback();
                return CL_S_NO_SPACE;
            }
            this->landedSlot = *landed;
            if (!this->take(PSender, LOC_INVENTORY, slot, qty))
            {
                this->rollback();
                return CL_S_ITEM_SLIPPED_AWAY;
            }
            if (!this->commit())
            {
                this->rollback();
                return CL_S_REFUSED;
            }
            return CL_S_OK;
        }

        // Gil between the same two, as the trade window's gil line moves it:
        // the sender pays, the receiver earns, both or neither
        auto moveGil(CCharEntity* PSender, CCharEntity* PReceiver, const uint32 amount) -> uint16
        {
            const CItem* PSent = PSender->getStorage(LOC_INVENTORY)->GetItem(0);
            const CItem* PHeld = PReceiver->getStorage(LOC_INVENTORY)->GetItem(0);
            if (PSent == nullptr || !PSent->isType(ITEM_CURRENCY) || PHeld == nullptr || !PHeld->isType(ITEM_CURRENCY))
            {
                ShowErrorFmt("pawn: no gil slot between {} and {}", PSender->getName(), PReceiver->getName());
                return CL_S_REFUSED;
            }
            if (amount == 0)
            {
                return CL_S_BAD_QUANTITY;
            }
            if (PSent->getQuantity() < amount)
            {
                return CL_S_NOT_ENOUGH_GIL;
            }
            if (static_cast<uint64>(PHeld->getQuantity()) + amount > PHeld->getStackSize())
            {
                return CL_S_GIL_FULL;
            }
            if (!this->pay(PSender, amount) || !this->earn(PReceiver, amount))
            {
                this->rollback();
                return CL_S_ITEM_BUSY;
            }
            if (!this->commit())
            {
                this->rollback();
                return CL_S_REFUSED;
            }
            return CL_S_OK;
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
} // namespace

namespace pawn::items
{
    namespace
    {
        // No item teleportation: a trade reaches pawn.TRADE_RANGE yalms, in
        // the same zone
        auto reach(const CCharEntity* PPlayer, const CCharEntity* PPawn) -> uint16
        {
            if (PPlayer->loc.zone != PPawn->loc.zone)
            {
                return CL_S_OTHER_ZONE;
            }
            if (distance(PPlayer->loc.p, PPawn->loc.p) > settings::get<float>("pawn.TRADE_RANGE"))
            {
                return CL_S_OUT_OF_REACH;
            }
            return CL_S_OK;
        }
    } // namespace

    auto legacyReason(const uint16 status) -> std::string
    {
        switch (status)
        {
            case CL_S_OK:
                return "";
            case CL_S_NO_ITEM:
                return "no item in that slot";
            case CL_S_ITEM_BUSY:
                return "item is busy";
            case CL_S_NOT_WHILE_PAUSED:
                return "not while paused";
            case CL_S_ITEM_UNUSABLE:
                return "item cannot be used";
            case CL_S_STANDING_UP:
                return "standing up";
            case CL_S_CANNOT_NOW:
                return "cannot do that now";
            case CL_S_NOT_CARRIED:
                return "she has none";
            case CL_S_ON_RECAST:
                return "recast";
            case CL_S_MALFORMED:
                return "bad action";
            default:
                return "refused";
        }
    }

    namespace
    {
        // The bags in the order the addon cycles them
        constexpr std::array<CONTAINER_ID, 11> kBagOrder = {
            LOC_MOGCASE,
            LOC_WARDROBE,
            LOC_WARDROBE2,
            LOC_WARDROBE3,
            LOC_WARDROBE4,
            LOC_WARDROBE5,
            LOC_WARDROBE6,
            LOC_WARDROBE7,
            LOC_WARDROBE8,
            LOC_MOGSATCHEL,
            LOC_MOGSACK,
        };
    } // namespace

    auto isWardrobe(const uint8 location) -> bool
    {
        return location == LOC_WARDROBE || (location >= LOC_WARDROBE2 && location <= LOC_WARDROBE8);
    }

    auto usableContainer(CCharEntity* PPawn, const uint8 location) -> bool
    {
        if (location == LOC_INVENTORY)
        {
            return true;
        }
        for (const auto& bag : bags(PPawn))
        {
            if (bag.location == location)
            {
                return true;
            }
        }
        return false;
    }

    namespace
    {

        // An enchanted item's recast is keyed by the slot and container it
        // was equipped from; a worn piece that changes slots takes the entry
        // along, so unequip still finds it and the old key answers for nothing
        void rekeyItemRecast(CCharEntity* PPawn, const uint8 fromLoc, const uint8 fromSlot, const uint8 toLoc, const uint8 toSlot)
        {
            const auto oldId = static_cast<Recast>(fromSlot << 8 | fromLoc);
            const auto newId = static_cast<Recast>(toSlot << 8 | toLoc);
            const auto* entry = PPawn->PRecastContainer->GetRecast(RECAST_ITEM, oldId);
            if (entry == nullptr || oldId == newId)
            {
                return;
            }
            const auto remaining  = entry->TimeStamp + entry->RecastTime - timer::now();
            const auto chargeTime = entry->chargeTime;
            const auto maxCharges = entry->maxCharges;
            PPawn->PRecastContainer->Del(RECAST_ITEM, oldId);
            if (remaining > timer::duration::zero())
            {
                PPawn->PRecastContainer->Add(RECAST_ITEM, newId, remaining, chargeTime, maxCharges);
            }
        }

        // No room where a stack was bound: her inventory, or one of her bags
        auto noRoom(const uint8 toLoc) -> uint16
        {
            return toLoc == LOC_INVENTORY ? CL_S_NO_SPACE : CL_S_BAG_FULL;
        }

        // The whole stack changes container the item-move handler's way, the
        // object itself carried across so augments, signature and extra data
        // ride along; a database row that does not follow puts the stack
        // back. Worn gear stays worn: the equip slot points at the object,
        // which now reports its new container and slot, so the saved equip
        // rows are written again and the recast entry follows.
        auto carryStack(CCharEntity* PPawn, CItemContainer* PSrc, CItemContainer* PDst, const uint8 fromLoc, const uint8 slot, const uint8 toLoc, const bool worn) -> uint16
        {
            const uint16 itemId = PSrc->GetItem(slot)->getID();
            const uint8  landed = PSrc->MoveItemTo(slot, *PDst);
            if (landed == ERROR_SLOTID)
            {
                return noRoom(toLoc);
            }
            const auto rset = db::preparedStmt("UPDATE char_inventory SET location = ?, slot = ? WHERE charid = ? AND location = ? AND slot = ?",
                                               toLoc,
                                               landed,
                                               PPawn->id,
                                               fromLoc,
                                               slot);
            if (!rset || !rset->rowsAffected())
            {
                ShowErrorFmt("pawn: {} could not move item {} from {}/{} to {}/{} in the database; the move is undone", PPawn->getName(), itemId, fromLoc, slot, toLoc, landed);
                if (PDst->MoveItemTo(landed, *PSrc, slot) == ERROR_SLOTID)
                {
                    ShowErrorFmt("pawn: {} could not put item {} back into {}/{}", PPawn->getName(), itemId, fromLoc, slot);
                }
                return CL_S_REFUSED;
            }
            if (worn)
            {
                rekeyItemRecast(PPawn, fromLoc, slot, toLoc, landed);
                charutils::SaveCharEquip(PPawn);
            }
            return CL_S_OK;
        }
    } // namespace

    auto giveToPawn(CCharEntity* PPlayer, CCharEntity* PPawn, const uint8 slot, const uint32 qty, uint8* landedSlot) -> uint16
    {
        if (const auto status = reach(PPlayer, PPawn); status != CL_S_OK)
        {
            return status;
        }

        CardianTransfer transfer;

        const auto result = transfer.move(PPlayer, PPawn, slot, qty);
        if (result != CL_S_OK)
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
        return tidyContainer(PPawn, LOC_INVENTORY);
    }

    auto tidyContainer(CCharEntity* PPawn, const uint8 location) -> uint8
    {
        CItemContainer* PContainer = PPawn->getStorage(location);
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

    auto sortBag(CCharEntity* PPawn, const uint8 location) -> uint16
    {
        if (!usableContainer(PPawn, location))
        {
            return CL_S_NO_SUCH_BAG;
        }
        auto* PContainer = PPawn->getStorage(location);
        if (PContainer == nullptr)
        {
            return CL_S_NO_SUCH_BAG;
        }

        // A worn charged item mid-use still points its cast at a slot
        if (PPawn->PAI->IsCurrentState<CItemState>())
        {
            return CL_S_USING_ITEM;
        }

        tidyContainer(PPawn, location);

        // Slot 0 is the inventory's gil and never moves. Nothing may be
        // mid-transaction or on a bazaar; worn gear is fine, its equip slot
        // holds the object, not the slot number.
        for (uint8 slot = 1; slot <= PContainer->GetSize(); ++slot)
        {
            const CItem* PItem = PContainer->GetItem(slot);
            if (PItem != nullptr && PItem->isBusy() && PItem->state() != ItemState::Equipped)
            {
                return CL_S_ITEM_BUSY;
            }
        }

        struct Placed
        {
            std::unique_ptr<CItem> item;
            uint8                  was  = 0;
            uint8                  now  = 0;
            bool                   worn = false;
        };
        std::vector<Placed> placed;
        for (uint8 slot = 1; slot <= PContainer->GetSize(); ++slot)
        {
            if (const CItem* PItem = PContainer->GetItem(slot); PItem != nullptr)
            {
                const bool worn = PItem->state() == ItemState::Equipped;
                placed.push_back({ PContainer->RemoveItem(slot), slot, 0, worn });
            }
        }
        std::stable_sort(placed.begin(), placed.end(), [](const Placed& a, const Placed& b)
                         {
                             if (a.item->getID() != b.item->getID())
                             {
                                 return a.item->getID() < b.item->getID();
                             }
                             return a.item->getQuantity() > b.item->getQuantity();
                         });

        bool moved     = false;
        bool wornMoved = false;
        for (std::size_t i = 0; i < placed.size(); ++i)
        {
            auto& p = placed[i];
            p.now   = static_cast<uint8>(i + 1);
            if (PContainer->InsertItem(std::move(p.item), p.now) == ERROR_SLOTID)
            {
                ShowErrorFmt("pawn: {} lost a stack sorting container {} (slot {} -> {})", PPawn->getName(), location, p.was, p.now);
            }
            if (p.was != p.now)
            {
                moved     = true;
                wornMoved = wornMoved || p.worn;
            }
        }
        if (!moved)
        {
            return CL_S_OK;
        }

        // The rows follow in two steps inside one transaction: every row
        // parked above the container's range, then each brought to its new
        // slot, so the primary key (charid, location, slot) never collides.
        // A park that does not touch one row per stack means the database
        // and the container disagree: nothing is written and the order goes
        // back.
        const uint32 charid = PPawn->id;
        const uint8  size   = PContainer->GetSize();
        const bool   saved  = db::transaction([&]()
                                              {
                                                  const auto parked = db::preparedStmt("UPDATE char_inventory SET slot = slot + 100 WHERE charid = ? AND location = ? AND slot > 0 AND slot <= ?", charid, location, size);
                                                  if (!parked || parked->rowsAffected() != placed.size())
                                                  {
                                                      throw std::runtime_error("the container's rows do not match its stacks");
                                                  }
                                                  db::executeBulk("UPDATE char_inventory SET slot = ? WHERE charid = ? AND location = ? AND slot = ?", placed, [&](const Placed& p)
                                                                  {
                                                                      return std::make_tuple(p.now, charid, location, static_cast<uint8>(p.was + 100));
                                                                  });
                                              });
        if (!saved)
        {
            ShowErrorFmt("pawn: {} could not sort container {} in the database; the order is put back", PPawn->getName(), location);
            for (auto& p : placed)
            {
                p.item = PContainer->RemoveItem(p.now);
            }
            for (auto& p : placed)
            {
                if (p.item != nullptr && PContainer->InsertItem(std::move(p.item), p.was) == ERROR_SLOTID)
                {
                    ShowErrorFmt("pawn: {} lost a stack putting container {} back (slot {})", PPawn->getName(), location, p.was);
                }
            }
            return CL_S_REFUSED;
        }
        if (wornMoved)
        {
            for (const auto& p : placed)
            {
                if (p.worn && p.was != p.now)
                {
                    rekeyItemRecast(PPawn, location, p.was, location, p.now);
                }
            }
            charutils::SaveCharEquip(PPawn);
        }
        return CL_S_OK;
    }

    auto takeFromPawn(CCharEntity* PPlayer, CCharEntity* PPawn, const uint8 slot, const uint32 qty) -> uint16
    {
        if (const auto status = reach(PPlayer, PPawn); status != CL_S_OK)
        {
            return status;
        }
        return CardianTransfer().move(PPawn, PPlayer, slot, qty);
    }

    auto moveGil(CCharEntity* PPlayer, CCharEntity* PPawn, const uint32 amount, const bool toPawn) -> uint16
    {
        if (const auto status = reach(PPlayer, PPawn); status != CL_S_OK)
        {
            return status;
        }
        return toPawn ? CardianTransfer().moveGil(PPlayer, PPawn, amount) : CardianTransfer().moveGil(PPawn, PPlayer, amount);
    }

    auto equip(CCharEntity* PPawn, const uint8 invSlot, const uint8 equipSlot, const uint8 location) -> uint16
    {
        // Inventory slot 0 is the gil slot; to EquipItem it means "unequip"
        if (equipSlot >= SLOT_LINK1 || invSlot == 0)
        {
            return CL_S_MALFORMED;
        }
        // Gear is worn from the inventory and the wardrobes only; the
        // storage-only bags are dead storage for it, as on retail
        if (location != LOC_INVENTORY && !isWardrobe(location))
        {
            return CL_S_WORN_FROM_BAG;
        }
        if (!usableContainer(PPawn, location))
        {
            return CL_S_NO_SUCH_BAG;
        }

        const auto* storage = PPawn->getStorage(location);
        const auto* PItem   = storage != nullptr ? dynamic_cast<CItemEquipment*>(storage->GetItem(invSlot)) : nullptr;
        if (PItem == nullptr)
        {
            return CL_S_NOT_EQUIPMENT;
        }

        charutils::EquipItem(PPawn, invSlot, equipSlot, location);
        if (PPawn->getEquip(static_cast<SLOTTYPE>(equipSlot)) != PItem)
        {
            return CL_S_CANNOT_WEAR;
        }

        luautils::CheckForGearSet(PPawn);
        PPawn->UpdateHealth();
        PPawn->retriggerLatents = true;
        return CL_S_OK;
    }

    auto unequip(CCharEntity* PPawn, const uint8 equipSlot) -> uint16
    {
        if (equipSlot >= SLOT_LINK1)
        {
            return CL_S_MALFORMED;
        }
        if (PPawn->getEquip(static_cast<SLOTTYPE>(equipSlot)) == nullptr)
        {
            return CL_S_OK;
        }

        charutils::EquipItem(PPawn, 0, equipSlot, LOC_INVENTORY);
        if (PPawn->getEquip(static_cast<SLOTTYPE>(equipSlot)) != nullptr)
        {
            return CL_S_CANNOT_REMOVE;
        }

        luautils::CheckForGearSet(PPawn);
        PPawn->UpdateHealth();
        PPawn->retriggerLatents = true;
        return CL_S_OK;
    }

    void equipSet(CCharEntity* PPawn, std::vector<EquipChange>& changes)
    {
        std::vector<EquipChange*> wears;
        for (auto& change : changes)
        {
            if (change.invSlot == 0)
            {
                change.result = unequip(PPawn, change.equipSlot);
            }
            else
            {
                wears.push_back(&change);
            }
        }
        std::ranges::stable_sort(wears, {}, &EquipChange::equipSlot);
        for (auto* change : wears)
        {
            change->result = equip(PPawn, change->invSlot, change->equipSlot, change->location);
        }
    }

    auto useItem(CCharEntity* PPawn, const uint8 slot, const uint8 location) -> uint16
    {
        // A held simulation (pause/pause.h) starts nothing, and a slot is no order to
        // keep for the release: her bag can be sorted meanwhile.
        if (cardian::pause::isHeld())
        {
            return CL_S_NOT_WHILE_PAUSED;
        }

        // Items are used from the inventory only; a bag's contents are worn
        // or fetched first
        if (location != LOC_INVENTORY)
        {
            return CL_S_INVENTORY_ONLY;
        }
        if (!usableContainer(PPawn, location))
        {
            return CL_S_NO_SUCH_BAG;
        }
        const auto* storage = PPawn->getStorage(location);
        const CItem* PItem  = storage != nullptr ? storage->GetItem(slot) : nullptr;

        if (PItem == nullptr || PItem->getQuantity() == 0)
        {
            return CL_S_NO_ITEM;
        }
        if (!PItem->isType(ITEM_USABLE))
        {
            return CL_S_ITEM_UNUSABLE;
        }
        if (PItem->isBusy())
        {
            return CL_S_ITEM_BUSY;
        }

        // Finish the rest transitions before the item's engine wind-up.
        if (auto* controller = dynamic_cast<CPawnController*>(PPawn->PAI->GetController()); controller != nullptr)
        {
            if (!controller->PrepareRestAction(true))
            {
                return CL_S_STANDING_UP;
            }
        }
        if (!PPawn->PAI->UseItem(EntityId(PPawn), location, slot))
        {
            return CL_S_CANNOT_NOW;
        }
        return CL_S_OK;
    }

    auto dropItem(CCharEntity* PPawn, const uint8 slot, const uint32 qty, const uint8 location) -> uint16
    {
        // Stacks are dropped from the inventory only; a bag's contents are
        // fetched first
        if (location != LOC_INVENTORY)
        {
            return CL_S_INVENTORY_ONLY;
        }
        const auto* storage = PPawn->getStorage(location);
        const CItem* PItem  = storage != nullptr ? storage->GetItem(slot) : nullptr;

        if (PItem == nullptr || PItem->getQuantity() == 0)
        {
            return CL_S_NO_ITEM;
        }
        if (PItem->isType(ITEM_CURRENCY))
        {
            return CL_S_GIL_NOT_AN_ITEM;
        }
        if (PItem->isBusy())
        {
            return CL_S_ITEM_BUSY;
        }
        if (qty == 0 || qty > PItem->getQuantity())
        {
            return CL_S_BAD_QUANTITY;
        }

        const uint32 before = PItem->getQuantity();
        charutils::DropItem(PPawn, location, slot, static_cast<int32>(qty), PItem->getID());

        const CItem* PAfter = storage->GetItem(slot);
        if (PAfter != nullptr && PAfter->getQuantity() == before)
        {
            return CL_S_REFUSED;
        }
        return CL_S_OK;
    }

    auto bags(CCharEntity* PPawn) -> std::vector<Bag>
    {
        std::vector<Bag> out;
        for (const auto location : kBagOrder)
        {
            const auto* storage = PPawn->getStorage(location);
            if (storage == nullptr || storage->GetSize() == 0)
            {
                continue;
            }
            out.push_back({ static_cast<uint8>(location), storage->GetSize(), static_cast<uint8>(storage->GetSize() - storage->GetFreeSlotsCount()) });
        }
        return out;
    }

    auto moveItem(CCharEntity* PPawn, const uint8 fromLoc, const uint8 slot, const uint8 toLoc, const uint32 qty, bool* partly) -> uint16
    {
        if (fromLoc == toLoc || !usableContainer(PPawn, fromLoc) || !usableContainer(PPawn, toLoc))
        {
            return CL_S_NO_SUCH_BAG;
        }
        if (fromLoc != LOC_INVENTORY && toLoc != LOC_INVENTORY)
        {
            return CL_S_VIA_INVENTORY;
        }

        auto* PSrc = PPawn->getStorage(fromLoc);
        auto* PDst = PPawn->getStorage(toLoc);
        if (PSrc == nullptr || PDst == nullptr)
        {
            return CL_S_NO_SUCH_BAG;
        }

        CItem* PItem = slot != 0 ? PSrc->GetItem(slot) : nullptr;
        if (PItem == nullptr || PItem->getQuantity() == 0)
        {
            return CL_S_NO_ITEM;
        }
        if (PItem->isType(ITEM_CURRENCY))
        {
            return CL_S_GIL_NOT_AN_ITEM;
        }
        if (isWardrobe(toLoc) && !PItem->isType(ITEM_EQUIPMENT) && !PItem->isType(ITEM_WEAPON))
        {
            return CL_S_WARDROBE_GEAR;
        }
        if (PItem->state() == ItemState::Equipped)
        {
            // Worn gear moves whole and stays worn, between the inventory
            // and a wardrobe only; the storage-only bags take nothing worn
            if (toLoc != LOC_INVENTORY && !isWardrobe(toLoc))
            {
                return CL_S_ITEM_EQUIPPED;
            }
            if (PPawn->PAI->IsCurrentState<CItemState>())
            {
                return CL_S_USING_ITEM;
            }
            return carryStack(PPawn, PSrc, PDst, fromLoc, slot, toLoc, true);
        }
        if (PItem->isBusy())
        {
            return CL_S_ITEM_BUSY;
        }
        if (qty == 0 || qty > PItem->getQuantity())
        {
            return CL_S_BAD_QUANTITY;
        }

        // Room first, so no merge commits ahead of a refusal: with no free
        // slot, the destination's same-item partial stacks must take it all
        if (PDst->GetFreeSlotsCount() == 0)
        {
            uint32 room = 0;
            for (uint8 into = 1; into <= PDst->GetSize(); ++into)
            {
                const CItem* PInto = PDst->GetItem(into);
                if (PInto != nullptr && PInto->getID() == PItem->getID() && !PInto->isBusy() && PInto->getQuantity() < PInto->getStackSize())
                {
                    room += PInto->getStackSize() - PInto->getQuantity();
                }
            }
            if (room < qty)
            {
                return noRoom(toLoc);
            }
        }

        const uint16 itemId = PItem->getID();
        uint32       left   = qty;

        // A refusal once the top-ups have begun: whatever they moved stays moved
        const auto refused = [&](const uint16 status) -> uint16
        {
            if (partly != nullptr)
            {
                *partly = left < qty;
            }
            return status;
        };

        // A same-item partial stack in the destination is topped up first,
        // one transaction per stack so a merged-away claim is released
        // before the next
        if (PItem->getStackSize() > 1)
        {
            for (uint8 into = 1; into <= PDst->GetSize() && left > 0; ++into)
            {
                const CItem* PInto = PDst->GetItem(into);
                if (PInto == nullptr || PInto->getID() != itemId || PInto->isBusy() || PInto->getQuantity() >= PInto->getStackSize())
                {
                    continue;
                }
                const uint32 part = std::min(left, PInto->getStackSize() - PInto->getQuantity());

                auto transaction = ItemClaimTransaction::start(PPawn);
                if (!transaction || !transaction->claimSlot(fromLoc, slot) || !transaction->claimSlot(toLoc, into))
                {
                    return refused(CL_S_ITEM_BUSY);
                }
                if (!transaction->moveBetween(fromLoc, slot, toLoc, into, part) || !transaction->commit())
                {
                    ShowErrorFmt("pawn: {} could not merge {} of item {} into {}/{}", PPawn->getName(), part, itemId, toLoc, into);
                    return refused(CL_S_REFUSED);
                }
                left -= part;
            }
        }
        if (left == 0)
        {
            return CL_S_OK;
        }

        // The stack as it stands after the merges
        PItem = PSrc->GetItem(slot);
        if (PItem == nullptr || PItem->getQuantity() < left)
        {
            return refused(CL_S_ITEM_SLIPPED_AWAY);
        }

        if (left < PItem->getQuantity())
        {
            // part of a stack: a new stack in a free slot of the destination
            auto transaction = ItemClaimTransaction::start(PPawn);
            if (!transaction || !transaction->claimSlot(fromLoc, slot))
            {
                return refused(CL_S_ITEM_BUSY);
            }
            if (!transaction->split(fromLoc, slot, toLoc, left) || !transaction->commit())
            {
                return refused(noRoom(toLoc));
            }
            return CL_S_OK;
        }

        const auto status = carryStack(PPawn, PSrc, PDst, fromLoc, slot, toLoc, false);
        return status == CL_S_OK ? status : refused(status);
    }

    auto gilOf(CCharEntity* PChar) -> uint32
    {
        const auto* PGil = PChar->getStorage(LOC_INVENTORY)->GetItem(0);
        return PGil != nullptr && PGil->isType(ITEM_CURRENCY) ? PGil->getQuantity() : 0;
    }
} // namespace pawn::items
