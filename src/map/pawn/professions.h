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
// the kit sent becomes her set. The map fishes one itself (angler.h). The
// venture keeper, the census watcher's Python
// (tools/economy/venture_keeper.py), still writes the menus' tables: the
// catalog of rods and baits with the price book's prices, and each fishing
// cardian's zones (#432 moves them to the map).
//
// She pays for her kit from her own purse, then from his: the character the
// player plays now (his gil in memory), or offline the one he played last
// (cardian_last_played, his saved gil). A purchase his gil helped with, and a
// venture's news, are rows his addon is told of (VENTURE_PAID, VENTURE_NEWS)
// at once or once it binds.
namespace pawn::professions
{
    // The tables the map reads and writes, made at boot; the keeper makes the
    // ones it works from as it starts
    void ensureTables();
    void registerHandlers();

    // The lines his addon has not had sent: every few seconds, from the zone
    // tick
    void tick();

    // A real player in a zone: the character his account plays now, the payer
    // of last resort while he is offline
    void zonedIn(CCharEntity* PChar);

    // She has taken up the profession
    auto hasProfession(uint32 charid, uint8 profession) -> bool;

    // A rod a venture of hers may take: a starter rod, bought as it starts,
    // or one she holds free (not worn, not in her bazaar)
    auto rodTakeable(uint32 charid, uint16 rod) -> bool;

    // Her profession's set: the kit she was last sent with, and whether she
    // let items go
    void saveSet(uint32 charid, uint8 profession, uint16 rod, uint16 bait, bool letItemsGo);

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

    // One of the game's baits and lures (fishing_bait)
    auto isBait(uint16 item) -> bool;

    // She has a venture report to show: any but one her player has read
    // since it was done (over, and nothing left to sell)
    auto hasReport(uint32 charid) -> bool;

    // Her last venture's profession when it is over and her player has not
    // read its report since (the page's "Fishing Venture Completed!"); 0
    // otherwise
    auto ventureDone(uint32 charid) -> uint8;

    // His share of a purchase: the character his account plays now, in
    // memory; offline, the one it played last, in its saved row (its body is
    // not loaded, so the row is the gil). Answers the payer, 0 when he is
    // short; refundHim hands it back when the purchase then fails
    auto payFromHim(uint32 accid, uint32 gil) -> uint32;
    void refundHim(uint32 payer, uint32 gil);

    // The catalog's price for one, nullopt where the auction house sells none
    auto priceOf(uint16 item) -> std::optional<uint32>;
    // A starter rod: the only rods a venture buys
    auto isStarter(uint16 rod) -> bool;

    // A purchase his gil helped pay for: the purchase line his addon is told
    struct Purchase
    {
        uint32 cardian    = 0;
        uint32 accid      = 0;
        uint8  profession = 0;
        uint16 item       = 0;
        uint16 quantity   = 0;
        uint32 herGil     = 0;
        uint32 hisGil     = 0;
        uint32 payer      = 0;
    };
    void recordPurchase(const Purchase& purchase);

    // A venture's news for his log (CL_NEWS_*)
    void recordNews(uint32 cardian, uint32 accid, uint8 news, uint16 item);
} // namespace pawn::professions
