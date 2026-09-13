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

// The party finder (ROADMAP H): the wild cardians who fit the party the
// player is building, and the invite that puts one in it. A candidate is
// a census body nobody has recruited, within FINDER_BAND levels of the
// player, in the player's zone or elsewhere in their city -- the range an
// invite reaches on its own (the same-city rule). Recruiting proper, the
// pearl, is a later verb; this is the party's door.

#include "common/cbasetypes.h"

#include <string>
#include <vector>

class CCharEntity;

namespace pawn::finder
{
    struct Candidate
    {
        std::string name;
        uint8       job   = 0;
        uint8       level = 0;
        std::string zone;  // the underscore name, as the roster line carries it
        std::string state; // here (standing in the player's zone), standing (elsewhere in the city), busy (standing, in a party), faded (online, no body), away
        int         rank  = 2; // sort order: here 0, standing 1, the rest 2
    };

    // Sorted: here before standing before the rest, then by level descending, then by name
    auto candidates(const CCharEntity* PPlayer) -> std::vector<Candidate>;

    // The party invite the player would send by hand, sent for them. The
    // list's own gate first (an unrecruited census body, in band, in the
    // player's zone or city -- the act does not trust the screen), then
    // the packet handler's checks (a leader or unpartied inviter, room in
    // the party, an invitee alive, unpartied and not already asked), then
    // the solicit packet the pawn answers by herself. "" on success, else
    // the reason
    auto invite(CCharEntity* PPlayer, const std::string& name) -> std::string;
} // namespace pawn::finder
