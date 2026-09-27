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

#include "cardian_link.h"
#include "pawn.h"
#include "pawn_controller.h"
#include "pawn_items.h"
#include "view.h"

#include "ai/ai_container.h"
#include "common/logging.h"
#include "entities/char_entity.h"
#include "enums/item_state.h"
#include "item_container.h"
#include "items/item.h"
#include "navmesh/navmesh.h"
#include "pause/input_gate.h"
#include "pause/pause.h"
#include "utils/zoneutils.h"
#include "zone.h"

#include <cmath>

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

        // A stack from his inventory to hers: her inventory as it now stands,
        // then the outcome
        void give(CCharEntity* PChar, const cl_give& ask, Reply& reply)
        {
            auto* PPawn = pawn::findManagedPawn(PChar, ask.cardian);
            if (PPawn == nullptr)
            {
                reply.finish(ask, CL_S_NO_SUCH_CARDIAN);
                return;
            }

            const auto status = pawn::items::giveToPawn(PChar, PPawn, ask.slot, ask.qty);
            if (status == CL_S_OK)
            {
                reply.more(inventoryOf(PPawn, LOC_INVENTORY));
            }
            reply.finish(ask, status);
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
    } // namespace

    void registerHandlers()
    {
        handle<cl_give>(give);
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
    }
} // namespace pawn::linkapi
