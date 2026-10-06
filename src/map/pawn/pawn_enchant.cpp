// Cardian: the enchanted-item lane (OPEN_ISSUES #297). An enchanted piece of
// gear -- an experience ring -- is used by wearing it, waiting out its delay
// after it is put on, using it, and putting back what it replaced. Used from
// a cardian's inventory or a wardrobe, the lane does all four: the piece goes
// on at once, its delay runs while her gambits and his orders carry on, the
// use goes the moment she is free -- ahead of her line of orders, holding no
// place in it, never into an action under way, never while he steers her --
// and the replaced piece goes back on. One at a time; her queue line shows it
// above the rest (QUEUE's lane).
#include "cardian_link.h"
#include "pawn.h"
#include "pawn_controller.h"
#include "pawn_items.h"

#include "ai/ai_container.h"
#include "common/logging.h"
#include "entities/char_entity.h"
#include "enums/item_state.h"
#include "item_container.h"
#include "items/item_equipment.h"
#include "items/item_usable.h"
#include "lua/luautils.h"
#include "pause/pause.h"
#include "status_effect_container.h"

#include <array>
#include <chrono>
#include <fmt/format.h>

namespace
{
    // A use that does not land -- refused by the game as it begins, or cut
    // short -- is tried this many times in all before the lane gives up and
    // puts the replaced piece back
    constexpr uint8 kEnchantTries = 3;

    // The containers gear is worn from: her inventory and the wardrobes
    constexpr std::array<uint8, 9> kWornFrom{ LOC_INVENTORY, LOC_WARDROBE, LOC_WARDROBE2, LOC_WARDROBE3, LOC_WARDROBE4,
                                              LOC_WARDROBE5, LOC_WARDROBE6, LOC_WARDROBE7, LOC_WARDROBE8 };

    // An enchanted piece of gear: one with charges. Every piece of gear is
    // ITEM_USABLE to the game; ITEM_CHARGED is what marks an enchantment
    auto enchanted(const CItem* PItem) -> bool
    {
        return PItem != nullptr && PItem->isType(ITEM_EQUIPMENT) && PItem->isSubType(ITEM_CHARGED);
    }

    // The piece the lane put on, found where it is worn rather than where it
    // lay: sorting her bags moves it, and worn it stays
    auto wornPiece(CCharEntity* PChar, const uint8 equipSlot, const uint16 itemId) -> CItemEquipment*
    {
        auto* PItem = PChar->getEquip(static_cast<SLOTTYPE>(equipSlot));
        return PItem != nullptr && PItem->getID() == itemId ? PItem : nullptr;
    }

    // A piece of hers by its item, not worn, wherever in her inventory or
    // wardrobes it is now
    auto unwornPiece(CCharEntity* PChar, const uint16 itemId) -> const CItem*
    {
        for (const uint8 location : kWornFrom)
        {
            auto* storage = PChar->getStorage(location);
            if (storage == nullptr)
            {
                continue;
            }
            for (uint8 slot = 1; slot <= storage->GetSize(); ++slot)
            {
                const auto* PItem = storage->GetItem(slot);
                if (PItem != nullptr && PItem->getID() == itemId && PItem->state() != ItemState::Equipped)
                {
                    return PItem;
                }
            }
        }
        return nullptr;
    }

    // Where the piece goes on: a slot it fits that is bare, else the last one
    // it fits -- a ring takes the second ring's place, an earring the second
    // earring's
    auto slotFor(CCharEntity* PChar, const CItemEquipment* PItem) -> std::optional<uint8>
    {
        const uint16         fits = PItem->getEquipSlotId();
        std::optional<uint8> last;
        for (uint8 slot = 0; slot < SLOT_LINK1; ++slot)
        {
            if ((fits & (1 << slot)) == 0)
            {
                continue;
            }
            if (PChar->getEquip(static_cast<SLOTTYPE>(slot)) == nullptr)
            {
                return slot;
            }
            last = slot;
        }
        return last;
    }

    // The slot the piece is worn in now, if it is
    auto wornIn(CCharEntity* PChar, const CItemEquipment* PItem) -> std::optional<uint8>
    {
        for (uint8 slot = 0; slot < SLOT_LINK1; ++slot)
        {
            if (PChar->getEquip(static_cast<SLOTTYPE>(slot)) == PItem)
            {
                return slot;
            }
        }
        return std::nullopt;
    }
} // namespace

void CPawnController::TellQueueLine() const
{
    if (const auto owner = pawn::ordersOwnerOf(static_cast<const CCharEntity*>(POwner)); owner != 0)
    {
        cardian::link::send(owner, QueueLine());
    }
}

auto CPawnController::StartEnchant(const uint8 location, const uint8 slot) -> uint16
{
    auto* PChar = static_cast<CCharEntity*>(POwner);
    if (POwner->isDead())
    {
        return CL_S_KNOCKED_OUT;
    }
    if (m_Enchant.has_value())
    {
        return CL_S_ENCHANT_BUSY;
    }
    auto* storage = PChar->getStorage(location);
    auto* PItem   = storage != nullptr ? dynamic_cast<CItemEquipment*>(storage->GetItem(slot)) : nullptr;
    if (!enchanted(PItem))
    {
        return CL_S_ITEM_UNUSABLE;
    }
    if (PItem->getCurrentCharges() == 0)
    {
        return CL_S_NO_CHARGES;
    }
    // Its own recast, not the delay wearing it starts: a piece used within its
    // recast is refused, not worn for the wait
    if (PItem->getLastUseTime() + PItem->getReuseDelay() > timer::now())
    {
        return CL_S_ON_RECAST;
    }
    // The item's own rule, asked before anything is put on: an experience ring
    // is refused while its effect is still on her, and the game would refuse
    // it after the wait as well
    if (const auto [error, param, value] = luautils::OnItemCheck(PChar, PItem, PChar); error != 0)
    {
        return CL_S_CANNOT_NOW;
    }

    Enchant lane{ .itemId = PItem->getID() };
    if (const auto worn = wornIn(PChar, PItem); worn.has_value())
    {
        lane.equipSlot = *worn; // worn already: nothing to put on, nothing to put back
        ShowInfoFmt("pawn: {} wears {} already; she uses it once it is ready ({} s to wait)", POwner->getName(), PItem->getName(),
                    std::chrono::ceil<std::chrono::seconds>(PItem->getReuseTime()).count());
    }
    else
    {
        const auto to = slotFor(PChar, PItem);
        if (!to.has_value())
        {
            return CL_S_CANNOT_WEAR;
        }
        if (const auto* PWorn = PChar->getEquip(static_cast<SLOTTYPE>(*to)); PWorn != nullptr)
        {
            lane.replacedId = PWorn->getID();
        }
        if (const auto status = pawn::items::equip(PChar, slot, *to, location); status != CL_S_OK)
        {
            return status;
        }
        lane.equipSlot = *to;
        lane.putOn     = true;
        ShowInfoFmt("pawn: {} puts on {} for its use ({}{} s to wait)", POwner->getName(), PItem->getName(),
                    lane.replacedId != 0 ? fmt::format("in place of item {}, ", lane.replacedId) : std::string(),
                    std::chrono::ceil<std::chrono::seconds>(PItem->getReuseTime()).count());
    }
    m_Enchant = lane;
    TellQueueLine();
    return CL_S_OK;
}

void CPawnController::EnchantTick()
{
    if (!m_Enchant.has_value())
    {
        return;
    }
    auto* PChar = static_cast<CCharEntity*>(POwner);
    auto& lane  = *m_Enchant;
    auto* PItem = wornPiece(PChar, lane.equipSlot, lane.itemId);
    // Taken off meanwhile (his own gear change): the lane is over, and what he
    // wore her in stays as he chose it (EndEnchant puts back only over the
    // piece still worn)
    if (PItem == nullptr)
    {
        EndEnchant("it was taken off");
        return;
    }

    if (lane.fired)
    {
        if (Acting())
        {
            return; // the use runs
        }
        lane.fired = false;
        if (PItem->getLastUseTime() != lane.usedBefore)
        {
            ShowInfoFmt("pawn: {} has used {}", POwner->getName(), PItem->getName());
            EndEnchant("used");
            return;
        }
        if (++lane.tries >= kEnchantTries)
        {
            EndEnchant(fmt::format("its use did not land in {} tries", kEnchantTries));
            return;
        }
        ShowInfoFmt("pawn: {}'s use of {} did not land; she tries again when free", POwner->getName(), PItem->getName());
        TellQueueLine();
        return;
    }

    // Ready, and she is free as the game judges it -- as her line's own pacer
    // does -- not kneeling or about to, no weapon skill held behind its
    // opener, not steered by him in a maneuver, and the simulation running.
    // Her line waits behind it this tick (FireQueuedOrder comes after), and
    // her gambits see her acting once it has gone
    if (PItem->getReuseTime() > 0s || cardian::pause::isHeld() || POwner->isDead() || !ReadyToAct() || m_HeldWs.has_value() || InManeuver() ||
        POwner->StatusEffectContainer->HasStatusEffect(xi::StatusEffect::Healing) || m_RestOrder.active())
    {
        return;
    }
    lane.usedBefore = PItem->getLastUseTime();
    if (!POwner->PAI->UseItem(EntityId(POwner), PItem->getLocationID(), PItem->getSlotID()))
    {
        // Refused as it began (the game said why, to her): a try spent
        if (++lane.tries >= kEnchantTries)
        {
            EndEnchant(fmt::format("the game refused its use {} times", kEnchantTries));
        }
        return;
    }
    lane.fired = true;
    ShowInfoFmt("pawn: {} uses {}", POwner->getName(), PItem->getName());
    TellQueueLine();
}

void CPawnController::EndEnchant(const std::string_view why)
{
    if (!m_Enchant.has_value())
    {
        return;
    }
    auto*      PChar = static_cast<CCharEntity*>(POwner);
    const auto lane  = *m_Enchant;
    m_Enchant.reset();
    // Put back only what the lane changed, and only while the piece is still
    // worn where it put it: the replaced piece found by its item wherever it
    // is now, or the slot bared if it was bare
    std::string back;
    if (lane.putOn && wornPiece(PChar, lane.equipSlot, lane.itemId) != nullptr)
    {
        if (lane.replacedId == 0)
        {
            back = pawn::items::unequip(PChar, lane.equipSlot) == CL_S_OK ? "; the slot is bare again" : "; the game kept the piece on";
        }
        else if (const auto* PBack = unwornPiece(PChar, lane.replacedId); PBack == nullptr)
        {
            back = "; the piece it replaced is no longer hers to wear, so it stays on";
        }
        else
        {
            back = pawn::items::equip(PChar, PBack->getSlotID(), lane.equipSlot, PBack->getLocationID()) == CL_S_OK
                       ? "; the piece it replaced is back on"
                       : "; the game would not put the piece it replaced back on, so it stays on";
        }
    }
    ShowInfoFmt("pawn: {}'s enchanted-item lane ends ({}){}", POwner->getName(), why, back);
    TellQueueLine();
}

auto CPawnController::CancelEnchant() -> bool
{
    if (!m_Enchant.has_value() || m_Enchant->fired)
    {
        return false;
    }
    EndEnchant("the player took it back");
    return true;
}
