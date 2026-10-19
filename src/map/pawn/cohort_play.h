/*
===========================================================================

  Cardian: the cohorts in play, and the town sign-in (RESEARCH.md §18.5,
  ROADMAP D).

  The census decides which cohort each real character's job rides and
  writes it in cardian_cohort_rides (census.py cohort_rides). A cohort is
  in play while a real character with a client online plays, as his main
  job, a job that rides it -- a client's session under his charid
  (players::hasClient), never one of his characters standing as a
  cardian: read here from the rides and his live job, at
  most every few seconds, alongside the world's read of the census
  (world.cpp dealableNames), never per question. A peer or a rival is in
  play while one of her cohorts is; the shared pools always are
  (cohort_play_math.h).

  The town sign-in keeps the cohorts in play about half of who is online
  in the towns: each city's bodies of the cohorts in play, seated and
  signed in, at its regulars -- the shared pools' bodies online there --
  times pawn.WORLD_COHORT_TOWN_SHARE. A body of a cohort in play the world
  holds nothing of signs in faded and seatless, to the town short the
  most (world::signInTown): search and the party finder's shout find her,
  and she stands only when a party brings her (the ladder's partyOnly).
  She signs out when her cohort leaves play, or when her town runs more
  than five over its target; never while in a real player's party, held
  for a player, or away. A seat out of a city outranks the sign-in: it may
  deal her, and she leaves her town for it (world.cpp chooseOccupants).
  Her town's entries past the faded cap sign her out too, and the world
  lets her go with her row (world::signInDropped). Paced by the world's
  placement budget, after every seat waiting to be placed; one who cannot
  be made ready is passed over for five minutes.

===========================================================================
*/

#pragma once

#include "common/cbasetypes.h"

#include <string>
#include <unordered_set>

namespace pawn::cohorts
{
    // The rides table as the census writes it, made as the map boots so a
    // map that boots before the watcher reads an empty one
    void ensureTable();

    // The rides read again and the cohorts in play worked out from the real
    // players online and the job each plays now. The world's read of the
    // census calls it, at most every few seconds
    void refresh();
    auto active() -> const std::unordered_set<uint32>&;

    // A body of the census in play as of the last refresh: one of the
    // shared pools always, a peer or a rival while one of her cohorts is
    auto inPlay(const std::string& anchor, uint32 cohort, uint32 cohort2) -> bool;

    // The town sign-in, from the zone tick; paced by itself
    void townTick();
} // namespace pawn::cohorts
