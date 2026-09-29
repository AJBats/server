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

#include "input_gate.h"

#include "common/logging.h"
#include "common/timer.h"
#include "entities/char_entity.h"
#include "enums/chat_message_type.h"
#include "enums/packet_c2s.h"
#include "item_container.h"
#include "items/item.h"
#include "packets/basic.h"
#include "packets/c2s/0x015_pos.h"
#include "packets/c2s/0x01a_action.h"
#include "packets/c2s/0x037_item_use.h"
#include "packets/c2s/0x0e7_reqlogout.h"
#include "packets/c2s/0x0e8_camp.h"
#include "packets/s2c/0x017_chat_std.h"
#include "packets/s2c/0x052_eventucoff.h"
#include "pawn/cardian_link.h"
#include "status_effect_container.h"
#include "utils/zoneutils.h"

#include <fmt/format.h>

#include <magic_enum/magic_enum.hpp>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>

namespace cardian::pause::input
{
namespace
{

struct Waiting
{
    Queued                        what;
    xi::ZoneId                    zone{};
    uint16                        itemId = 0; // 0x037 names a slot: what lay in it when he chose
    cl_action                     action{};   // what he chose, for his queue line
    std::unique_ptr<CBasicPacket> packet;
};

// By charid: a character's body does not outlive a zone line, his charid does.
std::unordered_map<uint32, Waiting> waiting;

// Everything off 0x01A is a command but these, which command nothing.
auto isCommand(const GP_CLI_COMMAND_ACTION_ACTIONID action) -> bool
{
    switch (action)
    {
        case GP_CLI_COMMAND_ACTION_ACTIONID::Talk:
        case GP_CLI_COMMAND_ACTION_ACTIONID::Assist:
        case GP_CLI_COMMAND_ACTION_ACTIONID::SendResRdy:
        case GP_CLI_COMMAND_ACTION_ACTIONID::Blockaid:
            return false;
        default:
            return true;
    }
}

auto describe(CBasicPacket& packet) -> std::optional<Queued>
{
    const auto packetId = packet.getType();
    switch (static_cast<PacketC2S>(packetId))
    {
        case PacketC2S::GP_CLI_COMMAND_ACTION:
        {
            const auto* action = packet.as<GP_CLI_COMMAND_ACTION>();
            if (!isCommand(static_cast<GP_CLI_COMMAND_ACTION_ACTIONID>(action->ActionID)))
            {
                return std::nullopt;
            }
            return Queued{ packetId, static_cast<uint16>(action->ActionID), static_cast<uint16>(action->ActIndex) };
        }
        case PacketC2S::GP_CLI_COMMAND_ITEM_USE:
            return Queued{ packetId, 0, static_cast<uint16>(packet.as<GP_CLI_COMMAND_ITEM_USE>()->ActIndex) };
        case PacketC2S::GP_CLI_COMMAND_CAMP:
            return Queued{ packetId, 0, 0 };
        default:
            return std::nullopt;
    }
}

// What lies in the slot an item-use packet names; 0 for nothing.
auto itemIn(CCharEntity* PChar, const CBasicPacket& packet) -> uint16
{
    const auto* use     = packet.as<GP_CLI_COMMAND_ITEM_USE>();
    const auto* storage = PChar->getStorage(use->Category);
    const auto* PItem   = storage != nullptr ? storage->GetItem(use->PropertyItemIndex) : nullptr;
    return PItem != nullptr ? PItem->getID() : 0;
}

auto nameOf(const Queued& what) -> std::string
{
    if (static_cast<PacketC2S>(what.packetId) == PacketC2S::GP_CLI_COMMAND_ACTION)
    {
        return std::string(magic_enum::enum_name(static_cast<GP_CLI_COMMAND_ACTION_ACTIONID>(what.actionId)));
    }
    return std::string(magic_enum::enum_name(static_cast<PacketC2S>(what.packetId)));
}

// What he chose, for his command window's queue line, as the Link names actions
// (cl_action): a spell, an ability, a weapon skill or a shot by its id, an item by
// the item, /heal, and the rest of the action menu by its 0x01A action id. The
// addon words it.
auto actionOf(const Queued& what, const uint16 itemId, const CBasicPacket& packet) -> cl_action
{
    cl_action action{};
    if (static_cast<PacketC2S>(what.packetId) == PacketC2S::GP_CLI_COMMAND_ACTION)
    {
        const auto* typed = packet.as<GP_CLI_COMMAND_ACTION>();
        switch (static_cast<GP_CLI_COMMAND_ACTION_ACTIONID>(what.actionId))
        {
            case GP_CLI_COMMAND_ACTION_ACTIONID::CastMagic:
                action = { CL_AK_MAGIC, 2, static_cast<uint16_t>(typed->CastMagic.SpellId) };
                break;
            case GP_CLI_COMMAND_ACTION_ACTIONID::JobAbility:
                action = { CL_AK_ABILITY, 2, static_cast<uint16_t>(typed->JobAbility.SkillId) };
                break;
            case GP_CLI_COMMAND_ACTION_ACTIONID::Weaponskill:
                action = { CL_AK_WEAPONSKILL, 2, static_cast<uint16_t>(typed->Weaponskill.SkillId) };
                break;
            case GP_CLI_COMMAND_ACTION_ACTIONID::Shoot:
                action = { CL_AK_RANGED, 0, 0 };
                break;
            default:
                action = { CL_AK_CLIENT, 0, what.actionId };
                break;
        }
    }
    else if (static_cast<PacketC2S>(what.packetId) == PacketC2S::GP_CLI_COMMAND_ITEM_USE)
    {
        action = { CL_AK_ITEM, 0, itemId };
    }
    else
    {
        action = { CL_AK_HEAL, 0, 0 };
    }
    return action;
}

// His queue line as the Link's QUEUE: what he has waiting, CL_AK_NONE for nothing
auto lineOf(const uint32 charid, const cl_action& action, const uint16 target) -> cl_queue
{
    auto line      = cardian::link::make<cl_queue>();
    line.character = charid;
    line.action    = action;
    line.target    = target;
    return line;
}

// His addon's queue line follows every change: a command queued, replaced, taken
// back, or gone at the release.
void tell(const CCharEntity* PChar, const cl_action& action = {}, const uint16 target = 0)
{
    cardian::link::send(PChar->id, lineOf(PChar->id, action, target));
}

// The dispatcher's own two steps: the command is judged as of now, not as of when
// it was queued.
template <typename T>
void deliver(CCharEntity* PChar, const CBasicPacket& packet, const Queued& what)
{
    const auto* typed = packet.as<T>();
    if (const auto result = typed->validate(PChar->PSession, PChar); result.valid())
    {
        ShowInfoFmt("pause: {}'s queued {} goes ahead{}", PChar->getName(), nameOf(what), PChar->isInEvent() ? ", with him in an event" : "");
        typed->process(PChar->PSession, PChar);
    }
    else
    {
        ShowInfoFmt("pause: {}'s queued {} is refused at the release: {}", PChar->getName(), nameOf(what), result.errorString());
    }
}

// A position packet while held is made to say where the server already has him, and
// then handled as ever: it still asks for the entities around him and carries who he
// looks at. His client is told speed 0 (packets/char_status.cpp); this covers the
// moment before it hears, and a client that does not listen.
void pin(const CCharEntity* PChar, CBasicPacket& packet)
{
    auto* pos      = packet.as<GP_CLI_COMMAND_POS>();
    pos->x         = PChar->loc.p.x;
    pos->z         = PChar->loc.p.y; // the packet's z is the server's y, as its handler reads it
    pos->y         = PChar->loc.p.z;
    pos->dir       = static_cast<int8_t>(PChar->loc.p.rotation);
    pos->MoveFlame = PChar->loc.p.moving;
}

// Would this logout request start his countdown? One that stops a countdown, or
// changes the kind of one already running, starts nothing.
auto startsLogout(const CCharEntity* PChar, const CBasicPacket& packet) -> bool
{
    const auto mode = static_cast<GP_CLI_COMMAND_REQLOGOUT_MODE>(packet.as<GP_CLI_COMMAND_REQLOGOUT>()->Mode);
    return mode != GP_CLI_COMMAND_REQLOGOUT_MODE::Off && !PChar->StatusEffectContainer->HasStatusEffect(xi::StatusEffect::Leavegame);
}

auto startsFishing(CBasicPacket& packet) -> bool
{
    return static_cast<PacketC2S>(packet.getType()) == PacketC2S::GP_CLI_COMMAND_ACTION &&
           static_cast<GP_CLI_COMMAND_ACTION_ACTIONID>(packet.as<GP_CLI_COMMAND_ACTION>()->ActionID) == GP_CLI_COMMAND_ACTION_ACTIONID::Fish;
}

} // namespace

auto intercept(CCharEntity* PChar, CBasicPacket& packet) -> bool
{
    if (!timer::is_held() || PChar == nullptr)
    {
        return false;
    }

    if (static_cast<PacketC2S>(packet.getType()) == PacketC2S::GP_CLI_COMMAND_REQLOGOUT)
    {
        if (!startsLogout(PChar, packet))
        {
            return false;
        }
        ShowInfoFmt("pause: {} may not log out of a held game", PChar->getName());
        PChar->pushPacket<GP_SERV_COMMAND_CHAT_STD>(PChar, MESSAGE_SYSTEM_3, "The game is paused. Resume it to log out.");
        return true;
    }

    if (static_cast<PacketC2S>(packet.getType()) == PacketC2S::GP_CLI_COMMAND_COMBINE_ASK)
    {
        ShowInfoFmt("pause: {} may not start a synthesis in a held game", PChar->getName());
        PChar->pushPacket<GP_SERV_COMMAND_CHAT_STD>(PChar, MESSAGE_SYSTEM_3, "Cannot perform synthesis while the game is paused.");
        return true;
    }

    if (startsFishing(packet))
    {
        ShowInfoFmt("pause: {} may not cast a line in a held game", PChar->getName());
        PChar->pushPacket<GP_SERV_COMMAND_CHAT_STD>(PChar, MESSAGE_SYSTEM_3, "Cannot fish while the game is paused.");
        // His client waits for an answer to a cast: let it go, as the game's own refusals do
        PChar->pushPacket<GP_SERV_COMMAND_EVENTUCOFF>(PChar, GP_SERV_COMMAND_EVENTUCOFF_MODE::Fishing);
        return true;
    }

    if (static_cast<PacketC2S>(packet.getType()) == PacketC2S::GP_CLI_COMMAND_POS)
    {
        pin(PChar, packet);
        return false;
    }

    const auto what = describe(packet);
    if (!what)
    {
        return false;
    }

    const auto previous = waiting.find(PChar->id);
    if (previous != waiting.end())
    {
        ShowInfoFmt("pause: {} queues {} on {} in place of {}", PChar->getName(), nameOf(*what), what->targetIndex, nameOf(previous->second.what));
    }
    else
    {
        ShowInfoFmt("pause: {} queues {} on {}", PChar->getName(), nameOf(*what), what->targetIndex);
    }

    const bool   isItem = static_cast<PacketC2S>(what->packetId) == PacketC2S::GP_CLI_COMMAND_ITEM_USE;
    const uint16 itemId = isItem ? itemIn(PChar, packet) : uint16{ 0 };
    const auto   action = actionOf(*what, itemId, packet);

    tell(PChar, action, what->targetIndex);
    waiting.insert_or_assign(PChar->id, Waiting{ *what, PChar->getZone(), itemId, action, packet.copy() });
    return true;
}

auto queued(const uint32 charid) -> std::optional<Queued>
{
    if (const auto it = waiting.find(charid); it != waiting.end())
    {
        return it->second.what;
    }
    return std::nullopt;
}

auto queueLine(const uint32 charid) -> cl_queue
{
    const auto it = waiting.find(charid);
    return it != waiting.end() ? lineOf(charid, it->second.action, it->second.what.targetIndex) : lineOf(charid, {}, 0);
}

auto cancel(CCharEntity* PChar) -> bool
{
    if (PChar == nullptr || waiting.erase(PChar->id) == 0)
    {
        return false;
    }
    ShowInfoFmt("pause: {} takes his queued command back", PChar->getName());
    tell(PChar);
    return true;
}

void replay()
{
    // A handler may do anything, this queue included: it is emptied first.
    const auto commands = std::exchange(waiting, {});

    for (const auto& [charid, command] : commands)
    {
        auto* PChar = zoneutils::GetChar(charid);
        if (PChar == nullptr || PChar->getZone() != command.zone)
        {
            ShowInfoFmt("pause: the queued {} of {} is dropped, he is {}", nameOf(command.what), charid, PChar == nullptr ? "gone" : "in another zone");
            if (PChar != nullptr)
            {
                tell(PChar);
            }
            continue;
        }

        tell(PChar);

        switch (static_cast<PacketC2S>(command.what.packetId))
        {
            case PacketC2S::GP_CLI_COMMAND_ACTION:
                deliver<GP_CLI_COMMAND_ACTION>(PChar, *command.packet, command.what);
                break;
            case PacketC2S::GP_CLI_COMMAND_ITEM_USE:
                // His bag can be sorted while held: the slot must still hold what he chose.
                if (itemIn(PChar, *command.packet) != command.itemId)
                {
                    ShowInfoFmt("pause: {}'s queued item is dropped, its slot holds something else now", PChar->getName());
                    break;
                }
                deliver<GP_CLI_COMMAND_ITEM_USE>(PChar, *command.packet, command.what);
                break;
            case PacketC2S::GP_CLI_COMMAND_CAMP:
                deliver<GP_CLI_COMMAND_CAMP>(PChar, *command.packet, command.what);
                break;
            default:
                break;
        }
    }
}

} // namespace cardian::pause::input
