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

#include "dbox.h"

#include "cardian_link_protocol.h"

#include "common/database.h"
#include "common/settings.h"

#include "entities/char_entity.h"
#include "enums/item_flag.h"
#include "enums/item_state.h"
#include "item_container.h"
#include "items/item.h"
#include "packets/c2s/0x04d_pbx.h"
#include "universal_container.h"
#include "utils/charutils.h"
#include "utils/dboxutils.h"

#include <algorithm>
#include <optional>

namespace pawn::dbox
{
    namespace
    {
        constexpr auto kIn  = GP_CLI_COMMAND_PBX_BOXNO::Incoming;
        constexpr auto kOut = GP_CLI_COMMAND_PBX_BOXNO::Outgoing;

        // What waits for a character past her eight slots has reached the
        // game's limit (dboxutils' own count): her box takes no more
        auto backlogged(const uint32 charid) -> bool
        {
            return waitingFor(charid) >= settings::get<uint32>("map.DELIVERY_BOX_MAX_INFLIGHT");
        }

        // The item in a slot of her open box; nullptr when it is empty
        auto cell(CCharEntity* PChar, const uint8 slot) -> CItem*
        {
            return PChar->UContainer->IsSlotEmpty(slot) ? nullptr : PChar->UContainer->GetItem(slot);
        }

        // Her inventory has room for it: gil needs none
        auto hasRoom(CCharEntity* PChar, const CItem* PItem) -> bool
        {
            return PItem->isType(ITEM_CURRENCY) || PChar->getStorage(LOC_INVENTORY)->GetFreeSlotsCount() > 0;
        }

        // A slot of her outgoing box the other end has received
        auto delivered(const uint32 charid, const uint8 slot) -> bool
        {
            const auto rset = db::preparedStmt("SELECT received FROM delivery_box WHERE charid = ? AND box = 2 AND slot = ?", charid, slot);
            return rset && rset->next() && rset->get<uint8>("received") == 1;
        }

        // Each empty slot of her open incoming box given what waits next, as
        // her client asks the game for it
        void pullWaiting(CCharEntity* PChar, Scheduler& scheduler)
        {
            for (uint8 slot = 0; slot < 8; ++slot)
            {
                if (cell(PChar, slot) == nullptr && waitingFor(PChar->id) > 0)
                {
                    dboxutils::SendNewItems(scheduler, PChar, kIn, slot);
                }
            }
        }

        // Box 1, what was sent to her: taken into her inventory, thrown
        // away, or sent back
        auto onReceive(CCharEntity* PChar, const uint8 action, const uint8 slot) -> uint16
        {
            if (action == CL_DBOX_LOOK)
            {
                return CL_S_OK;
            }
            CItem* PItem = cell(PChar, slot);
            if (PItem == nullptr)
            {
                return CL_S_NO_ITEM;
            }
            switch (action)
            {
                case CL_DBOX_TAKE:
                    if (!hasRoom(PChar, PItem))
                    {
                        return CL_S_NO_SPACE;
                    }
                    if (PItem->hasFlag(ItemFlag::Rare) && charutils::HasItem(PChar, PItem->getID()))
                    {
                        return CL_S_RARE_OWNED;
                    }
                    dboxutils::TakeItemFromCell(PChar, kIn, slot);
                    break;
                case CL_DBOX_DROP:
                    dboxutils::RemoveItemFromCell(PChar, kIn, slot);
                    break;
                case CL_DBOX_RETURN:
                {
                    const auto rset   = db::preparedStmt("SELECT senderid FROM delivery_box WHERE charid = ? AND box = 1 AND slot = ?", PChar->id, slot);
                    const auto sender = rset && rset->next() ? rset->get<uint32>("senderid") : 0;
                    if (sender == 0)
                    {
                        return CL_S_NO_SENDER;
                    }
                    if (backlogged(sender))
                    {
                        return CL_S_BOX_BACKLOGGED;
                    }
                    dboxutils::ReturnToSender(PChar, kIn, slot);
                    break;
                }
                default:
                    return CL_S_MALFORMED;
            }
            return cell(PChar, slot) == nullptr ? static_cast<uint16>(CL_S_OK) : static_cast<uint16>(CL_S_REFUSED);
        }

        // Whom a set is for, by charid: the name the game sends it by and
        // the account its delivery rule judges; nothing when there is nobody
        struct Recipient
        {
            std::string name;
            uint32      accid = 0;
        };

        auto recipientOf(const uint32 to) -> std::optional<Recipient>
        {
            const auto rset = db::preparedStmt("SELECT charname, accid FROM chars WHERE charid = ?", to);
            if (!rset || !rset->next())
            {
                return std::nullopt;
            }
            return Recipient{ rset->get<std::string>("charname"), rset->get<uint32>("accid") };
        }

        // The stack's own checks for a set of qty of it for `to`: CL_S_OK or
        // the refusal; the recipient, when there is one, comes back in r
        auto stackRefusal(const CCharEntity* PChar, const CItem* PItem, const uint32 qty, const uint32 to, std::optional<Recipient>& r) -> uint16
        {
            if (PItem == nullptr || PItem->getQuantity() == 0)
            {
                return CL_S_NO_ITEM;
            }
            if (PItem->state() == ItemState::Equipped)
            {
                return CL_S_ITEM_EQUIPPED;
            }
            if (qty == 0 || qty > PItem->getQuantity())
            {
                return CL_S_BAD_QUANTITY;
            }
            r = recipientOf(to);
            if (!r.has_value())
            {
                return CL_S_NO_RECIPIENT;
            }
            // the game's own rule (dboxutils::AddItemsToBeSent), told rather than left unsaid
            if (PItem->hasFlag(ItemFlag::NoDelivery) && (!PItem->hasFlag(ItemFlag::CanSendAccount) || PChar->accid != r->accid))
            {
                return CL_S_NOT_DELIVERABLE;
            }
            return CL_S_OK;
        }

        // A stack of her inventory set in an empty slot for `to`, by name
        // as the game sends it
        auto setForSending(CCharEntity* PChar, const uint8 slot, const uint8 invSlot, const uint32 qty, const uint32 to) -> uint16
        {
            if (cell(PChar, slot) != nullptr)
            {
                return CL_S_REFUSED;
            }
            std::optional<Recipient> r;
            if (const auto why = stackRefusal(PChar, PChar->getStorage(LOC_INVENTORY)->GetItem(invSlot), qty, to, r); why != CL_S_OK)
            {
                return why;
            }
            dboxutils::AddItemsToBeSent(PChar, kOut, slot, invSlot, qty, r->name);
            return cell(PChar, slot) != nullptr ? static_cast<uint16>(CL_S_OK) : static_cast<uint16>(CL_S_REFUSED);
        }

        // Box 2, what she sends: set, sent, called back, taken back unsent,
        // or cleared once delivered
        auto onSend(CCharEntity* PChar, const uint8 action, const uint8 slot, const uint8 invSlot, const uint32 qty, const uint32 to) -> uint16
        {
            if (action == CL_DBOX_LOOK)
            {
                return CL_S_OK;
            }
            if (action == CL_DBOX_SET)
            {
                return setForSending(PChar, slot, invSlot, qty, to);
            }
            CItem* PItem = cell(PChar, slot);
            if (PItem == nullptr)
            {
                return CL_S_NO_ITEM;
            }
            switch (action)
            {
                case CL_DBOX_SEND:
                {
                    if (PItem->isSent())
                    {
                        return CL_S_ALREADY_SENT;
                    }
                    const auto charid = charutils::getCharIdFromName(PItem->getReceiver());
                    if (charid == 0)
                    {
                        return CL_S_NO_RECIPIENT;
                    }
                    if (backlogged(charid))
                    {
                        return CL_S_BOX_BACKLOGGED;
                    }
                    dboxutils::SendConfirmation(PChar, kOut, slot);
                    const CItem* PAfter = cell(PChar, slot);
                    return PAfter != nullptr && PAfter->isSent() ? static_cast<uint16>(CL_S_OK) : static_cast<uint16>(CL_S_REFUSED);
                }
                case CL_DBOX_RECALL:
                {
                    if (!PItem->isSent() || delivered(PChar->id, slot))
                    {
                        return CL_S_REFUSED;
                    }
                    dboxutils::CancelSendingItem(PChar, kOut, slot);
                    const CItem* PAfter = cell(PChar, slot);
                    return PAfter != nullptr && !PAfter->isSent() ? static_cast<uint16>(CL_S_OK) : static_cast<uint16>(CL_S_REFUSED);
                }
                case CL_DBOX_TAKE:
                    if (PItem->isSent())
                    {
                        return CL_S_ALREADY_SENT;
                    }
                    if (!hasRoom(PChar, PItem))
                    {
                        return CL_S_NO_SPACE;
                    }
                    dboxutils::TakeItemFromCell(PChar, kOut, slot);
                    return cell(PChar, slot) == nullptr ? static_cast<uint16>(CL_S_OK) : static_cast<uint16>(CL_S_REFUSED);
                case CL_DBOX_CLEAR:
                {
                    if (!delivered(PChar->id, slot))
                    {
                        return CL_S_REFUSED;
                    }
                    // the game clears what was delivered in its own order, one
                    // at a time: every slot delivered goes
                    for (uint8 guard = 0; guard < 8; ++guard)
                    {
                        const auto rset = db::preparedStmt("SELECT COUNT(*) AS cnt FROM delivery_box WHERE charid = ? AND box = 2 AND received = 1", PChar->id);
                        if (!rset || !rset->next() || rset->get<uint32>("cnt") == 0)
                        {
                            break;
                        }
                        dboxutils::RemoveDeliveredItemFromSendingBox(PChar, kOut, slot);
                    }
                    return delivered(PChar->id, slot) ? static_cast<uint16>(CL_S_REFUSED) : static_cast<uint16>(CL_S_OK);
                }
                default:
                    return CL_S_MALFORMED;
            }
        }
    } // namespace

    auto act(CCharEntity* PChar, const uint8 box, const uint8 action, const uint8 slot, const uint8 invSlot, const uint32 qty, const uint32 to, Scheduler& scheduler)
        -> uint16
    {
        if ((box != 1 && box != 2) || slot >= 8 || action > CL_DBOX_CLEAR)
        {
            return CL_S_MALFORMED;
        }
        // the box opens in the container an item use or a synthesis of hers
        // holds while it lasts, and would empty it
        if (PChar->UContainer->GetType() != UCONTAINER_EMPTY)
        {
            return CL_S_DBOX_BUSY;
        }
        const auto no = box == 1 ? kIn : kOut;
        if (box == 1)
        {
            dboxutils::OpenRecvBox(PChar);
        }
        else
        {
            dboxutils::OpenSendBox(PChar);
        }
        dboxutils::SendOldItems(PChar, no);
        if (box == 1)
        {
            pullWaiting(PChar, scheduler);
        }
        const auto status = box == 1 ? onReceive(PChar, action, slot) : onSend(PChar, action, slot, invSlot, qty, to);
        if (box == 1 && status == CL_S_OK && action != CL_DBOX_LOOK)
        {
            pullWaiting(PChar, scheduler); // the slot it freed takes what waits next
        }
        dboxutils::CloseMailWindow(PChar, no);
        return status;
    }

    auto setRefusal(CCharEntity* PChar, const uint8 slot, const CItem* PItem, const uint32 qty, const uint32 to) -> uint16
    {
        if (slot >= 8)
        {
            return CL_S_MALFORMED;
        }
        if (PChar->UContainer->GetType() != UCONTAINER_EMPTY)
        {
            return CL_S_DBOX_BUSY;
        }
        if (slotsOf(PChar->id, 2)[slot].state != CL_DBOX_EMPTY)
        {
            return CL_S_REFUSED;
        }
        std::optional<Recipient> r;
        return stackRefusal(PChar, PItem, qty, to, r);
    }

    auto slotsOf(const uint32 charid, const uint8 box) -> std::array<Slot, 8>
    {
        std::array<Slot, 8> out{};
        const auto          rset = db::preparedStmt("SELECT slot, itemid, quantity, sent, received, sender FROM delivery_box WHERE charid = ? AND box = ? AND slot < 8",
                                                    charid, box);
        while (rset && rset->next())
        {
            const auto slot = rset->get<uint8>("slot");
            if (slot >= out.size())
            {
                continue;
            }
            auto& s = out[slot];
            s.item  = rset->get<uint16>("itemid");
            s.qty   = rset->get<uint32>("quantity");
            s.name  = rset->get<std::string>("sender"); // box 2 keeps whom it is for here
            if (box == 1)
            {
                s.state = CL_DBOX_WAITING;
            }
            else
            {
                s.state = rset->get<uint8>("received") == 1 ? CL_DBOX_DELIVERED : rset->get<uint8>("sent") == 1 ? CL_DBOX_SENT : CL_DBOX_WAITING;
            }
        }
        return out;
    }

    auto waitingFor(const uint32 charid) -> uint8
    {
        const auto rset = db::preparedStmt("SELECT COUNT(*) AS cnt FROM delivery_box WHERE charid = ? AND box = 1 AND slot >= 8", charid);
        return rset && rset->next() ? static_cast<uint8>(std::min<uint32>(rset->get<uint32>("cnt"), 255)) : 0;
    }
} // namespace pawn::dbox
