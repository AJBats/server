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

#include "food.h"

#include "gambit_defaults.h"
#include "pawn.h"
#include "pawn_items.h"
#include "seats.h"

#include "common/database.h"
#include "common/logging.h"
#include "common/settings.h"
#include "common/timer.h"
#include "entities/char_entity.h"
#include "item_container.h"
#include "items/item.h"
#include "items/item_equipment.h"
#include "items/item_weapon.h"
#include "items/transactions/item_claim.h"
#include "status_effect.h"
#include "status_effect_container.h"
#include "utils/itemutils.h"

#include <array>
#include <chrono>
#include <fstream>
#include <map>
#include <sstream>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace pawn::food
{
    namespace
    {
        using cardian::food::Carried;
        using cardian::food::Facts;
        using cardian::food::Pick;
        using cardian::party::Role;

        constexpr auto kPlanKept = std::chrono::seconds(60); // the census's picks read at most this often per body

        // The containers she reaches from the field: her inventory, then her bags
        auto containersOf(CCharEntity* PChar) -> std::vector<uint8>
        {
            std::vector<uint8> out{ LOC_INVENTORY };
            for (const auto& bag : pawn::items::bags(PChar))
            {
                out.push_back(bag.location);
            }
            return out;
        }

        // How many of each item she carries, over every container she reaches
        auto carried(CCharEntity* PChar) -> std::map<uint16, uint32>
        {
            std::map<uint16, uint32> out;
            for (const uint8 location : containersOf(PChar))
            {
                auto* storage = PChar->getStorage(location);
                if (storage == nullptr)
                {
                    continue;
                }
                for (uint8 slot = 1; slot <= storage->GetSize(); ++slot)
                {
                    if (const auto* PItem = storage->GetItem(slot); PItem != nullptr && PItem->getQuantity() > 0)
                    {
                        out[PItem->getID()] += PItem->getQuantity();
                    }
                }
            }
            return out;
        }

        // The census's picks for a body, by role, kept a while
        struct Plan
        {
            timer::time_point                  readAt{};
            std::array<std::optional<Pick>, 4> byRole{}; // indexed by Role: Tank, Healer, Damage
        };

        auto plans() -> std::unordered_map<uint32, Plan>&
        {
            static auto& kept = *new std::unordered_map<uint32, Plan>();
            return kept;
        }

        auto planOf(const CCharEntity* PChar) -> const Plan&
        {
            auto&      plan = plans()[PChar->id];
            const auto now  = timer::now();
            if (plan.readAt != timer::time_point{} && now - plan.readAt < kPlanKept)
            {
                return plan;
            }
            plan        = Plan{};
            plan.readAt = now;
            const auto rset = db::preparedStmt("SELECT role, itemid, cookie FROM cardian_food WHERE name = ?", PChar->getName());
            if (!rset)
            {
                return plan;
            }
            while (rset->next())
            {
                const auto role = rset->get<uint8>("role");
                if (role < plan.byRole.size())
                {
                    plan.byRole[role] = Pick{ rset->get<uint16>("itemid"), rset->get<uint8>("cookie") != 0 };
                }
            }
            return plan;
        }

        // Her own numbers (food_math.h Own): her level, her base STR, VIT,
        // DEX and AGI, her main and ranged weapons' skills and her evasion,
        // her base HP and MP, and her gear, with no effect on her, so a buff
        // or a meal coming and going never moves her pick
        auto ownStats(CCharEntity* PChar) -> cardian::food::Stats
        {
            cardian::food::Own own;
            own.level        = PChar->GetMLevel();
            own.str          = PChar->stats.STR;
            own.vit          = PChar->stats.VIT;
            own.dex          = PChar->stats.DEX;
            own.agi          = PChar->stats.AGI;
            own.hp           = PChar->health.maxhp;
            own.mp           = PChar->health.maxmp;
            own.vitFactor    = settings::get<float>("main.PLAYER_ALLIES_VIT_DEF_MULTIPLIER");
            own.evasionSkill = PChar->GetSkill(xi::SkillType::Evasion);
            for (uint8 slot = SLOT_MAIN; slot <= SLOT_BACK; ++slot)
            {
                if (auto* PEquip = PChar->getEquip(static_cast<SLOTTYPE>(slot)); PEquip != nullptr)
                {
                    own.str += PEquip->getModifier(xi::Mod::STR);
                    own.vit += PEquip->getModifier(xi::Mod::VIT);
                    own.dex += PEquip->getModifier(xi::Mod::DEX);
                    own.agi += PEquip->getModifier(xi::Mod::AGI);
                    own.hp += PEquip->getModifier(xi::Mod::HP);
                    own.mp += PEquip->getModifier(xi::Mod::MP);
                    own.gearAtt += PEquip->getModifier(xi::Mod::ATT);
                    own.gearDef += PEquip->getModifier(xi::Mod::DEF);
                    own.gearAcc += PEquip->getModifier(xi::Mod::ACC);
                    own.gearEva += PEquip->getModifier(xi::Mod::EVA);
                    own.gearRatt += PEquip->getModifier(xi::Mod::RATT);
                    own.gearRacc += PEquip->getModifier(xi::Mod::RACC);
                }
            }
            auto*      weapon    = dynamic_cast<CItemWeapon*>(PChar->getEquip(SLOT_MAIN));
            const bool handed    = weapon == nullptr || weapon->isHandToHand();
            const bool twoHanded = !handed && weapon->isTwoHanded();
            own.skill            = PChar->GetSkill(weapon != nullptr ? weapon->getSkillType() : xi::SkillType::HandToHand);
            own.strMultiplier    = settings::get<float>(handed      ? "main.HAND_TO_HAND_STR_ATTACK_MULTIPLIER"
                                                        : twoHanded ? "main.TWO_HANDED_STR_ATTACK_MULTIPLIER"
                                                                    : "main.ONE_HAND_MAIN_HAND_STR_ATTACK_MULTIPLIER");
            own.dexMultiplier    = settings::get<float>(handed      ? "main.HAND_TO_HAND_DEX_ACCURACY_MULTIPLIER"
                                                        : twoHanded ? "main.TWO_HANDED_DEX_ACCURACY_MULTIPLIER"
                                                                    : "main.ONE_HAND_MAIN_HAND_DEX_ACCURACY_MULTIPLIER");
            auto* shooter = dynamic_cast<CItemWeapon*>(PChar->getEquip(SLOT_RANGED));
            if (shooter == nullptr)
            {
                shooter = dynamic_cast<CItemWeapon*>(PChar->getEquip(SLOT_AMMO));
            }
            own.rangedSkill    = shooter != nullptr && shooter->getSkillType() != xi::SkillType::None ? PChar->GetSkill(shooter->getSkillType()) : 0;
            own.rStrMultiplier = settings::get<float>("main.RANGED_STR_ATTACK_MULTIPLIER");
            own.rAgiMultiplier = settings::get<float>("main.RANGED_AGI_ACCURACY_MULTIPLIER");
            return cardian::food::statsOf(own);
        }

        // An owned cardian's food for a role: the best her bags hold for her
        // seat (food_math.h pickOwned), by the gear scorer's weights
        auto ownedPick(CCharEntity* PChar, const Role role) -> std::optional<Pick>
        {
            std::vector<Carried> bag;
            for (const auto& [itemId, count] : carried(PChar))
            {
                if (const auto* facts = factsOf(itemId); facts != nullptr)
                {
                    bag.push_back({ itemId, count, *facts });
                }
            }
            if (bag.empty())
            {
                return std::nullopt;
            }
            const auto race = static_cast<CharRace>(PChar->look.race);
            const bool fish = race == CharRace::Mithra || PChar->getMod(xi::Mod::EAT_RAW_FISH) == 1;
            const bool meat = race == CharRace::Galka || PChar->getMod(xi::Mod::EAT_RAW_MEAT) == 1;
            const auto seat = cardian::food::seatFor(role, PChar->GetMJob());
            return cardian::food::pickOwned(bag, seat, role == Role::Healer, pawn::isMageJob(PChar->GetMJob()), ownStats(PChar), fish, meat);
        }

        // A stack of the item in her inventory she can use now
        auto inventorySlotOf(CCharEntity* PChar, const uint16 itemId) -> std::optional<uint8>
        {
            auto* inventory = PChar->getStorage(LOC_INVENTORY);
            if (inventory == nullptr)
            {
                return std::nullopt;
            }
            for (const uint8 slot : inventory->SearchItems(itemId))
            {
                if (const auto* PItem = inventory->GetItem(slot); PItem != nullptr && PItem->getQuantity() > 0 && !PItem->isBusy())
                {
                    return slot;
                }
            }
            return std::nullopt;
        }
    } // namespace

    void ensureTable()
    {
        db::preparedStmt("CREATE TABLE IF NOT EXISTS `cardian_food` ("
                         "`name` varchar(15) NOT NULL, "
                         "`role` tinyint(3) unsigned NOT NULL, "
                         "`itemid` smallint(5) unsigned NOT NULL, "
                         "`cookie` tinyint(1) unsigned NOT NULL DEFAULT '0', "
                         "PRIMARY KEY (`name`, `role`)) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_general_ci");
    }

    auto factsOf(const uint16 itemId) -> const Facts*
    {
        static auto& known = *new std::unordered_map<uint16, std::optional<Facts>>();
        if (itemId == 0)
        {
            return nullptr;
        }
        auto it = known.find(itemId);
        if (it == known.end())
        {
            std::optional<Facts> facts;
            if (const auto* PKind = xi::items::lookup(itemId); PKind != nullptr && PKind->isType(ITEM_USABLE))
            {
                std::ifstream     file(fmt::format("./scripts/items/{}.lua", PKind->getName()));
                std::stringstream text;
                text << file.rdbuf();
                facts = cardian::food::parseScript(text.str());
            }
            it = known.emplace(itemId, facts).first;
        }
        return it->second.has_value() ? &*it->second : nullptr;
    }

    auto pickFor(CCharEntity* PChar, const Role role) -> std::optional<Pick>
    {
        if (PChar == nullptr)
        {
            return std::nullopt;
        }
        if (pawn::seats::isWorlds(PChar->id))
        {
            return planOf(PChar).byRole[static_cast<std::size_t>(cardian::food::planRole(role))];
        }
        return ownedPick(PChar, role);
    }

    auto eating(const CCharEntity* PChar) -> uint16
    {
        if (PChar == nullptr || PChar->StatusEffectContainer == nullptr)
        {
            return 0;
        }
        const auto* PFood = PChar->StatusEffectContainer->GetStatusEffect(xi::StatusEffect::Food);
        return PFood != nullptr ? static_cast<uint16>(PFood->GetSourceTypeParam()) : 0;
    }

    auto plannedIds(const std::string& name) -> std::set<uint16>
    {
        std::set<uint16> out;
        const auto       rset = db::preparedStmt("SELECT itemid FROM cardian_food WHERE name = ?", name);
        while (rset && rset->next())
        {
            out.insert(rset->get<uint16>("itemid"));
        }
        return out;
    }

    void forget(const uint32 charid)
    {
        plans().erase(charid);
    }

    auto topUp(CCharEntity* PChar) -> uint32
    {
        // A bag with no room is said once, until a top-up lands again
        static auto&     full = *new std::unordered_set<uint32>();
        std::set<uint16> foods;
        for (const auto& pick : planOf(PChar).byRole)
        {
            if (pick.has_value() && pick->itemId != 0)
            {
                foods.insert(pick->itemId);
            }
        }
        if (foods.empty())
        {
            return 0;
        }
        const auto have  = carried(PChar);
        uint32     given = 0;
        for (const uint16 itemId : foods)
        {
            const auto* PKind = xi::items::lookup(itemId);
            if (PKind == nullptr)
            {
                continue;
            }
            const auto held = have.find(itemId);
            const auto more = cardian::food::topUpBy(held != have.end() ? held->second : 0, PKind->getStackSize());
            if (more == 0)
            {
                continue;
            }
            auto transaction = ItemClaimTransaction::start(PChar);
            if (!transaction)
            {
                continue;
            }
            if (const auto landed = transaction->give(LOC_INVENTORY, itemId, more, Silence::Yes); !landed.has_value() || !transaction->commit())
            {
                if (full.insert(PChar->id).second)
                {
                    ShowWarningFmt("world: {} has no room in her bag for her food ({}); not topped up", PChar->getName(), PKind->getName());
                }
                continue;
            }
            full.erase(PChar->id);
            given += more;
            ShowInfoFmt("world: {}'s food is topped up ({} x{})", PChar->getName(), PKind->getName(), more);
        }
        return given;
    }

    auto eat(CCharEntity* PChar, const uint16 itemId) -> uint16
    {
        auto slot = inventorySlotOf(PChar, itemId);
        if (!slot.has_value())
        {
            // Used from the inventory only: one fetched from her bags first
            for (const auto& bag : pawn::items::bags(PChar))
            {
                auto* storage = PChar->getStorage(bag.location);
                if (storage == nullptr)
                {
                    continue;
                }
                const auto found = storage->SearchItem(itemId);
                if (found == ERROR_SLOTID)
                {
                    continue;
                }
                if (pawn::items::moveItem(PChar, bag.location, found, LOC_INVENTORY, 1) == CL_S_OK)
                {
                    slot = inventorySlotOf(PChar, itemId);
                    break;
                }
            }
        }
        if (!slot.has_value())
        {
            return CL_S_NO_ITEM;
        }
        return pawn::items::useItem(PChar, *slot, LOC_INVENTORY, false);
    }
} // namespace pawn::food
