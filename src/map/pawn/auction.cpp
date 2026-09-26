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

#include "common/database.h"
#include "common/logging.h"
#include "common/settings.h"

#include "entities/char_entity.h"
#include "items/item_equipment.h"
#include "items/item_weapon.h"
#include "trait.h"
#include "utils/charutils.h"
#include "utils/itemutils.h"

#include <algorithm>

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
            const auto rset = db::preparedStmt("SELECT DISTINCT itemid FROM auction_house WHERE stack = 0");
            while (rset && rset->next())
            {
                const auto  itemId = rset->get<uint16>("itemid");
                const auto* PItem  = xi::items::lookup<CItemEquipment>(itemId);
                if (wearable(PChar, PItem, equipSlot, job, level))
                {
                    out.push_back({ itemId, PItem->getReqLvl(), 0, 0, PItem->getAHCat() });
                }
            }
        }
        if (out.empty())
        {
            return out;
        }

        std::vector<uint16> itemIds;
        itemIds.reserve(out.size());
        for (const auto& listing : out)
        {
            itemIds.push_back(listing.itemId);
        }

        std::unordered_map<uint16, uint32> stock;
        {
            const auto rset = db::preparedStmt(fmt::format("SELECT itemid, COUNT(*) AS stock FROM auction_house "
                                                           "WHERE buyer_name IS NULL AND stack = 0 AND itemid IN ({}) GROUP BY itemid",
                                                           fmt::join(itemIds, ",")));
            while (rset && rset->next())
            {
                stock[rset->get<uint16>("itemid")] = rset->get<uint32>("stock");
            }
        }
        const auto going = goingRates(itemIds, false);
        for (auto& listing : out)
        {
            if (const auto it = stock.find(listing.itemId); it != stock.end())
            {
                listing.stock = it->second;
            }
            if (const auto it = going.find(listing.itemId); it != going.end())
            {
                listing.going = it->second;
            }
        }

        std::sort(out.begin(), out.end(), [](const Listing& a, const Listing& b)
                  {
                      if (a.category != b.category)
                      {
                          return a.category < b.category;
                      }
                      if (a.level != b.level)
                      {
                          return a.level > b.level;
                      }
                      if (a.going != b.going)
                      {
                          return a.going > b.going;
                      }
                      return a.itemId < b.itemId;
                  });
        return out;
    }
} // namespace pawn::auction
