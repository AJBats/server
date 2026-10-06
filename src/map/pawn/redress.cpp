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
#include "food.h"
#include "pawn.h"
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
#include "packets/s2c/0x017_chat_std.h"
#include "party.h"
#include "utils/charutils.h"
#include "utils/itemutils.h"
#include "zone.h"

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
            if (!db::preparedStmt("INSERT INTO cardian_redress (charid, level, state, skills, issued, asked_at) VALUES (?, ?, 'asked', '', '', NOW()) "
                                  "ON DUPLICATE KEY UPDATE level = VALUES(level), state = 'asked', skills = '', issued = '', asked_at = NOW(), ready_at = NULL",
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
        // and the pieces it had issued her before this plan
        struct Answer
        {
            uint32      charid = 0;
            uint8       level  = 0;
            std::string skills;
            std::string issued;
        };

        auto readyAnswers() -> std::vector<Answer>
        {
            std::vector<Answer> out;
            const auto          rset = db::preparedStmt("SELECT charid, level, skills, issued FROM cardian_redress WHERE state = 'ready'");
            if (!rset)
            {
                return out;
            }
            while (rset->next())
            {
                out.push_back({ rset->get<uint32>("charid"), rset->get<uint8>("level"), rset->get<std::string>("skills"), rset->get<std::string>("issued") });
            }
            return out;
        }

        // She takes it now: up, out of a fight, standing in her zone
        auto canDress(const CCharEntity* PPawn) -> bool
        {
            return PPawn->loc.zone != nullptr && !PPawn->isDead() && PPawn->status == xi::Status::Normal && PPawn->PAI != nullptr &&
                   !PPawn->PAI->IsEngaged();
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
        // still worn) is tried once more after the rest. The pieces and the
        // food the census had issued her that the plan has no place for
        // leave her bag first, to make room for the new, and again at the
        // end, for the ones the new pieces took off her; the food the plan
        // keeps stays
        auto dress(CCharEntity* PPawn, const std::vector<Piece>& plan, const std::set<uint16>& issued, const std::set<uint16>& food) -> Dressed
        {
            Dressed out;
            auto*   bag = PPawn->getStorage(LOC_INVENTORY);
            if (bag == nullptr)
            {
                return out;
            }
            std::set<uint16> wanted = food;
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

        void markDone(const Answer& answer)
        {
            db::preparedStmt("UPDATE cardian_redress SET state = 'done', done_at = NOW() WHERE charid = ? AND state = 'ready' AND level = ?", answer.charid,
                             answer.level);
        }

        // The census's answer put on her: her gear, her food laid in her bag,
        // her spells, her skills, her health recomputed for them; the player
        // she is with told
        void apply(CCharEntity* PPawn, const Answer& answer)
        {
            const auto plan = planOf(PPawn->getName());
            if (!plan.has_value())
            {
                ShowErrorFmt("world: {}'s wardrobe could not be read; her re-dress waits", PPawn->getName());
                return;
            }
            pawn::food::forget(PPawn->id);
            const auto dressed = dress(PPawn, *plan, cardian::redress::parseIds(answer.issued), pawn::food::plannedIds(PPawn->getName()));
            pawn::food::topUp(PPawn);
            const auto learned = learnSpells(PPawn);
            const auto raised  = raiseSkills(PPawn, answer.skills);
            PPawn->UpdateHealth();
            PPawn->clearPacketList();
            markDone(answer);

            ShowInfoFmt("world: {} is dressed for level {} at the auction house ({} pieces put on, {} taken off, {} spells learned, {} skills raised)",
                        PPawn->getName(), answer.level, dressed.worn, dressed.dropped, learned, raised);
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
                         "`asked_at` datetime DEFAULT NULL, "
                         "`ready_at` datetime DEFAULT NULL, "
                         "`done_at` datetime DEFAULT NULL, "
                         "PRIMARY KEY (`charid`), KEY `state` (`state`)) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_general_ci");
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
