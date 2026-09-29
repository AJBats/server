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

#include "link_api.h"

#include "action_keys.h"
#include "auction.h"
#include "cardian_link.h"
#include "gate_guards.h"
#include "pawn_gambits.h"
#include "pawn.h"
#include "pawn_controller.h"
#include "pawn_items.h"
#include "view.h"

#include "ability.h"
#include "ai/ai_container.h"
#include "common/logging.h"
#include "entities/char_entity.h"
#include "enums/item_state.h"
#include "item_container.h"
#include "items/item.h"
#include "navmesh/navmesh.h"
#include "pause/input_gate.h"
#include "pause/pause.h"
#include "recast_container.h"
#include "utils/battleutils.h"
#include "utils/charutils.h"
#include "utils/zoneutils.h"
#include "zone.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <set>
#include <string>
#include <vector>

namespace pawn::linkapi
{
    namespace
    {
        using namespace cardian::link;

        // One of her containers as it stands now
        auto inventoryOf(CCharEntity* PPawn, const uint8 location) -> cl_inventory
        {
            auto msg     = make<cl_inventory>();
            msg.cardian  = PPawn->id;
            msg.loc      = location;
            const auto* storage = PPawn->getStorage(location);
            if (storage == nullptr)
            {
                return msg;
            }
            msg.size = storage->GetSize();
            msg.free = storage->GetFreeSlotsCount();

            constexpr std::size_t capacity = sizeof(cl_inventory::items) / sizeof(cl_item);
            for (uint8 slot = 1; slot <= storage->GetSize() && msg.count < capacity; ++slot)
            {
                const CItem* PItem = storage->GetItem(slot);
                if (PItem == nullptr || PItem->getQuantity() == 0)
                {
                    continue;
                }
                auto& item = msg.items[msg.count++];
                item.slot  = slot;
                item.id    = PItem->getID();
                item.qty   = PItem->getQuantity();
                item.flags = static_cast<uint8_t>(PItem->state() == ItemState::Equipped ? CL_ITEM_EQUIPPED : 0);
            }
            return msg;
        }

        // A number into a field of two bytes, held at its top rather than wrapped
        auto clamp16(const int64 value) -> uint16_t
        {
            return static_cast<uint16_t>(std::clamp<int64>(value, 0, UINT16_MAX));
        }

        // A zone's name for people: the game's, its underscores as spaces
        template <std::size_t N>
        void setZoneName(char (&field)[N], CZone* PZone)
        {
            std::string name = PZone != nullptr ? PZone->getName() : std::string("?");
            std::replace(name.begin(), name.end(), '_', ' ');
            setText(field, name);
        }

        // What the player himself stands by, worked out once for all the lines
        // that tell it: the auction counter and the gate guard within his reach
        struct PlayerReach
        {
            const CBaseEntity* counter = nullptr;
            bool               byGuard = false;
        };

        auto reachOf(CCharEntity* PPlayer) -> PlayerReach
        {
            return PlayerReach{ pawn::auction::counterNear(PPlayer), pawn::guards::guardNear(PPlayer) != nullptr };
        }

        // One cardian as his roster shows her; managed: she is his to manage
        auto memberOf(CCharEntity* PPlayer, CCharEntity* PPawn, const PlayerReach& reach, const bool managed) -> cl_member
        {
            auto member      = make<cl_member>();
            member.cardian   = PPawn->id;
            setText(member.name, PPawn->getName());
            member.mainJob   = static_cast<uint8_t>(PPawn->GetMJob());
            member.mainLevel = PPawn->GetMLevel();
            member.subJob    = static_cast<uint8_t>(PPawn->GetSJob());
            member.subLevel  = PPawn->GetSLevel();
            member.hp        = clamp16(PPawn->health.hp);
            member.maxHp     = clamp16(PPawn->GetMaxHP());
            member.mp        = clamp16(PPawn->health.mp);
            member.maxMp     = clamp16(PPawn->GetMaxMP());
            member.tp        = clamp16(PPawn->health.tp);
            member.zone      = static_cast<uint16_t>(PPawn->getZone());
            setZoneName(member.zoneName, PPawn->loc.zone);

            const auto job = static_cast<uint8>(PPawn->GetMJob());
            member.exp     = job < MAX_JOBTYPE ? PPawn->jobs.exp[job] : 0;
            member.tnl     = charutils::GetExpNEXTLevel(PPawn->GetMLevel());

            const auto* PController = dynamic_cast<const CPawnController*>(PPawn->PAI->GetController());
            uint8       flags       = 0;
            if (PController != nullptr && PController->IsWaiting())
            {
                flags |= CL_MEMBER_WAITING;
            }
            if (managed)
            {
                flags |= CL_MEMBER_OWNED;
            }
            if (pawn::auction::whereShopping(PPlayer, PPawn, reach.counter) == CL_S_OK)
            {
                flags |= CL_MEMBER_BY_COUNTER;
            }
            if (reach.byGuard)
            {
                flags |= CL_MEMBER_BY_GUARD;
            }
            member.flags = flags;
            return member;
        }

        // Her status pane; her gil only when she is his to manage (a wild
        // cardian's purse is her own)
        auto statsOf(CCharEntity* PPawn, const bool managed) -> cl_member_stats
        {
            auto stats    = make<cl_member_stats>();
            stats.cardian = PPawn->id;
            const std::array<std::pair<uint16, xi::Mod>, 7> kStats{ {
                { PPawn->STR(), xi::Mod::STR },
                { PPawn->DEX(), xi::Mod::DEX },
                { PPawn->VIT(), xi::Mod::VIT },
                { PPawn->AGI(), xi::Mod::AGI },
                { PPawn->INT(), xi::Mod::INT },
                { PPawn->MND(), xi::Mod::MND },
                { PPawn->CHR(), xi::Mod::CHR },
            } };
            for (std::size_t i = 0; i < kStats.size(); ++i)
            {
                stats.total[i] = static_cast<int16_t>(kStats[i].first);
                stats.bonus[i] = static_cast<int16_t>(PPawn->getMod(kStats[i].second));
            }
            stats.attack  = clamp16(PPawn->ATT(SLOT_MAIN));
            stats.defence = clamp16(PPawn->DEF());
            stats.gil     = managed ? pawn::items::gilOf(PPawn) : 0;
            return stats;
        }

        // What she wears, by equipment slot
        auto gearOf(CCharEntity* PPawn) -> cl_gear
        {
            auto gear    = make<cl_gear>();
            gear.cardian = PPawn->id;
            for (uint8 equipSlot = SLOT_MAIN; equipSlot <= SLOT_BACK; ++equipSlot)
            {
                if (const auto* PItem = PPawn->getEquip(static_cast<SLOTTYPE>(equipSlot)); PItem != nullptr)
                {
                    gear.worn[equipSlot] = cl_worn{ PItem->getID(), PItem->getLocationID(), PItem->getSlotID() };
                }
            }
            return gear;
        }

        // Her storage bags, in the order the menu cycles them
        auto bagsOf(CCharEntity* PPawn) -> cl_bags
        {
            auto bags    = make<cl_bags>();
            bags.cardian = PPawn->id;
            constexpr std::size_t kMax = sizeof(cl_bags::bags) / sizeof(cl_bag);
            for (const auto& bag : pawn::items::bags(PPawn))
            {
                if (bags.count >= kMax)
                {
                    break;
                }
                bags.bags[bags.count++] = cl_bag{ bag.location, bag.size, bag.used, 0 };
            }
            return bags;
        }

        // A change to the items or gear of a cardian of his to manage: made by
        // change(PPawn, partly), which returns its outcome and sets partly when
        // a refusal came after part of it had moved; what it moved, told by
        // moved(PPawn) as answers when it moved anything; then its outcome
        template <typename Message, typename Change, typename Moved>
        void changeItems(CCharEntity* PChar, const Message& ask, Reply& reply, Change&& change, Moved&& moved)
        {
            auto* PPawn = pawn::findManagedPawn(PChar, ask.cardian);
            if (PPawn == nullptr)
            {
                reply.finish(ask, CL_S_NO_SUCH_CARDIAN);
                return;
            }
            bool       partly = false;
            const auto status = change(PPawn, partly);
            if (status == CL_S_OK || partly)
            {
                moved(PPawn);
            }
            reply.finish(ask, status);
        }

        // A stack from his inventory to hers, or back
        void give(CCharEntity* PChar, const cl_give& ask, Reply& reply)
        {
            changeItems(
                PChar, ask, reply, [&](CCharEntity* PPawn, bool&) { return pawn::items::giveToPawn(PChar, PPawn, ask.slot, ask.qty); },
                [&](CCharEntity* PPawn) { reply.more(inventoryOf(PPawn, LOC_INVENTORY)); });
        }

        void take(CCharEntity* PChar, const cl_take& ask, Reply& reply)
        {
            changeItems(
                PChar, ask, reply, [&](CCharEntity* PPawn, bool&) { return pawn::items::takeFromPawn(PChar, PPawn, ask.slot, ask.qty); },
                [&](CCharEntity* PPawn) { reply.more(inventoryOf(PPawn, LOC_INVENTORY)); });
        }

        // The trade window's gil line: her status pane carries her new purse,
        // his client counts his own
        void gil(CCharEntity* PChar, const cl_gil& ask, Reply& reply)
        {
            changeItems(
                PChar, ask, reply, [&](CCharEntity* PPawn, bool&) { return pawn::items::moveGil(PChar, PPawn, ask.amount, ask.toHer != 0); },
                [&](CCharEntity* PPawn) { reply.more(statsOf(PPawn, true)); });
        }

        // She uses an item on herself: the stack thins when the use completes,
        // so the outcome is all there is to tell now
        void use(CCharEntity* PChar, const cl_use& ask, Reply& reply)
        {
            changeItems(
                PChar, ask, reply, [&](CCharEntity* PPawn, bool&) { return pawn::items::useItem(PPawn, ask.slot, ask.bag); },
                [](CCharEntity*) {});
        }

        void drop(CCharEntity* PChar, const cl_drop& ask, Reply& reply)
        {
            changeItems(
                PChar, ask, reply, [&](CCharEntity* PPawn, bool&) { return pawn::items::dropItem(PPawn, ask.slot, ask.qty, ask.bag); },
                [&](CCharEntity* PPawn)
                {
                    reply.more(inventoryOf(PPawn, ask.bag));
                    reply.more(bagsOf(PPawn));
                });
        }

        // One of her containers merged and put in order; a worn piece may sit
        // in a new slot, so her gear follows
        void sortContainer(CCharEntity* PChar, const cl_sort& ask, Reply& reply)
        {
            changeItems(
                PChar, ask, reply, [&](CCharEntity* PPawn, bool&) { return pawn::items::sortBag(PPawn, ask.bag); },
                [&](CCharEntity* PPawn)
                {
                    reply.more(inventoryOf(PPawn, ask.bag));
                    reply.more(bagsOf(PPawn));
                    reply.more(gearOf(PPawn));
                });
        }

        // A stack between her inventory and one of her bags; a worn piece
        // carried into a wardrobe reports its new home in her gear
        void moveStack(CCharEntity* PChar, const cl_move& ask, Reply& reply)
        {
            changeItems(
                PChar, ask, reply, [&](CCharEntity* PPawn, bool& partly) { return pawn::items::moveItem(PPawn, ask.from, ask.slot, ask.to, ask.qty, &partly); },
                [&](CCharEntity* PPawn)
                {
                    reply.more(inventoryOf(PPawn, ask.from));
                    reply.more(inventoryOf(PPawn, ask.to));
                    reply.more(bagsOf(PPawn));
                    reply.more(gearOf(PPawn));
                });
        }

        // The wardrobes her worn pieces sit in: a worn mark lives in its
        // container's rows
        auto wornWardrobes(CCharEntity* PPawn) -> std::set<uint8>
        {
            std::set<uint8> out;
            for (uint8 equipSlot = SLOT_MAIN; equipSlot <= SLOT_BACK; ++equipSlot)
            {
                if (const auto* PItem = PPawn->getEquip(static_cast<SLOTTYPE>(equipSlot)); PItem != nullptr && pawn::items::isWardrobe(PItem->getLocationID()))
                {
                    out.insert(PItem->getLocationID());
                }
            }
            return out;
        }

        // A loadout in one pass (pawn::items::equipSet), each slot's outcome
        // in the answer. Tried, it answers with her status pane, gear and
        // inventory, and every wardrobe that held a worn piece before or
        // after: a piece put on from a wardrobe is worn from it after
        void equip(CCharEntity* PChar, const cl_equip& ask, Reply& reply)
        {
            auto* PPawn = pawn::findManagedPawn(PChar, ask.cardian);
            if (PPawn == nullptr)
            {
                reply.finish(ask, CL_S_NO_SUCH_CARDIAN);
                return;
            }
            constexpr std::size_t kMaxSlots = sizeof(cl_equip::slots) / sizeof(cl_equip_slot);
            if (ask.count == 0 || ask.count > kMaxSlots)
            {
                reply.finish(ask, CL_S_MALFORMED);
                return;
            }

            auto                                  touched = wornWardrobes(PPawn);
            std::vector<pawn::items::EquipChange> changes;
            for (std::size_t i = 0; i < ask.count; ++i)
            {
                changes.push_back({ ask.slots[i].equipSlot, ask.slots[i].bag, ask.slots[i].slot });
            }
            pawn::items::equipSet(PPawn, changes);
            touched.merge(wornWardrobes(PPawn));

            auto   answer = ask;
            uint16 status = CL_S_OK;
            for (std::size_t i = 0; i < changes.size(); ++i)
            {
                answer.results[i] = changes[i].result;
                if (status == CL_S_OK)
                {
                    status = changes[i].result;
                }
            }
            reply.more(statsOf(PPawn, true));
            reply.more(gearOf(PPawn));
            reply.more(inventoryOf(PPawn, LOC_INVENTORY));
            for (const auto location : touched)
            {
                reply.more(inventoryOf(PPawn, location));
            }
            reply.finish(answer, status);
        }

        // The scroll's way: the stack given, then used from wherever it landed
        void giveUse(CCharEntity* PChar, const cl_give_use& ask, Reply& reply)
        {
            auto* PPawn = pawn::findManagedPawn(PChar, ask.cardian);
            if (PPawn == nullptr)
            {
                reply.finish(ask, CL_S_NO_SUCH_CARDIAN);
                return;
            }
            // Refused whole while held, as the use would be: not half of it, the transfer
            if (cardian::pause::isHeld())
            {
                reply.finish(ask, CL_S_NOT_WHILE_PAUSED);
                return;
            }
            uint8 landed = 0;
            if (const auto status = pawn::items::giveToPawn(PChar, PPawn, ask.slot, ask.qty, &landed); status != CL_S_OK)
            {
                reply.finish(ask, status);
                return;
            }
            reply.more(inventoryOf(PPawn, LOC_INVENTORY));
            auto answer  = ask;
            answer.given = 1;
            reply.finish(answer, pawn::items::useItem(PPawn, landed));
        }

        // A point the mesh moved less than this (yalms) is the point asked
        constexpr float kRingMoved = 0.02f;

        // What the steering player hears about his walk order: the point as
        // taken when the mesh moved it, or why it was refused (status)
        void tellTaken(Reply& reply, const cl_walk& ask, const float x, const float y, const float z, const uint16 status)
        {
            auto taken    = make<cl_walk_taken>();
            taken.cardian = ask.cardian;
            taken.x       = x;
            taken.y       = y;
            taken.z       = z;
            taken.askedX  = ask.x;
            taken.askedZ  = ask.z;
            reply.notify(taken, status);
        }

        void tellTaken(Reply& reply, const cl_walk& ask, const position_t& at)
        {
            if (std::abs(at.x - ask.x) > kRingMoved || std::abs(at.y - ask.y) > kRingMoved || std::abs(at.z - ask.z) > kRingMoved)
            {
                tellTaken(reply, ask, at.x, at.y, at.z, CL_S_OK);
            }
        }

        // A walk order (pawn.h): a point in her zone, or none, streamed one-way.
        // The point is the player's ring, which is its own thing on his client
        // (no mesh there): it is slid along the mesh from the last point toward
        // the one asked -- a wall or a ledge stops it, so it never leaves the
        // floor she can walk -- and its height is the mesh's. He hears only
        // when it was not taken as asked.
        void walk(CCharEntity* PChar, const cl_walk& ask, Reply& reply)
        {
            auto* PPawn = pawn::findCommandablePawn(PChar, ask.cardian);
            if (PPawn == nullptr)
            {
                tellTaken(reply, ask, ask.x, ask.y, ask.z, CL_S_NO_SUCH_CARDIAN);
                return;
            }
            auto* PController = dynamic_cast<CPawnController*>(PPawn->PAI->GetController());

            if (ask.off != 0)
            {
                // A composed maneuver's route is its order's, not the ring's: the ring
                // going (the camera home) leaves it for her to walk at the release
                if (PController == nullptr || !PController->ManeuverComposed())
                {
                    pawn::clearWalkOrder(PPawn->id);
                }
                return;
            }

            if (!std::isfinite(ask.x) || !std::isfinite(ask.y) || !std::isfinite(ask.z))
            {
                return; // nothing to say where a ring could follow
            }
            if (PPawn->loc.zone == nullptr || PChar->loc.zone != PPawn->loc.zone)
            {
                tellTaken(reply, ask, ask.x, ask.y, ask.z, CL_S_OTHER_ZONE);
                return;
            }

            // Composed, her maneuver's way is set: a point still in flight from the ring moves nothing
            if (PController != nullptr && PController->ManeuverComposed())
            {
                tellTaken(reply, ask, pawn::walkOrderOf(PPawn->id).value_or(PPawn->loc.p));
                return;
            }

            position_t point{ ask.x, ask.y, ask.z, 0, 0 };
            if (auto* PMesh = PPawn->loc.zone->navMesh(); PMesh != nullptr)
            {
                const auto from = pawn::walkOrderOf(PPawn->id).value_or(PPawn->loc.p);
                if (const auto slid = PMesh->findFurthestValidPoint(from, point); slid.has_value())
                {
                    point = *slid;
                }
                else
                {
                    point = from; // nowhere to slide from: the ring stays where it was
                }
                PMesh->snapToValidPosition(point); // the surface's own height
            }
            // Held, in a maneuver, the ring lays a route (docs/maneuvers.md)
            const bool laying = PController != nullptr && PController->InManeuver() && cardian::pause::isHeld();
            pawn::setWalkOrder(PPawn->id, point, PChar->id, laying);
            tellTaken(reply, ask, point);
        }

        // The view origin (ROADMAP C, pawn/view.h): the player's client is
        // looking through this cardian, so the world around her must reach him
        void lookThrough(CCharEntity* PChar, const cl_view& ask, Reply& reply)
        {
            if (PChar->loc.zone == nullptr)
            {
                reply.finish(ask, CL_S_REFUSED);
                return;
            }
            if (ask.cardian == 0)
            {
                cardian::view::clear(PChar);
                reply.finish(ask, CL_S_OK);
                return;
            }
            auto* PPawn = pawn::findCommandablePawn(PChar, ask.cardian);
            if (PPawn == nullptr)
            {
                reply.finish(ask, CL_S_NO_SUCH_CARDIAN);
                return;
            }
            if (PPawn->loc.zone != PChar->loc.zone)
            {
                reply.finish(ask, CL_S_OTHER_ZONE);
                return;
            }
            cardian::view::set(PChar, PPawn);
            ShowInfoFmt("pawn: {} looks through {}", PChar->getName(), PPawn->getName());
            reply.finish(ask, CL_S_OK);
        }
        // A maneuver (docs/maneuvers.md, pawn_controller.h): begun on her, ended,
        // or given its order that is no action (a move, a rest)
        void maneuver(CCharEntity* PChar, const cl_maneuver& ask, Reply& reply)
        {
            auto* PPawn       = pawn::findCommandablePawn(PChar, ask.cardian);
            auto* PController = PPawn != nullptr ? dynamic_cast<CPawnController*>(PPawn->PAI->GetController()) : nullptr;
            if (PController == nullptr)
            {
                reply.finish(ask, CL_S_NO_SUCH_CARDIAN);
                return;
            }

            auto answer = ask;
            switch (ask.action)
            {
                case CL_MV_BEGIN:
                {
                    uint32 other      = 0; // never the packed field's own address
                    const auto status = PController->BeginManeuver(PChar, &other);
                    answer.other      = other;
                    reply.finish(answer, status);
                    return;
                }
                case CL_MV_OFF:
                    if (!PController->InManeuver())
                    {
                        reply.finish(answer, CL_S_NO_MANEUVER);
                        return;
                    }
                    PController->EndManeuver(fmt::format("{} cancels the maneuver", PChar->getName()));
                    reply.finish(answer, CL_S_OK);
                    return;
                case CL_MV_MOVE:
                case CL_MV_MOVE_WAIT:
                    reply.finish(answer, PController->ComposeMove(ask.action == CL_MV_MOVE_WAIT));
                    return;
                case CL_MV_REST:
                    reply.finish(answer, PController->ComposeRest(ask.percent));
                    return;
                default:
                    reply.finish(answer, CL_S_MALFORMED);
                    return;
            }
        }

        // His maneuvers as they stand, for an addon that has just bound: each
        // composed one waiting on a cardian he commands, then the live one
        void maneuvers(CCharEntity* PChar, const cl_maneuvers& ask, Reply& reply)
        {
            for (auto* PPawn : pawn::commandablePawns(PChar))
            {
                const auto* PController = dynamic_cast<const CPawnController*>(PPawn->PAI->GetController());
                if (PController != nullptr && PController->ManeuverComposed() && PController->ManeuverBy() == PChar->id)
                {
                    auto state    = make<cl_maneuver_state>();
                    state.cardian = PPawn->id;
                    state.state   = CL_MS_COMPOSED;
                    reply.more(state);
                }
            }

            auto answer = ask;
            if (const auto* PPawn = zoneutils::GetChar(pawn::maneuverOf(PChar->id)); PPawn != nullptr && PPawn->PAI != nullptr)
            {
                const auto* PController = dynamic_cast<const CPawnController*>(PPawn->PAI->GetController());
                answer.live             = PController != nullptr && PController->InManeuver() ? PPawn->id : 0;
            }
            reply.finish(answer, CL_S_OK);
        }
        // The party's standing orders as they stand (pawn.h, M3.9)
        auto ordersOf(const CCharEntity* PChar) -> cl_orders
        {
            const auto rules = pawn::huntRulesOf(PChar->id);
            const auto stake = pawn::stakeOf(PChar->id);

            auto msg       = make<cl_orders>();
            msg.strategy   = static_cast<uint8_t>(pawn::strategyOf(PChar->id));
            msg.strategies = static_cast<uint8_t>(pawn::kStrategyCount);
            msg.retreat    = pawn::isRetreating(PChar->id) ? 1 : 0;
            msg.huntMin    = rules.minCheck;
            msg.huntMax    = rules.maxCheck;
            msg.pull       = rules.pullFirst;
            msg.aggressive = rules.aggressive ? 1 : 0;
            msg.links      = rules.links ? 1 : 0;
            msg.staked     = stake.has_value() ? 1 : 0;
            msg.stakeZone  = stake.has_value() ? static_cast<uint16_t>(stake->zone) : 0;
            return msg;
        }

        void orders(CCharEntity* PChar, const cl_orders& /* ask */, Reply& reply)
        {
            reply.finish(ordersOf(PChar), CL_S_OK);
        }

        // A change to the orders: they come back as they now stand, then its
        // outcome, refused or not, so the card never keeps a guess
        template <typename T>
        void ordersChanged(CCharEntity* PChar, const T& ask, Reply& reply, const uint16 status)
        {
            reply.more(ordersOf(PChar));
            reply.finish(ask, status);
        }

        void setStrategy(CCharEntity* PChar, const cl_set_strategy& ask, Reply& reply)
        {
            const uint16 want = ask.mode == CL_STRATEGY_NEXT ? (pawn::strategyOf(PChar->id) + 1) % pawn::kStrategyCount : ask.strategy;
            if (ask.mode > CL_STRATEGY_NEXT || want >= pawn::kStrategyCount)
            {
                ordersChanged(PChar, ask, reply, CL_S_MALFORMED);
                return;
            }
            pawn::setStrategy(PChar, want);
            ordersChanged(PChar, ask, reply, CL_S_OK);
        }

        void setHunt(CCharEntity* PChar, const cl_set_hunt& ask, Reply& reply)
        {
            ordersChanged(PChar, ask, reply, pawn::setHuntRule(PChar, ask.rule, ask.value));
        }

        // "On me": set, cleared, or the other way round from how it stands
        void retreat(CCharEntity* PChar, const cl_retreat& ask, Reply& reply)
        {
            if (ask.mode > CL_SWITCH_TOGGLE)
            {
                ordersChanged(PChar, ask, reply, CL_S_MALFORMED);
                return;
            }
            const bool on = ask.mode == CL_SWITCH_ON || (ask.mode == CL_SWITCH_TOGGLE && !pawn::isRetreating(PChar->id));
            pawn::setRetreat(PChar, on);
            ordersChanged(PChar, ask, reply, CL_S_OK);
        }

        // The camp: set or moved where he stands, cleared, or toggled -- the
        // toggle decided here, so two presses before the first answer still
        // alternate
        void stake(CCharEntity* PChar, const cl_stake& ask, Reply& reply)
        {
            const bool clear = ask.mode == CL_STAKE_CLEAR || (ask.mode == CL_STAKE_TOGGLE && pawn::stakeOf(PChar->id).has_value());
            uint16     status = CL_S_OK;
            if (ask.mode > CL_STAKE_TOGGLE)
            {
                status = CL_S_MALFORMED;
            }
            else if (clear)
            {
                status = pawn::clearStake(PChar->id, "cleared") ? CL_S_OK : CL_S_NO_STAKE;
            }
            else
            {
                status = pawn::setStake(PChar);
            }
            ordersChanged(PChar, ask, reply, status);
        }

        // Every cardian of his in his zone fights his target
        void engage(CCharEntity* PChar, const cl_engage& ask, Reply& reply)
        {
            reply.finish(ask, pawn::partyEngage(PChar, ask.target));
        }

        // Wait here, or follow him. Follow from another zone is a travel
        // order to his: she treks the world to meet him.
        void wait(CCharEntity* PChar, const cl_wait& ask, Reply& reply)
        {
            auto* PPawn       = pawn::findCommandablePawn(PChar, ask.cardian);
            auto* PController = PPawn != nullptr ? dynamic_cast<CPawnController*>(PPawn->PAI->GetController()) : nullptr;
            if (PController == nullptr)
            {
                reply.finish(ask, CL_S_NO_SUCH_CARDIAN);
                return;
            }
            const bool on = ask.on != 0;
            PController->SetWaiting(on, true);
            if (on)
            {
                pawn::clearTravelOrder(PPawn->id);
                ShowInfoFmt("pawn: {} waits here (ordered)", PPawn->getName());
            }
            else if (PPawn->loc.zone != PChar->loc.zone)
            {
                ShowInfoFmt("pawn: {} sets out to meet {} in zone {}", PPawn->getName(), PChar->getName(), static_cast<uint16>(PChar->getZone()));
                pawn::orderTravel(PPawn->id, static_cast<uint16>(PChar->getZone()), PChar->id);
            }
            else
            {
                ShowInfoFmt("pawn: {} follows (ordered)", PPawn->getName());
            }
            reply.finish(ask, CL_S_OK);
        }

        // A stuck cardian to his side, within reach and off cooldown
        void rescue(CCharEntity* PChar, const cl_rescue& ask, Reply& reply)
        {
            auto* PPawn = pawn::findCommandablePawn(PChar, ask.cardian);
            if (PPawn == nullptr)
            {
                reply.finish(ask, CL_S_NO_SUCH_CARDIAN);
                return;
            }
            pawn::RescueRefusal refusal;
            const auto          status = pawn::rescue(PChar, PPawn, refusal);
            auto                answer = ask;
            answer.away                = refusal.away;
            answer.range               = refusal.range;
            answer.cooldownLeft        = refusal.cooldownLeft;
            reply.finish(answer, status);
        }

        // A KO'd cardian to his home point, where she waits
        void homePoint(CCharEntity* PChar, const cl_homepoint& ask, Reply& reply)
        {
            auto* PPawn = pawn::findCommandablePawn(PChar, ask.cardian);
            reply.finish(ask, PPawn != nullptr ? pawn::orderHomePoint(PChar, PPawn) : CL_S_NO_SUCH_CARDIAN);
        }

        // A queued command taken back: hers, or with no cardian named, his own
        // (the pause's input gate)
        void cancel(CCharEntity* PChar, const cl_cancel& ask, Reply& reply)
        {
            if (ask.cardian == 0)
            {
                reply.finish(ask, cardian::pause::input::cancel(PChar) ? CL_S_OK : CL_S_NOTHING_QUEUED);
                return;
            }
            auto* PPawn       = pawn::findCommandablePawn(PChar, ask.cardian);
            auto* PController = PPawn != nullptr ? dynamic_cast<CPawnController*>(PPawn->PAI->GetController()) : nullptr;
            if (PController == nullptr)
            {
                reply.finish(ask, CL_S_NO_SUCH_CARDIAN);
                return;
            }
            reply.finish(ask, PController->CancelQueuedOrder() ? CL_S_OK : CL_S_NOTHING_QUEUED);
        }

        // The command window: one action now, on a target index in her zone (0 =
        // herself), or held as her queued order (pawn_controller.h, DoAction). Her
        // items are hers to use only when she is his to manage
        void doAction(CCharEntity* PChar, const cl_do& ask, Reply& reply)
        {
            auto* PPawn       = pawn::findCommandablePawn(PChar, ask.cardian);
            auto* PController = PPawn != nullptr ? dynamic_cast<CPawnController*>(PPawn->PAI->GetController()) : nullptr;
            if (PController == nullptr)
            {
                reply.finish(ask, CL_S_NO_SUCH_CARDIAN);
                return;
            }
            const auto key = pawn::keyOfAction(ask.action);
            if (key.empty())
            {
                reply.finish(ask, CL_S_MALFORMED);
                return;
            }
            if (ask.action.kind == CL_AK_ITEM && pawn::findManagedPawn(PChar, ask.cardian) == nullptr)
            {
                reply.finish(ask, CL_S_NOT_MANAGED);
                return;
            }
            if (PPawn->loc.zone == nullptr)
            {
                reply.finish(ask, CL_S_OTHER_ZONE);
                return;
            }
            auto* PTarget = ask.target == 0 ? static_cast<CBattleEntity*>(PPawn)
                                            : dynamic_cast<CBattleEntity*>(PPawn->loc.zone->GetEntity(ask.target, TYPE_PC | TYPE_MOB | TYPE_NPC));
            if (PTarget == nullptr)
            {
                reply.finish(ask, CL_S_NO_TARGET);
                return;
            }

            uint16     wait   = 0; // never the packed field's own address
            const auto status = PController->DoAction(key, PTarget, &wait);
            if (status == CL_S_OK)
            {
                ShowInfoFmt("pawn: {} is ordered {} on {} by {}", PPawn->getName(), key, PTarget->getName(), PChar->getName());
            }
            auto answer = ask;
            answer.wait = wait;
            reply.finish(answer, status);
        }

        // His queue lines as they stand, for an addon that has just bound: his own
        // command waiting through a pause, then each of his cardians' orders
        void queues(CCharEntity* PChar, const cl_queues& ask, Reply& reply)
        {
            if (const auto own = cardian::pause::input::queueLine(PChar->id); own.action.kind != CL_AK_NONE)
            {
                reply.more(own);
            }
            for (auto* PPawn : pawn::commandablePawns(PChar))
            {
                const auto* PController = dynamic_cast<const CPawnController*>(PPawn->PAI->GetController());
                if (PController == nullptr)
                {
                    continue;
                }
                if (const auto line = PController->QueueLine(); line.action.kind != CL_AK_NONE)
                {
                    reply.more(line);
                }
            }
            reply.finish(ask, CL_S_OK);
        }

        // The Auction House screen shops for a member of his party: himself, by
        // his own charid, or a cardian of his to manage (a wild one's gear and
        // gil are the world's); nullptr for anyone else
        auto shopperOf(CCharEntity* PChar, const uint32 member) -> CCharEntity*
        {
            return member == PChar->id ? PChar : pawn::findManagedPawn(PChar, member);
        }

        // A shelf of the auction house for one member (pawn/auction.h), in
        // parts: each carries the request's own fields and its share of the rows
        void ahShelf(CCharEntity* PChar, const cl_ah_shelf& ask, Reply& reply)
        {
            constexpr std::size_t kMaxCategories = sizeof(cl_ah_shelf::categories);
            if (ask.kind > CL_SHELF_LEARNABLE || (ask.kind == CL_SHELF_SLOT && ask.slot > SLOT_BACK) ||
                (ask.kind != CL_SHELF_SLOT && (ask.count == 0 || ask.count > kMaxCategories)))
            {
                reply.finish(ask, CL_S_MALFORMED);
                return;
            }
            auto* PMember = shopperOf(PChar, ask.member);
            if (PMember == nullptr)
            {
                reply.finish(ask, CL_S_NO_SUCH_CARDIAN);
                return;
            }
            if (const auto status = pawn::auction::whereShopping(PChar, PMember); status != CL_S_OK)
            {
                reply.finish(ask, status);
                return;
            }

            std::vector<pawn::auction::Listing> listings;
            if (ask.kind == CL_SHELF_SLOT)
            {
                listings = pawn::auction::wearableAtAuction(PMember, ask.slot);
            }
            else
            {
                std::vector<uint8> categories;
                for (std::size_t i = 0; i < ask.count; ++i)
                {
                    if (ask.categories[i] != 0)
                    {
                        categories.push_back(ask.categories[i]);
                    }
                }
                listings = pawn::auction::inCategories(PMember, categories, ask.kind == CL_SHELF_LEARNABLE);
            }

            constexpr std::size_t kPerPart = sizeof(cl_ah_shelf::listings) / sizeof(cl_ah_listing);
            std::size_t           next     = 0;
            while (true)
            {
                auto part = ask;
                part.rows = 0;
                for (; part.rows < kPerPart && next < listings.size(); ++next)
                {
                    const auto& listing = listings[next];
                    auto&       row     = part.listings[part.rows++];
                    row                 = cl_ah_listing{};
                    row.item            = listing.itemId;
                    row.level           = listing.level;
                    row.category        = listing.category;
                    row.stock           = listing.stock;
                    row.going           = listing.going;
                    row.stack           = listing.stack ? 1 : 0;
                    row.stackSize       = static_cast<uint16_t>(std::min<uint32>(listing.stackSize, UINT16_MAX));
                }
                if (next < listings.size())
                {
                    reply.more(part);
                    continue;
                }
                reply.finish(part, CL_S_OK);
                return;
            }
        }

        // An item's stock, going rate and last sales in one form, as the game's
        // own auction house shows them (the buy panel)
        void ahHistory(CCharEntity* /* PChar */, const cl_ah_history& ask, Reply& reply)
        {
            const auto history = pawn::auction::history(ask.item, ask.stack != 0);
            auto       answer  = ask;
            answer.stock       = history.stock;
            answer.going       = history.going;
            answer.count       = 0;
            constexpr std::size_t kSales = sizeof(cl_ah_history::sales) / sizeof(cl_ah_sale);
            for (const auto& sale : history.sales)
            {
                if (answer.count >= kSales)
                {
                    break;
                }
                auto& row = answer.sales[answer.count++];
                row.date  = sale.date;
                row.price = sale.price;
                setText(row.seller, sale.seller);
                setText(row.buyer, sale.buyer);
            }
            reply.finish(answer, CL_S_OK);
        }

        // A bid for one piece or one stack (pawn::auction::bid), a cardian's
        // purse with his behind it
        void ahBid(CCharEntity* PChar, const cl_ah_bid& ask, Reply& reply)
        {
            auto* PMember = shopperOf(PChar, ask.member);
            if (PMember == nullptr)
            {
                reply.finish(ask, CL_S_NO_SUCH_CARDIAN);
                return;
            }
            if (const auto status = pawn::auction::whereShopping(PChar, PMember); status != CL_S_OK)
            {
                reply.finish(ask, status);
                return;
            }
            const auto result = pawn::auction::bid(PMember, PChar, ask.item, ask.stack != 0, ask.price, ask.bag, ask.slot, ask.equip != 0);
            auto       answer = ask;
            if (result.status == CL_S_OK)
            {
                answer.bag       = result.location;
                answer.equipped  = result.equipped ? 1 : 0;
                answer.notWorn   = result.notWorn;
                answer.fromPurse = result.fromPurse;
            }
            reply.finish(answer, result.status);
        }

        // His party's roster: every cardian he commands, by name, who is in a
        // zone (one between zones joins it once she lands), then what he himself
        // stands by
        void roster(CCharEntity* PChar, const cl_roster& ask, Reply& reply)
        {
            const auto reach  = reachOf(PChar);
            auto       answer = ask;
            answer.count      = 0;
            for (auto* PPawn : pawn::commandablePawns(PChar))
            {
                if (PPawn->loc.zone == nullptr)
                {
                    continue;
                }
                reply.more(memberOf(PChar, PPawn, reach, pawn::findManagedPawn(PChar, PPawn->id) != nullptr));
                answer.count = static_cast<uint8_t>(std::min<std::size_t>(answer.count + 1, UINT8_MAX));
            }
            answer.byCounter = reach.counter != nullptr ? 1 : 0;
            reply.finish(answer, CL_S_OK);
        }

        // The equipment screen's whole view of her: roster line, status pane,
        // gear and, his to manage, her inventory
        void sync(CCharEntity* PChar, const cl_sync& ask, Reply& reply)
        {
            auto* PPawn = pawn::findCommandablePawn(PChar, ask.cardian);
            if (PPawn == nullptr)
            {
                reply.finish(ask, CL_S_NO_SUCH_CARDIAN);
                return;
            }
            const bool managed = pawn::findManagedPawn(PChar, ask.cardian) != nullptr;
            reply.more(memberOf(PChar, PPawn, reachOf(PChar), managed));
            reply.more(statsOf(PPawn, managed));
            reply.more(gearOf(PPawn));
            if (managed)
            {
                reply.more(inventoryOf(PPawn, LOC_INVENTORY));
            }
            reply.finish(ask, CL_S_OK);
        }

        // One of her containers, asked for: the inventory or a bag (his to manage)
        void inventory(CCharEntity* PChar, const cl_inventory& ask, Reply& reply)
        {
            auto* PPawn = pawn::findManagedPawn(PChar, ask.cardian);
            if (PPawn == nullptr)
            {
                reply.finish(ask, CL_S_NO_SUCH_CARDIAN);
                return;
            }
            if (ask.loc > LOC_WARDROBE8)
            {
                reply.finish(ask, CL_S_MALFORMED);
                return;
            }
            reply.finish(inventoryOf(PPawn, ask.loc), CL_S_OK);
        }

        void bags(CCharEntity* PChar, const cl_bags& ask, Reply& reply)
        {
            auto* PPawn = pawn::findManagedPawn(PChar, ask.cardian);
            if (PPawn == nullptr)
            {
                reply.finish(ask, CL_S_NO_SUCH_CARDIAN);
                return;
            }
            reply.finish(bagsOf(PPawn), CL_S_OK);
        }

        // What she cannot do yet: the seconds left on every spell and ability
        // still on recast, by the action the command window lists it as
        void recasts(CCharEntity* PChar, const cl_recasts& ask, Reply& reply)
        {
            auto* PPawn = pawn::findCommandablePawn(PChar, ask.cardian);
            if (PPawn == nullptr)
            {
                reply.finish(ask, CL_S_NO_SUCH_CARDIAN);
                return;
            }
            std::vector<cl_recast> all;
            const auto             now  = timer::now();
            const auto             left = [&](const Recast_t& recast) -> float
            {
                auto remaining = (recast.TimeStamp + recast.RecastTime) - now;
                // A charged ability is usable while any charge is back, so only
                // the wait for the next charge counts: the recast holds every
                // spent charge's time end to end, and the ability is ready once
                // fewer than all but one remain (HasRecast)
                if (recast.chargeTime != 0s && recast.maxCharges > 0)
                {
                    remaining -= recast.chargeTime * (recast.maxCharges - 1);
                }
                return remaining > 0s ? std::chrono::duration<float>(remaining).count() : 0.0f;
            };
            const auto add = [&](const uint8_t kind, const uint16_t id, const float seconds)
            {
                if (seconds > 0.0f)
                {
                    all.push_back(cl_recast{ cl_action{ kind, 2, id }, seconds });
                }
            };

            if (auto* PList = PPawn->PRecastContainer->GetRecastList(RECAST_MAGIC); PList != nullptr)
            {
                for (const auto& recast : *PList)
                {
                    add(CL_AK_MAGIC, static_cast<uint16_t>(recast.ID), left(recast));
                }
            }
            // Abilities are stored by recast id, the command window lists them by
            // ability id, so they are matched through her own ability list
            if (auto* PList = PPawn->PRecastContainer->GetRecastList(RECAST_ABILITY); PList != nullptr)
            {
                for (auto* PAbility : pawn::abilitiesFor(PPawn))
                {
                    for (const auto& recast : *PList)
                    {
                        if (recast.ID == PAbility->getRecastId())
                        {
                            add(CL_AK_ABILITY, PAbility->getID(), left(recast));
                        }
                    }
                }
            }

            // In parts, each with its share: a shared recast puts every ability of
            // its family on the list
            constexpr std::size_t kPerPart = sizeof(cl_recasts::recasts) / sizeof(cl_recast);
            std::size_t           next     = 0;
            while (true)
            {
                auto part  = ask;
                part.count = 0;
                for (; part.count < kPerPart && next < all.size(); ++next)
                {
                    part.recasts[part.count++] = all[next];
                }
                if (next < all.size())
                {
                    reply.more(part);
                    continue;
                }
                reply.finish(part, CL_S_OK);
                return;
            }
        }

        // What the client's own Profile screen shows, for her
        void profile(CCharEntity* PChar, const cl_profile& ask, Reply& reply)
        {
            auto* PPawn = pawn::findCommandablePawn(PChar, ask.cardian);
            if (PPawn == nullptr)
            {
                reply.finish(ask, CL_S_NO_SUCH_CARDIAN);
                return;
            }
            const auto nation = std::min<uint8>(PPawn->profile.nation, 2);
            auto       answer = ask;
            answer.title      = PPawn->profile.title;
            answer.nation     = nation;
            answer.race       = static_cast<uint8_t>(PPawn->look.race);
            answer.rank       = PPawn->profile.rank[nation];
            answer.rankPoints = PPawn->profile.rankpoints;
            answer.homeZone   = static_cast<uint16_t>(PPawn->profile.home_point.destination);
            setZoneName(answer.homeName, zoneutils::GetZone(PPawn->profile.home_point.destination));
            reply.finish(answer, CL_S_OK);
        }

        // Her level in every job
        void jobs(CCharEntity* PChar, const cl_jobs& ask, Reply& reply)
        {
            auto* PPawn = pawn::findCommandablePawn(PChar, ask.cardian);
            if (PPawn == nullptr)
            {
                reply.finish(ask, CL_S_NO_SUCH_CARDIAN);
                return;
            }
            auto answer = ask;
            for (std::size_t job = 0; job < sizeof(cl_jobs::levels) && job < MAX_JOBTYPE; ++job)
            {
                answer.levels[job] = PPawn->jobs.job[job];
            }
            reply.finish(answer, CL_S_OK);
        }

        // The combat or the magic skills her jobs can raise, each at its level
        // and its cap at her level: the higher of main and support job
        void skills(CCharEntity* PChar, const cl_skills& ask, Reply& reply)
        {
            static constexpr std::array<uint8, 19> kCombat{ 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 25, 26, 27, 28, 29, 30, 31 };
            static constexpr std::array<uint8, 14> kMagic{ 32, 33, 34, 35, 36, 37, 38, 39, 40, 41, 42, 43, 44, 45 };

            auto* PPawn = pawn::findCommandablePawn(PChar, ask.cardian);
            if (PPawn == nullptr)
            {
                reply.finish(ask, CL_S_NO_SUCH_CARDIAN);
                return;
            }
            if (ask.kind > CL_SKILLS_MAGIC)
            {
                reply.finish(ask, CL_S_MALFORMED);
                return;
            }
            auto       answer   = ask;
            answer.count        = 0;
            const auto mainJob  = PPawn->GetMJob();
            const auto subJob   = PPawn->GetSJob();
            const auto mainLvl  = PPawn->GetMLevel();
            const auto subLvl   = PPawn->GetSLevel();
            const auto addSkill = [&](const uint8 skill)
            {
                const auto type = static_cast<xi::SkillType>(skill);
                uint16     cap  = battleutils::GetMaxSkill(type, mainJob, mainLvl);
                if (static_cast<uint8>(subJob) != 0 && subLvl > 0)
                {
                    cap = std::max(cap, battleutils::GetMaxSkill(type, subJob, subLvl));
                }
                if (cap > 0 && answer.count < sizeof(cl_skills::skills) / sizeof(cl_skill))
                {
                    const uint16 level          = std::min<uint16>(PPawn->RealSkills.skill[skill] / 10, cap);
                    answer.skills[answer.count++] = cl_skill{ skill, level, cap, 0 };
                }
            };
            if (ask.kind == CL_SKILLS_COMBAT)
            {
                std::for_each(kCombat.begin(), kCombat.end(), addSkill);
            }
            else
            {
                std::for_each(kMagic.begin(), kMagic.end(), addSkill);
            }
            reply.finish(answer, CL_S_OK);
        }
    } // namespace

    void tellInventory(CCharEntity* PPlayer, CCharEntity* PPawn, const uint8 location)
    {
        if (PPlayer != nullptr && PPawn != nullptr && location <= LOC_WARDROBE8)
        {
            cardian::link::send(PPlayer->id, inventoryOf(PPawn, location));
        }
    }

    void registerHandlers()
    {
        handle<cl_give>(give);
        handle<cl_take>(take);
        handle<cl_gil>(gil);
        handle<cl_equip>(equip);
        handle<cl_use>(use);
        handle<cl_drop>(drop);
        handle<cl_sort>(sortContainer);
        handle<cl_move>(moveStack);
        handle<cl_give_use>(giveUse);
        handle<cl_walk>(walk);
        handle<cl_view>(lookThrough);
        handle<cl_maneuver>(maneuver);
        handle<cl_maneuvers>(maneuvers);
        handle<cl_orders>(orders);
        handle<cl_set_strategy>(setStrategy);
        handle<cl_set_hunt>(setHunt);
        handle<cl_retreat>(retreat);
        handle<cl_stake>(stake);
        handle<cl_engage>(engage);
        handle<cl_wait>(wait);
        handle<cl_rescue>(rescue);
        handle<cl_homepoint>(homePoint);
        handle<cl_cancel>(cancel);
        handle<cl_do>(doAction);
        handle<cl_queues>(queues);
        handle<cl_ah_shelf>(ahShelf);
        handle<cl_ah_history>(ahHistory);
        handle<cl_ah_bid>(ahBid);
        handle<cl_roster>(roster);
        handle<cl_sync>(sync);
        handle<cl_inventory>(inventory);
        handle<cl_bags>(bags);
        handle<cl_recasts>(recasts);
        handle<cl_profile>(profile);
        handle<cl_jobs>(jobs);
        handle<cl_skills>(skills);
    }
} // namespace pawn::linkapi
