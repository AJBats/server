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
#include "engage_math.h"
#include "gambit_wire.h"
#include "gate_guards.h"
#include "live_controller.h"
#include "party_finder.h"
#include "party_roster.h"
#include "pawn_gambits.h"
#include "pawn.h"
#include "pawn_controller.h"
#include "pawn_items.h"
#include "view.h"

#include "ability.h"
#include "ai/ai_container.h"
#include "common/logging.h"
#include "common/settings.h"
#include "entities/char_entity.h"
#include "enums/item_flag.h"
#include "items/item_linkshell.h"
#include "lua/lua_base_entity.h"
#include "lua/luautils.h"
#include "enums/item_state.h"
#include "item_container.h"
#include "items/item.h"
#include "navmesh/navmesh.h"
#include "packets/s2c/0x01d_item_same.h"
#include "packets/s2c/0x020_item_attr.h"
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
#include <map>
#include <optional>
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
            auto       stats   = make<cl_member_stats>();
            const auto numbers = pawn::statusNumbers(PPawn);
            stats.cardian      = PPawn->id;
            for (std::size_t i = 0; i < numbers.total.size(); ++i)
            {
                stats.total[i] = numbers.total[i];
                stats.bonus[i] = numbers.bonus[i];
            }
            stats.attack  = numbers.attack;
            stats.defence = numbers.defence;
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

        // The character a request to manage names: a cardian of his, or
        // himself. His own gear and bags are driven over the Link as hers
        // are (ROADMAP, the road to subjob item 5), by the calls his
        // client's own packets make
        auto managedOrSelf(CCharEntity* PChar, const uint32 id) -> CCharEntity*
        {
            return id == PChar->id ? PChar : pawn::findManagedPawn(PChar, id);
        }

        // What the game's own item handlers refuse him (packets/c2s/validation.cpp:
        // in an event, or not standing as normal -- mounted, zoning), the
        // Link refuses him too
        auto refusesOwn(const CCharEntity* PChar) -> bool
        {
            return PChar->isInEvent() || PChar->status != xi::Status::Normal;
        }

        // A change to his own bags is told to his client as the game's own
        // handlers tell it: every slot of each container touched, the close,
        // then his worn slots asserted again, since a sort re-slots a bag
        // and a move carries a worn piece. A change made through an item
        // transaction tells itself; those two do not
        void tellClient(CCharEntity* PChar, const std::set<uint8>& locations)
        {
            for (const auto location : locations)
            {
                auto* PContainer = PChar->getStorage(location);
                if (PContainer == nullptr)
                {
                    continue;
                }
                for (uint8 slot = 1; slot <= PContainer->GetSize(); ++slot)
                {
                    PChar->pushPacket<GP_SERV_COMMAND_ITEM_ATTR>(PContainer->GetItem(slot), static_cast<CONTAINER_ID>(location), slot);
                }
            }
            PChar->pushPacket<GP_SERV_COMMAND_ITEM_SAME>(PChar);
            PChar->resyncEquipment();
        }

        // His own drop, as the game's own handler drops (packets/c2s/0x028):
        // a storage slip that holds gear is refused, a linkshell is refused
        // here rather than broken from a menu, and an ordinary item in his
        // inventory goes to the recycle bin when the server keeps one. Both
        // ways tell his client themselves
        auto dropOwn(CCharEntity* PChar, const cl_drop& ask) -> uint16
        {
            if (ask.bag != LOC_INVENTORY)
            {
                return CL_S_INVENTORY_ONLY;
            }
            auto*  PContainer = PChar->getStorage(ask.bag);
            CItem* PItem      = PContainer != nullptr ? PContainer->GetItem(ask.slot) : nullptr;
            if (PItem == nullptr || PItem->getQuantity() == 0)
            {
                return CL_S_NO_ITEM;
            }
            if (PItem->isType(ITEM_CURRENCY))
            {
                return CL_S_GIL_NOT_AN_ITEM;
            }
            if (PItem->isBusy())
            {
                return CL_S_ITEM_BUSY;
            }
            if (ask.qty == 0 || ask.qty > PItem->getQuantity())
            {
                return CL_S_BAD_QUANTITY;
            }
            if (PItem->isStorageSlip())
            {
                int slipData = 0;
                for (int i = 0; i < CItem::extra_size; ++i)
                {
                    slipData += PItem->m_extra[i];
                }
                if (slipData != 0)
                {
                    return CL_S_REFUSED;
                }
            }
            if (dynamic_cast<CItemLinkshell*>(PItem) != nullptr)
            {
                return CL_S_REFUSED;
            }

            const uint32 before = PItem->getQuantity();
            if (!settings::get<bool>("map.ENABLE_ITEM_RECYCLE_BIN") || PItem->hasFlag(ItemFlag::NoRecycle))
            {
                charutils::DropItem(PChar, ask.bag, ask.slot, static_cast<int32>(ask.qty), PItem->getID());
            }
            else
            {
                charutils::AddItemToRecycleBin(PChar, ask.bag, ask.slot, static_cast<uint8>(ask.qty));
            }
            const CItem* PAfter = PContainer->GetItem(ask.slot);
            if (PAfter != nullptr && PAfter->getQuantity() == before)
            {
                return CL_S_REFUSED;
            }
            return CL_S_OK;
        }

        // A change to the items or gear of a cardian of his to manage -- or,
        // given ownTouched, of his own, in which case his client is told of
        // the containers it names afterwards. Made by change(PPawn, partly),
        // which returns its outcome and sets partly when a refusal came after
        // part of it had moved; what it moved, told by moved(PPawn) as
        // answers when it moved anything; then its outcome
        template <typename Message, typename Change, typename Moved>
        void changeItems(CCharEntity* PChar, const Message& ask, Reply& reply, const std::optional<std::set<uint8>>& ownTouched, Change&& change, Moved&& moved)
        {
            auto* PPawn = ownTouched.has_value() ? managedOrSelf(PChar, ask.cardian) : pawn::findManagedPawn(PChar, ask.cardian);
            if (PPawn == nullptr)
            {
                reply.finish(ask, CL_S_NO_SUCH_CARDIAN);
                return;
            }
            if (PPawn == PChar && refusesOwn(PChar))
            {
                reply.finish(ask, CL_S_REFUSED);
                return;
            }
            bool       partly = false;
            const auto status = change(PPawn, partly);
            if (status == CL_S_OK || partly)
            {
                moved(PPawn);
                if (PPawn == PChar && !ownTouched->empty())
                {
                    tellClient(PChar, *ownTouched);
                }
            }
            reply.finish(ask, status);
        }

        // A stack from his inventory to hers, or back
        void give(CCharEntity* PChar, const cl_give& ask, Reply& reply)
        {
            changeItems(
                PChar, ask, reply, std::nullopt, [&](CCharEntity* PPawn, bool&) { return pawn::items::giveToPawn(PChar, PPawn, ask.slot, ask.qty); },
                [&](CCharEntity* PPawn) { reply.more(inventoryOf(PPawn, LOC_INVENTORY)); });
        }

        void take(CCharEntity* PChar, const cl_take& ask, Reply& reply)
        {
            changeItems(
                PChar, ask, reply, std::nullopt, [&](CCharEntity* PPawn, bool&) { return pawn::items::takeFromPawn(PChar, PPawn, ask.slot, ask.qty); },
                [&](CCharEntity* PPawn) { reply.more(inventoryOf(PPawn, LOC_INVENTORY)); });
        }

        // The trade window's gil line: her status pane carries her new purse,
        // his client counts his own
        void gil(CCharEntity* PChar, const cl_gil& ask, Reply& reply)
        {
            changeItems(
                PChar, ask, reply, std::nullopt, [&](CCharEntity* PPawn, bool&) { return pawn::items::moveGil(PChar, PPawn, ask.amount, ask.toHer != 0); },
                [&](CCharEntity* PPawn) { reply.more(statsOf(PPawn, true)); });
        }

        // She uses an item on herself: the stack thins when the use completes,
        // so the outcome is all there is to tell now. His own item use is his
        // client's (own.lua: a chat line), never this
        void use(CCharEntity* PChar, const cl_use& ask, Reply& reply)
        {
            changeItems(
                PChar, ask, reply, std::nullopt, [&](CCharEntity* PPawn, bool&) { return pawn::items::useItem(PPawn, ask.slot, ask.bag); },
                [](CCharEntity*) {});
        }

        // His own drop tells his client itself (dropOwn), so nothing to re-send
        void drop(CCharEntity* PChar, const cl_drop& ask, Reply& reply)
        {
            changeItems(
                PChar, ask, reply, std::set<uint8>{},
                [&](CCharEntity* PPawn, bool&) { return PPawn == PChar ? dropOwn(PChar, ask) : pawn::items::dropItem(PPawn, ask.slot, ask.qty, ask.bag); },
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
                PChar, ask, reply, std::set<uint8>{ ask.bag },
                [&](CCharEntity* PPawn, bool&) { return pawn::items::sortBag(PPawn, ask.bag); },
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
                PChar, ask, reply, std::set<uint8>{ ask.from, ask.to },
                [&](CCharEntity* PPawn, bool& partly) { return pawn::items::moveItem(PPawn, ask.from, ask.slot, ask.to, ask.qty, &partly); },
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
            auto* PPawn = managedOrSelf(PChar, ask.cardian);
            if (PPawn == nullptr)
            {
                reply.finish(ask, CL_S_NO_SUCH_CARDIAN);
                return;
            }
            if (PPawn == PChar && refusesOwn(PChar))
            {
                reply.finish(ask, CL_S_REFUSED);
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

        // ---- gambits (the gambit editor, M3.85) ----------------------------

        static_assert(static_cast<uint8>(cardian::tactician::State::Order) == CL_GS_ORDER && static_cast<uint8>(cardian::tactician::State::Line) == CL_GS_LINE &&
                      static_cast<uint8>(cardian::tactician::State::Allows) == CL_GS_ALLOWS && static_cast<uint8>(cardian::tactician::State::NotBelow) == CL_GS_NOT_BELOW &&
                      static_cast<uint8>(cardian::tactician::State::Clock) == CL_GS_CLOCK && static_cast<uint8>(cardian::tactician::State::NoChoice) == CL_GS_NO_CHOICE &&
                      static_cast<uint8>(cardian::tactician::State::Misfit) == CL_GS_MISFIT && static_cast<uint8>(cardian::tactician::State::Client) == CL_GS_CLIENT,
                      "a row's state crosses as its number");
        static_assert(static_cast<uint8>(pawn::Side::Self) == CL_SIDE_SELF && static_cast<uint8>(pawn::Side::Ally) == CL_SIDE_ALLY && static_cast<uint8>(pawn::Side::Foe) == CL_SIDE_FOE);
        static_assert(static_cast<uint8>(pawn::Takes::Nothing) == CL_VC_NOTHING && static_cast<uint8>(pawn::Takes::Number) == CL_VC_NUMBER &&
                      static_cast<uint8>(pawn::Takes::Status) == CL_VC_STATUS);
        static_assert(static_cast<uint8>(pawn::ActionGroup::Fight) == CL_AG_FIGHT && static_cast<uint8>(pawn::ActionGroup::Behaviours) == CL_AG_BEHAVIOURS &&
                      static_cast<uint8>(pawn::ActionGroup::Magic) == CL_AG_MAGIC && static_cast<uint8>(pawn::ActionGroup::Abilities) == CL_AG_ABILITIES &&
                      static_cast<uint8>(pawn::ActionGroup::WeaponSkills) == CL_AG_WEAPON_SKILLS && static_cast<uint8>(pawn::ActionGroup::Ranged) == CL_AG_RANGED);

        // The gambit set a request names, and whose it is: a cardian he
        // commands, or his own when it names him (live_controller.h)
        struct GambitSet
        {
            CCharEntity*    PWho = nullptr;
            pawn::CGambits* PSet = nullptr;
        };

        auto gambitSetOf(CCharEntity* PChar, const uint32 id) -> GambitSet
        {
            if (id == PChar->id)
            {
                auto* PLive = dynamic_cast<CLiveController*>(PChar->PAI->GetController());
                return { PChar, PLive != nullptr ? &PLive->Gambits() : nullptr };
            }
            auto* PPawn       = pawn::findCommandablePawn(PChar, id);
            auto* PController = PPawn != nullptr ? dynamic_cast<CPawnController*>(PPawn->PAI->GetController()) : nullptr;
            return { PPawn, PController != nullptr ? &PController->Gambits() : nullptr };
        }

        // Her rows as they now stand, each a GAMBIT_ROW answer, and the GAMBITS
        // that closes them: the last answer to GAMBITS, or the one ahead of an
        // edit's outcome
        auto rowsOf(CCharEntity* PPawn, pawn::CGambits& set, Reply& reply) -> cl_gambits
        {
            static_assert(CL_GO_OWN == static_cast<int>(cardian::layers::Origin::Own) && CL_GO_LENT == static_cast<int>(cardian::layers::Origin::Lent) &&
                              CL_GO_BOTH == static_cast<int>(cardian::layers::Origin::Both),
                          "the Link's origins are the layers'");
            auto summary    = make<cl_gambits>();
            summary.cardian = PPawn->id;
            summary.master  = set.MasterOn() ? 1 : 0;
            std::size_t count = 0;
            for (const auto& shown : set.Shown())
            {
                if (++count > UINT8_MAX)
                {
                    break;
                }
                auto msg    = make<cl_gambit_row>();
                msg.cardian = PPawn->id;
                msg.index   = static_cast<uint8_t>(shown.index);
                msg.on      = shown.on ? 1 : 0;
                msg.state   = static_cast<uint8_t>(shown.state);
                msg.fits    = pawn::wire::toWire(shown.row->gambit, msg.gambit) ? 1 : 0;
                msg.origin  = static_cast<uint8_t>(shown.origin);
                msg.lender  = shown.origin == cardian::layers::Origin::Own ? CL_ROLE_NONE : static_cast<uint8_t>(set.LentBy());
                const auto label = pawn::labelGambit(shown.row->gambit);
                setText(msg.head, label.head);
                setText(msg.action, label.action);
                reply.more(msg);
                summary.count = static_cast<uint8_t>(count);
            }
            return summary;
        }

        void gambits(CCharEntity* PChar, const cl_gambits& ask, Reply& reply)
        {
            const auto [PWho, PSet] = gambitSetOf(PChar, ask.cardian);
            if (PSet == nullptr)
            {
                reply.finish(ask, CL_S_NO_SUCH_CARDIAN);
                return;
            }
            reply.finish(rowsOf(PWho, *PSet, reply), CL_S_OK);
        }

        // An edit of her rows: made by edit(set), which returns its outcome,
        // and saved when it took; then her rows as they now stand, refused or
        // not, so the editor never keeps a guess; then the outcome
        template <typename Message, typename Edit>
        void editGambits(CCharEntity* PChar, const Message& ask, Reply& reply, Edit&& edit)
        {
            const auto [PWho, PSet] = gambitSetOf(PChar, ask.cardian);
            if (PSet == nullptr)
            {
                reply.finish(ask, CL_S_NO_SUCH_CARDIAN);
                return;
            }
            const uint16 status = edit(*PSet);
            if (status == CL_S_OK)
            {
                pawn::saveGambits(PWho);
            }
            reply.more(rowsOf(PWho, *PSet, reply));
            reply.finish(ask, status);
        }

        // A row the editor sent, as the gambit engine takes it, or why not:
        // the row grammar's own refusals, and the editor's pairing rules
        auto rowFrom(const cl_gambit& fields, uint16& status) -> std::optional<gambits::Gambit_t>
        {
            auto gambit = pawn::wire::fromWire(fields);
            if (!gambit.has_value())
            {
                status = CL_S_MALFORMED;
                return std::nullopt;
            }
            switch (cardian::engage::pairingOf(*gambit))
            {
                case cardian::engage::Pairing::AttackAlone:
                    status = CL_S_ATTACK_ALONE;
                    return std::nullopt;
                case cardian::engage::Pairing::AttackOnClock:
                    status = CL_S_ATTACK_ON_CLOCK;
                    return std::nullopt;
                default:
                    status = CL_S_OK;
                    return gambit;
            }
        }

        void gambitToggle(CCharEntity* PChar, const cl_gambit_toggle& ask, Reply& reply)
        {
            editGambits(PChar, ask, reply, [&](pawn::CGambits& set) -> uint16
                        {
                            if (set.Locked(ask.index))
                            {
                                return CL_S_ROLE_LOCKED;
                            }
                            return set.SetEnabled(ask.index, ask.on != 0) ? CL_S_OK : CL_S_NO_SUCH_ROW;
                        });
        }

        void gambitMove(CCharEntity* PChar, const cl_gambit_move& ask, Reply& reply)
        {
            editGambits(PChar, ask, reply, [&](pawn::CGambits& set) -> uint16
                        {
                            if (set.Locked(ask.from))
                            {
                                return CL_S_ROLE_LOCKED;
                            }
                            return set.Move(ask.from, ask.to) ? CL_S_OK : CL_S_NO_SUCH_ROW;
                        });
        }

        void gambitDelete(CCharEntity* PChar, const cl_gambit_delete& ask, Reply& reply)
        {
            editGambits(PChar, ask, reply, [&](pawn::CGambits& set) -> uint16
                        {
                            if (set.Locked(ask.index))
                            {
                                return CL_S_ROLE_LOCKED;
                            }
                            return set.Erase(ask.index) ? CL_S_OK : CL_S_NO_SUCH_ROW;
                        });
        }

        void gambitInsert(CCharEntity* PChar, const cl_gambit_insert& ask, Reply& reply)
        {
            editGambits(PChar, ask, reply, [&](pawn::CGambits& set) -> uint16
                        {
                            uint16 status = CL_S_OK;
                            auto   gambit = rowFrom(ask.gambit, status);
                            if (!gambit.has_value())
                            {
                                return status;
                            }
                            return set.Insert(ask.index, std::move(*gambit)) ? CL_S_OK : CL_S_NO_SUCH_ROW;
                        });
        }

        // Rewritten in place, the row keeps its switch
        void gambitReplace(CCharEntity* PChar, const cl_gambit_replace& ask, Reply& reply)
        {
            editGambits(PChar, ask, reply, [&](pawn::CGambits& set) -> uint16
                        {
                            uint16 status = CL_S_OK;
                            auto   gambit = rowFrom(ask.gambit, status);
                            if (!gambit.has_value())
                            {
                                return status;
                            }
                            if (set.Locked(ask.index))
                            {
                                return CL_S_ROLE_LOCKED;
                            }
                            return set.Replace(ask.index, std::move(*gambit)) ? CL_S_OK : CL_S_NO_SUCH_ROW;
                        });
        }

        void gambitMaster(CCharEntity* PChar, const cl_gambit_master& ask, Reply& reply)
        {
            editGambits(PChar, ask, reply, [&](pawn::CGambits& set) -> uint16
                        {
                            set.SetMaster(ask.on != 0);
                            return CL_S_OK;
                        });
        }

        // Entries in parts, as many to a message as its array holds, each part
        // an answer (CL_F_MORE); none for an empty list
        template <typename Part, typename Item, std::size_t N, typename Entry, typename Fill>
        void inParts(Reply& reply, const uint32 cardian, Item (Part::*array)[N], const std::vector<Entry>& entries, Fill&& fill)
        {
            for (std::size_t next = 0; next < entries.size();)
            {
                auto part    = make<Part>();
                part.cardian = cardian;
                for (; part.count < N && next < entries.size(); ++next)
                {
                    fill(entries[next], (part.*array)[part.count++]);
                }
                reply.more(part);
            }
        }

        // The pickers' catalogue for her (pawn::vocabularyFor): its clauses,
        // statuses and actions in parts, then her jobs and levels
        void gambitVocab(CCharEntity* PChar, const cl_gambit_vocab& ask, Reply& reply)
        {
            const bool own   = ask.cardian == PChar->id;
            auto*      PPawn = own ? PChar : pawn::findCommandablePawn(PChar, ask.cardian);
            if (PPawn == nullptr)
            {
                reply.finish(ask, CL_S_NO_SUCH_CARDIAN);
                return;
            }
            const auto vocab = pawn::vocabularyFor(PPawn, own);
            inParts(reply, PPawn->id, &cl_vocab_conditions::conditions, vocab.conditions, [](const pawn::VocabCondition& c, cl_vocab_condition& out)
                    {
                        out.target    = static_cast<uint16_t>(c.target);
                        out.condition = static_cast<uint16_t>(c.condition);
                        out.takes     = static_cast<uint8_t>(c.takes);
                        out.side      = static_cast<uint8_t>(c.side);
                        out.min       = c.min;
                        out.max       = c.max;
                        out.step      = c.step;
                        out.initial   = c.initial;
                        setText(out.label, c.label);
                    });
            inParts(reply, PPawn->id, &cl_vocab_statuses::statuses, vocab.statuses, [](const pawn::VocabStatus& s, cl_vocab_status& out)
                    {
                        out.id = s.id;
                        setText(out.label, s.label);
                    });
            inParts(reply, PPawn->id, &cl_vocab_actions::actions, vocab.actions, [](const pawn::VocabAction& a, cl_vocab_action& out)
                    {
                        out.action  = cl_gambit_action{ static_cast<uint16_t>(a.reaction), static_cast<uint16_t>(a.select), a.arg };
                        out.targets = a.targets;
                        out.mp      = a.mp;
                        out.group   = static_cast<uint8_t>(a.group);
                        out.usable  = a.usable ? 1 : 0;
                        setText(out.label, a.label);
                    });
            auto answer      = ask;
            answer.mainJob   = vocab.mjob;
            answer.mainLevel = vocab.mlvl;
            answer.subJob    = vocab.sjob;
            answer.subLevel  = vocab.slvl;
            reply.finish(answer, CL_S_OK);
        }

        // ---- his cardians, and the party finder ----------------------------

        static_assert(static_cast<uint8>(pawn::finder::Goal::Kind::Experience) == CL_GOAL_EXP &&
                      static_cast<uint8>(pawn::finder::Goal::Kind::Mission) == CL_GOAL_MISSION &&
                      static_cast<uint8>(pawn::finder::Goal::Kind::Quest) == CL_GOAL_QUEST);
        static_assert(static_cast<uint8>(pawn::finder::Presence::Here) == CL_PRESENCE_HERE &&
                      static_cast<uint8>(pawn::finder::Presence::Standing) == CL_PRESENCE_STANDING &&
                      static_cast<uint8>(pawn::finder::Presence::Busy) == CL_PRESENCE_BUSY &&
                      static_cast<uint8>(pawn::finder::Presence::Faded) == CL_PRESENCE_FADED &&
                      static_cast<uint8>(pawn::finder::Presence::Away) == CL_PRESENCE_AWAY);
        static_assert(static_cast<uint8>(pawn::finder::MissionFit::Free) == CL_FIT_FREE &&
                      static_cast<uint8>(pawn::finder::MissionFit::Behind) == CL_FIT_BEHIND &&
                      static_cast<uint8>(pawn::finder::MissionFit::On) == CL_FIT_ON &&
                      static_cast<uint8>(pawn::finder::MissionFit::Done) == CL_FIT_DONE);

        // Every character he could spawn as a cardian, in parts
        void owned(CCharEntity* PChar, const cl_owned& ask, Reply& reply)
        {
            const auto            members  = pawn::accountPawns(PChar);
            constexpr std::size_t kPerPart = sizeof(cl_owned::cardians) / sizeof(cl_owned_cardian);
            std::size_t           next     = 0;
            while (true)
            {
                auto part  = ask;
                part.count = 0;
                for (; part.count < kPerPart && next < members.size(); ++next)
                {
                    auto& row   = part.cardians[part.count++];
                    row         = cl_owned_cardian{};
                    row.cardian = members[next].first;
                    row.out     = pawn::findPawn(members[next].first) != nullptr ? 1 : 0;
                    setText(row.name, members[next].second);
                }
                if (next < members.size())
                {
                    reply.more(part);
                    continue;
                }
                reply.finish(part, CL_S_OK);
                return;
            }
        }

        // One of those, by charid, by name; "" for none of his
        auto ownedName(CCharEntity* PChar, const uint32 charid) -> std::string
        {
            for (auto& [id, name] : pawn::accountPawns(PChar))
            {
                if (id == charid)
                {
                    return name;
                }
            }
            return {};
        }

        // The Lua libraries some answers come from (modules/cardian/lua), by
        // their table under xi.cardian: a library that did not load, or a
        // function that failed, says so in the map log once and answers
        // CL_S_REFUSED
        constexpr std::array<std::pair<const char*, const char*>, 2> kLibraries{ {
            { "finder", "./modules/cardian/lua/finder_goals.lua" },
            { "exchange", "./modules/cardian/lua/conquest_exchange.lua" },
        } };

        auto libraryCall(const char* library, const char* name) -> std::optional<sol::protected_function>
        {
            const sol::object cardian = ::lua["xi"]["cardian"];
            sol::object       table;
            if (cardian.get_type() == sol::type::table)
            {
                table = cardian.as<sol::table>()[library];
            }
            if (table.get_type() != sol::type::table || table.as<sol::table>()[name].get_type() != sol::type::function)
            {
                static std::set<std::string> said;
                if (said.insert(fmt::format("{}.{}", library, name)).second)
                {
                    ShowError("link: xi.cardian.{}.{} is not loaded (modules/cardian/lua); its answers are refused", library, name);
                }
                return std::nullopt;
            }
            return sol::protected_function(table.as<sol::table>()[name]);
        }

        // The result, or nullopt when the call failed (said once per name)
        auto libraryTable(const char* name, const sol::protected_function_result& res) -> std::optional<sol::table>
        {
            if (!res.valid())
            {
                static std::set<std::string> said;
                if (said.insert(name).second)
                {
                    sol::error err = res;
                    ShowError("link: {} failed: {}", name, err.what());
                }
                return std::nullopt;
            }
            if (res.get_type(0) != sol::type::table)
            {
                return std::nullopt;
            }
            return res.get<sol::table>(0);
        }

        // The Debug screen's spawn and despawn (pawn::spawn, pawn::despawn)
        void spawnCardian(CCharEntity* PChar, const cl_spawn& ask, Reply& reply)
        {
            const auto name = ownedName(PChar, ask.cardian);
            reply.finish(ask, !name.empty() && pawn::spawn(PChar, name) ? CL_S_OK : CL_S_CANNOT_SPAWN);
        }

        void despawnCardian(CCharEntity* PChar, const cl_despawn& ask, Reply& reply)
        {
            const auto name = ownedName(PChar, ask.cardian);
            if (name.empty())
            {
                reply.finish(ask, CL_S_NO_SUCH_CARDIAN);
                return;
            }
            reply.finish(ask, pawn::despawn(name) ? CL_S_OK : CL_S_NOT_OUT);
        }

        // A goal as the finder takes it, from its fields
        auto goalOf(const uint8 kind, const uint8 log) -> std::optional<pawn::finder::Goal>
        {
            if (kind > CL_GOAL_QUEST)
            {
                return std::nullopt;
            }
            return pawn::finder::Goal{ static_cast<pawn::finder::Goal::Kind>(kind), log };
        }

        // What he could recruit for besides experience (xi.cardian.finder.goals):
        // each mission log's current mission and every quest under way, a GOAL
        // each, then the missions done on each log
        void goals(CCharEntity* PChar, const cl_goals& ask, Reply& reply)
        {
            auto call  = libraryCall("finder", "goals");
            auto found = call ? libraryTable("finder.goals", (*call)(CLuaBaseEntity(PChar))) : std::nullopt;
            if (!found)
            {
                reply.finish(ask, CL_S_REFUSED);
                return;
            }
            auto       answer = ask;
            const auto send   = [&](const uint8 kind, const sol::object& list)
            {
                if (list.get_type() != sol::type::table)
                {
                    return;
                }
                const auto rows = list.as<sol::table>();
                for (std::size_t i = 1; i <= rows.size(); ++i)
                {
                    const sol::table row = rows[i];
                    auto             msg = make<cl_goal>();
                    msg.kind             = kind;
                    msg.log              = row.get_or<uint8>("log", 0);
                    msg.id               = row.get_or<uint16>("id", 0);
                    setText(msg.title, row.get_or<std::string>("title", ""));
                    reply.more(msg);
                    answer.count = static_cast<uint8_t>(std::min<int>(answer.count + 1, UINT8_MAX));
                }
            };
            send(CL_GOAL_MISSION, (*found)["missions"]);
            send(CL_GOAL_QUEST, (*found)["quests"]);
            if (const sol::object done = (*found)["completed"]; done.get_type() == sol::type::table)
            {
                for (const auto& [log, count] : done.as<sol::table>())
                {
                    if (log.get_type() == sol::type::number && count.get_type() == sol::type::number && log.as<std::size_t>() < sizeof(answer.completed) / sizeof(answer.completed[0]))
                    {
                        answer.completed[log.as<std::size_t>()] = clamp16(count.as<int64>());
                    }
                }
            }
            reply.finish(answer, CL_S_OK);
        }

        // One who heard the shout, as the screen shows her
        auto responderOf(const uint32 shoutId, const pawn::finder::Responder& r) -> cl_shout_responder
        {
            const auto& c   = r.c;
            auto        msg = make<cl_shout_responder>();
            msg.shout       = shoutId;
            msg.cardian     = c.charid;
            setText(msg.name, c.name);
            msg.job      = c.job;
            msg.level    = c.level;
            msg.race     = c.race;
            msg.nation   = c.nation;
            msg.rank     = c.rank;
            msg.presence = static_cast<uint8_t>(c.presence);
            msg.willing  = c.answer.yes ? 1 : 0;
            msg.fit      = static_cast<uint8_t>(c.answer.fit);
            msg.affinity = c.affinity;
            msg.revealMs = r.revealMs;
            msg.decideMs = r.decideMs;
            msg.zone     = c.zoneId;
            std::string zone = c.zone;
            std::replace(zone.begin(), zone.end(), '_', ' ');
            setText(msg.zoneName, zone);
            setText(msg.line, c.answer.line);
            return msg;
        }

        // The shout (pawn::finder::shout): each who heard it, then the shout;
        // the one he has again, or refused, with the wait for the cooldown
        void shout(CCharEntity* PChar, const cl_shout& ask, Reply& reply)
        {
            const auto goal = goalOf(ask.goal, ask.log);
            if (!goal.has_value())
            {
                reply.finish(ask, CL_S_MALFORMED);
                return;
            }
            uint16      refusal = CL_S_OK;
            uint32      waitMs  = 0;
            const auto* made    = pawn::finder::shout(PChar, *goal, ask.again != 0, refusal, waitMs);
            auto        answer  = ask;
            if (made == nullptr)
            {
                answer.waitMs = waitMs;
                reply.finish(answer, refusal);
                return;
            }
            for (const auto& r : made->rows)
            {
                reply.more(responderOf(made->id, r));
            }
            answer.goal   = static_cast<uint8_t>(made->goal.kind);
            answer.log    = made->goal.log;
            answer.id     = made->id;
            answer.waitMs = made->waitMs;
            answer.count  = static_cast<uint8_t>(std::min<std::size_t>(made->rows.size(), UINT8_MAX));
            reply.finish(answer, CL_S_OK);
        }

        // A look at one who answered his shout, or whom his contract holds
        void peek(CCharEntity* PChar, const cl_peek& ask, Reply& reply)
        {
            const auto p = pawn::finder::peek(PChar, ask.cardian);
            if (!p.has_value())
            {
                reply.finish(ask, CL_S_NOT_IN_SHOUT);
                return;
            }
            auto answer     = ask;
            answer.job      = p->job;
            answer.level    = p->level;
            answer.subJob   = p->sjob;
            answer.subLevel = p->slvl;
            answer.nation   = p->nation;
            answer.rank     = p->rank;
            answer.standing = p->standing ? 1 : 0;
            answer.known    = p->known ? 1 : 0;
            answer.affinity = p->affinity;
            answer.hp       = clamp16(p->hp);
            answer.maxHp    = clamp16(p->maxhp);
            answer.mp       = clamp16(p->mp);
            answer.maxMp    = clamp16(p->maxmp);
            for (std::size_t i = 0; i < p->numbers.total.size(); ++i)
            {
                answer.total[i] = p->numbers.total[i];
                answer.bonus[i] = p->numbers.bonus[i];
            }
            answer.attack  = p->numbers.attack;
            answer.defence = p->numbers.defence;
            for (std::size_t slot = 0; slot < p->items.size(); ++slot)
            {
                answer.items[slot] = p->items[slot];
            }
            reply.finish(answer, CL_S_OK);
        }

        // The party invite, sent for him; declined, with her words
        void invite(CCharEntity* PChar, const cl_invite& ask, Reply& reply)
        {
            const auto goal = goalOf(ask.goal, ask.log);
            if (!goal.has_value())
            {
                reply.finish(ask, CL_S_MALFORMED);
                return;
            }
            std::string line;
            const auto  status = pawn::finder::invite(PChar, ask.cardian, *goal, line);
            auto        answer = ask;
            setText(answer.line, line);
            reply.finish(answer, status);
        }

        static_assert(static_cast<uint8>(pawn::finder::ContractState::Party) == CL_CONTRACT_PARTY &&
                      static_cast<uint8>(pawn::finder::ContractState::Standing) == CL_CONTRACT_STANDING &&
                      static_cast<uint8>(pawn::finder::ContractState::Faded) == CL_CONTRACT_FADED &&
                      static_cast<uint8>(pawn::finder::ContractState::Out) == CL_CONTRACT_OUT);

        // His open contracts, as his Your contract page shows them
        void contracts(CCharEntity* PChar, const cl_contracts& ask, Reply& reply)
        {
            auto                  answer = ask;
            constexpr std::size_t kMax   = sizeof(cl_contracts::contracts) / sizeof(cl_contract);
            answer.count                 = 0;
            for (const auto& c : pawn::finder::contractsOf(PChar))
            {
                if (answer.count >= kMax)
                {
                    break;
                }
                auto& row   = answer.contracts[answer.count++];
                row         = cl_contract{};
                row.cardian = c.charid;
                row.goal    = static_cast<uint8_t>(c.goal.kind);
                row.job     = c.job;
                row.level   = c.level;
                row.zone    = c.zone;
                row.state   = static_cast<uint8_t>(c.state);
                setText(row.name, c.name);
            }
            reply.finish(answer, CL_S_OK);
        }

        void endContract(CCharEntity* PChar, const cl_end_contract& ask, Reply& reply)
        {
            reply.finish(ask, pawn::finder::release(PChar, ask.cardian));
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

        // The party's roles as the party screen shows them (party_roster.h,
        // RESEARCH §17): each member as an answer, the player first. How many
        // were sent
        auto sendPartyRoles(CCharEntity* PChar, Reply& reply) -> uint8
        {
            static_assert(CL_ROLE_NONE == static_cast<int>(cardian::party::Role::None) && CL_ROLE_TANK == static_cast<int>(cardian::party::Role::Tank) &&
                          CL_ROLE_HEALER == static_cast<int>(cardian::party::Role::Healer) && CL_ROLE_DAMAGE == static_cast<int>(cardian::party::Role::Damage) &&
                          CL_ROLE_PULLER == static_cast<int>(cardian::party::Role::Puller),
                          "the Link's role numbers are the rule's");
            uint8 count = 0;
            for (const auto& row : pawn::roster::rolesOf(PChar))
            {
                auto msg = make<cl_party_role>();
                msg.member = row.id;
                setText(msg.name, row.name);
                msg.mainJob   = row.mainJob;
                msg.mainLevel = row.mainLevel;
                msg.subJob    = row.subJob;
                msg.subLevel  = row.subLevel;
                msg.role      = static_cast<uint8_t>(row.role);
                msg.byPlayer  = row.byPlayer ? 1 : 0;
                msg.self      = row.id == PChar->id ? 1 : 0;

                // What her column shows of her (pawn::statusNumbers, as her
                // equipment screen's status pane has them)
                const auto numbers = pawn::statusNumbers(row.who);
                msg.hp      = clamp16(row.who->health.hp);
                msg.maxHp   = clamp16(row.who->GetMaxHP());
                msg.mp      = clamp16(row.who->health.mp);
                msg.maxMp   = clamp16(row.who->GetMaxMP());
                msg.tp      = clamp16(row.who->health.tp);
                msg.attack  = numbers.attack;
                msg.defence = numbers.defence;
                for (std::size_t i = 0; i < numbers.total.size(); ++i)
                {
                    msg.total[i] = numbers.total[i];
                    msg.bonus[i] = numbers.bonus[i];
                }
                for (uint8 equipSlot = SLOT_MAIN; equipSlot <= SLOT_BACK; ++equipSlot)
                {
                    if (const auto* PItem = row.who->getEquip(static_cast<SLOTTYPE>(equipSlot)); PItem != nullptr)
                    {
                        msg.worn[equipSlot] = PItem->getID();
                    }
                }
                reply.more(msg);
                ++count;
            }
            return count;
        }

        void partyRoles(CCharEntity* PChar, const cl_party_roles& ask, Reply& reply)
        {
            auto answer  = ask;
            answer.count = sendPartyRoles(PChar, reply);
            reply.finish(answer, CL_S_OK);
        }

        // The player's choice of a member's role, or his taking it back
        // (CL_ROLE_AUTO): the roles come back as they now stand, then its
        // outcome, so the screen never keeps a guess
        void setPartyRole(CCharEntity* PChar, const cl_set_party_role& ask, Reply& reply)
        {
            uint16 status = CL_S_MALFORMED;
            if (ask.role == CL_ROLE_AUTO)
            {
                status = pawn::roster::release(PChar, ask.member);
            }
            else if (ask.role < cardian::party::kRoleCount)
            {
                status = pawn::roster::choose(PChar, ask.member, static_cast<cardian::party::Role>(ask.role));
            }
            sendPartyRoles(PChar, reply);
            reply.finish(ask, status);
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
        // gear and, his to manage, her inventory. His own view has no roster
        // line: his client holds that
        void sync(CCharEntity* PChar, const cl_sync& ask, Reply& reply)
        {
            if (ask.cardian == PChar->id)
            {
                reply.more(statsOf(PChar, true));
                reply.more(gearOf(PChar));
                reply.more(inventoryOf(PChar, LOC_INVENTORY));
                reply.finish(ask, CL_S_OK);
                return;
            }
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

        // One of her containers, asked for: the inventory or a bag (his to
        // manage); or one of his own
        void inventory(CCharEntity* PChar, const cl_inventory& ask, Reply& reply)
        {
            auto* PPawn = managedOrSelf(PChar, ask.cardian);
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
            auto* PPawn = managedOrSelf(PChar, ask.cardian);
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

        // The conquest exchange's refusals, as conquest_exchange.lua names them
        auto exchangeRefusal(const std::string& name) -> uint16
        {
            static const std::map<std::string, uint16> kCodes{
                { "NO_SPACE", CL_S_NO_SPACE },
                { "NOT_SOLD", CL_S_NOT_SOLD },
                { "NOT_BY_PROXY", CL_S_NOT_BY_PROXY },
                { "OUTRANKED", CL_S_OUTRANKED },
                { "FOREIGN_PLACE", CL_S_FOREIGN_PLACE },
                { "NATION_PLACE", CL_S_NATION_PLACE },
                { "TOO_FEW_POINTS", CL_S_TOO_FEW_POINTS },
                { "RANK_TOO_LOW", CL_S_RANK_TOO_LOW },
                { "GUARD_REFUSED", CL_S_GUARD_REFUSED },
            };
            if (const auto it = kCodes.find(name); it != kCodes.end())
            {
                return it->second;
            }
            ShowError("link: the conquest exchange named a refusal the Link does not know: {}", name);
            return CL_S_REFUSED;
        }

        // The gate guard within his reach sells to a cardian of his to manage
        // (pawn/gate_guards.h); nullptr with the outcome when either is missing
        auto exchangeParties(CCharEntity* PChar, const uint32 cardian, uint16& refusal) -> std::pair<CCharEntity*, const pawn::guards::Guard*>
        {
            auto* PPawn = pawn::findManagedPawn(PChar, cardian);
            if (PPawn == nullptr)
            {
                refusal = CL_S_NO_SUCH_CARDIAN;
                return {};
            }
            const auto* PGuard = pawn::guards::guardNear(PChar);
            if (PGuard == nullptr)
            {
                refusal = CL_S_NO_GUARD;
                return {};
            }
            return { PPawn, PGuard };
        }

        // What the guard sells her (xi.cardian.exchange.shop): a CP_ITEM each,
        // then where she stands with him
        void cpShop(CCharEntity* PChar, const cl_cp_shop& ask, Reply& reply)
        {
            uint16 refusal       = CL_S_OK;
            auto [PPawn, PGuard] = exchangeParties(PChar, ask.cardian, refusal);
            if (PPawn == nullptr)
            {
                reply.finish(ask, refusal);
                return;
            }
            auto call = libraryCall("exchange", "shop");
            auto shop = call ? libraryTable("exchange.shop", (*call)(CLuaBaseEntity(PPawn), PGuard->nation)) : std::nullopt;
            if (!shop)
            {
                reply.finish(ask, CL_S_REFUSED);
                return;
            }
            auto answer = ask;
            if (const sol::object items = (*shop)["items"]; items.get_type() == sol::type::table)
            {
                const auto rows = items.as<sol::table>();
                for (std::size_t i = 1; i <= rows.size(); ++i)
                {
                    const sol::table row = rows[i];
                    auto             msg = make<cl_cp_item>();
                    msg.option           = row.get_or<uint16>("option", 0);
                    msg.item             = row.get_or<uint16>("item", 0);
                    msg.price            = row.get_or<uint32>("price", 0);
                    msg.level            = row.get_or<uint8>("level", 0);
                    msg.rank             = row.get_or<uint8>("rank", 0);
                    msg.place            = row.get_or<uint8>("place", 0);
                    reply.more(msg);
                    answer.count = static_cast<uint8_t>(std::min<int>(answer.count + 1, UINT8_MAX));
                }
            }
            answer.cp          = shop->get_or<uint32>("cp", 0);
            answer.rank        = shop->get_or<uint8>("rank", 0);
            answer.nation      = shop->get_or<uint8>("nation", 0);
            answer.guardNation = PGuard->nation;
            answer.nationRank  = shop->get_or<uint8>("nationRank", 0);
            answer.foreign     = shop->get_or("foreign", false) ? 1 : 0;
            answer.blocked     = shop->get_or("blocked", false) ? 1 : 0;
            setText(answer.guard, std::string(PGuard->name));
            reply.finish(answer, CL_S_OK);
        }

        // One thing bought for her (xi.cardian.exchange.buy): her inventory,
        // then the outcome, her points as they stand either way
        void cpBuy(CCharEntity* PChar, const cl_cp_buy& ask, Reply& reply)
        {
            uint16 refusal       = CL_S_OK;
            auto [PPawn, PGuard] = exchangeParties(PChar, ask.cardian, refusal);
            if (PPawn == nullptr)
            {
                reply.finish(ask, refusal);
                return;
            }
            auto call = libraryCall("exchange", "buy");
            auto sale = call ? libraryTable("exchange.buy", (*call)(CLuaBaseEntity(PPawn), PGuard->nation, PGuard->type, ask.option)) : std::nullopt;
            if (!sale)
            {
                reply.finish(ask, CL_S_REFUSED);
                return;
            }
            auto answer = ask;
            answer.cp   = sale->get_or<uint32>("cp", 0);
            answer.have = sale->get_or<uint32>("have", 0);
            answer.need = sale->get_or<uint32>("need", 0);
            if (const auto named = sale->get<sol::optional<std::string>>("refusal"))
            {
                reply.finish(answer, exchangeRefusal(*named));
                return;
            }
            reply.more(inventoryOf(PPawn, LOC_INVENTORY));
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

    void loadLibraries()
    {
        for (const auto& [library, path] : kLibraries)
        {
            const auto res = ::lua.safe_script_file(path);
            if (!res.valid())
            {
                sol::error err = res;
                ShowError("link: xi.cardian.{} did not load: {}", library, err.what());
            }
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
        handle<cl_gambits>(gambits);
        handle<cl_gambit_toggle>(gambitToggle);
        handle<cl_gambit_move>(gambitMove);
        handle<cl_gambit_delete>(gambitDelete);
        handle<cl_gambit_insert>(gambitInsert);
        handle<cl_gambit_replace>(gambitReplace);
        handle<cl_gambit_master>(gambitMaster);
        handle<cl_gambit_vocab>(gambitVocab);
        handle<cl_owned>(owned);
        handle<cl_spawn>(spawnCardian);
        handle<cl_despawn>(despawnCardian);
        handle<cl_shout>(shout);
        handle<cl_peek>(peek);
        handle<cl_invite>(invite);
        handle<cl_contracts>(contracts);
        handle<cl_end_contract>(endContract);
        handle<cl_goals>(goals);
        handle<cl_cp_shop>(cpShop);
        handle<cl_cp_buy>(cpBuy);
        handle<cl_walk>(walk);
        handle<cl_view>(lookThrough);
        handle<cl_maneuver>(maneuver);
        handle<cl_maneuvers>(maneuvers);
        handle<cl_orders>(orders);
        handle<cl_set_strategy>(setStrategy);
        handle<cl_set_hunt>(setHunt);
        handle<cl_party_roles>(partyRoles);
        handle<cl_set_party_role>(setPartyRole);
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
