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

#include "redress.h"
#include "redress_math.h"

#include "auction.h"
#include "pawn.h"
#include "pawn_controller.h"
#include "pawn_items.h"
#include "seats.h"

#include "ai/ai_container.h"
#include "common/database.h"
#include "common/logging.h"
#include "common/timer.h"
#include "entities/char_entity.h"
#include "enums/chat_message_type.h"
#include "enums/item_state.h"
#include "item_container.h"
#include "items/item.h"
#include "items/transactions/item_claim.h"
#include "job_points.h"
#include "latent_effect_container.h"
#include "packets/c2s/0x100_myroom_job.h"
#include "packets/s2c/0x017_chat_std.h"
#include "party.h"
#include "utils/charutils.h"
#include "utils/itemutils.h"
#include "utils/petutils.h"
#include "utils/puppetutils.h"
#include "zone.h"

#include <magic_enum/magic_enum.hpp>

#include <algorithm>
#include <chrono>
#include <optional>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

namespace pawn::redress
{
    namespace
    {
        using cardian::redress::Record;
        using cardian::redress::State;

        constexpr auto kLookEvery  = std::chrono::seconds(3); // a zone's counters
        constexpr auto kApplyEvery = std::chrono::seconds(3); // the census's answers
        constexpr uint32 kAmmoStack = 99;                     // ammunition is issued by the stack, as the census's finish issues it

        std::unordered_map<uint16, timer::time_point> lookedAt; // by zone
        timer::time_point                             appliedAt{};

        // Her level: her main job's own, whatever a level sync holds her to
        auto levelOf(const CCharEntity* PChar) -> uint8
        {
            return PChar->jobs.job[static_cast<uint8>(PChar->GetMJob())];
        }

        // Her row, if she has one; `read` false when the table could not be read
        struct Lookup
        {
            bool                  read = false;
            std::optional<Record> record;
        };

        auto recordOf(const uint32 charid) -> Lookup
        {
            const auto rset = db::preparedStmt("SELECT level, state FROM cardian_redress WHERE charid = ?", charid);
            if (!rset)
            {
                return {};
            }
            if (!rset->next())
            {
                return { .read = true };
            }
            const auto state = cardian::redress::stateOf(rset->get<std::string>("state"));
            return { .read = true, .record = Record{ rset->get<uint8>("level"), state.value_or(State::Done) } };
        }

        void ask(const CCharEntity* PPawn, const uint8 level)
        {
            if (!db::preparedStmt("INSERT INTO cardian_redress (charid, level, state, skills, issued, sub, sublevel, asked_at) VALUES (?, ?, 'asked', '', '', 0, 0, NOW()) "
                                  "ON DUPLICATE KEY UPDATE level = VALUES(level), state = 'asked', skills = '', issued = '', sub = 0, sublevel = 0, asked_at = NOW(), "
                                  "ready_at = NULL",
                                  PPawn->id, level))
            {
                ShowErrorFmt("world: {} could not ask the census to dress her for level {}", PPawn->getName(), level);
                return;
            }
            ShowInfoFmt("world: {} asks the census to dress her for level {} (by an auction counter)", PPawn->getName(), level);
        }

        // Every real player of the zone who stands by a counter: each of the
        // world's adventurers in his party who stands by it too, risen past
        // the level she was last dressed for, asks the census
        void lookAtCounters(CZone* PZone)
        {
            PZone->ForEachChar([](CCharEntity* PChar)
            {
                if (PChar == nullptr || pawn::isPawn(PChar) || PChar->PParty == nullptr)
                {
                    return;
                }
                const auto* PCounter = pawn::auction::counterNear(PChar);
                if (PCounter == nullptr)
                {
                    return;
                }
                for (auto* PMember : PChar->PParty->members)
                {
                    auto* PPawn = dynamic_cast<CCharEntity*>(PMember);
                    if (PPawn == nullptr || PPawn == PChar || !pawn::isPawn(PPawn) || !pawn::seats::isWorlds(PPawn->id) ||
                        pawn::auction::whereShopping(PChar, PPawn, PCounter) != CL_S_OK)
                    {
                        continue;
                    }
                    const uint8 level  = levelOf(PPawn);
                    const auto  looked = recordOf(PPawn->id);
                    if (looked.read && cardian::redress::shouldAsk(level, looked.record))
                    {
                        ask(PPawn, level);
                    }
                }
            });
        }

        // The census's answer for one of them: the level, her skill values,
        // the pieces it had issued her before this plan, and the support job
        // the convention gives that level (job 0 for none)
        struct Answer
        {
            uint32      charid = 0;
            uint8       level  = 0;
            std::string skills;
            std::string issued;
            uint8       sub      = 0;
            uint8       sublevel = 0;
        };

        auto readyAnswers() -> std::vector<Answer>
        {
            std::vector<Answer> out;
            const auto          rset = db::preparedStmt("SELECT charid, level, skills, issued, sub, sublevel FROM cardian_redress WHERE state = 'ready'");
            if (!rset)
            {
                return out;
            }
            while (rset->next())
            {
                out.push_back({ rset->get<uint32>("charid"), rset->get<uint8>("level"), rset->get<std::string>("skills"), rset->get<std::string>("issued"),
                                rset->get<uint8>("sub"), rset->get<uint8>("sublevel") });
            }
            return out;
        }

        // She takes it now: up, standing in her zone, in no event, and out of
        // a fight -- not engaged, nor attending the party's fight from its
        // edge -- the refusals of her player's Mog House (mog_house.cpp
        // changeJobs), since a re-dress may change her support job
        auto canDress(const CCharEntity* PPawn) -> bool
        {
            if (PPawn->loc.zone == nullptr || PPawn->isDead() || PPawn->isInEvent() || PPawn->status != xi::Status::Normal || PPawn->PAI == nullptr ||
                PPawn->PAI->IsEngaged())
            {
                return false;
            }
            const auto* PController = dynamic_cast<CPawnController*>(PPawn->PAI->GetController());
            return PController == nullptr || PController->PartyFightTarget() == nullptr;
        }

        struct Piece
        {
            uint8  slot   = 0;
            uint16 itemId = 0;
        };

        auto planOf(const std::string& name) -> std::optional<std::vector<Piece>>
        {
            const auto rset = db::preparedStmt("SELECT slot, itemid FROM cardian_wardrobe WHERE name = ? ORDER BY slot", name);
            if (!rset)
            {
                return std::nullopt;
            }
            std::vector<Piece> plan;
            while (rset->next())
            {
                plan.push_back({ rset->get<uint8>("slot"), rset->get<uint16>("itemid") });
            }
            return plan;
        }

        struct Dressed
        {
            uint32 worn    = 0; // pieces put on
            uint32 dropped = 0; // census pieces taken out of her bag
        };

        // Her wardrobe as the census planned it: each piece she is not
        // wearing found in her bag, or given her, and worn. A piece that does
        // not fit in her bag, or that she cannot wear, is left out and said;
        // a piece that could not go on in the first pass (one under a cover
        // still worn) is tried once more after the rest. The pieces the
        // census had issued her that the plan has no place for leave her bag
        // first, to make room for the new, and again at the end, for the
        // ones the new pieces took off her
        auto dress(CCharEntity* PPawn, const std::vector<Piece>& plan, const std::set<uint16>& issued) -> Dressed
        {
            Dressed out;
            auto*   bag = PPawn->getStorage(LOC_INVENTORY);
            if (bag == nullptr)
            {
                return out;
            }
            std::set<uint16> wanted;
            for (const auto& piece : plan)
            {
                wanted.insert(piece.itemId);
            }

            const auto dropUnplanned = [&]
            {
                for (uint8 slot = 1; slot <= bag->GetSize(); ++slot)
                {
                    const CItem* PItem = bag->GetItem(slot);
                    if (PItem == nullptr || PItem->getQuantity() == 0 ||
                        !cardian::redress::dropsPiece(PItem->getID(), PItem->state() == ItemState::Equipped, wanted, issued))
                    {
                        continue;
                    }
                    if (pawn::items::dropItem(PPawn, slot, PItem->getQuantity(), LOC_INVENTORY) == CL_S_OK)
                    {
                        ++out.dropped;
                    }
                }
            };
            dropUnplanned();

            // A copy in her bag she is not wearing, else one given her
            const auto bagSlotFor = [&](const Piece& piece) -> std::optional<uint8>
            {
                for (const uint8 slot : bag->SearchItems(piece.itemId))
                {
                    const CItem* PItem = bag->GetItem(slot);
                    if (PItem != nullptr && PItem->state() != ItemState::Equipped)
                    {
                        return slot;
                    }
                }
                uint32 quantity = 1;
                if (piece.slot == SLOT_AMMO)
                {
                    const auto* PKind = xi::items::lookup(piece.itemId);
                    quantity          = std::clamp<uint32>(PKind != nullptr ? PKind->getStackSize() : 1, 1, kAmmoStack);
                }
                auto transaction = ItemClaimTransaction::start(PPawn);
                if (!transaction)
                {
                    return std::nullopt;
                }
                const auto landed = transaction->give(LOC_INVENTORY, piece.itemId, quantity, Silence::Yes);
                if (!landed.has_value() || !transaction->commit())
                {
                    return std::nullopt;
                }
                return *landed;
            };

            const auto wearing = [&](const Piece& piece)
            {
                const CItem* PWorn = PPawn->getEquip(static_cast<SLOTTYPE>(piece.slot));
                return PWorn != nullptr && PWorn->getID() == piece.itemId;
            };

            std::vector<Piece> again;
            const auto         putOn = [&](const Piece& piece, const bool last)
            {
                if (wearing(piece))
                {
                    return;
                }
                const auto slot = bagSlotFor(piece);
                if (!slot.has_value())
                {
                    ShowWarningFmt("world: {} has no room in her bag for item {} (slot {}); left out", PPawn->getName(), piece.itemId, piece.slot);
                    return;
                }
                if (const auto status = pawn::items::equip(PPawn, *slot, piece.slot, LOC_INVENTORY); status != CL_S_OK)
                {
                    if (!last)
                    {
                        again.push_back(piece);
                        return;
                    }
                    ShowWarningFmt("world: {} cannot wear item {} in slot {} (outcome 0x{:04X}); left out", PPawn->getName(), piece.itemId, piece.slot, status);
                    return;
                }
                ++out.worn;
            };
            for (const auto& piece : plan)
            {
                putOn(piece, false);
            }
            const auto retries = std::move(again);
            again.clear();
            for (const auto& piece : retries)
            {
                putOn(piece, true);
            }
            dropUnplanned();

            if (out.worn > 0)
            {
                charutils::SaveCharEquip(PPawn);
            }
            return out;
        }

        // Her spellbook as the census planned it: each spell she lacks, learned
        auto learnSpells(CCharEntity* PPawn) -> uint32
        {
            uint32     learned = 0;
            const auto rset    = db::preparedStmt("SELECT spellid FROM cardian_spells WHERE name = ?", PPawn->getName());
            if (!rset)
            {
                return learned;
            }
            while (rset->next())
            {
                const auto spellId = rset->get<uint16>("spellid");
                if (charutils::addSpell(PPawn, spellId) != 0)
                {
                    charutils::SaveSpell(PPawn, spellId);
                    ++learned;
                }
            }
            return learned;
        }

        // Her skills raised to the census's values for her level, never
        // lowered, and the weapon skills they open hers: the whole list built
        // again, since a raise can pass more than one at once
        auto raiseSkills(CCharEntity* PPawn, const std::string& text) -> uint32
        {
            const auto wanted = cardian::redress::parseSkills(text);
            const auto raises = cardian::redress::raises(wanted, [&](const uint8 skill) -> uint16
            {
                return skill < MAX_SKILLTYPE ? PPawn->RealSkills.skill[skill] : UINT16_MAX;
            });
            for (const auto& raise : raises)
            {
                PPawn->RealSkills.skill[raise.skill] = raise.value;
                charutils::SaveCharSkills(PPawn, raise.skill);
            }
            if (!raises.empty())
            {
                charutils::BuildingCharSkillsTable(PPawn);
                charutils::BuildingCharWeaponSkills(PPawn);
            }
            return static_cast<uint32>(raises.size());
        }

        // Her support job as the census planned it for the level, before her
        // gear: the support job and that job unlocked, the job raised to the
        // plan's level. Another support job than hers is set by the game's own
        // job change, as her player's Mog House does it (mog_house.cpp
        // changeJobs: her pet sent away, a waiting order and an enchanted
        // piece on its way let go), which rebuilds her stats, abilities and
        // traits and takes a second weapon off her -- so the plan's gear goes
        // on after it, its second blade wanting the Dual Wield a Ninja support
        // job brings. The same support job at a higher level takes only what
        // the game's own level change does (charutils, a ding): her buffs, her
        // recasts, her orders and her gear stay. Whether it changed anything
        auto takeSub(CCharEntity* PPawn, const Answer& answer) -> bool
        {
            const uint8 planned = answer.sub < MAX_JOBTYPE ? answer.sub : 0;
            const auto  change  = cardian::redress::subChange({ planned, answer.sublevel }, { static_cast<uint8>(PPawn->GetMJob()), static_cast<uint8>(PPawn->GetSJob()),
                                                                                              PPawn->jobs.job[planned], PPawn->jobs.unlocked });
            if (!change.has_value())
            {
                return false;
            }
            const auto before = fmt::format("{} {}", magic_enum::enum_name(PPawn->GetSJob()), PPawn->GetSLevel());
            PPawn->jobs.unlocked     = change->unlocked;
            PPawn->jobs.job[planned] = change->jobLevel;
            charutils::SaveCharJob(PPawn, static_cast<xi::Job>(planned));

            if (change->switchJob)
            {
                if (PPawn->PPet != nullptr)
                {
                    petutils::DespawnPet(PPawn);
                }
                if (auto* PController = dynamic_cast<CPawnController*>(PPawn->PAI->GetController()); PController != nullptr)
                {
                    PController->EndEnchant("her support job changed");
                    PController->ClearQueuedOrders("her support job changed");
                }
                GP_CLI_COMMAND_MYROOM_JOB jobChange{};
                jobChange.MainJobIndex    = 0;
                jobChange.SupportJobIndex = planned;
                jobChange.process(nullptr, PPawn);
            }
            else
            {
                const uint8 slvlBefore = PPawn->GetSLevel();
                PPawn->SetSLevel(PPawn->jobs.job[planned]);
                if (PPawn->GetSLevel() != slvlBefore)
                {
                    jobpointutils::RefreshGiftMods(PPawn);
                    charutils::BuildingCharSkillsTable(PPawn);
                    charutils::CalculateStats(PPawn);
                    charutils::BuildingCharAbilityTable(PPawn);
                    charutils::BuildingCharTraitsTable(PPawn);
                    charutils::BuildingCharWeaponSkills(PPawn);
                    puppetutils::LoadAutomaton(PPawn);
                    PPawn->PLatentEffectContainer->CheckLatentsJobLevel();
                    if (PPawn->PParty != nullptr)
                    {
                        PPawn->PParty->ReloadParty();
                    }
                }
            }

            ShowInfoFmt("world: {} takes the support job of level {}: {} -> {} {}", PPawn->getName(), answer.level, before, magic_enum::enum_name(PPawn->GetSJob()),
                        PPawn->GetSLevel());
            return true;
        }

        void markDone(const Answer& answer)
        {
            db::preparedStmt("UPDATE cardian_redress SET state = 'done', done_at = NOW() WHERE charid = ? AND state = 'ready' AND level = ?", answer.charid,
                             answer.level);
        }

        // The census's answer put on her: her support job, her gear, her
        // spells, her skills, her health recomputed for them; the player she
        // is with told
        void apply(CCharEntity* PPawn, const Answer& answer)
        {
            const auto plan = planOf(PPawn->getName());
            if (!plan.has_value())
            {
                ShowErrorFmt("world: {}'s wardrobe could not be read; her re-dress waits", PPawn->getName());
                return;
            }
            const bool subbed  = takeSub(PPawn, answer);
            const auto dressed = dress(PPawn, *plan, cardian::redress::parseIds(answer.issued));
            const auto learned = learnSpells(PPawn);
            const auto raised  = raiseSkills(PPawn, answer.skills);
            PPawn->UpdateHealth();
            PPawn->clearPacketList();
            markDone(answer);

            ShowInfoFmt("world: {} is dressed for level {} at the auction house ({}{} pieces put on, {} taken off, {} spells learned, {} skills raised)",
                        PPawn->getName(), answer.level, subbed ? "her support job set, " : "", dressed.worn, dressed.dropped, learned, raised);
            if (auto* PPlayer = pawn::partyPlayer(PPawn); PPlayer != nullptr)
            {
                PPlayer->pushPacket<GP_SERV_COMMAND_CHAT_STD>(PPlayer, MESSAGE_SYSTEM_3, fmt::format("{} is dressed for level {}.", PPawn->getName(), answer.level));
            }
        }

        // Every answer the census has given, put on whoever it is for that
        // stands ready for it; the rest wait for her next stand. One the
        // player has recruited since she asked is his now, and her answer is
        // set aside unworn
        void applyReady()
        {
            for (const auto& answer : readyAnswers())
            {
                auto* PPawn = pawn::findPawn(answer.charid);
                if (PPawn == nullptr || !canDress(PPawn))
                {
                    continue;
                }
                if (!pawn::seats::isWorlds(PPawn->id))
                {
                    ShowInfoFmt("world: {} is no longer one of the world's; the census's plan for level {} is set aside", PPawn->getName(), answer.level);
                    markDone(answer);
                    continue;
                }
                if (!cardian::redress::answerFits(answer.level, levelOf(PPawn)))
                {
                    ShowInfoFmt("world: {} is level {} now; the census's plan for level {} is set aside, and she asks again at a counter", PPawn->getName(),
                                levelOf(PPawn), answer.level);
                    markDone(answer);
                    continue;
                }
                apply(PPawn, answer);
            }
        }
    } // namespace

    void ensureTable()
    {
        db::preparedStmt("CREATE TABLE IF NOT EXISTS `cardian_redress` ("
                         "`charid` int(10) unsigned NOT NULL, "
                         "`level` tinyint(3) unsigned NOT NULL, "
                         "`state` enum('asked','ready','done') NOT NULL DEFAULT 'asked', "
                         "`skills` varchar(1024) NOT NULL DEFAULT '', "
                         "`issued` varchar(512) NOT NULL DEFAULT '', "
                         "`sub` tinyint(3) unsigned NOT NULL DEFAULT '0', "
                         "`sublevel` tinyint(3) unsigned NOT NULL DEFAULT '0', "
                         "`asked_at` datetime DEFAULT NULL, "
                         "`ready_at` datetime DEFAULT NULL, "
                         "`done_at` datetime DEFAULT NULL, "
                         "PRIMARY KEY (`charid`), KEY `state` (`state`)) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_general_ci");
        // The support job's two columns, on a table made before the answer carried them
        db::preparedStmt("ALTER TABLE `cardian_redress` "
                         "ADD COLUMN IF NOT EXISTS `sub` tinyint(3) unsigned NOT NULL DEFAULT '0' AFTER `issued`, "
                         "ADD COLUMN IF NOT EXISTS `sublevel` tinyint(3) unsigned NOT NULL DEFAULT '0' AFTER `sub`");
    }

    void tick(CZone* PZone)
    {
        const auto now = timer::now();
        if (PZone != nullptr)
        {
            auto& looked = lookedAt[static_cast<uint16>(PZone->GetID())];
            if (now - looked >= kLookEvery)
            {
                looked = now;
                lookAtCounters(PZone);
            }
        }
        if (now - appliedAt >= kApplyEvery)
        {
            appliedAt = now;
            applyReady();
        }
    }
} // namespace pawn::redress
