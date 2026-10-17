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

#include "pull.h"

#include "cardian_link_protocol.h"
#include "pawn.h"
#include "pawn_danger.h"
#include "pawn_items.h"
#include "pawn_rules.h"
#include "role_support.h"
#include "seats.h"

#include "ai/ai_container.h"
#include "common/database.h"
#include "data/enums/ecosystem.h"
#include "data/enums/species.h"
#include "common/logging.h"
#include "common/settings.h"
#include "common/timer.h"
#include "entities/char_entity.h"
#include "entities/mob_entity.h"
#include "item_container.h"
#include "items/item.h"
#include "items/item_equipment.h"
#include "items/item_weapon.h"
#include "items/transactions/item_claim.h"
#include "party.h"
#include "spell.h"
#include "utils/charutils.h"
#include "utils/itemutils.h"

#include <fmt/format.h>

#include <array>
#include <cctype>
#include <chrono>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace pawn::pull
{
    namespace
    {
        using cardian::pull::Held;
        using cardian::pull::Kit;

        constexpr auto kPlanKept = std::chrono::seconds(60); // the census's kit read at most this often per body

        // What she wore before her kit, on her character: the kit is on, and
        // the item in each slot then (0 for none)
        constexpr auto kKitWorn  = "[CARDIAN]PullKitWorn";
        constexpr auto kWasRanged = "[CARDIAN]PullWasRanged";
        constexpr auto kWasAmmo   = "[CARDIAN]PullWasAmmo";

        auto itemName(const uint16 itemId) -> std::string
        {
            const auto* PKind = xi::items::lookup(itemId);
            return PKind != nullptr ? cardian::pull::mobWords(PKind->getName()) : fmt::format("item {}", itemId);
        }

        // The census's kit for a body, and the ammunition it planned for her
        // (her own gear's and the kit's), kept a while
        struct Plan
        {
            timer::time_point readAt{};
            std::set<uint16>  kit;
            std::set<uint16>  ammo;
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
            if (!pawn::seats::isWorlds(PChar->id))
            {
                return plan;
            }
            if (const auto rset = db::preparedStmt("SELECT slot, itemid FROM cardian_pull_kit WHERE name = ?", PChar->getName()); rset)
            {
                while (rset->next())
                {
                    const auto itemId = rset->get<uint16>("itemid");
                    plan.kit.insert(itemId);
                    if (rset->get<uint8>("slot") == SLOT_AMMO)
                    {
                        plan.ammo.insert(itemId);
                    }
                }
            }
            if (const auto rset = db::preparedStmt("SELECT itemid FROM cardian_wardrobe WHERE name = ? AND slot = ?", PChar->getName(), static_cast<uint8>(SLOT_AMMO));
                rset)
            {
                while (rset->next())
                {
                    plan.ammo.insert(rset->get<uint16>("itemid"));
                }
            }
            return plan;
        }

        // Where gear is worn from: her inventory and her wardrobes
        auto gearContainers(CCharEntity* PChar) -> std::vector<uint8>
        {
            std::vector<uint8> out{ LOC_INVENTORY };
            for (const auto& bag : pawn::items::bags(PChar))
            {
                if (pawn::items::isWardrobe(bag.location))
                {
                    out.push_back(bag.location);
                }
            }
            return out;
        }

        // Her job, her level and her race allow the piece
        auto wears(const CCharEntity* PChar, const CItemEquipment* PItem) -> bool
        {
            const auto job = static_cast<uint8>(PChar->GetMJob());
            return PItem != nullptr && job > 0 && (PItem->getJobs() & (1u << (job - 1))) != 0 && PItem->getReqLvl() <= PChar->GetMLevel() &&
                   PItem->isEquippableByRace(PChar->look.race);
        }

        auto idIn(CCharEntity* PChar, const SLOTTYPE slot) -> uint16
        {
            const auto* PItem = PChar->getEquip(slot);
            return PItem != nullptr ? PItem->getID() : uint16{ 0 };
        }

        // Every ranged-slot weapon and stack of ammunition she carries where
        // gear is worn from, an item's stacks counted together
        auto gather(CCharEntity* PChar) -> std::vector<Held>
        {
            const auto&       planned  = planOf(PChar).kit;
            const CItem*      rangedOn = PChar->getEquip(SLOT_RANGED);
            const CItem*      ammoOn   = PChar->getEquip(SLOT_AMMO);
            std::vector<Held> out;
            for (const uint8 location : gearContainers(PChar))
            {
                auto* storage = PChar->getStorage(location);
                for (uint8 slot = 1; storage != nullptr && slot <= storage->GetSize(); ++slot)
                {
                    auto* PItem = dynamic_cast<CItemWeapon*>(storage->GetItem(slot));
                    if (PItem == nullptr || PItem->getQuantity() == 0 || PItem->getCharPrice() != 0)
                    {
                        continue;
                    }
                    const uint16 mask   = PItem->getEquipSlotId();
                    const bool   ammo   = (mask & (1 << SLOT_AMMO)) != 0;
                    const bool   ranged = (mask & (1 << SLOT_RANGED)) != 0;
                    if (!ammo && !ranged)
                    {
                        continue;
                    }
                    const bool worn = static_cast<const CItem*>(PItem) == rangedOn || static_cast<const CItem*>(PItem) == ammoOn;
                    auto       it   = std::ranges::find_if(out, [&](const Held& h) { return h.id == PItem->getID() && h.ammo == ammo; });
                    if (it != out.end())
                    {
                        it->count += PItem->getQuantity();
                        it->worn = it->worn || worn;
                        continue;
                    }
                    out.push_back(Held{
                        .id       = PItem->getID(),
                        .ammo     = ammo,
                        .skill    = static_cast<uint8>(PItem->getSkillType()),
                        .subskill = PItem->getSubSkillType(),
                        .level    = PItem->getReqLvl(),
                        .count    = PItem->getQuantity(),
                        .wearable = wears(PChar, PItem),
                        .worn     = worn,
                        .planned  = planned.contains(PItem->getID()),
                    });
                }
            }
            return out;
        }

        auto fallbacksFor(const CCharEntity* PChar) -> std::vector<cardian::pull::Fallback>
        {
            std::vector<cardian::pull::Fallback> out;
            for (const auto& gift : cardian::pull::kGifts)
            {
                const bool ranged = gift.ranged == 0 || wears(PChar, xi::items::lookup<CItemEquipment>(gift.ranged));
                const bool ammo   = gift.ammo == 0 || wears(PChar, xi::items::lookup<CItemEquipment>(gift.ammo));
                out.push_back({ gift.ranged, gift.ammo, ranged && ammo });
            }
            return out;
        }

        // How many of an item she carries where gear is worn from
        auto carried(CCharEntity* PChar, const uint16 itemId) -> uint32
        {
            uint32 count = 0;
            for (const uint8 location : gearContainers(PChar))
            {
                auto* storage = PChar->getStorage(location);
                for (uint8 slot = 1; storage != nullptr && slot <= storage->GetSize(); ++slot)
                {
                    if (const auto* PItem = storage->GetItem(slot); PItem != nullptr && PItem->getID() == itemId)
                    {
                        count += PItem->getQuantity();
                    }
                }
            }
            return count;
        }

        // Given into her inventory, without a word: how many landed
        auto give(CCharEntity* PChar, const uint16 itemId, const uint32 quantity) -> uint32
        {
            // a bag with no room is said once, until a gift lands again
            static auto& full = *new std::unordered_set<uint32>();
            if (quantity == 0)
            {
                return 0;
            }
            auto transaction = ItemClaimTransaction::start(PChar);
            if (!transaction)
            {
                return 0;
            }
            if (const auto landed = transaction->give(LOC_INVENTORY, itemId, quantity, Silence::Yes); !landed.has_value() || !transaction->commit())
            {
                if (full.insert(PChar->id).second)
                {
                    ShowWarningFmt("pull: {} has no room in her bag for {}; not given", PChar->getName(), itemName(itemId));
                    pawn::tactics::role::sayParty(PChar, "My bag is full, so I can't restock my ammo.");
                }
                return 0;
            }
            full.erase(PChar->id);
            return quantity;
        }

        // Ammunition to a stack once half of it has gone
        auto topUpAmmo(CCharEntity* PChar, const uint16 itemId) -> uint32
        {
            const auto* PKind = xi::items::lookup(itemId);
            if (PKind == nullptr)
            {
                return 0;
            }
            return give(PChar, itemId, cardian::pull::ammoTopUp(carried(PChar, itemId), PKind->getStackSize()));
        }

        // The map's own kit: its weapon once, its ammunition to a stack
        auto giveKit(CCharEntity* PChar, const Kit& kit) -> uint32
        {
            uint32 given = 0;
            if (kit.ranged != 0 && carried(PChar, kit.ranged) == 0)
            {
                given += give(PChar, kit.ranged, 1);
            }
            if (kit.ammo != 0)
            {
                given += topUpAmmo(PChar, kit.ammo);
            }
            return given;
        }

        // A piece of hers worn in its slot from wherever gear is worn from
        auto wear(CCharEntity* PChar, const uint16 itemId, const SLOTTYPE equipSlot) -> bool
        {
            for (const uint8 location : gearContainers(PChar))
            {
                auto* storage = PChar->getStorage(location);
                for (uint8 slot = 1; storage != nullptr && slot <= storage->GetSize(); ++slot)
                {
                    const auto* PItem = storage->GetItem(slot);
                    if (PItem != nullptr && PItem->getID() == itemId && PItem->getQuantity() > 0 && PItem->getCharPrice() == 0)
                    {
                        return pawn::items::equip(PChar, slot, equipSlot, location) == CL_S_OK;
                    }
                }
            }
            return false;
        }
    } // namespace

    void ensureTable()
    {
        db::preparedStmt("CREATE TABLE IF NOT EXISTS `cardian_pull_kit` ("
                         "`name` varchar(15) NOT NULL, "
                         "`slot` tinyint(3) unsigned NOT NULL, "
                         "`itemid` smallint(5) unsigned NOT NULL, "
                         "PRIMARY KEY (`name`, `slot`)) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_general_ci");
    }

    auto plannedIds(const std::string& name) -> std::set<uint16>
    {
        std::set<uint16> out;
        const auto       rset = db::preparedStmt("SELECT itemid FROM cardian_pull_kit WHERE name = ?", name);
        while (rset && rset->next())
        {
            out.insert(rset->get<uint16>("itemid"));
        }
        return out;
    }

    auto keptIds(const std::string& name) -> std::set<uint16>
    {
        auto out = plannedIds(name);
        for (const auto& gift : cardian::pull::kGifts)
        {
            if (gift.ranged != 0)
            {
                out.insert(gift.ranged);
            }
            if (gift.ammo != 0)
            {
                out.insert(gift.ammo);
            }
        }
        return out;
    }

    void forget(const uint32 charid)
    {
        plans().erase(charid);
    }

    auto layKit(CCharEntity* PChar, const uint8 location) -> uint32
    {
        forget(PChar->id);
        uint32 laid = 0;
        for (const uint16 itemId : plannedIds(PChar->getName()))
        {
            const auto* PKind = xi::items::lookup<CItemEquipment>(itemId);
            if (PKind == nullptr || carried(PChar, itemId) > 0)
            {
                continue;
            }
            const bool   ammo     = (PKind->getEquipSlotId() & (1 << SLOT_AMMO)) != 0;
            const uint32 quantity = ammo ? std::clamp<uint32>(PKind->getStackSize(), 1, 99) : 1;
            auto         transaction = ItemClaimTransaction::start(PChar);
            if (!transaction)
            {
                continue;
            }
            if (const auto landed = transaction->give(location, itemId, quantity, Silence::Yes); landed.has_value() && transaction->commit())
            {
                ++laid;
            }
            else
            {
                ShowWarningFmt("world: {} has no room in her bag for her pull kit's {}; left out", PChar->getName(), itemName(itemId));
            }
        }
        return laid;
    }

    namespace
    {
        auto calls() -> std::unordered_map<uint32, uint8>&
        {
            static auto& kept = *new std::unordered_map<uint32, uint8>();
            return kept;
        }
    } // namespace

    void setCall(const uint32 playerCharID, const uint8 call)
    {
        const uint8 kept = call <= cardian::chat::kCallCount ? call : uint8{ 0 };
        if (kept == 0)
        {
            calls().erase(playerCharID);
            return;
        }
        calls()[playerCharID] = kept;
    }

    auto callOf(const uint32 playerCharID) -> uint8
    {
        const auto it = calls().find(playerCharID);
        return it != calls().end() ? it->second : uint8{ 0 };
    }

    auto canShoot(CCharEntity* PChar) -> bool
    {
        // the game's own test (CRangeState::CanUseRangedAttack): a ranged
        // weapon decides by its skill, else ammunition thrown by itself
        const auto* PRanged = dynamic_cast<CItemWeapon*>(PChar->getEquip(SLOT_RANGED));
        const auto* PAmmo   = dynamic_cast<CItemWeapon*>(PChar->getEquip(SLOT_AMMO));
        if (PRanged != nullptr)
        {
            const auto skill = PRanged->getSkillType();
            if (skill == xi::SkillType::Throwing)
            {
                return true;
            }
            return (skill == xi::SkillType::Archery || skill == xi::SkillType::Marksmanship) && PAmmo != nullptr && PAmmo->getSkillType() == skill &&
                   PAmmo->getQuantity() > 0;
        }
        return PAmmo != nullptr && PAmmo->isThrowing() && PAmmo->getQuantity() > 0;
    }

    auto wearsKit(CCharEntity* PChar) -> bool
    {
        return PChar->getCharVar(kKitWorn) != 0;
    }

    void forgetWorn(CCharEntity* PChar)
    {
        if (!wearsKit(PChar))
        {
            return;
        }
        PChar->setCharVar(kKitWorn, 0);
        PChar->setCharVar(kWasRanged, 0);
        PChar->setCharVar(kWasAmmo, 0);
    }

    auto wearKit(CCharEntity* PChar, const bool rangedToo) -> Worn
    {
        // A Bard pulls with a song: her instrument stays where it is
        if (PChar->GetMJob() == xi::Job::BRD)
        {
            // a kit put on her as another job, or before Bards sang, comes off
            if (rangedToo && wearsKit(PChar))
            {
                if (auto said = putBack(PChar); !said.empty())
                {
                    return { pullSong(PChar) != 0, true, said };
                }
            }
            const auto song = pullSong(PChar);
            if (song == 0)
            {
                return { false, false, "has no song to pull with yet" };
            }
            auto* PSpell = spell::GetSpell(static_cast<SpellID>(song));
            return { true, false, PSpell != nullptr ? fmt::format("pulls with {}", cardian::pull::mobWords(PSpell->getName())) : std::string() };
        }
        const auto held = gather(PChar);
        const auto kit  = cardian::pull::chooseKit(held, fallbacksFor(PChar));
        if (!kit.has_value())
        {
            return { false, false, "has nothing her job can throw or shoot to pull with" };
        }
        if (kit->given)
        {
            giveKit(PChar, *kit);
        }
        const uint16 rangedNow  = idIn(PChar, SLOT_RANGED);
        const uint16 ammoNow    = idIn(PChar, SLOT_AMMO);
        const bool   needRanged = kit->ranged != 0 ? rangedNow != kit->ranged : rangedNow != 0;
        const bool   needAmmo   = kit->ammo != 0 && ammoNow != kit->ammo;
        if ((!needRanged && !needAmmo) || (needRanged && !rangedToo))
        {
            return { canShoot(PChar), false, "" };
        }
        // Her own launcher kept and only its ammunition put on is a refill,
        // not a kit: nothing of it is put back as she leaves the seat (the
        // review, 2026-10-17: a Ranger's spent arrows refilled were taken
        // off again, her bow left with none)
        const bool refill = !needRanged && kit->ranged != 0 && rangedNow == kit->ranged;
        if (!refill && !wearsKit(PChar))
        {
            PChar->setCharVar(kKitWorn, 1);
            PChar->setCharVar(kWasRanged, rangedNow);
            PChar->setCharVar(kWasAmmo, ammoNow);
        }
        bool ok = true;
        if (needRanged)
        {
            ok = kit->ranged != 0 ? wear(PChar, kit->ranged, SLOT_RANGED) : pawn::items::unequip(PChar, SLOT_RANGED) == CL_S_OK;
        }
        if (needAmmo)
        {
            ok = wear(PChar, kit->ammo, SLOT_AMMO) && ok;
        }
        charutils::SaveCharEquip(PChar);
        const auto what = kit->ranged != 0 && kit->ammo != 0 ? fmt::format("{} and {}", itemName(kit->ranged), itemName(kit->ammo))
                          : kit->ranged != 0                 ? itemName(kit->ranged)
                                                             : itemName(kit->ammo);
        return { canShoot(PChar), true,
                 fmt::format("{}: {}{}{}", refill ? "refills her ammunition" : "puts on her pulling kit", refill ? itemName(kit->ammo) : what, kit->given ? " (the map's own)" : "",
                             ok ? "" : "; the game refused part of it") };
    }

    auto putBack(CCharEntity* PChar) -> std::string
    {
        if (!wearsKit(PChar))
        {
            return {};
        }
        const auto   wasRanged = static_cast<uint16>(PChar->getCharVar(kWasRanged));
        const auto   wasAmmo   = static_cast<uint16>(PChar->getCharVar(kWasAmmo));
        const uint16 rangedNow = idIn(PChar, SLOT_RANGED);
        const uint16 ammoNow   = idIn(PChar, SLOT_AMMO);
        forgetWorn(PChar);
        // Only a slot that differs is touched: taking the ranged slot off
        // costs her TP, and a launcher changed takes its ammunition with it
        const bool rangedChanged = rangedNow != wasRanged;
        const bool ammoChanged   = ammoNow != wasAmmo || rangedChanged;
        if (!ammoChanged)
        {
            return {};
        }
        pawn::items::unequip(PChar, SLOT_AMMO);
        if (rangedChanged)
        {
            pawn::items::unequip(PChar, SLOT_RANGED);
            if (wasRanged != 0)
            {
                wear(PChar, wasRanged, SLOT_RANGED);
            }
        }
        if (wasAmmo != 0)
        {
            wear(PChar, wasAmmo, SLOT_AMMO);
        }
        charutils::SaveCharEquip(PChar);
        const auto name = [](const uint16 id) { return id != 0 ? itemName(id) : std::string("nothing"); };
        return fmt::format("puts back what she wore before her pulling kit: {} and {}", name(wasRanged), name(wasAmmo));
    }

    auto topUp(CCharEntity* PChar, const bool puller, const bool world) -> uint32
    {
        uint32 given = 0;
        // a Bard pulls with a song: no kit to top up
        if (puller && PChar->GetMJob() != xi::Job::BRD)
        {
            // the map's own, given or already in her bag, never runs out
            const auto gift = [](const uint16 id)
            {
                return id != 0 && std::ranges::any_of(cardian::pull::kGifts, [id](const auto& g) { return g.ranged == id || g.ammo == id; });
            };
            if (const auto kit = cardian::pull::chooseKit(gather(PChar), fallbacksFor(PChar)); kit.has_value() && (kit->given || gift(kit->ammo)))
            {
                given += giveKit(PChar, *kit);
            }
        }
        if (world)
        {
            for (const uint16 ammo : planOf(PChar).ammo)
            {
                given += topUpAmmo(PChar, ammo);
            }
        }
        if (given > 0)
        {
            ShowInfoFmt("pull: {}'s ammunition is topped up ({} given)", PChar->getName(), given);
        }
        return given;
    }

    auto pullSong(CCharEntity* PChar) -> uint16
    {
        if (PChar == nullptr || PChar->GetMJob() != xi::Job::BRD)
        {
            return 0;
        }
        return cardian::pull::pullSong([&](const uint16 id)
                                       { return charutils::hasSpell(PChar, id) != 0 && spell::CanUseSpell(PChar, static_cast<SpellID>(id)); });
    }

    auto toolOf(CCharEntity* PChar) -> Tool
    {
        if (const auto song = pullSong(PChar); song != 0)
        {
            return { 0, song };
        }
        if (const auto ranged = idIn(PChar, SLOT_RANGED); ranged != 0)
        {
            return { ranged, 0 };
        }
        return { idIn(PChar, SLOT_AMMO), 0 };
    }

    auto kindOf(CMobEntity* PMob) -> std::string
    {
        if (PMob->m_EcoSystem != xi::Ecosystem::Beastmen)
        {
            return std::string(PMob->getName());
        }
        // The race as players call them together, the species' variants with
        // it (an armored_goblin is a Goblin); else the species' own name
        static constexpr std::array<std::pair<std::string_view, std::string_view>, 9> races{ {
            { "goblin", "Goblins" },  { "orc", "Orcs" },             { "quadav", "Quadav" }, { "yagudo", "Yagudo" },       { "sahagin", "Sahagin" },
            { "troll", "Trolls" },    { "tonberry", "Tonberries" }, { "antica", "Antica" }, { "mamool_ja", "Mamool_Ja" },
        } };
        const auto species = xi::data::EnumTraits<xi::Species>::toName(static_cast<xi::Species>(PMob->m_Species));
        // the race's word whole within the species' name, between underscores
        const auto hasStem = [&species](const std::string_view stem)
        {
            const std::string padded = "_" + std::string(species) + "_";
            return padded.find("_" + std::string(stem) + "_") != std::string::npos;
        };
        for (const auto& [stem, race] : races)
        {
            if (hasStem(stem))
            {
                return std::string(race);
            }
        }
        if (species.empty())
        {
            return std::string(PMob->getName());
        }
        std::string out(species);
        out[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(out[0])));
        return out;
    }

    auto wayBackKin(CMobEntity* PMob, const position_t& from, const position_t& to) -> CMobEntity*
    {
        // The game's rule: an engaged mob calls the idle members of its
        // spawn group within their link radius of it, as it goes -- those of
        // its own family only when it links with kin -- unless it links one
        // way only
        if (PMob == nullptr || PMob->PParty == nullptr || PMob->getMobMod(xi::MobMod::OneWayLinking))
        {
            return nullptr;
        }
        const float pad = settings::get<float>("pawn.AVOID_TAIL") + cardian::rules::kClearance;
        for (auto* PMember : PMob->PParty->members)
        {
            if (PMember == PMob || PMember->objtype != TYPE_MOB)
            {
                continue;
            }
            auto* PKin = static_cast<CMobEntity*>(PMember);
            if (PKin->PMaster != nullptr || !PKin->isAlive() || PKin->PAI->IsEngaged() || PKin->getMobMod(xi::MobMod::NoLink) > 0)
            {
                continue;
            }
            if (PKin->m_Family == PMob->m_Family && !PMob->m_Link && !PMob->ShouldForceLink())
            {
                continue;
            }
            if (cardian::pull::segmentDistance(from, to, PKin->loc.p) <= static_cast<float>(PKin->getMobMod(xi::MobMod::LinkRadius)) + pad)
            {
                return PKin;
            }
        }
        return nullptr;
    }

    auto conLevel(const CCharEntity* PPlayer) -> uint8
    {
        std::vector<uint8> levels;
        if (PPlayer->PParty != nullptr)
        {
            for (const auto* PMember : PPlayer->PParty->members)
            {
                if (PMember != nullptr && PMember->loc.zone == PPlayer->loc.zone)
                {
                    levels.push_back(PMember->GetMLevel());
                }
            }
        }
        return cardian::pull::conLevel(settings::get<bool>("cardian.EXP_PARTY_AVERAGE"), PPlayer->GetMLevel(), levels);
    }

    namespace
    {
        // A mob a party could set out to fight, engaged or not: no
        // notorious, event, fished or battlefield mob, nobody's pet, nothing
        // underground, and none but the world's own mobs
        auto listable(CMobEntity* PMob) -> bool
        {
            const bool special = (PMob->m_Type & xi::MobType::Event) != xi::MobType::Normal ||
                                 (PMob->m_Type & xi::MobType::Fished) != xi::MobType::Normal ||
                                 (PMob->m_Type & xi::MobType::Battlefield) != xi::MobType::Normal ||
                                 (PMob->m_Type & xi::MobType::Notorious) != xi::MobType::Normal;
            return !special && PMob->PMaster == nullptr && PMob->isAlive() && PMob->allegiance == xi::Allegiance::Mob && !pawn::isUnderground(PMob);
        }
    } // namespace

    auto kindsAround(CCharEntity* PPlayer) -> Kinds
    {
        Kinds out;
        out.level        = conLevel(PPlayer);
        const auto stake = pawn::stakeOf(PPlayer->id);
        out.staked       = stake.has_value() && stake->zone == PPlayer->getZone();
        const auto at    = out.staked ? stake->at : PPlayer->loc.p;

        std::vector<cardian::pull::Seen> seen;
        pawn::forEachMobNear(pawn::entitiesAround(PPlayer), at, cardian::pull::kPullReach,
                             [&](CMobEntity* PMob)
                             {
                                 if (!listable(PMob) || distance(at, PMob->loc.p) > cardian::pull::kPullReach)
                                 {
                                     return;
                                 }
                                 // a name the Link cannot carry whole is no row
                                 if (auto kind = kindOf(PMob); kind.size() < sizeof(cl_pull_kind::name))
                                 {
                                     seen.push_back({ std::move(kind), static_cast<uint8_t>(charutils::CheckMob(out.level, PMob)), distance(PPlayer->loc.p, PMob->loc.p) });
                                 }
                             });
        const auto list = pawn::pullListOf(PPlayer->id);
        out.rows        = cardian::pull::kindsOf(seen, list);
        return out;
    }
} // namespace pawn::pull
