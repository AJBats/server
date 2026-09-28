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

#include "auction.h"
#include "pawn_items.h"

#include "common/database.h"
#include "common/earth_time.h"
#include "common/logging.h"
#include "common/settings.h"

#include "entities/char_entity.h"
#include "enums/item_flag.h"
#include "enums/item_state.h"
#include "item_container.h"
#include "items/item_equipment.h"
#include "items/item_weapon.h"
#include "items/transaction.h"
#include "items/transactions/item_claim.h"
#include "lua/luautils.h"
#include "packets/c2s/0x04e_auc.h"
#include "spell.h"
#include "trait.h"
#include "utils/auctionutils.h"
#include "utils/charutils.h"
#include "utils/itemutils.h"
#include "utils/jailutils.h"
#include "zone.h"

#include <set>

#include <algorithm>
#include <optional>

namespace pawn::auction
{
    namespace
    {
        // The level the equip handler weighs a piece against
        auto equipLevel(const CCharEntity* PChar) -> uint8
        {
            return settings::get<bool>("map.DISABLE_GEAR_SCALING") ? PChar->GetMLevel()
                                                                   : PChar->jobs.job[static_cast<uint8>(PChar->GetMJob())];
        }

        // Could she wear it in that slot, as charutils::EquipArmor judges:
        // the slot, her main job, her level, her superior level and her
        // race. In Sub a weapon needs Dual Wield; a shield or a grip goes by
        // the main weapon she holds when she equips it, so both are offered.
        auto wearable(CCharEntity* PChar, const CItemEquipment* PItem, const uint8 equipSlot, const uint8 job, const uint8 level) -> bool
        {
            if (PItem == nullptr ||
                (PItem->getEquipSlotId() & (1 << equipSlot)) == 0 ||
                (PItem->getJobs() & (1 << (job - 1))) == 0 ||
                PItem->getReqLvl() > level ||
                const_cast<CItemEquipment*>(PItem)->getSuperiorLevel() > PChar->getMod(xi::Mod::SUPERIOR_LEVEL) || // upstream's getter is not const
                !PItem->isEquippableByRace(PChar->look.race))
            {
                return false;
            }
            if (equipSlot == SLOT_SUB && PItem->isType(ITEM_WEAPON))
            {
                const auto* PWeapon = dynamic_cast<const CItemWeapon*>(PItem);
                if (PWeapon != nullptr && PWeapon->getSkillType() != xi::SkillType::None && !charutils::hasTrait(PChar, TRAIT_DUAL_WIELD))
                {
                    return false;
                }
            }
            return true;
        }

        auto median(std::vector<uint32> sales) -> uint32
        {
            std::sort(sales.begin(), sales.end());
            return sales[sales.size() / 2];
        }

        // The slots of a container holding the item, taken before a purchase
        // or a move, so the piece that lands is told from the ones she had:
        // an older copy may carry augments or a signature, and moving it by a
        // take and a fresh give would lose them
        auto slotsWith(CCharEntity* PChar, const uint8 location, const uint16 itemId) -> std::set<uint8>
        {
            std::set<uint8> out;
            const auto*     storage = PChar->getStorage(location);
            for (uint8 slot = 1; storage != nullptr && slot <= storage->GetSize(); ++slot)
            {
                if (const CItem* PItem = storage->GetItem(slot); PItem != nullptr && PItem->getID() == itemId)
                {
                    out.insert(slot);
                }
            }
            return out;
        }

        // The slot the new piece landed in: one holding the item, idle and
        // unworn, that was not among `before`. A stack may have topped up one
        // she had, so for a stack any idle, unworn slot holding a whole one
        // (a stack bought here carries no augments). 0 when there is none
        auto landedSlot(CCharEntity* PChar, const uint8 location, const uint16 itemId, const std::set<uint8>& before, const uint32 quantity) -> uint8
        {
            const auto* storage = PChar->getStorage(location);
            for (uint8 slot = 1; storage != nullptr && slot <= storage->GetSize(); ++slot)
            {
                const CItem* PItem = storage->GetItem(slot);
                if (PItem == nullptr || PItem->getID() != itemId || PItem->state() == ItemState::Equipped || PItem->isBusy())
                {
                    continue;
                }
                if (quantity > 1 ? PItem->getQuantity() >= quantity : !before.contains(slot))
                {
                    return slot;
                }
            }
            return 0;
        }

        auto gilOf(CCharEntity* PChar) -> uint32
        {
            const CItem* PGil = PChar->getStorage(LOC_INVENTORY)->GetItem(0);
            return PGil != nullptr ? PGil->getQuantity() : 0;
        }

        // The shared purchase's work in one transaction across both
        // characters, as pawn_items' CardianTransfer moves gil between two:
        // her share and his paid, her piece given, one commit or one rollback
        class SharedPurchase final : public Transaction
        {
        public:
            ~SharedPurchase() override
            {
                this->rollbackIfOpen();
            }

            auto settle(CCharEntity* PChar, CCharEntity* PPurse, const uint32 hers, const uint32 his, const uint16 itemId, const uint32 quantity) -> bool
            {
                return (hers == 0 || this->pay(PChar, hers)) &&
                       (his == 0 || this->pay(PPurse, his)) &&
                       this->give(PChar, LOC_INVENTORY, itemId, quantity).has_value();
            }

        protected:
            // give and pay above already applied and recorded the work
            auto doCommit() -> bool override
            {
                return true;
            }

            void doRollback() override
            {
            }
        };

        // A cardian's purchase with the player's purse behind hers (the user,
        // 2026-09-26: gil moves only on a won bid): the game's own purchase
        // copied (auctionutils::PurchasingItems) -- the cheapest listing at or
        // under the bid marked sold to her at the bid, the payment and her
        // piece in one database transaction -- with the payment split, hers
        // first and `fromPurse` of it his. All of it, or none. A copy, so
        // compare it with PurchasingItems at every merge from base
        auto purchaseShared(CCharEntity* PChar, CCharEntity* PPurse, const uint16 itemId, const bool stack, const uint32 quantity, const uint32 price, const uint32 fromPurse) -> bool
        {
            SharedPurchase transaction;
            const auto     success = db::transaction(
                [&]()
                {
                    const auto rset = db::preparedStmt("UPDATE auction_house SET buyer = ?, buyer_name = ?, sale = ?, sell_date = ? WHERE itemid = ? AND buyer_name IS NULL "
                                                       "AND stack = ? AND price <= ? ORDER BY price LIMIT 1",
                                                       PChar->id,
                                                       PChar->getName(),
                                                       price,
                                                       earth_time::timestamp(),
                                                       itemId,
                                                       stack ? 1 : 0,
                                                       price);
                    if (rset && rset->rowsAffected() && transaction.settle(PChar, PPurse, price - fromPurse, fromPurse, itemId, quantity))
                    {
                        return;
                    }
                    throw std::runtime_error(fmt::format("auction: {} could not buy item {} at {}", PChar->getName(), itemId, price));
                });
            return success && transaction.commit();
        }

        // Is there a listing at or under the bid? Asked before a purchase is
        // tried, and never shown as a price: the answer to a bid is the same
        // either way (the blind auction house), only no purchase is tried
        // that cannot win -- the game's own logs every failed one as critical.
        // A query that fails says yes: the purchase is tried and answers
        auto listedAtOrUnder(const uint16 itemId, const bool stack, const uint32 price) -> bool
        {
            const auto rset = db::preparedStmt("SELECT 1 FROM auction_house WHERE itemid = ? AND stack = ? AND buyer_name IS NULL AND price <= ? LIMIT 1",
                                               itemId,
                                               stack ? 1 : 0,
                                               price);
            return !rset || rset->next();
        }

        // The level at which she learns the scroll's spell -- her main job's,
        // else her sub job's unless the spell is the main job's alone -- when
        // she can learn it now and has not; none for a spell she knows, one
        // above her levels, one of an expansion switched off, and anything
        // that is not a spell scroll. The level test of a scroll's own check
        // (CLuaBaseEntity::canLearnSpell), without its tests of a status
        // being up (a scholar's Addendum)
        auto learnLevel(CCharEntity* PChar, const CItem* PItem) -> std::optional<uint8>
        {
            // subid is the spell a scroll teaches, but for Haste II's, which
            // names Atomos (its script teaches Haste II); three scrolls have
            // no script to use them by: Comet, Meteor, Breakga
            constexpr uint16 kHasteII = 4692;
            const uint16     itemId   = PItem->getID();
            if (itemId == 4827 || itemId == 4851 || itemId == 4889 || !PItem->hasFlag(ItemFlag::Scroll) || PItem->getSubID() == 0)
            {
                return std::nullopt;
            }
            const auto spellId = itemId == kHasteII ? SpellID::Haste_II : static_cast<SpellID>(PItem->getSubID());
            CSpell*    PSpell  = spell::GetSpell(spellId);
            if (PSpell == nullptr || PSpell->getSpellGroup() == SPELLGROUP_BLUE || PSpell->getSpellGroup() == SPELLGROUP_TRUST ||
                charutils::hasSpell(PChar, static_cast<uint16>(spellId)) || !luautils::IsContentEnabled(PSpell->getContentTag()))
            {
                return std::nullopt;
            }
            // getJob answers 255 for a job that never learns it
            if (const uint8 main = PSpell->getJob(PChar->GetMJob()); PChar->GetMLevel() >= main)
            {
                return main;
            }
            if ((PSpell->getRequirements() & SPELLREQ_MAIN_JOB_ONLY) == 0)
            {
                if (const uint8 sub = PSpell->getJob(PChar->GetSJob()); PChar->GetSLevel() >= sub)
                {
                    return sub;
                }
            }
            return std::nullopt;
        }

        // Every row's stock and going rate in its form, told in three
        // queries whatever the rows. Returns each item's price by the piece,
        // from whichever form has sold, so an item's two rows sort together
        auto priced(std::vector<Listing>& out) -> std::unordered_map<uint16, uint32>
        {
            std::unordered_map<uint16, uint32> perPiece;
            if (out.empty())
            {
                return perPiece;
            }

            std::vector<uint16> itemIds;
            std::vector<uint16> singles;
            std::vector<uint16> stacks;
            for (const auto& listing : out)
            {
                itemIds.push_back(listing.itemId);
                (listing.stack ? stacks : singles).push_back(listing.itemId);
            }

            // An item and its form as one key
            const auto keyOf = [](const uint16 itemId, const bool stack) -> uint32
            {
                return (static_cast<uint32>(itemId) << 1) | (stack ? 1 : 0);
            };
            std::unordered_map<uint32, uint32> stock;
            {
                const auto rset = db::preparedStmt(fmt::format("SELECT itemid, stack, COUNT(*) AS stock FROM auction_house "
                                                               "WHERE buyer_name IS NULL AND itemid IN ({}) GROUP BY itemid, stack",
                                                               fmt::join(itemIds, ",")));
                while (rset && rset->next())
                {
                    stock[keyOf(rset->get<uint16>("itemid"), rset->get<uint8>("stack") != 0)] = rset->get<uint32>("stock");
                }
            }
            const auto goingSingle = goingRates(singles, false);
            const auto goingStack  = goingRates(stacks, true);

            for (auto& listing : out)
            {
                if (const auto it = stock.find(keyOf(listing.itemId, listing.stack)); it != stock.end())
                {
                    listing.stock = it->second;
                }
                const auto& going = listing.stack ? goingStack : goingSingle;
                if (const auto it = going.find(listing.itemId); it != going.end())
                {
                    listing.going = it->second;
                }
                auto& piece = perPiece[listing.itemId];
                piece       = std::max(piece, listing.going / listing.stackSize);
            }
            return perPiece;
        }
    } // namespace

    auto goingRates(const std::vector<uint16>& itemIds, const bool stack) -> std::unordered_map<uint16, uint32>
    {
        std::unordered_map<uint16, uint32> out;
        if (itemIds.empty())
        {
            return out;
        }

        std::unordered_map<uint16, std::vector<uint32>> sales;
        const auto rset = db::preparedStmt(fmt::format("SELECT itemid, sale FROM (SELECT itemid, sale, ROW_NUMBER() OVER (PARTITION BY itemid ORDER BY sell_date DESC) AS rn "
                                                       "FROM auction_house WHERE stack = ? AND buyer_name IS NOT NULL AND itemid IN ({})) AS recent WHERE rn <= 10",
                                                       fmt::join(itemIds, ",")),
                                           stack ? 1 : 0);
        while (rset && rset->next())
        {
            sales[rset->get<uint16>("itemid")].push_back(rset->get<uint32>("sale"));
        }
        for (auto& [itemId, list] : sales)
        {
            out[itemId] = median(std::move(list));
        }
        return out;
    }

    auto wearableAtAuction(CCharEntity* PChar, const uint8 equipSlot) -> std::vector<Listing>
    {
        std::vector<Listing> out;
        const auto           job = PChar != nullptr ? static_cast<uint8>(PChar->GetMJob()) : 0;
        if (job == 0 || equipSlot > SLOT_BACK)
        {
            return out;
        }

        const auto level = equipLevel(PChar);
        {
            const auto rset = db::preparedStmt("SELECT DISTINCT itemid, stack FROM auction_house");
            while (rset && rset->next())
            {
                const auto  itemId = rset->get<uint16>("itemid");
                const bool  stack  = rset->get<uint8>("stack") != 0;
                const auto* PItem  = xi::items::lookup<CItemEquipment>(itemId);
                if (wearable(PChar, PItem, equipSlot, job, level) && (!stack || PItem->getStackSize() > 1))
                {
                    out.push_back({ itemId, PItem->getReqLvl(), 0, 0, PItem->getAHCat(), stack, stack ? PItem->getStackSize() : 1 });
                }
            }
        }
        if (out.empty())
        {
            return out;
        }

        auto perPiece = priced(out);
        std::sort(out.begin(), out.end(), [&perPiece](const Listing& a, const Listing& b)
                  {
                      if (a.category != b.category)
                      {
                          return a.category < b.category;
                      }
                      if (a.level != b.level)
                      {
                          return a.level > b.level;
                      }
                      if (perPiece[a.itemId] != perPiece[b.itemId])
                      {
                          return perPiece[a.itemId] > perPiece[b.itemId];
                      }
                      if (a.itemId != b.itemId)
                      {
                          return a.itemId < b.itemId;
                      }
                      return !a.stack && b.stack;
                  });
        return out;
    }

    auto inCategories(CCharEntity* PChar, const std::vector<uint8>& categories, const bool learnable) -> std::vector<Listing>
    {
        std::vector<Listing> out;
        if (PChar == nullptr || categories.empty())
        {
            return out;
        }

        // A category's place in the order asked for
        std::unordered_map<uint8, size_t> order;
        for (size_t i = 0; i < categories.size(); ++i)
        {
            order.try_emplace(categories[i], i);
        }

        std::unordered_map<uint16, std::string> names;
        {
            const auto rset = db::preparedStmt(fmt::format("SELECT DISTINCT ah.itemid, ah.stack FROM auction_house AS ah "
                                                           "INNER JOIN item_basic AS ib ON ib.itemid = ah.itemid WHERE ib.aH IN ({})",
                                                           fmt::join(categories, ",")));
            while (rset && rset->next())
            {
                const auto  itemId = rset->get<uint16>("itemid");
                const bool  stack  = rset->get<uint8>("stack") != 0;
                const auto* PItem  = xi::items::lookup(itemId);
                if (PItem == nullptr || (stack && PItem->getStackSize() <= 1))
                {
                    continue;
                }
                uint8 level = 0;
                if (learnable)
                {
                    const auto needs = learnLevel(PChar, PItem);
                    if (!needs)
                    {
                        continue;
                    }
                    level = *needs;
                }
                else if (const auto* PEquip = dynamic_cast<const CItemEquipment*>(PItem))
                {
                    level = PEquip->getReqLvl();
                }
                out.push_back({ itemId, level, 0, 0, PItem->getAHCat(), stack, stack ? PItem->getStackSize() : 1 });
                names.try_emplace(itemId, PItem->getName());
            }
        }

        priced(out);
        std::sort(out.begin(), out.end(), [&order, &names](const Listing& a, const Listing& b)
                  {
                      if (a.category != b.category)
                      {
                          return order[a.category] < order[b.category];
                      }
                      if (a.level != b.level)
                      {
                          return a.level > b.level;
                      }
                      if (a.itemId != b.itemId)
                      {
                          const auto& nameA = names[a.itemId];
                          const auto& nameB = names[b.itemId];
                          return nameA != nameB ? nameA < nameB : a.itemId < b.itemId; // names are not unique
                      }
                      return !a.stack && b.stack;
                  });
        return out;
    }

    auto history(const uint16 itemId, const bool stack) -> History
    {
        History out;
        if (const auto rset = db::preparedStmt("SELECT COUNT(*) AS stock FROM auction_house WHERE buyer_name IS NULL AND stack = ? AND itemid = ?", stack ? 1 : 0, itemId);
            rset && rset->next())
        {
            out.stock = rset->get<uint32>("stock");
        }

        std::vector<uint32> prices;
        const auto          rset = db::preparedStmt("SELECT sell_date, sale, COALESCE(seller_name, '') AS seller_name, buyer_name FROM auction_house "
                                                    "WHERE itemid = ? AND stack = ? AND buyer_name IS NOT NULL ORDER BY sell_date DESC LIMIT 10",
                                                    itemId,
                                                    stack ? 1 : 0);
        while (rset && rset->next())
        {
            out.sales.push_back({ rset->get<uint32>("sell_date"), rset->get<uint32>("sale"), rset->get<std::string>("seller_name"), rset->get<std::string>("buyer_name") });
            prices.push_back(out.sales.back().price);
        }
        if (!prices.empty())
        {
            out.going = median(std::move(prices));
        }
        return out;
    }

    auto bid(CCharEntity* PChar, CCharEntity* PPurse, const uint16 itemId, const bool stack, const uint32 price, const uint8 location, const uint8 equipSlot, const bool equip) -> BidResult
    {
        BidResult   result;
        const auto* PItem  = xi::items::lookup(itemId);
        const auto  job    = PChar != nullptr ? static_cast<uint8>(PChar->GetMJob()) : 0;
        const auto  refuse = [&result](std::string why) -> BidResult
        {
            result.refused = std::move(why);
            return result;
        };

        if (PItem == nullptr || job == 0)
        {
            return refuse("no such item");
        }
        if (stack && PItem->getStackSize() <= 1)
        {
            return refuse("that does not come in stacks");
        }
        const uint32 quantity = stack ? PItem->getStackSize() : 1;
        if (price == 0 || price > 999999999)
        {
            return refuse("no such price");
        }
        // The game's own gates on a bid (the auction packet's validator), on
        // whoever is bidding and on the player placing it for her
        CCharEntity* PActor = PPurse != nullptr ? PPurse : PChar;
        if (PChar->isInEvent() || PActor->isInEvent())
        {
            return refuse("not during an event");
        }
        if (jailutils::InPrison(PChar) || jailutils::InPrison(PActor))
        {
            return refuse("not from jail");
        }
        if (PChar->loc.zone == nullptr || !PChar->loc.zone->CanUseMisc(xi::ZoneMisc::AuctionHouse))
        {
            return refuse("there is no auction house here");
        }
        if (!pawn::items::usableContainer(PChar, location))
        {
            return refuse("no such bag");
        }
        if (pawn::items::isWardrobe(location) && dynamic_cast<const CItemEquipment*>(PItem) == nullptr)
        {
            return refuse("only gear goes in a wardrobe"); // the game's own item move's rule
        }
        if (equip && location != LOC_INVENTORY && !pawn::items::isWardrobe(location))
        {
            return refuse("gear is worn from the inventory or a wardrobe");
        }
        if (equip && (equipSlot > SLOT_BACK || !wearable(PChar, dynamic_cast<const CItemEquipment*>(PItem), equipSlot, job, equipLevel(PChar))))
        {
            return refuse("that cannot be worn there");
        }
        if (PChar->getStorage(LOC_INVENTORY)->GetFreeSlotsCount() == 0)
        {
            return refuse("no room in the inventory");
        }
        if (location != LOC_INVENTORY && PChar->getStorage(location)->GetFreeSlotsCount() == 0)
        {
            return refuse("no room in that bag");
        }
        const bool   shared    = PPurse != nullptr && PPurse != PChar;
        const uint32 own       = gilOf(PChar);
        const uint32 shortfall = own < price ? price - own : 0;
        if (shortfall > 0 && (!shared || gilOf(PPurse) < shortfall))
        {
            return refuse(shared ? "not enough gil, hers and yours together" : "not enough gil");
        }
        if (PItem->hasFlag(ItemFlag::Rare) && charutils::HasItem(PChar, itemId))
        {
            return refuse("it is Rare, and one is already owned");
        }

        if (!listedAtOrUnder(itemId, stack, price))
        {
            return refuse("nothing at that price or less");
        }

        const auto before = slotsWith(PChar, LOC_INVENTORY, itemId);

        // A cardian buys with the purse behind her in one transaction; the
        // player through the game's own purchase, its packets to his client
        // and all
        bool bought = false;
        if (shared)
        {
            bought = purchaseShared(PChar, PPurse, itemId, stack, quantity, price, shortfall);
        }
        else
        {
            GP_AUC_PARAM_BID param{};
            param.BidPrice   = price;
            param.ItemNo     = itemId;
            param.ItemStacks = stack ? 0 : 1; // the purchase reads 0 as a whole stack, 1 as one piece
            bought           = auctionutils::PurchasingItems(PChar, param);
        }
        if (!bought)
        {
            return refuse("the purchase failed; try again"); // a listing was there: busy gil, the database, or another buyer first
        }
        result.fromPurse = shortfall;

        // The purchase lands in the inventory; a split (a take and a fresh
        // give, as the game's own item move splits) sends it on to the bag,
        // and the client is told of both
        result.won      = true;
        result.location = LOC_INVENTORY;
        uint8 slot      = landedSlot(PChar, LOC_INVENTORY, itemId, before, quantity);
        if (location != LOC_INVENTORY && slot != 0)
        {
            const auto inBag       = slotsWith(PChar, location, itemId);
            auto       transaction = ItemClaimTransaction::start(PChar);
            if (transaction && transaction->claimSlot(LOC_INVENTORY, slot) != nullptr && transaction->split(LOC_INVENTORY, slot, location, quantity) &&
                transaction->commit())
            {
                result.location = location;
                slot            = landedSlot(PChar, location, itemId, inBag, quantity);
            }
            else
            {
                result.note = "it stayed in the inventory";
            }
        }
        // The equip packet's own gate: nothing is put on out of a normal status
        if (equip && slot != 0 && PChar->status != xi::Status::Normal)
        {
            result.note += (result.note.empty() ? "" : "; ") + std::string("not worn: not now");
        }
        else if (equip && slot != 0)
        {
            if (const auto err = pawn::items::equip(PChar, slot, equipSlot, result.location); err.empty())
            {
                result.equipped = true;
            }
            else
            {
                result.note += (result.note.empty() ? "" : "; ") + std::string("not worn: ") + err;
            }
        }
        return result;
    }
} // namespace pawn::auction
