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

#include "common/cbasetypes.h"

class CBaseEntity;
class CCharEntity;
class CZone;
class Scheduler;

#include <functional>

// A player's view origin: the entity his client is looking through. The
// server tells a client about the world within render range of the player's
// body; while the player controls a cardian, the addon holds his camera on
// her, and the world around HER must reach his client too. The zone's spawn
// and packet-push checks (zone_entities.cpp) ask here, and count a player as
// seeing whatever is within range of his body OR of his view origin. The
// body's own sphere never goes away: what stands beside him keeps rendering
// and keeps aggroing. Core (MAP_CORE_SOURCES): the zone asks, and xi_test
// links it without a stub.
namespace cardian::view
{
    // Look through this entity (in his zone), or through nobody
    void set(const CCharEntity* PChar, const CBaseEntity* PEntity);
    void clear(const CCharEntity* PChar);
    void clearById(uint32 charid); // the Link's unbind: his session is gone, his view with it

    // The entity he looks through, resolved now: nullptr when he looks through
    // nobody, or it is gone or in another zone
    auto origin(const CCharEntity* PChar) -> CBaseEntity*;

    // Every player in this zone looking through somebody, with that somebody:
    // for the entity update fan-out, which finds its recipients by proximity
    // to the entity and so cannot find a viewer standing far from it
    void forEachViewer(CZone* PZone, const std::function<void(CCharEntity*, const CBaseEntity*)>& fn);

    // Is anybody looking through this entity: its position updates then go
    // out at the steering cadence (entities/char_entity.cpp)
    auto isViewed(const CBaseEntity* PEntity) -> bool;

    // The steer tick: a main-thread timer every kSteerPeriodMs, armed with
    // the Link's scheduler (cardian_link.cpp), that runs whatever the pawn
    // module registers -- the steered cardians' steps. Core holds only the
    // function; xi_test never arms it, and never sets it.
    // 16 ms: one cardian's step and re-path at frame rate cost microseconds,
    // and the logic tick's 400 ms is the one delay this cannot touch
    constexpr auto kSteerPeriodMs = 16;
    void startTimers(Scheduler& scheduler);
    void setSteerTick(std::function<void()> fn);
    auto timersArmed() -> bool;
} // namespace cardian::view
