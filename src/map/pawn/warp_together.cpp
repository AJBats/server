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

#include "warp_together.h"

#include "action_keys.h"
#include "cardian_link_messages.h"
#include "offers.h"
#include "pawn.h"
#include "pawn_controller.h"

#include "ai/ai_container.h"
#include "ai/states/item_state.h"
#include "ai/states/magic_state.h"
#include "common/logging.h"
#include "common/settings.h"
#include "common/timer.h"
#include "entities/char_entity.h"
#include "enums/item_state.h"
#include "enums/packet_c2s.h"
#include "item_container.h"
#include "items/item.h"
#include "items/item_equipment.h"
#include "items/item_usable.h"
#include "items/transactions/item_claim.h"
#include "packets/basic.h"
#include "packets/c2s/0x01a_action.h"
#include "packets/c2s/0x037_item_use.h"
#include "pause/input_gate.h"
#include "recast_container.h"
#include "spell.h"
#include "status_effect_container.h"
#include "utils/charutils.h"
#include "utils/itemutils.h"
#include "utils/zoneutils.h"

#include <fmt/format.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace pawn::together
{
    namespace
    {
        using namespace std::chrono_literals;

        constexpr uint16 kInstantWarp    = 4181;
        constexpr uint16 kInstantReraise = 4182;
        constexpr uint16 kWarpRing       = 28540;
        constexpr uint16 kWarpCudgel     = 17040;

        // The containers gear is worn from, as the enchanted-item lane looks:
        // her inventory and the wardrobes
        constexpr std::array<uint8, 9> kWornFrom{ LOC_INVENTORY, LOC_WARDROBE, LOC_WARDROBE2, LOC_WARDROBE3, LOC_WARDROBE4,
                                                  LOC_WARDROBE5, LOC_WARDROBE6, LOC_WARDROBE7, LOC_WARDROBE8 };
        constexpr auto   kPatience       = std::chrono::seconds(30); // the party's warp's own
        constexpr auto   kStartGrace     = std::chrono::seconds(1);  // his warp's start, seen before it is judged
        constexpr auto   kLandWithin     = std::chrono::seconds(60); // their warps' landings, kept this long
        constexpr auto   kLookEvery      = std::chrono::milliseconds(250);

        // His warp, held while he answers: the packet as it came in
        struct Held
        {
            uint8                         how = CL_WAY_NONE; // his own way: the spell, the scroll, the ring or the cudgel
            std::unique_ptr<CBasicPacket> packet;
        };

        // A cardian sent home with him: her way, and its order's key (a piece
        // goes by her enchanted-item lane, its key naming the item)
        struct Follower
        {
            uint32      charid = 0;
            std::string key;
            uint8       way = CL_WAY_NONE;
        };

        // The party gone home on his answer: where they land, and his warp
        // watched until it has taken or come to nothing
        struct Going
        {
            location_t            home;
            xi::ZoneId            zone{};
            timer::time_point     since{};
            bool                  took = false;
            std::vector<Follower> followers;
        };

        std::unordered_map<uint32, Held>  held;  // by his charid
        std::unordered_map<uint32, Going> going; // by his charid
        timer::time_point                 lastLook{};

        auto nameOfWay(const uint8 way) -> std::string_view
        {
            switch (way)
            {
                case CL_WAY_SCROLL:
                    return "an Instant Warp";
                case CL_WAY_SPELL:
                    return "Warp";
                case CL_WAY_RING:
                    return "a Warp Ring";
                case CL_WAY_CUDGEL:
                    return "a Warp Cudgel";
                default:
                    return "nothing";
            }
        }

        // The item a way uses; 0 for the spell and for none
        auto itemOfWay(const uint8 way) -> uint16
        {
            switch (way)
            {
                case CL_WAY_SCROLL:
                    return kInstantWarp;
                case CL_WAY_RING:
                    return kWarpRing;
                case CL_WAY_CUDGEL:
                    return kWarpCudgel;
                default:
                    return 0;
            }
        }

        auto wayOfItem(const uint16 itemId) -> uint8
        {
            switch (itemId)
            {
                case kInstantWarp:
                    return CL_WAY_SCROLL;
                case kWarpRing:
                    return CL_WAY_RING;
                case kWarpCudgel:
                    return CL_WAY_CUDGEL;
                default:
                    return CL_WAY_NONE;
            }
        }

        auto isPiece(const uint8 way) -> bool
        {
            return way == CL_WAY_RING || way == CL_WAY_CUDGEL;
        }

        auto keyOfWay(const uint8 way) -> std::string
        {
            if (way == CL_WAY_SPELL)
            {
                return keyOfAction({ CL_AK_MAGIC, 2, static_cast<uint16_t>(SpellID::Warp) });
            }
            const uint16 itemId = itemOfWay(way);
            return itemId != 0 ? keyOfAction({ CL_AK_ITEM, 0, itemId }) : std::string();
        }

        // What lies in the slot an item-use packet names; 0 for nothing
        auto itemIn(CCharEntity* PChar, const CBasicPacket& packet) -> uint16
        {
            const auto* use     = packet.as<GP_CLI_COMMAND_ITEM_USE>();
            const auto* storage = PChar->getStorage(use->Category);
            const auto* PItem   = storage != nullptr ? storage->GetItem(use->PropertyItemIndex) : nullptr;
            return PItem != nullptr ? PItem->getID() : 0;
        }

        // The warp this packet of his starts: an Instant Warp read, a Warp
        // Ring or Warp Cudgel used, on himself, or Warp cast; CL_WAY_NONE for
        // anything else
        auto warpIn(CCharEntity* PChar, CBasicPacket& packet) -> uint8
        {
            if (packet.getType() == std::to_underlying(PacketC2S::GP_CLI_COMMAND_ITEM_USE))
            {
                const auto* use = packet.as<GP_CLI_COMMAND_ITEM_USE>();
                return use->ActIndex == PChar->targid ? wayOfItem(itemIn(PChar, packet)) : uint8{ CL_WAY_NONE };
            }
            if (packet.getType() == std::to_underlying(PacketC2S::GP_CLI_COMMAND_ACTION))
            {
                const auto* action = packet.as<GP_CLI_COMMAND_ACTION>();
                return action->ActionID == GP_CLI_COMMAND_ACTION_ACTIONID::CastMagic && action->CastMagic.SpellId == static_cast<uint32>(SpellID::Warp)
                           ? uint8{ CL_WAY_SPELL }
                           : uint8{ CL_WAY_NONE };
            }
            return CL_WAY_NONE;
        }

        // His piece as the game would let him use it now: worn, with a charge,
        // its delay after wearing it and its recast both over
        auto pieceReady(CCharEntity* PChar, CBasicPacket& packet) -> bool
        {
            const auto* use     = packet.as<GP_CLI_COMMAND_ITEM_USE>();
            const auto* storage = PChar->getStorage(use->Category);
            auto*       PItem   = storage != nullptr ? dynamic_cast<CItemUsable*>(storage->GetItem(use->PropertyItemIndex)) : nullptr;
            return PItem != nullptr && PItem->state() == ItemState::Equipped && PItem->getCurrentCharges() > 0 && PItem->getReuseTime() <= 0s;
        }

        // A piece of hers by its item, worn or not, in her inventory or a
        // wardrobe; nullptr when she has none
        auto pieceOf(CCharEntity* PPawn, const uint16 itemId) -> CItemEquipment*
        {
            for (const uint8 location : kWornFrom)
            {
                auto* storage = PPawn->getStorage(location);
                for (uint8 slot = 1; storage != nullptr && slot <= storage->GetSize(); ++slot)
                {
                    // busy as worn is still hers to use; busy in a trade is not
                    if (auto* PItem = dynamic_cast<CItemEquipment*>(storage->GetItem(slot));
                        PItem != nullptr && PItem->getID() == itemId && (!PItem->isBusy() || PItem->state() == ItemState::Equipped))
                    {
                        return PItem;
                    }
                }
            }
            return nullptr;
        }

        // Her main job wears it, at her level as the game judges it
        // (charutils::EquipArmor: her job's own level unless gear scaling is
        // off), and her race
        auto wears(CCharEntity* PPawn, const CItemEquipment* PItem) -> bool
        {
            const uint8 level = settings::get<bool>("map.DISABLE_GEAR_SCALING") ? PPawn->GetMLevel() : PPawn->jobs.job[static_cast<uint8>(PPawn->GetMJob())];
            return PItem->getReqLvl() <= level && (PItem->getJobs() & (1 << (static_cast<uint8>(PPawn->GetMJob()) - 1))) != 0 &&
                   PItem->isEquippableByRace(PPawn->look.race);
        }

        // A piece she can wear, by way: ready (a charge, off its recast; the
        // delay after wearing it is the lane's to wait out) or not
        enum class Piece : uint8
        {
            None,
            Ready,
            Spent,
        };
        auto pieceFor(CCharEntity* PPawn, const uint8 way) -> Piece
        {
            auto* PItem = pieceOf(PPawn, itemOfWay(way));
            if (PItem == nullptr || !wears(PPawn, PItem))
            {
                return Piece::None;
            }
            return PItem->getCurrentCharges() > 0 && PItem->getLastUseTime() + PItem->getReuseDelay() <= timer::now() ? Piece::Ready : Piece::Spent;
        }

        // A scroll in her inventory her order can use: as the order finds one,
        // not tied up in a trade
        auto carriesScroll(CCharEntity* PPawn) -> bool
        {
            const auto* storage = PPawn->getStorage(LOC_INVENTORY);
            for (uint8 slot = 1; storage != nullptr && slot <= storage->GetSize(); ++slot)
            {
                const auto* PItem = storage->GetItem(slot);
                if (PItem != nullptr && PItem->getID() == kInstantWarp && PItem->getQuantity() > 0 && !PItem->isBusy())
                {
                    return true;
                }
            }
            return false;
        }

        // She knows Warp, and her jobs cast it
        auto knowsWarp(CCharEntity* PPawn) -> bool
        {
            return charutils::hasSpell(PPawn, static_cast<uint16>(SpellID::Warp)) != 0 && spell::CanUseSpell(PPawn, SpellID::Warp);
        }

        // She could cast Warp now: she knows it, her job casts it, she has the
        // MP, she can speak, and it is off its recast
        auto castsWarp(CCharEntity* PPawn) -> bool
        {
            const CSpell* PSpell = spell::GetSpell(SpellID::Warp);
            return PSpell != nullptr && knowsWarp(PPawn) && PPawn->health.mp >= PSpell->getMPCost() &&
                   !PPawn->StatusEffectContainer->HasStatusEffect({ xi::StatusEffect::Silence, xi::StatusEffect::Mute }) &&
                   !PPawn->PRecastContainer->HasRecast(RECAST_MAGIC, static_cast<Recast>(SpellID::Warp), 0s);
        }

        // Every way she has, a bit for each (cl_offer_member's ways): staying
        // behind always among them
        auto waysOf(CCharEntity* PPawn) -> uint8
        {
            uint8 ways = 1 << CL_WAY_NONE;
            if (castsWarp(PPawn))
            {
                ways |= 1 << CL_WAY_SPELL;
            }
            for (const uint8 way : { uint8{ CL_WAY_RING }, uint8{ CL_WAY_CUDGEL } })
            {
                if (pieceFor(PPawn, way) == Piece::Ready)
                {
                    ways |= 1 << way;
                }
            }
            if (carriesScroll(PPawn))
            {
                ways |= 1 << CL_WAY_SCROLL;
            }
            return ways;
        }

        // Her way home: her own spell first, which costs only MP, then a ring
        // or a cudgel, which recharge, then a scroll, which a use spends
        auto wayOf(CCharEntity* PPawn) -> uint8
        {
            const uint8 ways = waysOf(PPawn);
            for (const uint8 way : { uint8{ CL_WAY_SPELL }, uint8{ CL_WAY_RING }, uint8{ CL_WAY_CUDGEL }, uint8{ CL_WAY_SCROLL } })
            {
                if ((ways & (1 << way)) != 0)
                {
                    return way;
                }
            }
            return CL_WAY_NONE;
        }

        // Her piece of a way as the question shows it: its charges and the
        // seconds until its recast is over; nothing for a piece she has not
        // or cannot wear
        auto pieceState(CCharEntity* PPawn, const uint8 way) -> offers::Offer::Piece
        {
            auto* PItem = pieceOf(PPawn, itemOfWay(way));
            if (PItem == nullptr || !wears(PPawn, PItem))
            {
                return {};
            }
            const auto left = PItem->getLastUseTime() + PItem->getReuseDelay() - timer::now();
            return { PItem->getCurrentCharges(), static_cast<uint16>(std::clamp<int64>(std::chrono::ceil<std::chrono::seconds>(left).count(), 0, UINT16_MAX)) };
        }

        // The ways she has but cannot use now, the same bits: her Warp short
        // of MP, silenced or on its recast; a piece on its recast or out of
        // charges
        auto barredOf(CCharEntity* PPawn) -> uint8
        {
            uint8 barred = knowsWarp(PPawn) && !castsWarp(PPawn) ? uint8{ 1 << CL_WAY_SPELL } : uint8{ 0 };
            for (const uint8 way : { uint8{ CL_WAY_RING }, uint8{ CL_WAY_CUDGEL } })
            {
                if (pieceFor(PPawn, way) == Piece::Spent)
                {
                    barred |= 1 << way;
                }
            }
            return barred;
        }

        // The cardians of his party beside him: his zone, alive
        auto beside(const CCharEntity* PPlayer) -> std::vector<CCharEntity*>
        {
            std::vector<CCharEntity*> out;
            for (auto* PPawn : commandablePawns(PPlayer))
            {
                if (PPawn->PParty != nullptr && PPawn->PParty == PPlayer->PParty && PPawn->loc.zone == PPlayer->loc.zone && !PPawn->isDead())
                {
                    out.push_back(PPawn);
                }
            }
            return out;
        }

        // Casting Warp, or using an Instant Warp, a Warp Ring or a Warp Cudgel,
        // now: a warp's action under way, not any other cast or item
        auto warping(CCharEntity* PChar) -> bool
        {
            if (const auto* PMagic = dynamic_cast<const CMagicState*>(PChar->PAI->GetCurrentState()); PMagic != nullptr)
            {
                return PMagic->GetSpell() != nullptr && PMagic->GetSpell()->getID() == SpellID::Warp;
            }
            if (const auto* PUse = dynamic_cast<const CItemState*>(PChar->PAI->GetCurrentState()); PUse != nullptr)
            {
                return PUse->GetItem() != nullptr && wayOfItem(PUse->GetItem()->getID()) != CL_WAY_NONE;
            }
            return false;
        }

        // His warp has taken: under way to his home point, or gone
        auto taken(const CCharEntity* PPlayer, const Going& g) -> bool
        {
            return PPlayer == nullptr || PPlayer->loc.zone == nullptr || PPlayer->getZone() != g.zone || PPlayer->requestedWarp != WarpRequest::None ||
                   PPlayer->StatusEffectContainer->HasStatusEffect(xi::StatusEffect::Teleport);
        }

        // The gate's ask: his warp held, and the question put
        auto claim(CCharEntity* PChar, CBasicPacket& packet) -> bool
        {
            if (PChar->PSession == nullptr || PChar->loc.zone == nullptr || isPawn(PChar))
            {
                return false;
            }
            const uint8 how = warpIn(PChar, packet);
            if (how == CL_WAY_NONE)
            {
                return false;
            }
            // A warp of his already under way, or answered and not yet gone,
            // is not asked about again; a Warp he cannot cast now, or a piece
            // he cannot use yet, is the game's to refuse at once
            const auto underWay = going.find(PChar->id);
            if (warping(PChar) || PChar->requestedWarp != WarpRequest::None || PChar->StatusEffectContainer->HasStatusEffect(xi::StatusEffect::Teleport) ||
                (underWay != going.end() && !underWay->second.took) || (how == CL_WAY_SPELL && !castsWarp(PChar)) || (isPiece(how) && !pieceReady(PChar, packet)))
            {
                return false;
            }
            const auto party = beside(PChar);
            if (party.empty())
            {
                return false;
            }

            offers::Offer offer;
            offer.kind = CL_OFFER_WARP_TOGETHER;
            offer.own  = how;
            std::string list;
            for (auto* PPawn : party)
            {
                const uint8 way = wayOf(PPawn);
                offer.members.push_back(PPawn->id);
                offer.ways.push_back(way);
                offer.held.push_back(waysOf(PPawn));
                offer.barred.push_back(barredOf(PPawn));
                offer.pieces.push_back({ pieceState(PPawn, CL_WAY_RING), pieceState(PPawn, CL_WAY_CUDGEL) });
                list += fmt::format("{}{} ({})", list.empty() ? "" : ", ", PPawn->getName(), nameOfWay(way));
            }
            if (!offers::put(PChar, std::move(offer), kPatience))
            {
                return false;
            }
            held.insert_or_assign(PChar->id, Held{ how, packet.copy() });
            ShowInfoFmt("together: {} would warp by {}, and is asked how the party follows: {}", PChar->getName(), nameOfWay(how), list);
            return true;
        }

        // One way home started for her: her line given up for it only once it
        // has (her lane runs ahead of the line; an order must be its first).
        // A piece's lane already on its way with that piece is kept: started
        // afresh, the piece would come off and its delay start over. She gets
        // up from a rest for a piece, as an order stands her up
        auto startWay(CCharEntity* PPawn, CPawnController* PController, const uint8 way) -> uint16
        {
            if (!isPiece(way))
            {
                PController->ClearQueuedOrders("she warps home with the player");
                return PController->DoAction(keyOfWay(way), PPawn);
            }
            const auto* PItem = pieceOf(PPawn, itemOfWay(way));
            if (PItem == nullptr)
            {
                return CL_S_NOT_CARRIED;
            }
            uint16 status = CL_S_OK;
            if (const auto lane = PController->QueueLine().lane.action; lane.kind != CL_AK_ITEM || lane.id != itemOfWay(way))
            {
                std::ignore = PController->CallOffRest();
                status      = PController->StartEnchant(PItem->getLocationID(), PItem->getSlotID());
                if (status == CL_S_ENCHANT_BUSY)
                {
                    PController->EndEnchant("she warps home with the player");
                    status = PController->StartEnchant(PItem->getLocationID(), PItem->getSlotID());
                }
            }
            if (status == CL_S_OK)
            {
                PController->ClearQueuedOrders("she warps home with the player");
            }
            return status;
        }

        // Each cardian beside him with a way home is ordered to use the one
        // he picked, ahead of everything in her line; one that will not start,
        // or a pick she no longer has, gives way to her next best
        auto sendHome(CCharEntity* PPlayer, const offers::Offer& offer) -> std::vector<Follower>
        {
            std::vector<Follower> out;
            const auto            party = beside(PPlayer);
            for (std::size_t i = 0; i < offer.members.size(); ++i)
            {
                const uint32 charid = offer.members[i];
                const uint8  picked = i < offer.ways.size() ? offer.ways[i] : uint8{ CL_WAY_NONE };
                const auto it = std::ranges::find_if(party, [&](const CCharEntity* PPawn)
                                                     {
                                                         return PPawn->id == charid;
                                                     });
                if (it == party.end())
                {
                    ShowInfoFmt("together: {} is no longer beside {}, and stays", charid, PPlayer->getName());
                    continue;
                }
                auto* PPawn       = *it;
                auto* PController = dynamic_cast<CPawnController*>(PPawn->PAI->GetController());
                if (picked == CL_WAY_NONE)
                {
                    ShowInfoFmt("together: {} stays behind, as {} picked", PPawn->getName(), PPlayer->getName());
                    continue;
                }
                if (PController == nullptr)
                {
                    continue;
                }
                // his pick first while she has it, then the rest of hers in
                // the server's order
                const uint8        have = waysOf(PPawn);
                std::vector<uint8> tries;
                if (picked <= CL_WAY_CUDGEL && (have & (1 << picked)) != 0)
                {
                    tries.push_back(picked);
                }
                else
                {
                    ShowInfoFmt("together: {} no longer has {}", PPawn->getName(), nameOfWay(picked));
                }
                for (const uint8 way : { uint8{ CL_WAY_SPELL }, uint8{ CL_WAY_RING }, uint8{ CL_WAY_CUDGEL }, uint8{ CL_WAY_SCROLL } })
                {
                    if (way != picked && (have & (1 << way)) != 0)
                    {
                        tries.push_back(way);
                    }
                }
                std::optional<uint8> went;
                for (const uint8 way : tries)
                {
                    if (const auto status = startWay(PPawn, PController, way); status != CL_S_OK)
                    {
                        ShowInfoFmt("together: {} cannot use {} ({})", PPawn->getName(), nameOfWay(way), status);
                        continue;
                    }
                    went = way;
                    break;
                }
                if (!went.has_value())
                {
                    ShowInfoFmt("together: {} has no way home that starts, and stays", PPawn->getName());
                    continue;
                }
                ShowInfoFmt("together: {} warps home with {} by {}", PPawn->getName(), PPlayer->getName(), nameOfWay(*went));
                out.push_back(Follower{ PPawn->id, keyOfWay(*went), *went });
            }
            return out;
        }

        // His warp came to nothing: hers is stopped while she reads or casts
        // it, and let go of while it waits in her line. One whose warp has
        // already taken cannot be stopped: those are returned, to land at his
        // home point all the same
        auto callOff(const CCharEntity* PPlayer, const Going& g) -> std::vector<Follower>
        {
            std::vector<Follower> gone;
            for (const auto& follower : g.followers)
            {
                auto* PPawn       = findPawn(follower.charid);
                auto* PController = PPawn != nullptr ? dynamic_cast<CPawnController*>(PPawn->PAI->GetController()) : nullptr;
                if (PController == nullptr)
                {
                    continue;
                }
                if (PPawn->requestedWarp != WarpRequest::None || PPawn->StatusEffectContainer->HasStatusEffect(xi::StatusEffect::Teleport))
                {
                    ShowInfoFmt("together: {}'s warp has taken already, and she lands at his home point", PPawn->getName());
                    gone.push_back(follower);
                    continue;
                }
                const auto line = PController->QueueLine();
                if (isPiece(follower.way))
                {
                    // her lane: a use under way stopped, and the piece it
                    // replaced put back
                    if (line.lane.action.kind == CL_AK_ITEM && line.lane.action.id == itemOfWay(follower.way))
                    {
                        if (line.lane.action.mode == 1)
                        {
                            PPawn->PAI->InterruptStates();
                        }
                        PController->EndEnchant("the player's warp came to nothing");
                    }
                    continue;
                }
                const auto order = actionOfKey(follower.key);
                const auto same  = [&](const cl_action& action)
                {
                    return action.kind == order.kind && action.id == order.id;
                };
                if (same(line.running.action))
                {
                    ShowInfoFmt("together: {}'s warp is stopped: {}'s came to nothing", PPawn->getName(), PPlayer != nullptr ? PPlayer->getName() : "the player");
                    PPawn->PAI->InterruptStates();
                }
                else if (same(line.action))
                {
                    PController->ClearQueuedOrders("the player's warp came to nothing");
                }
            }
            return gone;
        }

        void resolve(CCharEntity* PPlayer, const offers::Offer& offer, const uint8 choice)
        {
            auto node = held.extract(PPlayer->id);
            if (node.empty())
            {
                ShowErrorFmt("together: {}'s answer finds no warp of his held", PPlayer->getName());
                return;
            }
            Held h = std::move(node.mapped());
            if (choice != 1 && choice != 2)
            {
                ShowInfoFmt("together: {} calls his warp off; nothing is spent", PPlayer->getName());
                return;
            }
            // His bag can be sorted while he answers: the slot must still hold
            // the item he used
            if (h.how != CL_WAY_SPELL && itemIn(PPlayer, *h.packet) != itemOfWay(h.how))
            {
                ShowInfoFmt("together: {}'s warp is dropped, the slot of {} holds something else now", PPlayer->getName(), nameOfWay(h.how));
                return;
            }

            const auto home = PPlayer->profile.home_point;
            cardian::pause::input::handOn(PPlayer, *h.packet);
            if (choice != 1)
            {
                ShowInfoFmt("together: {} warps alone", PPlayer->getName());
                return;
            }
            // Refused at once (the game's own word to him): nobody is sent
            if (!timer::is_held() && !warping(PPlayer) && !taken(PPlayer, Going{ .zone = PPlayer->getZone() }))
            {
                ShowInfoFmt("together: {}'s warp did not start; the party stays", PPlayer->getName());
                return;
            }
            auto followers = sendHome(PPlayer, offer);
            if (followers.empty())
            {
                return;
            }
            going.insert_or_assign(PPlayer->id, Going{ .home = home, .zone = PPlayer->getZone(), .since = timer::now(), .followers = std::move(followers) });
        }
    } // namespace

    void init()
    {
        cardian::pause::input::setAsk(claim);
        offers::setResolver(CL_OFFER_WARP_TOGETHER, resolve);
    }

    auto landingFor(const CCharEntity* PPawn) -> std::optional<location_t>
    {
        for (auto& [player, g] : going)
        {
            const auto it = std::ranges::find(g.followers, PPawn->id, &Follower::charid);
            if (it != g.followers.end())
            {
                g.followers.erase(it);
                return g.home;
            }
        }
        return std::nullopt;
    }

    void tick()
    {
        const auto now = timer::now();
        if (going.empty() || now - lastLook < kLookEvery)
        {
            return;
        }
        lastLook = now;

        for (auto it = going.begin(); it != going.end();)
        {
            auto& [charid, g] = *it;
            if (g.took && g.followers.empty())
            {
                it = going.erase(it);
                continue;
            }
            if (now - g.since > kLandWithin)
            {
                for (const auto& follower : g.followers)
                {
                    ShowInfoFmt("together: {} never warped after {}", follower.charid, charid);
                }
                it = going.erase(it);
                continue;
            }
            if (g.took)
            {
                ++it;
                continue;
            }
            auto* PPlayer = zoneutils::GetChar(charid);
            if (taken(PPlayer, g))
            {
                g.took = true;
                ++it;
                continue;
            }
            // Judged only on the game's own time, a moment after it started
            if (timer::is_held() || now - g.since < kStartGrace || warping(PPlayer))
            {
                ++it;
                continue;
            }
            ShowInfoFmt("together: {}'s warp came to nothing; the party's are called off", PPlayer->getName());
            if (auto gone = callOff(PPlayer, g); !gone.empty())
            {
                g.followers = std::move(gone);
                g.took      = true;
                ++it;
                continue;
            }
            it = going.erase(it);
        }
    }

    auto topUpKit(CCharEntity* PPawn) -> uint32
    {
        // A bag with no room is said once, until the kit lands again
        static auto& full  = *new std::unordered_set<uint32>();
        uint32       given = 0;
        for (const uint16 itemId : std::array{ kInstantWarp, kInstantReraise })
        {
            // Rare: one anywhere in her bags is all she may hold
            if (charutils::HasItem(PPawn, itemId))
            {
                continue;
            }
            auto transaction = ItemClaimTransaction::start(PPawn);
            if (!transaction)
            {
                continue;
            }
            if (const auto landed = transaction->give(LOC_INVENTORY, itemId, 1, Silence::Yes); !landed.has_value() || !transaction->commit())
            {
                if (full.insert(PPawn->id).second)
                {
                    ShowWarningFmt("world: {} has no room in her bag for her scrolls; not topped up", PPawn->getName());
                }
                continue;
            }
            full.erase(PPawn->id);
            ++given;
            const auto* PKind = xi::items::lookup(itemId);
            ShowInfoFmt("world: {} is given her {}", PPawn->getName(), PKind != nullptr ? PKind->getName() : std::to_string(itemId));
        }
        return given;
    }
} // namespace pawn::together
