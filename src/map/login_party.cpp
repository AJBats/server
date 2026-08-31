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
#include "common/settings.h"

#include "entities/char_entity.h"
#include "enums/packet_s2c.h"
#include "map_session.h"
#include "packets/basic.h"
#include "packets/char_status.h"
#include "packets/s2c/0x0c8_group_tbl.h"
#include "packets/s2c/0x0dd_group_list.h"
#include "utils/moduleutils.h"

#include <unordered_set>

// EXPERIMENT (on ice, 2026-08-31): a client that logs in solo is never told
// its party structure -- LSB sends the "you are solo" triple (empty table,
// self row at slot 0, self attrs) only when leaving a party. On this dev
// machine the retail client then sometimes shows itself twice in the party
// window from the first self-attribute packet, and every party formed
// afterwards stacks on the stray row. Sending the triple inside the GAMEOK
// response corrects it (verified in-client). Why it happens here and is
// unreported upstream is unknown: bisected against plain xiloader, Ashita
// bare, Ashita+cardian addon, Ashita+fps -- all clean -- then it stopped
// reproducing on the full config too. Gated behind
// cardian.SOLO_PARTY_AT_LOGIN.
class LoginPartyModule : public CPPModule
{
    std::unordered_set<uint32> pending; // solo zone-ins waiting for their GAMEOK dump

    void OnInit() override
    {
    }

    void OnCharZoneIn(CCharEntity* PChar) override
    {
        if (settings::get<bool>("cardian.SOLO_PARTY_AT_LOGIN") && PChar->PSession != nullptr && PChar->PParty == nullptr)
        {
            pending.insert(PChar->id);
        }
    }

    void OnCharZoneOut(CCharEntity* PChar) override
    {
        pending.erase(PChar->id);
    }

    // pushPacket runs this hook before appending the triggering packet, and
    // the merit packet is queued by the GAMEOK handler only: the triple lands
    // inside the same response the client is waiting for.
    void OnPushPacket(CCharEntity* PChar, const std::unique_ptr<CBasicPacket>& packet) override
    {
        if (packet->getType() != std::to_underlying(PacketS2C::GP_SERV_COMMAND_MERIT) || pending.erase(PChar->id) == 0)
        {
            return;
        }
        if (PChar->PSession == nullptr || PChar->PParty != nullptr)
        {
            return;
        }
        PChar->pushPacket<GP_SERV_COMMAND_GROUP_TBL>(nullptr);
        PChar->pushPacket<GP_SERV_COMMAND_GROUP_LIST>(PChar, 0, 0, PChar->getZone());
        PChar->pushPacket<CCharStatusPacket>(PChar);
        ShowInfoFmt("cardian: told {} ({}) its solo party structure inside the handshake", PChar->getName(), PChar->id);
    }
};
REGISTER_CPP_MODULE(LoginPartyModule);
