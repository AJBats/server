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
#include "pawn_controller.h"
#include "pawn_items.h"
#include "seats.h"
#include "world.h"

#include "ai/ai_container.h"
#include "common/database.h"
#include "common/logging.h"
#include "common/timer.h"
#include "entities/char_entity.h"
#include "enums/chat_message_type.h"
#include "enums/item_state.h"
#include "item_container.h"
#include "items/item.h"
#include "items/item_equipment.h"
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
#include <array>
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
        using cardian::redress::DingNow;
        using cardian::redress::DingPlan;
        using cardian::redress::DingState;
        using cardian::redress::DingStep;
        using cardian::redress::Record;
        using cardian::redress::State;

        constexpr auto kLookEvery  = std::chrono::seconds(3); // a zone's counters
        constexpr auto kApplyEvery = std::chrono::seconds(3); // the census's answers, and its dings
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
        // the convention gives that level (job 0 for none) with the job's own
        // level as she levelled it, ahead of the half of her main the game
        // shows (census.py sub_trained)
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
        // changeJobs), since a re-dress may change her support job; and with
        // no enchanted piece on her lane (StartEnchant), which puts back what
        // that piece replaced
        auto canDress(const CCharEntity* PPawn) -> bool
        {
            if (PPawn->loc.zone == nullptr || PPawn->isDead() || PPawn->isInEvent() || PPawn->status != xi::Status::Normal || PPawn->PAI == nullptr ||
                PPawn->PAI->IsEngaged())
            {
                return false;
            }
            const auto* PController = dynamic_cast<CPawnController*>(PPawn->PAI->GetController());
            return PController == nullptr || (PController->PartyFightTarget() == nullptr && PController->QueueLine().lane.action.kind == CL_AK_NONE);
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
            uint32 dropped = 0; // census pieces taken out of her bags
            uint32 moved   = 0; // pieces carried from her inventory into Mog Wardrobe 1
            uint32 bared   = 0; // pieces taken off a slot the plan leaves bare
        };

        // The container her gear lives in: Mog Wardrobe 1 when it holds at
        // least what her inventory does (the server's default is 80 to 30),
        // so her food and the scrolls she buys never crowd out a re-dress;
        // her inventory on a server that gives the wardrobe less. The census
        // writes her gear by the same rule (census.py finish)
        auto gearLocationOf(CCharEntity* PPawn) -> uint8
        {
            const auto* wardrobe  = PPawn->getStorage(LOC_WARDROBE);
            const auto* inventory = PPawn->getStorage(LOC_INVENTORY);
            return wardrobe != nullptr && inventory != nullptr && wardrobe->GetSize() > 0 && wardrobe->GetSize() >= inventory->GetSize() ? LOC_WARDROBE
                                                                                                                                         : LOC_INVENTORY;
        }

        // A stack taken out of the game from any of her containers, without a
        // word: a census piece the plan has no place for
        auto discard(CCharEntity* PPawn, const uint8 location, const uint8 slot, const uint32 quantity) -> bool
        {
            auto transaction = ItemClaimTransaction::start(PPawn);
            return transaction && transaction->take(location, slot, quantity) && transaction->commit();
        }

        // Her wardrobe as the census planned it, in the container her gear
        // lives in (gearLocationOf): each planned piece in her inventory is
        // carried there first, worn or not (a piece moves between the
        // inventory and a wardrobe as it is, and stays worn); then each piece
        // she is not wearing is found there, or given her, and worn. Where
        // that container is full, the piece she wears in the slot, which the
        // plan has no place for, leaves first, so a re-dress swaps in place.
        // A piece that still does not fit, or that she cannot wear, is left
        // out and said; one that could not go on in the first pass (under a
        // cover still worn) is tried once more after the rest. Her bags keep
        // only what the census's own rule keeps (keepsItem): whatever else
        // she holds -- loot, old food, a piece of an older plan, worn or not
        // -- is taken off and out of the game first, to make room for the
        // new, and again at the end, for what the new pieces took off her.
        // A slot the plan leaves bare is bared, as the census's finish
        // leaves it: a second copy of a piece the plan now wants once stays
        // in her bag, unworn
        auto dress(CCharEntity* PPawn, const std::vector<Piece>& plan, const std::set<uint16>& food) -> Dressed
        {
            Dressed     out;
            const uint8 gearLocation = gearLocationOf(PPawn);
            auto*       bag          = PPawn->getStorage(gearLocation);
            auto*       inventory    = PPawn->getStorage(LOC_INVENTORY);
            if (bag == nullptr || inventory == nullptr)
            {
                return out;
            }
            std::set<uint16> pieces;
            for (const auto& piece : plan)
            {
                pieces.insert(piece.itemId);
            }
            std::set<uint16> wanted = food;
            wanted.insert(pieces.begin(), pieces.end());

            const auto keeps = [&](const CItem* PItem, const uint8 location)
            {
                return cardian::redress::keepsItem(PItem->getID(), location == LOC_INVENTORY, PItem->isType(ITEM_CURRENCY),
                                                   pawn::items::usableOnWild(PItem->getID()), wanted);
            };
            // The equipment slot she wears a piece in, if she wears it
            const auto wornIn = [&](const CItem* PItem) -> std::optional<uint8>
            {
                for (uint8 equipSlot = SLOT_MAIN; equipSlot <= SLOT_BACK; ++equipSlot)
                {
                    if (static_cast<const CItem*>(PPawn->getEquip(static_cast<SLOTTYPE>(equipSlot))) == PItem)
                    {
                        return equipSlot;
                    }
                }
                return std::nullopt;
            };
            // A piece her bags do not keep: taken off if she wears it, and out
            // of the game
            const auto throwOut = [&](const CItem* PItem, const uint8 location) -> bool
            {
                const uint8  slot     = PItem->getSlotID();
                const uint32 quantity = PItem->getQuantity();
                if (const auto equipSlot = wornIn(PItem); equipSlot.has_value() && pawn::items::unequip(PPawn, *equipSlot) != CL_S_OK)
                {
                    return false;
                }
                if (!discard(PPawn, location, slot, quantity))
                {
                    return false;
                }
                ++out.dropped;
                return true;
            };
            const auto clearOut = [&]
            {
                for (const uint8 location : std::array<uint8, 2>{ static_cast<uint8>(LOC_INVENTORY), gearLocation })
                {
                    auto* storage = PPawn->getStorage(location);
                    for (uint8 slot = 1; storage != nullptr && slot <= storage->GetSize(); ++slot)
                    {
                        const CItem* PItem = storage->GetItem(slot);
                        if (PItem != nullptr && PItem->getQuantity() > 0 && !keeps(PItem, location))
                        {
                            throwOut(PItem, location);
                        }
                    }
                    if (location == gearLocation)
                    {
                        break; // the inventory is her gear's container: looked at once
                    }
                }
            };
            clearOut();

            // Her planned pieces still in her inventory, worn or not, go to
            // where her gear lives
            if (gearLocation != LOC_INVENTORY)
            {
                for (uint8 slot = 1; slot <= inventory->GetSize(); ++slot)
                {
                    const auto* PItem = dynamic_cast<CItemEquipment*>(inventory->GetItem(slot));
                    if (PItem != nullptr && pieces.contains(PItem->getID()) &&
                        pawn::items::moveItem(PPawn, LOC_INVENTORY, slot, gearLocation, PItem->getQuantity()) == CL_S_OK)
                    {
                        ++out.moved;
                    }
                }
            }

            // A copy where her gear lives she is not wearing, else one given her
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
                const auto landed = transaction->give(gearLocation, piece.itemId, quantity, Silence::Yes);
                if (!landed.has_value() || !transaction->commit())
                {
                    return std::nullopt;
                }
                return *landed;
            };

            // Her container full: the piece she wears in the slot, which the
            // plan has no place for, taken off and out of the game
            const auto makeRoom = [&](const Piece& piece) -> bool
            {
                const CItem* PWorn = PPawn->getEquip(static_cast<SLOTTYPE>(piece.slot));
                return PWorn != nullptr && !keeps(PWorn, PWorn->getLocationID()) && throwOut(PWorn, PWorn->getLocationID());
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
                auto slot = bagSlotFor(piece);
                if (!slot.has_value() && makeRoom(piece))
                {
                    slot = bagSlotFor(piece);
                }
                if (!slot.has_value())
                {
                    ShowWarningFmt("world: {} has no room in her bag for item {} (slot {}); left out", PPawn->getName(), piece.itemId, piece.slot);
                    return;
                }
                if (const auto status = pawn::items::equip(PPawn, *slot, piece.slot, gearLocation); status != CL_S_OK)
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
            std::set<uint8> planned;
            for (const auto& piece : plan)
            {
                planned.insert(piece.slot);
            }
            for (uint8 equipSlot = SLOT_MAIN; equipSlot <= SLOT_BACK; ++equipSlot)
            {
                if (!planned.contains(equipSlot) && PPawn->getEquip(static_cast<SLOTTYPE>(equipSlot)) != nullptr && pawn::items::unequip(PPawn, equipSlot) == CL_S_OK)
                {
                    ++out.bared;
                }
            }
            clearOut();

            if (out.worn > 0 || out.moved > 0 || out.dropped > 0 || out.bared > 0)
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
        // level she levelled it to, which the game shows only up to half her
        // main, so it rises with her main by itself as she dings. Another
        // support job than hers is set by the game's own
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
        // food laid in her bag, her spells, her skills, her health
        // recomputed for them; the player she is with told
        void apply(CCharEntity* PPawn, const Answer& answer)
        {
            const auto plan = planOf(PPawn->getName());
            if (!plan.has_value())
            {
                ShowErrorFmt("world: {}'s wardrobe could not be read; her re-dress waits", PPawn->getName());
                return;
            }
            const bool subbed  = takeSub(PPawn, answer);
            pawn::food::forget(PPawn->id);
            const auto dressed = dress(PPawn, *plan, pawn::food::plannedIds(PPawn->getName()));
            pawn::food::topUp(PPawn);
            const auto learned = learnSpells(PPawn);
            const auto raised  = raiseSkills(PPawn, answer.skills);
            PPawn->UpdateHealth();
            PPawn->clearPacketList();
            markDone(answer);

            ShowInfoFmt("world: {} is dressed for level {} at the auction house ({}{} pieces put on, {} taken off, {} moved to Mog Wardrobe 1, {} spells learned, "
                        "{} skills raised)",
                        PPawn->getName(), answer.level, subbed ? "her support job set, " : "", dressed.worn, dressed.dropped + dressed.bared, dressed.moved, learned, raised);
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

        // -- The live census: a ding out of sight (redress.h) ------------------

        // The bodies the census is writing in her rows, by charid, and when the
        // map claimed each: the clock on the wall, so a claim the watcher never
        // finishes lapses whatever the simulation does
        std::unordered_map<uint32, realtime::time_point> claims;

        // A look's work, bounded: bodies handed to the census, about what its
        // watcher writes in the same time (census.py CLAIMS_PER_LOOK), so a
        // crowd of dings never keeps many bodies from standing at once; and
        // dings put on bodies the map holds, each a few saves on its thread
        constexpr uint32 kClaimsPerLook  = 20;
        constexpr uint32 kAppliesPerLook = 5;

        // A row of cardian_ding the map still has to act on
        struct Ding
        {
            uint32      charid = 0;
            DingPlan    plan;
            std::string skills;
            uint8       sub      = 0;
            uint8       sublevel = 0;
            DingState   state    = DingState::Planned;
        };

        auto openDings() -> std::optional<std::vector<Ding>>
        {
            const auto rset = db::preparedStmt("SELECT charid, kind, job, level, gear, skills, sub, sublevel, state FROM cardian_ding "
                                               "WHERE state IN ('planned', 'claimed') ORDER BY planned_at, charid");
            if (!rset)
            {
                return std::nullopt;
            }
            std::vector<Ding> out;
            while (rset->next())
            {
                const auto state = cardian::redress::dingStateOf(rset->get<std::string>("state"));
                if (!state.has_value())
                {
                    continue;
                }
                out.push_back({ .charid   = rset->get<uint32>("charid"),
                                .plan     = { .dress = rset->get<std::string>("kind") == "dress",
                                              .job   = rset->get<uint8>("job"),
                                              .level = rset->get<uint8>("level"),
                                              .gear  = rset->get<uint8>("gear") != 0 },
                                .skills   = rset->get<std::string>("skills"),
                                .sub      = rset->get<uint8>("sub"),
                                .sublevel = rset->get<uint8>("sublevel"),
                                .state    = *state });
            }
            return out;
        }

        auto inCity(const CCharEntity* PPawn) -> bool
        {
            return PPawn->loc.zone != nullptr && (PPawn->loc.zone->GetTypeMask() & xi::ZoneType::City) != xi::ZoneType::Unknown;
        }

        // Where she is, for the ding's rules (redress_math.h dingStep)
        auto dingNow(const uint32 charid, const DingPlan& plan) -> DingNow
        {
            DingNow now{};
            now.withPlayer    = pawn::withRealPlayer(charid) || (pawn::world::hasBody(charid) && !pawn::world::inTheWild(charid));
            const auto* PPawn = pawn::findPawn(charid);
            if (PPawn == nullptr)
            {
                return now;
            }
            now.standing    = true;
            now.seen        = PPawn->loc.zone == nullptr || pawn::world::playerIn(static_cast<uint16>(PPawn->getZone()));
            now.busy        = !canDress(PPawn);
            now.farming     = pawn::world::isFarming(charid);
            now.inCity      = inCity(PPawn);
            now.mainJob     = static_cast<uint8>(PPawn->GetMJob());
            const uint8 job = plan.job != 0 && plan.job < MAX_JOBTYPE ? plan.job : now.mainJob;
            now.jobLevel    = PPawn->jobs.job[job];
            return now;
        }

        // The job she plays changed, as her player's Mog House changes it
        // (mog_house.cpp changeJobs): the job unlocked, her pet sent away, a
        // waiting order and an enchanted piece on its way let go, then the
        // game's own job change, which takes her gear off and rebuilds her
        // stats, abilities and traits; her gear goes on again after it.
        // `sub` is the support job she takes with it, 0 to keep hers
        auto switchMain(CCharEntity* PPawn, const uint8 job, const uint8 sub) -> bool
        {
            if (job == 0 || job >= MAX_JOBTYPE || job == static_cast<uint8>(PPawn->GetMJob()))
            {
                return false;
            }
            PPawn->jobs.unlocked |= (1u << job);
            PPawn->jobs.job[job] = std::max<uint8>(PPawn->jobs.job[job], 1);
            charutils::SaveCharJob(PPawn, static_cast<xi::Job>(job));
            if (PPawn->PPet != nullptr)
            {
                petutils::DespawnPet(PPawn);
            }
            if (auto* PController = dynamic_cast<CPawnController*>(PPawn->PAI->GetController()); PController != nullptr)
            {
                PController->EndEnchant("the job she plays changed");
                PController->ClearQueuedOrders("the job she plays changed");
            }
            GP_CLI_COMMAND_MYROOM_JOB jobChange{};
            jobChange.MainJobIndex    = job;
            jobChange.SupportJobIndex = sub < MAX_JOBTYPE && sub != job ? sub : 0;
            jobChange.process(nullptr, PPawn);
            return true;
        }

        // Her main job raised to the level, as the game's own ding raises it
        // (charutils::AddExperiencePoints): the level, her support job's shown
        // level under it, her stats, abilities, traits and weapon skills; her
        // experience starts the level from nothing
        auto raiseLevel(CCharEntity* PPawn, const uint8 level) -> bool
        {
            const auto mjob = static_cast<uint8>(PPawn->GetMJob());
            if (level == 0 || PPawn->jobs.job[mjob] >= level)
            {
                return false;
            }
            PPawn->jobs.job[mjob] = level;
            PPawn->jobs.exp[mjob] = 0;
            PPawn->SetMLevel(level);
            PPawn->SetSLevel(PPawn->jobs.job[static_cast<uint8>(PPawn->GetSJob())]);
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
            charutils::SaveCharStats(PPawn);
            charutils::SaveCharJob(PPawn, PPawn->GetMJob());
            charutils::SaveCharExp(PPawn, PPawn->GetMJob());
            return true;
        }

        // The ding put on a body the map holds, out of sight: the job she
        // plays (a city's change), her level, her support job and her skills
        // for it, and where it was planned and she stands in a city her gear,
        // food and spells as the census planned them -- the census's own rule
        // for her, as at the auction house. The row says whether her gear went
        // on (`dressed`), for the census's bookkeeping
        void applyDing(CCharEntity* PPawn, const Ding& ding, const bool gear)
        {
            const auto   before   = fmt::format("{} {}", magic_enum::enum_name(PPawn->GetMJob()), PPawn->GetMLevel());
            const bool   switched = switchMain(PPawn, ding.plan.job, ding.sub);
            const bool   raised   = !ding.plan.dress && raiseLevel(PPawn, ding.plan.level);
            const Answer answer{ ding.charid, ding.plan.level, ding.skills, "", ding.sub, ding.sublevel };
            const bool   subbed  = takeSub(PPawn, answer);
            Dressed      dressed{};
            uint32       learned = 0;
            if (gear)
            {
                if (const auto plan = planOf(PPawn->getName()); plan.has_value())
                {
                    pawn::food::forget(PPawn->id);
                    dressed = dress(PPawn, *plan, pawn::food::plannedIds(PPawn->getName()));
                    pawn::food::topUp(PPawn);
                    learned = learnSpells(PPawn);
                }
            }
            const auto skills = raiseSkills(PPawn, ding.skills);
            PPawn->UpdateHealth();
            PPawn->health.hp = PPawn->GetMaxHP();
            PPawn->health.mp = PPawn->GetMaxMP();
            PPawn->updatemask |= UPDATE_HP;
            PPawn->clearPacketList();
            if (raised || switched)
            {
                pawn::world::noteLevel(PPawn->id, PPawn->GetMLevel());
            }

            db::preparedStmt("UPDATE cardian_ding SET state = 'applied', dressed = ?, done_at = NOW() WHERE charid = ? AND state = 'planned'", gear ? 1 : 0,
                             ding.charid);
            if (gear)
            {
                // Dressed for this level out of sight: a re-dress at a counter
                // is asked again only once she rises past it
                db::preparedStmt("UPDATE cardian_redress SET state = 'done', level = ?, done_at = NOW() WHERE charid = ?", PPawn->GetMLevel(), ding.charid);
            }
            const auto zone = PPawn->loc.zone != nullptr ? std::string(PPawn->loc.zone->getName()) : std::string("nowhere");
            if (ding.plan.dress)
            {
                ShowInfoFmt("world: {} changes her gear out of sight in {} at level {} ({} pieces put on, {} taken off, {} spells learned{})", PPawn->getName(),
                            zone, PPawn->GetMLevel(), dressed.worn, dressed.dropped + dressed.bared, learned, subbed ? ", her support job set" : "");
                return;
            }
            ShowInfoFmt("world: {} dings out of sight in {}: {} -> {} {} ({} skills raised{}{})", PPawn->getName(), zone, before,
                        magic_enum::enum_name(PPawn->GetMJob()), PPawn->GetMLevel(), skills, subbed ? ", her support job set" : "",
                        gear ? fmt::format(", her gear changed: {} pieces put on, {} taken off, {} spells learned", dressed.worn, dressed.dropped + dressed.bared, learned)
                             : std::string(", her gear as it was"));
        }

        // A ding set aside goes back to the census unworn, its reason in the
        // note: a plan of gear written for it is still owed her
        void setAsideDing(const Ding& ding, const std::string& why)
        {
            db::preparedStmt("UPDATE cardian_ding SET state = 'applied', dressed = 0, note = ?, done_at = NOW() WHERE charid = ? AND state = 'planned'", why,
                             ding.charid);
            ShowInfoFmt("world: {}'s ding to level {} is set aside: {}", pawn::seats::nameOf(ding.charid), ding.plan.level, why);
        }

        // A body the map does not hold, claimed for the census: it writes her
        // rows, and she stands for nobody until it is done
        void claim(const Ding& ding)
        {
            const auto rset = db::preparedStmt("UPDATE cardian_ding SET state = 'claimed', claimed_at = NOW() WHERE charid = ? AND state = 'planned'", ding.charid);
            if (rset && rset->rowsAffected() > 0)
            {
                claims[ding.charid] = realtime::now();
            }
        }

        // A claim held past its time is taken back, unless the census has
        // finished it meanwhile: the census's write is one transaction that
        // ends by marking the row done only while it is still claimed
        void lapse(const uint32 charid)
        {
            const auto rset = db::preparedStmt("UPDATE cardian_ding SET state = 'planned', claimed_at = NULL WHERE charid = ? AND state = 'claimed'", charid);
            if (rset && rset->rowsAffected() > 0)
            {
                ShowWarningFmt("world: the census has not written {}'s ding in {} s; she may stand again, and the ding waits", pawn::seats::nameOf(charid),
                               cardian::redress::kClaimLapseSeconds);
            }
            claims.erase(charid);
        }

        // Every ding the census has planned, acted on where she is: put on a
        // body out of sight and free, claimed for the census where the map
        // holds no body of hers, set aside where she has the level already,
        // or left for a later look. A claim the census has finished is let
        // go, so she may stand
        void applyDings()
        {
            const auto dings = openDings();
            if (!dings.has_value())
            {
                return;
            }
            std::set<uint32> stillClaimed;
            for (const auto& ding : *dings)
            {
                if (ding.state == DingState::Claimed)
                {
                    stillClaimed.insert(ding.charid);
                }
            }
            for (auto it = claims.begin(); it != claims.end();)
            {
                if (!stillClaimed.contains(it->first))
                {
                    it = claims.erase(it);
                    continue;
                }
                ++it;
            }
            const auto now     = realtime::now();
            uint32     claimed = 0;
            uint32     applied = 0;
            for (const auto& ding : *dings)
            {
                if (ding.state == DingState::Claimed)
                {
                    const auto it = claims.find(ding.charid);
                    if (it == claims.end())
                    {
                        claims[ding.charid] = now; // claimed before this map started: she waits for the census as much
                    }
                    else if (cardian::redress::claimLapsed(static_cast<uint32>(std::chrono::duration_cast<std::chrono::seconds>(now - it->second).count())))
                    {
                        lapse(ding.charid);
                    }
                    continue;
                }
                const auto where = dingNow(ding.charid, ding.plan);
                switch (cardian::redress::dingStep(ding.plan, where))
                {
                    case DingStep::Wait:
                        break;
                    case DingStep::Claim:
                        if (claimed < kClaimsPerLook && claims.size() < 3 * kClaimsPerLook)
                        {
                            claim(ding);
                            ++claimed;
                        }
                        break;
                    case DingStep::SetAside:
                        setAsideDing(ding, fmt::format("she is level {} already", where.jobLevel));
                        break;
                    case DingStep::Apply:
                    {
                        auto* PPawn = pawn::findPawn(ding.charid);
                        if (PPawn == nullptr || applied >= kAppliesPerLook)
                        {
                            break;
                        }
                        ++applied;
                        if (!pawn::seats::isWorlds(ding.charid))
                        {
                            setAsideDing(ding, "she is no longer one of the world's");
                            break;
                        }
                        applyDing(PPawn, ding, cardian::redress::dressesNow(ding.plan, where));
                        break;
                    }
                }
            }
        }
    } // namespace

    void ensureTable()
    {
        db::preparedStmt("CREATE TABLE IF NOT EXISTS `cardian_ding` ("
                         "`charid` int(10) unsigned NOT NULL, "
                         "`kind` enum('ding','dress') NOT NULL DEFAULT 'ding', "
                         "`job` tinyint(3) unsigned NOT NULL DEFAULT '0', "
                         "`level` tinyint(3) unsigned NOT NULL DEFAULT '0', "
                         "`gear` tinyint(1) unsigned NOT NULL DEFAULT '0', "
                         "`dressed` tinyint(1) unsigned NOT NULL DEFAULT '0', "
                         "`skills` varchar(1024) NOT NULL DEFAULT '', "
                         "`sub` tinyint(3) unsigned NOT NULL DEFAULT '0', "
                         "`sublevel` tinyint(3) unsigned NOT NULL DEFAULT '0', "
                         "`state` enum('planned','claimed','applied','done') NOT NULL DEFAULT 'planned', "
                         "`note` varchar(128) NOT NULL DEFAULT '', "
                         "`planned_at` datetime DEFAULT NULL, "
                         "`claimed_at` datetime DEFAULT NULL, "
                         "`done_at` datetime DEFAULT NULL, "
                         "PRIMARY KEY (`charid`), KEY `state` (`state`)) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_general_ci");
        // Her career's jobs after her first on a census table made before careers
        // (census.py CAREER_COLUMNS, which the watcher adds too): the world
        // reads her target on the job she plays (world.cpp readCensus)
        db::preparedStmt("ALTER TABLE IF EXISTS `cardian_census` "
                         "ADD COLUMN IF NOT EXISTS `job2` tinyint(3) unsigned NOT NULL DEFAULT '0', "
                         "ADD COLUMN IF NOT EXISTS `cohort2` int(10) unsigned NOT NULL DEFAULT '0', "
                         "ADD COLUMN IF NOT EXISTS `target2` tinyint(3) unsigned NOT NULL DEFAULT '0', "
                         "ADD COLUMN IF NOT EXISTS `later_jobs` varchar(64) NOT NULL DEFAULT ''");
        // A census write under way when the map last stopped: she stands for
        // nobody until the census says it is done
        if (const auto rset = db::preparedStmt("SELECT charid FROM cardian_ding WHERE state = 'claimed'"); rset)
        {
            while (rset->next())
            {
                claims[rset->get<uint32>("charid")] = realtime::now();
            }
        }
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
            applyDings();
        }
    }

    auto isClaimed(const uint32 charid) -> bool
    {
        return claims.contains(charid);
    }
} // namespace pawn::redress
