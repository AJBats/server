/*
===========================================================================

  Cardian: the seat waterfall's adapter (ROADMAP H). Owns the one Ladder
  (seat_ladder.h), hands it its lookups and its engine, and is the only
  code that talks to it. The rest of the pawn code says what a cardian IS
  through the calls below; the ladder says where she stands.

  The engine callbacks never call back into the ladder: a run holds
  references into its own vector while it acts.

===========================================================================
*/

#pragma once

#include "common/cbasetypes.h"

#include <string>
#include <vector>

namespace pawn::seats
{
    // Built once at module init
    void init();

    // What she is. A seat dealt to one of the world's own (her tier from
    // the party memory); an owned cardian, an alt or a recruit, under her
    // player (her tier from cardian_pawns); KO'd past the world's delay or
    // up again; a zone line crossed; out of the waterfall for good
    void offerWorld(uint32 charid, uint16 zone);
    void offerOwned(uint32 charid, uint32 ownerCharID, uint16 zone);
    void setDown(uint32 charid, bool down);
    void moved(uint32 charid, uint16 zone);
    void touch(uint32 charid);
    void withdraw(uint32 charid);
    auto withdrawOwnedBy(uint32 ownerCharID) -> uint32;
    bool has(uint32 charid);

    // The party memory: the two of them were in a party, most recently
    // now. The row, her tier if she is the world's, and a touch
    void notePartied(uint32 playerCharID, uint32 pawnCharID);

    // A summon (!pawnspawn): the ladder's next stand of her lands beside
    // this player rather than where the game saved her
    void summon(uint32 charid, uint32 playerCharID);

    // The run: tick() every 500 ms from the zone tick, run() at once after
    // a deliberate act so it can say whether she stood
    void tick();
    void run();
    bool isStanding(uint32 charid);

    // The caps, live: 0 puts the setting back. The line reads both caps,
    // the server's standing population zone by zone, and this zone's gap
    void setCaps(uint32 standing, uint32 faded);
    auto capsLine(uint16 zoneId) -> std::string;

    // The recall list: her player's cardians without a body, by name; and
    // a recall, to the front of her tier and a run. "" on success, else
    // why not
    auto fadedNames(uint32 ownerCharID) -> std::vector<std::string>;
    auto recall(uint32 ownerCharID, const std::string& name) -> std::string;
} // namespace pawn::seats
