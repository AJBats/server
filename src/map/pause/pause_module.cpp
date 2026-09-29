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

#include "calendar_store.h"
#include "input_gate.h"
#include "pause.h"

#include "entities/char_entity.h"
#include "lua/lua_base_entity.h"
#include "pawn/cardian_link.h"
#include "utils/moduleutils.h"

#include <fmt/format.h>

namespace
{
    // The pause button's refusal as a typed `!cardian pause` answers it
    auto refusalText(const uint16 status) -> std::string
    {
        switch (status)
        {
            case CL_S_OK:
                return "";
            case CL_S_PAUSE_OFF:
                return "the pause is switched off on this server";
            case CL_S_LOGGING_OUT:
                return "you are logging out";
            case CL_S_SYNTHESIZING:
                return "Cannot pause while performing synthesis.";
            case CL_S_FISHING:
                return "Cannot pause while fishing.";
            case CL_S_PAUSED_BY_OTHER:
                return fmt::format("{} has the game paused", cardian::pause::status().holderName);
            default:
                return fmt::format("refused ({})", status);
        }
    }
} // namespace

// The pause's own seat in the module system, for the two duties the module hooks
// serve. A tick that keeps coming while the simulation is held, to let go of a hold
// whose holder went offline: the time server is process-wide and runs every 2.4 s,
// held or not. And a word on every validated client packet before its handler, which
// is the input gate (input_gate.h).
class CardianPauseModule : public CPPModule
{
    // The pause button's server side: the addon's PAUSE over the Link, and a typed
    // `!cardian pause` (player:cardianPause(), which answers with why not, or with
    // nothing when the hold was taken or let go)
    void OnInit() override
    {
        cardian::link::handle<cl_pause>([](CCharEntity* PChar, const cl_pause& ask, cardian::link::Reply& reply)
                                        {
                                            reply.finish(ask, cardian::pause::toggle(PChar));
                                        });

        lua["CBaseEntity"]["cardianPause"] = [](CLuaBaseEntity* PLuaBaseEntity) -> std::string
        {
            auto* PChar = dynamic_cast<CCharEntity*>(PLuaBaseEntity->GetBaseEntity());
            if (PChar == nullptr)
            {
                return "no character";
            }
            return refusalText(cardian::pause::toggle(PChar));
        };

        // The player's own queued command as his queue line names it -- { kind, mode,
        // id, target }, the Link's cl_action and target index; nil with none -- and him
        // taking it back
        lua["CBaseEntity"]["cardianQueuedOwn"] = [](CLuaBaseEntity* PLuaBaseEntity) -> sol::object
        {
            const auto line = cardian::pause::input::queueLine(PLuaBaseEntity->GetBaseEntity()->id);
            if (line.action.kind == CL_AK_NONE)
            {
                return sol::lua_nil;
            }
            auto fields      = ::lua.create_table();
            fields["kind"]   = line.action.kind;
            fields["mode"]   = line.action.mode;
            fields["id"]     = line.action.id;
            fields["target"] = line.target;
            return fields;
        };
        lua["CBaseEntity"]["cardianCancelOwn"] = [](CLuaBaseEntity* PLuaBaseEntity) -> bool
        {
            return cardian::pause::input::cancel(dynamic_cast<CCharEntity*>(PLuaBaseEntity->GetBaseEntity()));
        };
    }

    auto OnIncomingPacket(MapSession* /* PSession */, CCharEntity* PChar, CBasicPacket& packet) -> bool override
    {
        return cardian::pause::input::intercept(PChar, packet);
    }

    void OnTimeServerTick() override
    {
        cardian::pause::letGoIfHolderLeft();
        cardian::pause::calendar::saveIfDue();
    }
};

REGISTER_CPP_MODULE(CardianPauseModule);
