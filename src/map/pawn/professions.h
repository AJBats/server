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
#include "common/mmo.h"

#include <optional>
#include <string>

class CCharEntity;

// Professions and the money venture, the map's side (RESEARCH §11.13,
// "Professions"). A profession is set up on an alt or a cardian his account
// owns from her Linkshell submenu (the Link's PROFESSIONS, START_PROFESSION).
// Its ventures go out as errands of kind money (errands.h), each with its kit
// -- fishing: a rod and a bait, each one she holds or one bought at the
// auction house as it starts (only the starter rods are ever bought) -- and
// the kit sent becomes her set. While one is under way the venture keeper,
// the census watcher's Python (tools/economy/venture_keeper.py), works it on
// the game clock, and this side answers what the keeper alone knows from its
// tables: the catalog of rods and baits with the price book's prices, each
// fishing cardian's safe spots with every rod and bait's estimate there, and
// her venture report. Its news -- a rod broken -- goes to his addon as the
// payments' lines do.
//
// She pays for her kit from her own purse, then from his: the keeper asks
// for a shortfall in cardian_venture_pay and this side takes it from the
// character the player plays now (his gil in memory), or offline from the
// one he played last (cardian_last_played, his saved gil), and tells his
// addon (VENTURE_PAID) at once or once it binds.
namespace pawn::professions
{
    // The tables the map reads and writes, made at boot; the keeper makes the
    // ones it works from as it starts
    void ensureTables();
    void registerHandlers();

    // The payment asks answered, and the lines his addon has not had sent:
    // every few seconds, from the zone tick
    void tick();

    // A real player in a zone: the character his account plays now, the payer
    // of last resort while he is offline
    void zonedIn(CCharEntity* PChar);

    // The keeper beat lately: a money venture is worked only while it runs
    auto keeperAlive() -> bool;

    // She has taken up the profession
    auto hasProfession(uint32 charid, uint8 profession) -> bool;

    // A rod a venture of hers may take: a starter rod, bought as it starts,
    // or one she holds free (not worn, not in her bazaar)
    auto rodTakeable(uint32 charid, uint16 rod) -> bool;

    // Her profession's set: the kit she was last sent with
    void saveSet(uint32 charid, uint8 profession, uint16 rod, uint16 bait);

    // She has taken up a profession, any
    auto hasAnyProfession(uint32 charid) -> bool;

    // A spot of hers, from the keeper's rows: safe at her level, by its area
    struct Spot
    {
        uint16      zone = 0;
        uint16      area = 0;
        std::string name;     // the area's own name
        position_t  centre{}; // fishing_area's centre: where she is saved while she works there
    };
    auto spotOf(uint32 charid, uint8 profession, uint16 zone, uint16 area) -> std::optional<Spot>;

    // A bait of the catalog
    auto isBait(uint16 item) -> bool;

    // She has a venture report to show
    auto hasReport(uint32 charid) -> bool;
} // namespace pawn::professions
