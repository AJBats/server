/*
===========================================================================

  Cardian: the world's adventurers (ROADMAP D, RESEARCH.md §11).

  Census bodies stood in a zone with no summoner and no party. Identities
  live in cardian_census; a body is minted on first need and exists only
  while a real player is in her zone -- she fades out when the zone empties
  and back in at her point when someone arrives. D0: the bodies, the fade
  and the measurement. The farm loop is D1, the slot tables D3.

===========================================================================
*/

#pragma once

#include "common/cbasetypes.h"
#include "pawn_travel.h" // position_t

#include <chrono>
#include <optional>
#include <string>

class CZone;

namespace pawn::world
{
    bool isEnabled();

    // Is this character one of the world's bodies, standing right now
    auto isBody(uint32 charid) -> bool;

    // Stand the named census adventurer at the point in the zone, minted on
    // first use. A pinned body never fades. False if she is not in the
    // census, already present, or the world is off.
    bool spawnByName(const std::string& name, CZone* PZone, const position_t& point, bool pinned);

    // Fade the named body, or every body for "all"; how many faded
    auto despawnByName(const std::string& name) -> uint32;

    // Queue count census bodies for a ring round the centre, pinned, a few
    // standing per zone tick, farming if asked. How many were queued
    auto ring(CZone* PZone, const position_t& centre, uint32 count, bool farming) -> uint32;

    // Farming (ROADMAP D1): on, she picks mobs in her band within the hunt
    // radius, and with none in reach heads for the nearest farther off and
    // fights what she meets (CPawnController::RoamTick). How many bodies
    // the name meant; "all" is a name too
    auto farm(const std::string& name, bool on) -> uint32;
    auto isFarming(uint32 charid) -> bool;

    // Once per zone tick: the debug ring, then fade with the zone's real
    // players
    void onZoneTick(CZone* PZone);

    // The D0 measurement, on under pawn.WORLD_TICK_DEBUG: the pawn module's
    // zone tick and every controller tick, summed per zone and said every
    // hundred ticks
    auto tickDebug() -> bool;
    void noteModuleTick(CZone* PZone, std::chrono::nanoseconds elapsed, uint32 pawnsInZone);
    void noteBrainTick(uint16 zoneId, std::chrono::nanoseconds elapsed);

    // The load line, always on: every pawn.WORLD_LOAD_REPORT seconds, how
    // many bodies stand in how many zones, what a body's tick costs, and
    // the process's CPU and memory
    void reportLoad(std::chrono::steady_clock::time_point now);
} // namespace pawn::world
