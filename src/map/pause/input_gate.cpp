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
#include "enums/packet_c2s.h"
#include "item_container.h"
#include "items/item.h"
#include "packets/basic.h"
#include "packets/c2s/0x015_pos.h"
#include "packets/c2s/0x01a_action.h"
#include "packets/c2s/0x037_item_use.h"
#include "packets/c2s/0x0e8_camp.h"
#include "pawn/cardian_link.h"
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
    std::string                   line;       // "2:2:1 1024": its key and target index, for his queue line
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

// What he chose and on whom, for his command window's queue line: the action's key
// and the target's index, in the terms the server already speaks -- the catalogue's
// kind:mode:id where the action has one (pawn_gambits.cpp), `item:<id>`, and
// `cmd:<name>` for the rest of the action menu. The addon words it.
auto lineOf(const Queued& what, const uint16 itemId, const CBasicPacket& packet) -> std::string
{
    std::string key;
    if (static_cast<PacketC2S>(what.packetId) == PacketC2S::GP_CLI_COMMAND_ACTION)
    {
        const auto* typed = packet.as<GP_CLI_COMMAND_ACTION>();
        switch (static_cast<GP_CLI_COMMAND_ACTION_ACTIONID>(what.actionId))
        {
            case GP_CLI_COMMAND_ACTION_ACTIONID::CastMagic:
                key = fmt::format("2:2:{}", typed->CastMagic.SpellId);
                break;
            case GP_CLI_COMMAND_ACTION_ACTIONID::JobAbility:
                key = fmt::format("3:2:{}", typed->JobAbility.SkillId);
                break;
            case GP_CLI_COMMAND_ACTION_ACTIONID::Weaponskill:
                key = fmt::format("4:2:{}", typed->Weaponskill.SkillId);
                break;
            case GP_CLI_COMMAND_ACTION_ACTIONID::Shoot:
                key = "1:0:0";
                break;
            default:
                key = "cmd:" + nameOf(what);
                break;
        }
    }
    else if (static_cast<PacketC2S>(what.packetId) == PacketC2S::GP_CLI_COMMAND_ITEM_USE)
    {
        key = fmt::format("item:{}", itemId);
    }
    else
    {
        key = "cmd:Heal";
    }
    return fmt::format("{} {}", key, what.targetIndex);
}

// His addon's queue line follows every change: a command queued, replaced, taken
// back, or gone at the release.
void tell(const CCharEntity* PChar, const std::string& line)
{
    cardian::link::sendToCharacter(PChar->id, line.empty() ? fmt::format("cd q {}", PChar->getName()) : fmt::format("cd q {} {}", PChar->getName(), line));
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

} // namespace

auto intercept(CCharEntity* PChar, CBasicPacket& packet) -> bool
{
    if (!timer::is_held() || PChar == nullptr)
    {
        return false;
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
    auto         line   = lineOf(*what, itemId, packet);

    tell(PChar, line);
    waiting.insert_or_assign(PChar->id, Waiting{ *what, PChar->getZone(), itemId, std::move(line), packet.copy() });
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

auto queuedLine(const uint32 charid) -> std::string
{
    const auto it = waiting.find(charid);
    return it != waiting.end() ? it->second.line : std::string();
}

auto cancel(CCharEntity* PChar) -> bool
{
    if (PChar == nullptr || waiting.erase(PChar->id) == 0)
    {
        return false;
    }
    ShowInfoFmt("pause: {} takes his queued command back", PChar->getName());
    tell(PChar, "");
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
                tell(PChar, "");
            }
            continue;
        }

        tell(PChar, "");

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
