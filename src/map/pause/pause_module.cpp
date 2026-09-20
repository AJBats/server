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
#include "pause.h"

#include "entities/char_entity.h"
#include "lua/lua_base_entity.h"
#include "utils/moduleutils.h"

// The pause's own seat in the module system, for the two duties the module hooks
// serve. A tick that keeps coming while the simulation is held, to let go of a hold
// whose holder went offline: the time server is process-wide and runs every 2.4 s,
// held or not. And a word on every validated client packet before its handler, which
// is the input gate (input_gate.h).
class CardianPauseModule : public CPPModule
{
    // The pause button's server side: `!cardian pause`, which the addon sends over the
    // Link. Answers with why not, or with nothing when the hold was taken or let go.
    void OnInit() override
    {
        lua["CBaseEntity"]["cardianPause"] = [](CLuaBaseEntity* PLuaBaseEntity) -> std::string
        {
            const auto* PChar = dynamic_cast<CCharEntity*>(PLuaBaseEntity->GetBaseEntity());
            if (PChar == nullptr)
            {
                return "no character";
            }
            return cardian::pause::toggle(PChar->id, PChar->getName());
        };

        // The player's own queued command, for his command window's queue line ("" with
        // none), and him taking it back
        lua["CBaseEntity"]["cardianQueuedOwn"] = [](CLuaBaseEntity* PLuaBaseEntity) -> std::string
        {
            return cardian::pause::input::queuedLine(PLuaBaseEntity->GetBaseEntity()->id);
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
    }
};

REGISTER_CPP_MODULE(CardianPauseModule);
