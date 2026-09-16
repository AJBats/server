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

#pragma once

#include "conveyor.h"
#include "conveyor_math.h"

#include "common/cbasetypes.h"

class CCharEntity;

namespace pawn::tactics
{
    class FightLog;
} // namespace pawn::tactics

namespace pawn::tactics::role
{
    using cardian::tactics::Pace;

    // The Support Mage role's feeders (RESEARCH §12.2 item 2, §12.6,
    // §12.14): one row switches it on, and it stands on its own with no
    // other row. Any job may hold it; nothing here reads her job, only
    // what she can cast and what the bank says it is worth. It feeds the
    // conveyor; the conveyor decides who casts.

    // Every tick: a member about to die is fed as the reflex, the role's
    // one shortcut past the queue
    void reflex(CCharEntity* PHolder, FightLog& log, Conveyor& conveyor, const Conveyor::Scope& scope, double now);

    // On her think: cures where the missing HP has piled up to a tier she
    // has (the biggest in a fight, the smallest between, so nothing
    // overcures), and the debuffs the bank prices as worth it, while she
    // is in the fight
    void think(CCharEntity* PHolder, FightLog& log, Conveyor& conveyor, const Conveyor::Scope& scope, bool engaged, double now);

    // Her pace at the spot, one cycle per stretch of fighting: what the
    // cycle cost her against her MP's net change since the last, said
    // once each way it turns, in the map log and to the party
    void cycleOpened(CCharEntity* PHolder, Pace& pace);
    void cycleClosed(CCharEntity* PHolder, int32 spent, Pace& pace);
    void speakPace(CCharEntity* PHolder, Pace& pace);
} // namespace pawn::tactics::role
