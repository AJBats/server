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

    struct Threat
    {
        double biggestHit = 0.0;
        double takenPerSecond = 0.0;
        double expectedPerSecond = 0.0; // the part of the rate expected of the mobs on her (the map log's word on it)
    };
    auto threat(FightLog& log, CBattleEntity* PMember, double now) -> Threat;

    // The Support Mage role's feeders (RESEARCH §12.2 item 2, §12.6,
    // §12.14): one row switches it on, and it stands on its own with no
    // other row. Any job may hold it; nothing here reads her job, only
    // what she can cast and what the bank says it is worth. It feeds the
    // conveyor; the conveyor decides who casts.

    // On her think: cures where the missing HP has piled up to where her
    // smallest tier lands whole, and the debuffs the bank prices as worth
    // it, while she is in the fight
    void think(CCharEntity* PHolder, FightLog& log, Conveyor& conveyor, const Conveyor::Scope& scope, bool engaged, double now);

    // Her pace at the spot, one cycle per stretch of fighting: what the
    // cycle cost her against her MP's net change since the last, said
    // once each way it turns, in the map log and, from the Healer seat, to
    // the party (sayPace)
    void cycleOpened(CCharEntity* PHolder, Pace& pace);
    void cycleClosed(CCharEntity* PHolder, int32 spent, Pace& pace);
    void speakPace(CCharEntity* PHolder, Pace& pace);

    // A line in her party's chat, as the game sends one
    void sayParty(CCharEntity* PChar, const std::string& text);
    // A line about the party's MP pace -- behind it or back on it, ready in
    // so long, her MP running out before the fight does -- said in party
    // chat only by the cardian in the Healer seat: the pace is the Healer's
    // to call, and every other mage's stays in the map log
    void sayPace(CCharEntity* PChar, const std::string& text);
} // namespace pawn::tactics::role
