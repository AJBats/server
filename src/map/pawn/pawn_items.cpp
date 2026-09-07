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
    namespace
    {
        // No item teleportation: a trade reaches pawn.TRADE_RANGE yalms, in
        // the same zone
        auto outOfReach(const CCharEntity* PPlayer, const CCharEntity* PPawn) -> std::string
        {
            if (PPlayer->loc.zone != PPawn->loc.zone)
            {
                return fmt::format("{} is in another zone", PPawn->getName());
            }
            const float range = settings::get<float>("pawn.TRADE_RANGE");
            if (const float away = distance(PPlayer->loc.p, PPawn->loc.p); away > range)
            {
                return fmt::format("{} is {:.0f} y away, out of trading reach ({:.0f})", PPawn->getName(), away, range);
            }
            return "";
        }
    } // namespace

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

        // The whole stack changes container the item-move handler's way, the
        // object itself carried across so augments, signature and extra data
        // ride along; a database row that does not follow puts the stack
        // back. Worn gear stays worn: the equip slot points at the object,
        // which now reports its new container and slot, so the saved equip
        // rows are written again and the recast entry follows.
        auto carryStack(CCharEntity* PPawn, CItemContainer* PSrc, CItemContainer* PDst, const uint8 fromLoc, const uint8 slot, const uint8 toLoc, const bool worn) -> std::string
        {
            const uint16 itemId = PSrc->GetItem(slot)->getID();
            const uint8  landed = PSrc->MoveItemTo(slot, *PDst);
            if (landed == ERROR_SLOTID)
            {
                return "no space";
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
                return "move refused";
            }
            if (worn)
            {
                rekeyItemRecast(PPawn, fromLoc, slot, toLoc, landed);
                charutils::SaveCharEquip(PPawn);
            }
            return {};
        }
    } // namespace

    auto giveToPawn(CCharEntity* PPlayer, CCharEntity* PPawn, const uint8 slot, const uint32 qty, uint8* landedSlot) -> std::string
    {
        if (const auto far = outOfReach(PPlayer, PPawn); !far.empty())
        {
            return far;
        }

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

    auto sortBag(CCharEntity* PPawn, const uint8 location) -> std::string
    {
        if (!usableContainer(PPawn, location))
        {
            return "no such bag";
        }
        auto* PContainer = PPawn->getStorage(location);
        if (PContainer == nullptr)
        {
            return "no such bag";
        }

        // A worn charged item mid-use still points its cast at a slot
        if (PPawn->PAI->IsCurrentState<CItemState>())
        {
            return "busy using an item";
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
                return "an item is busy";
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
            return {};
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
            return "sort refused";
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
        return {};
    }

    auto takeFromPawn(CCharEntity* PPlayer, CCharEntity* PPawn, const uint8 slot, const uint32 qty) -> std::string
    {
        if (const auto far = outOfReach(PPlayer, PPawn); !far.empty())
        {
            return far;
        }
        return CardianTransfer().move(PPawn, PPlayer, slot, qty);
    }

    auto equip(CCharEntity* PPawn, const uint8 invSlot, const uint8 equipSlot, const uint8 location) -> std::string
    {
        // Inventory slot 0 is the gil slot; to EquipItem it means "unequip"
        if (equipSlot >= SLOT_LINK1 || invSlot == 0)
        {
            return "bad slot";
        }
        // Gear is worn from the inventory and the wardrobes only; the
        // storage-only bags are dead storage for it, as on retail
        if (location != LOC_INVENTORY && !isWardrobe(location))
        {
            return "not an equippable bag";
        }
        if (!usableContainer(PPawn, location))
        {
            return "no such bag";
        }

        const auto* storage = PPawn->getStorage(location);
        const auto* PItem   = storage != nullptr ? dynamic_cast<CItemEquipment*>(storage->GetItem(invSlot)) : nullptr;
        if (PItem == nullptr)
        {
            return "not equipment";
        }

        charutils::EquipItem(PPawn, invSlot, equipSlot, location);
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

    auto dressFromBag(CCharEntity* PPawn) -> uint32
    {
        const auto* storage = PPawn->getStorage(LOC_INVENTORY);
        if (storage == nullptr)
        {
            return 0;
        }
        const uint8 job   = static_cast<uint8>(PPawn->GetMJob());
        const uint8 level = PPawn->GetMLevel();
        uint32      worn  = 0;
        // A piece already worn in some slot is not free for another
        const auto alreadyWorn = [&](const CItem* PItem) -> bool
        {
            for (uint8 s = 0; s < SLOT_LINK1; ++s)
            {
                if (PPawn->getEquip(static_cast<SLOTTYPE>(s)) == PItem)
                {
                    return true;
                }
            }
            return false;
        };
        for (uint8 equipSlot = 0; equipSlot < SLOT_LINK1; ++equipSlot)
        {
            if (PPawn->getEquip(static_cast<SLOTTYPE>(equipSlot)) != nullptr)
            {
                continue;
            }
            for (uint8 invSlot = 1; invSlot <= storage->GetSize(); ++invSlot)
            {
                const auto* PItem = dynamic_cast<CItemEquipment*>(storage->GetItem(invSlot));
                if (PItem == nullptr || alreadyWorn(PItem) ||
                    !(PItem->getEquipSlotId() & (1 << equipSlot)) ||
                    !(PItem->getJobs() & (1 << (job - 1))) ||
                    PItem->getReqLvl() > level)
                {
                    continue;
                }
                if (equip(PPawn, invSlot, equipSlot, LOC_INVENTORY).empty())
                {
                    ++worn;
                    break;
                }
            }
        }
        if (worn > 0)
        {
            charutils::SaveCharEquip(PPawn);
        }
        return worn;
    }

    auto useItem(CCharEntity* PPawn, const uint8 slot, const uint8 location) -> std::string
    {
        // Items are used from the inventory only; a bag's contents are worn
        // or fetched first
        if (location != LOC_INVENTORY)
        {
            return "used from the inventory only";
        }
        if (!usableContainer(PPawn, location))
        {
            return "no such bag";
        }
        const auto* storage = PPawn->getStorage(location);
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

        if (!PPawn->PAI->UseItem(EntityId(PPawn), location, slot))
        {
            return "cannot use right now";
        }
        return {};
    }

    auto dropItem(CCharEntity* PPawn, const uint8 slot, const uint32 qty, const uint8 location) -> std::string
    {
        // Stacks are dropped from the inventory only; a bag's contents are
        // fetched first
        if (location != LOC_INVENTORY)
        {
            return "dropped from the inventory only";
        }
        const auto* storage = PPawn->getStorage(location);
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
        charutils::DropItem(PPawn, location, slot, static_cast<int32>(qty), PItem->getID());

        const CItem* PAfter = storage->GetItem(slot);
        if (PAfter != nullptr && PAfter->getQuantity() == before)
        {
            return "cannot drop";
        }
        return {};
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

    auto moveItem(CCharEntity* PPawn, const uint8 fromLoc, const uint8 slot, const uint8 toLoc, const uint32 qty) -> std::string
    {
        if (fromLoc == toLoc || !usableContainer(PPawn, fromLoc) || !usableContainer(PPawn, toLoc))
        {
            return "no such bag";
        }
        if (fromLoc != LOC_INVENTORY && toLoc != LOC_INVENTORY)
        {
            return "moves go through the inventory";
        }

        auto* PSrc = PPawn->getStorage(fromLoc);
        auto* PDst = PPawn->getStorage(toLoc);
        if (PSrc == nullptr || PDst == nullptr)
        {
            return "no such bag";
        }

        CItem* PItem = slot != 0 ? PSrc->GetItem(slot) : nullptr;
        if (PItem == nullptr || PItem->getQuantity() == 0)
        {
            return "no item in that slot";
        }
        if (PItem->isType(ITEM_CURRENCY))
        {
            return "gil stays in the inventory";
        }
        if (isWardrobe(toLoc) && !PItem->isType(ITEM_EQUIPMENT) && !PItem->isType(ITEM_WEAPON))
        {
            return "only equipment goes in a wardrobe";
        }
        if (PItem->state() == ItemState::Equipped)
        {
            // Worn gear moves whole and stays worn, between the inventory
            // and a wardrobe only; the storage-only bags take nothing worn
            if (toLoc != LOC_INVENTORY && !isWardrobe(toLoc))
            {
                return "unequip it first";
            }
            if (PPawn->PAI->IsCurrentState<CItemState>())
            {
                return "busy using an item";
            }
            return carryStack(PPawn, PSrc, PDst, fromLoc, slot, toLoc, true);
        }
        if (PItem->isBusy())
        {
            return "item is busy";
        }
        if (qty == 0 || qty > PItem->getQuantity())
        {
            return "bad quantity";
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
                return "no space";
            }
        }

        const uint16 itemId = PItem->getID();
        uint32       left   = qty;

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
                    return "item is busy";
                }
                if (!transaction->moveBetween(fromLoc, slot, toLoc, into, part) || !transaction->commit())
                {
                    ShowErrorFmt("pawn: {} could not merge {} of item {} into {}/{}", PPawn->getName(), part, itemId, toLoc, into);
                    return "move refused";
                }
                left -= part;
            }
        }
        if (left == 0)
        {
            return {};
        }

        // The stack as it stands after the merges
        PItem = PSrc->GetItem(slot);
        if (PItem == nullptr || PItem->getQuantity() < left)
        {
            return "item slipped away";
        }

        if (left < PItem->getQuantity())
        {
            // part of a stack: a new stack in a free slot of the destination
            auto transaction = ItemClaimTransaction::start(PPawn);
            if (!transaction || !transaction->claimSlot(fromLoc, slot))
            {
                return "item is busy";
            }
            if (!transaction->split(fromLoc, slot, toLoc, left) || !transaction->commit())
            {
                return "no space";
            }
            return {};
        }

        return carryStack(PPawn, PSrc, PDst, fromLoc, slot, toLoc, false);
    }

    auto containerChunks(CCharEntity* PPawn, const uint8 location) -> std::vector<std::string>
    {
        std::vector<std::string> chunks;

        const auto* storage = PPawn->getStorage(location);
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
            auto entry = fmt::format("{}:{}:{}", equipSlot, PItem->getID(), PItem->getSlotID());
            if (PItem->getLocationID() != LOC_INVENTORY)
            {
                entry += fmt::format(":{}", PItem->getLocationID());
            }
            packEntry(chunks, entry);
        }
        return chunks;
    }
} // namespace pawn::items
