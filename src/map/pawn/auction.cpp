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
#include "cardian_link_messages.h"
#include "pawn_items.h"
#include "scrolls.h"

#include "common/database.h"
#include "common/logging.h"
#include "common/settings.h"
#include "common/timer.h"
#include "common/utils.h"

#include "entities/char_entity.h"
#include "enums/item_flag.h"
#include "enums/item_state.h"
#include "item_container.h"
#include "items/item_equipment.h"
#include "items/item_weapon.h"
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
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>

namespace pawn::auction
{
    namespace
    {
        // Every item ever listed, by its id and whether as a stack: what the
        // shelves offer. It only grows, so it is read from the whole sale
        // history once and then from the rows added since (the shelf is asked
        // every few seconds while open, and the history runs to hundreds of
        // thousands of rows). "Since" reaches kLateRows back: the market
        // writes thousands of rows in one transaction, whose ids can sit below
        // a listing the map committed first
        auto listedEver() -> const std::set<std::pair<uint16, bool>>&
        {
            constexpr uint32                         kLateRows = 20000;
            static std::set<std::pair<uint16, bool>> listed;
            static uint32                            readTo = 0;
            const uint32                             from   = readTo > kLateRows ? readTo - kLateRows : 0;
            const auto rset = db::preparedStmt("SELECT itemid, stack, MAX(id) AS last FROM auction_house WHERE id > ? GROUP BY itemid, stack", from);
            while (rset && rset->next())
            {
                listed.emplace(rset->get<uint16>("itemid"), rset->get<uint8>("stack") != 0);
                readTo = std::max(readTo, rset->get<uint32>("last"));
            }
            return listed;
        }

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
            const auto taught = pawn::scrolls::spellOf(PItem);
            if (!taught.has_value())
            {
                return std::nullopt;
            }
            const auto spellId = *taught;
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
        for (const auto& [itemId, stack] : listedEver())
        {
            const auto* PItem = xi::items::lookup<CItemEquipment>(itemId);
            if (wearable(PChar, PItem, equipSlot, job, level) && (!stack || PItem->getStackSize() > 1))
            {
                out.push_back({ itemId, PItem->getReqLvl(), 0, 0, PItem->getAHCat(), stack, stack ? PItem->getStackSize() : 1 });
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
        for (const auto& [itemId, stack] : listedEver())
        {
            const auto* PItem = xi::items::lookup(itemId);
            if (PItem == nullptr || !order.contains(PItem->getAHCat()) || (stack && PItem->getStackSize() <= 1))
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

    auto quotes(const std::vector<std::pair<uint16, bool>>& asked) -> std::vector<uint32>
    {
        struct Quote
        {
            uint32              going = 0;
            realtime::time_point at;
        };
        static std::unordered_map<uint32, Quote> kept;
        constexpr auto                           kKept = std::chrono::seconds(60);
        const auto                               now   = realtime::now();
        if (kept.size() > 8192)
        {
            kept.clear(); // a bound, never reached by one player's bags
        }

        const auto keyOf = [](const uint16 itemId, const bool stack)
        {
            return static_cast<uint32>(itemId) * 2u + (stack ? 1u : 0u);
        };
        std::vector<uint16> stale[2];
        for (const auto& [itemId, stack] : asked)
        {
            const auto it   = kept.find(keyOf(itemId, stack));
            auto&      list = stale[stack ? 1 : 0];
            if ((it == kept.end() || now - it->second.at > kKept) && std::find(list.begin(), list.end(), itemId) == list.end())
            {
                list.push_back(itemId);
            }
        }
        for (const bool stack : { false, true })
        {
            const auto& list = stale[stack ? 1 : 0];
            if (list.empty())
            {
                continue;
            }
            const auto rates = goingRates(list, stack);
            for (const uint16 itemId : list)
            {
                const auto rate               = rates.find(itemId);
                kept[keyOf(itemId, stack)] = Quote{ rate != rates.end() ? rate->second : 0, now };
            }
        }

        std::vector<uint32> out;
        out.reserve(asked.size());
        for (const auto& [itemId, stack] : asked)
        {
            out.push_back(kept[keyOf(itemId, stack)].going);
        }
        return out;
    }

    auto startPrice(const uint16 itemId, const bool stack, const uint32 sellerId, const uint32 going) -> uint32
    {
        uint32     start = going;
        const auto rset  = db::preparedStmt("SELECT COALESCE(MIN(price), 0) AS cheapest FROM auction_house WHERE itemid = ? AND stack = ? AND sale = 0 AND seller <> ?",
                                            itemId,
                                            stack ? 1 : 0,
                                            sellerId);
        if (rset && rset->next())
        {
            const auto cheapest = rset->get<uint32>("cheapest");
            if (cheapest > 1 && (start == 0 || cheapest - 1 < start))
            {
                start = cheapest - 1;
            }
        }
        return start;
    }

    auto bid(CCharEntity* PChar, CCharEntity* PPurse, const uint16 itemId, const bool stack, const uint32 price, const uint8 location, const uint8 equipSlot, const bool equip) -> BidResult
    {
        BidResult   result;
        const auto* PItem  = xi::items::lookup(itemId);
        const auto  job    = PChar != nullptr ? static_cast<uint8>(PChar->GetMJob()) : 0;
        const auto  refuse = [&result](const uint16 why) -> BidResult
        {
            result.status = why;
            return result;
        };

        if (PItem == nullptr || job == 0)
        {
            return refuse(CL_S_NO_SUCH_ITEM);
        }
        if (stack && PItem->getStackSize() <= 1)
        {
            return refuse(CL_S_NOT_IN_STACKS);
        }
        const uint32 quantity = stack ? PItem->getStackSize() : 1;
        if (price == 0 || price > 999999999)
        {
            return refuse(CL_S_BAD_PRICE);
        }
        // The game's own gates on a bid (the auction packet's validator), on
        // whoever is bidding and on the player placing it for her
        CCharEntity* PActor = PPurse != nullptr ? PPurse : PChar;
        if (PChar->isInEvent() || PActor->isInEvent())
        {
            return refuse(CL_S_IN_EVENT);
        }
        if (jailutils::InPrison(PChar) || jailutils::InPrison(PActor))
        {
            return refuse(CL_S_IN_JAIL);
        }
        if (PChar->loc.zone == nullptr || !PChar->loc.zone->CanUseMisc(xi::ZoneMisc::AuctionHouse))
        {
            return refuse(CL_S_NO_AUCTION_HOUSE);
        }
        if (!pawn::items::usableContainer(PChar, location))
        {
            return refuse(CL_S_NO_SUCH_BAG);
        }
        if (pawn::items::isWardrobe(location) && dynamic_cast<const CItemEquipment*>(PItem) == nullptr)
        {
            return refuse(CL_S_WARDROBE_GEAR); // the game's own item move's rule
        }
        if (equip && location != LOC_INVENTORY && !pawn::items::isWardrobe(location))
        {
            return refuse(CL_S_WORN_FROM_BAG);
        }
        if (equip && (equipSlot > SLOT_BACK || !wearable(PChar, dynamic_cast<const CItemEquipment*>(PItem), equipSlot, job, equipLevel(PChar))))
        {
            return refuse(CL_S_CANNOT_WEAR);
        }
        if (PChar->getStorage(LOC_INVENTORY)->GetFreeSlotsCount() == 0)
        {
            return refuse(CL_S_NO_SPACE);
        }
        if (location != LOC_INVENTORY && PChar->getStorage(location)->GetFreeSlotsCount() == 0)
        {
            return refuse(CL_S_BAG_FULL);
        }
        const bool   shared    = PPurse != nullptr && PPurse != PChar;
        const uint32 own       = pawn::items::gilOf(PChar);
        const uint32 shortfall = own < price ? price - own : 0;
        if (shortfall > 0 && (!shared || pawn::items::gilOf(PPurse) < shortfall))
        {
            return refuse(CL_S_NOT_ENOUGH_GIL);
        }
        if (PItem->hasFlag(ItemFlag::Rare) && charutils::HasItem(PChar, itemId))
        {
            return refuse(CL_S_RARE_OWNED);
        }

        if (!listedAtOrUnder(itemId, stack, price))
        {
            return refuse(CL_S_NOTHING_AT_PRICE);
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
            return refuse(CL_S_PURCHASE_FAILED); // a listing was there: busy gil, the database, or another buyer first
        }
        result.fromPurse = shortfall;

        // The purchase lands in the inventory; a split (a take and a fresh
        // give, as the game's own item move splits) sends it on to the bag,
        // and the client is told of both
        result.status   = CL_S_OK;
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
                ShowInfoFmt("auction: {}'s {} stayed in the inventory: the move on to bag {} fell through", PChar->getName(), itemId, location);
            }
        }
        // The equip packet's own gate: nothing is put on out of a normal status
        if (equip && slot != 0 && PChar->status != xi::Status::Normal)
        {
            result.notWorn = CL_S_CANNOT_NOW;
        }
        else if (equip && slot != 0)
        {
            if (const auto status = pawn::items::equip(PChar, slot, equipSlot, result.location); status == CL_S_OK)
            {
                result.equipped = true;
            }
            else
            {
                ShowInfoFmt("auction: {} does not wear the {} she won (outcome 0x{:04X})", PChar->getName(), itemId, status);
                result.notWorn = status;
            }
        }
        return result;
    }

    namespace
    {
        constexpr float kCounterReach = 8.0f;  // yalms, the player to a counter (a gate guard's reach)
        constexpr float kShopReach    = 15.0f; // yalms, a member to the counter the player stands at
    } // namespace

    namespace
    {
        // A script that opens the auction menu, by name; gated when it opens
        // it only under a condition of its own (Tavnazian Safehold's clerks,
        // after a Promathia mission): its call sits deeper than its
        // function's own body, which upstream's formatter indents four spaces
        struct Auctioneer
        {
            std::string name;
            bool        gated = false;
        };

        auto auctioneersOf(CZone* PZone) -> const std::vector<Auctioneer>&
        {
            static std::unordered_map<uint16, std::vector<Auctioneer>> byZone;
            const auto zoneId = static_cast<uint16>(PZone->GetID());
            if (const auto known = byZone.find(zoneId); known != byZone.end())
            {
                return known->second;
            }
            std::vector<Auctioneer> found;
            std::error_code         error;
            for (const auto& file : std::filesystem::directory_iterator(fmt::format("./scripts/zones/{}/npcs", PZone->getName()), error))
            {
                if (file.path().extension() != ".lua")
                {
                    continue;
                }
                std::ifstream in(file.path(), std::ios::binary);
                std::string   line;
                bool          opens = false;
                bool          gated = true;
                while (std::getline(in, line))
                {
                    if (line.find("menuType.AUCTION") != std::string::npos)
                    {
                        opens = true;
                        gated = gated && line.find_first_not_of(' ') > 4;
                    }
                }
                if (opens)
                {
                    found.push_back({ file.path().stem().string(), gated });
                }
            }
            if (!found.empty())
            {
                std::string list;
                for (const auto& a : found)
                {
                    list += fmt::format("{}{}{}", list.empty() ? "" : ", ", a.name, a.gated ? " (gated: the crowd's, not the player's)" : "");
                }
                ShowInfoFmt("auction: {}'s auction counters: {}", PZone->getName(), list);
            }
            return byZone.emplace(zoneId, std::move(found)).first->second;
        }
    } // namespace

    auto countersIn(CZone* PZone, const bool withGated) -> std::vector<const CBaseEntity*>
    {
        std::vector<const CBaseEntity*> counters;
        if (PZone == nullptr)
        {
            return counters;
        }
        for (const auto& a : auctioneersOf(PZone))
        {
            if (a.gated && !withGated)
            {
                continue;
            }
            for (const auto* PEntity : PZone->queryEntitiesByName(a.name))
            {
                if (PEntity != nullptr && PEntity->status != xi::Status::Disappear)
                {
                    counters.push_back(PEntity);
                }
            }
        }
        return counters;
    }

    auto counterNear(const CCharEntity* PPlayer) -> const CBaseEntity*
    {
        if (PPlayer == nullptr || PPlayer->loc.zone == nullptr)
        {
            return nullptr;
        }
        const CBaseEntity* PNearest = nullptr;
        float              nearest  = kCounterReach;
        for (const auto* PCounter : countersIn(PPlayer->loc.zone, false))
        {
            if (PCounter == nullptr)
            {
                continue;
            }
            if (const float away = distance(PPlayer->loc.p, PCounter->loc.p); away <= nearest)
            {
                PNearest = PCounter;
                nearest  = away;
            }
        }
        return PNearest;
    }

    auto whereShopping(const CCharEntity* PPlayer, const CCharEntity* PMember) -> uint16
    {
        return whereShopping(PPlayer, PMember, counterNear(PPlayer));
    }

    auto whereShopping(const CCharEntity* PPlayer, const CCharEntity* PMember, const CBaseEntity* PCounter) -> uint16
    {
        if (PCounter == nullptr)
        {
            return CL_S_NOT_BY_COUNTER;
        }
        if (PMember == PPlayer)
        {
            return CL_S_OK;
        }
        if (PMember == nullptr || PMember->loc.zone != PPlayer->loc.zone || distance(PMember->loc.p, PCounter->loc.p) > kShopReach)
        {
            return CL_S_TOO_FAR_TO_SHOP;
        }
        return CL_S_OK;
    }
} // namespace pawn::auction
