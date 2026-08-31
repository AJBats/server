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

#include "common/logging.h"

#include "entities/char_entity.h"
#include "enums/packet_s2c.h"
#include "map_session.h"
#include "packets/basic.h"
#include "packets/char_status.h"
#include "packets/s2c/0x0c8_group_tbl.h"
#include "packets/s2c/0x0dd_group_list.h"
#include "party.h"
#include "party_teach.h"
#include "utils/moduleutils.h"

#include <unordered_set>

// Party re-teaching after possession (see party_teach.h): the client's
// identity changed mid-party and its cached "my row" is stale until a
// structural reset. Once the GAMEOK dump is being queued, send the "you are
// solo" triple LSB uses on leaving a party, then the party's real table.
namespace
{
    std::unordered_set<uint32> pendingReteach; // waiting for the GAMEOK dump
} // namespace

namespace partyteach
{
    void requestReteach(const uint32 charID)
    {
        pendingReteach.insert(charID);
    }
} // namespace partyteach

class PartyTeachModule : public CPPModule
{
    void OnInit() override
    {
    }

    void OnCharZoneOut(CCharEntity* PChar) override
    {
        pendingReteach.erase(PChar->id);
    }

    // Fires while the GAMEOK handler is queueing its dump (the merit packet
    // is queued by that handler only). pushPacket runs this hook before it
    // appends the triggering packet, so everything pushed here lands inside
    // the same response the client is waiting for.
    void OnPushPacket(CCharEntity* PChar, const std::unique_ptr<CBasicPacket>& packet) override
    {
        if (packet->getType() != std::to_underlying(PacketS2C::GP_SERV_COMMAND_MERIT) || PChar->PSession == nullptr)
        {
            return;
        }
        if (pendingReteach.erase(PChar->id) == 0)
        {
            return;
        }

        PChar->pushPacket<GP_SERV_COMMAND_GROUP_TBL>(nullptr);
        PChar->pushPacket<GP_SERV_COMMAND_GROUP_LIST>(PChar, 0, 0, PChar->getZone());
        PChar->pushPacket<CCharStatusPacket>(PChar);

        if (PChar->PParty != nullptr)
        {
            PChar->PParty->ReloadParty();
        }
        ShowInfoFmt("possess: re-taught {} ({}) its party inside the handshake", PChar->getName(), PChar->id);
    }
};
REGISTER_CPP_MODULE(PartyTeachModule);
