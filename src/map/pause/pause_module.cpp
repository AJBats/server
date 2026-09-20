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

#include "utils/moduleutils.h"

// The pause's own seat in the module system, for the two duties the module hooks
// serve. A tick that keeps coming while the simulation is held, to let go of a hold
// whose holder went offline: the time server is process-wide and runs every 2.4 s,
// held or not. And a word on every validated client packet before its handler, which
// is the input gate (input_gate.h).
class CardianPauseModule : public CPPModule
{
    void OnInit() override
    {
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
