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

#include "pawn.h"

#include "common/logging.h"

#include "entities/char_entity.h"
#include "enums/packet_s2c.h"
#include "enums/party_kind.h"
#include "lua/lua_base_entity.h"
#include "lua/luautils.h"
#include "packets/basic.h"
#include "utils/moduleutils.h"

#include <utility>

namespace pawn
{
    void applyStarterKit(CCharEntity* PPawn)
    {
        const auto result = lua["xi"]["player"]["charCreate"](CLuaBaseEntity(PPawn));
        if (!result.valid())
        {
            const sol::error err = result;
            ShowErrorFmt("pawn: starter kit failed for {}: {}", PPawn->getName(), err.what());
        }
    }
} // namespace pawn

// Bindings and hooks live apart from the pawn logic: this TU pays the sol2
// template compile cost, pawn.cpp does not.
class PawnModule : public CPPModule
{
    void OnInit() override
    {
        pawn::cleanupStaleRows();

        lua["CBaseEntity"]["pawnCreate"] = [](CLuaBaseEntity* PLuaBaseEntity, const std::string& targetName) -> bool
        {
            return pawn::create(dynamic_cast<CCharEntity*>(PLuaBaseEntity->GetBaseEntity()), targetName);
        };

        lua["CBaseEntity"]["pawnSpawn"] = [](CLuaBaseEntity* PLuaBaseEntity, const std::string& targetName) -> bool
        {
            return pawn::spawn(dynamic_cast<CCharEntity*>(PLuaBaseEntity->GetBaseEntity()), targetName);
        };

        lua["CBaseEntity"]["pawnDespawn"] = [](CLuaBaseEntity* PLuaBaseEntity, const std::string& targetName) -> bool
        {
            std::ignore = PLuaBaseEntity;
            return pawn::despawn(targetName);
        };
    }

    void OnZoneTick(CZone* PZone) override
    {
        pawn::onZoneTick(PZone);
    }

    void OnPushPacket(CCharEntity* PChar, const std::unique_ptr<CBasicPacket>& packet) override
    {
        if (!pawn::isPawn(PChar) || packet->getType() != std::to_underlying(PacketS2C::GP_SERV_COMMAND_GROUP_SOLICIT_REQ))
        {
            return;
        }

        if (packet->ref<uint8>(0x0B) == std::to_underlying(PartyKind::Party))
        {
            pawn::noteInvite(PChar);
        }
    }
};
REGISTER_CPP_MODULE(PawnModule);
