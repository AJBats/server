/*
===========================================================================

  Cardian: the world's adventurers (ROADMAP D, RESEARCH.md §11).

  Census bodies stood in a zone with no summoner and no party. Identities
  live in cardian_census; a body is minted on first need and exists only
  while a real player is in her zone -- she fades out when the zone empties
  and back in at her point when someone arrives. D0: the bodies, the fade
  and the measurement; D1 the farm loop; D3 the slot tables: a Cardian-owned
  YAML per zone (modules/cardian/world/<Zone>.yaml) says what happens where,
  a query fills it from the census, every occupant has presence from boot.

===========================================================================
*/

#pragma once

#include "common/cbasetypes.h"
#include "pawn_travel.h" // position_t

#include <chrono>
#include <optional>
#include <string>
#include <utility>
#include <vector>

class CZone;
class CCharEntity;

namespace pawn::world
{
    bool isEnabled();

    // Is this character one of the world's bodies, standing right now
    auto isBody(uint32 charid) -> bool;
    // A world body standing or faded: a seat, a stand, the ring
    auto hasBody(uint32 charid) -> bool;

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
    // Her slot's home pull, if it has one: her starting point and the roam
    // distance at which the pull weighs as much as the walk (pawn::HuntRules)
    auto homeOf(uint32 charid) -> std::optional<std::pair<position_t, float>>;

    // Camps (ROADMAP D5): a camp slot's occupants are a party. Her camp's
    // present leader (the highest non-healer, else the highest; herself when
    // she is it), 0 when she holds no camp seat or nobody of it stands; and
    // the camp's party size, 1 for a solo seat
    auto campLeaderOf(uint32 charid) -> uint32;
    auto campSizeOf(uint32 charid) -> uint32;

    // Her brain from modules/cardian/world/brains.yaml (re-read when the file
    // changes): common rows, her job's, her role's -- tank (the highest
    // Warrior of her party), melee or mage -- in the row grammar
    auto brainRows(const CCharEntity* PPawn) -> std::vector<std::pair<std::string, bool>>;
    auto roleName(uint32 charid) -> std::string;

    // The zone's slot table: one line per slot with its occupants; refill
    // the zone from a fresh read of its file (its bodies fade and return to
    // the pool first); append a slot at a point to the file and fill it
    auto slots(CZone* PZone) -> std::vector<std::string>;
    auto fill(CZone* PZone) -> uint32;
    auto addSlot(CZone* PZone, const std::string& activity, uint8 low, uint8 high, uint8 count, float spread, const position_t& at) -> bool;

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
