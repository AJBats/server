// Cardian: the game's trade packets as a cardian reads them (pawn/club.h).
// A player's trade with her is played on her side by the server: the game
// pushes her what it would push a player's client -- his request (0x021),
// the window's answers (0x022) and every slot he fills (0x023) -- and these
// read them off the packet by the game's own packet layouts. Header-only, so
// xi_test pins the reading against the game's packets.
#pragma once

#include "club_math.h"

#include "packets/s2c/0x021_item_trade_req.h"
#include "packets/s2c/0x022_item_trade_res.h"
#include "packets/s2c/0x023_item_trade_list.h"

#include <cstddef>
#include <cstdint>

namespace cardian::club
{
    // Where a packet's own fields start, past the game's header
    constexpr std::size_t kTradePacketData = sizeof(GP_SERV_HEADER);

    // 0x021: the charid of the player asking her to trade
    inline auto tradeAsker(CBasicPacket& packet) -> uint32_t
    {
        using Data = GP_SERV_COMMAND_ITEM_TRADE_REQ::PacketData;
        return packet.ref<uint32_t>(kTradePacketData + offsetof(Data, UniqueNo));
    }

    // 0x022: what became of the trade -- opened, his Trade pressed, cancelled, done
    inline auto tradeResult(CBasicPacket& packet) -> GP_ITEM_TRADE_RES_KIND
    {
        using Data = GP_SERV_COMMAND_ITEM_TRADE_RES::PacketData;
        return static_cast<GP_ITEM_TRADE_RES_KIND>(packet.ref<uint32_t>(kTradePacketData + offsetof(Data, Kind)));
    }

    // 0x023: one slot of his offer, as the window shows it her. A linkshell
    // item carries its shell and its kind where its extra data keeps them
    struct OfferedSlot
    {
        uint8_t slot = 0;
        Offered offered;
    };

    inline auto offeredSlot(CBasicPacket& packet) -> OfferedSlot
    {
        using Data = GP_SERV_COMMAND_ITEM_TRADE_LIST::PacketData;
        OfferedSlot out;
        out.slot        = packet.ref<uint8_t>(kTradePacketData + offsetof(Data, TradeIndex));
        out.offered.qty = packet.ref<uint32_t>(kTradePacketData + offsetof(Data, ItemNum));
        if (out.offered.qty == 0)
        {
            return out;
        }
        out.offered.item = packet.ref<uint16_t>(kTradePacketData + offsetof(Data, ItemNo));
        if (isLinkshellItem(out.offered.item))
        {
            const auto* attr   = &packet.ref<uint8_t>(kTradePacketData + offsetof(Data, Attr));
            out.offered.lsid   = lsidOf(attr, sizeof(Data::Attr));
            out.offered.lsType = lsTypeOf(attr, sizeof(Data::Attr));
        }
        return out;
    }
} // namespace cardian::club
