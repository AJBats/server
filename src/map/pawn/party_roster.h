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

#include "party_roles.h"

#include "common/cbasetypes.h"

#include <string>
#include <vector>

class CBattleEntity;
class CCharEntity;

// A player's party and its roles, as the party screen shows them (RESEARCH
// §17). The join rule (party_roles.h) is pure; this is its adapter: it reads
// the party the game holds, gathers what the rule asks of each member, and
// keeps the roles the player chose himself. The choices are the player's and
// in memory, like his party's strategy: a server restart forgets them, and
// so does the member's leaving his party, her body's being taken down included.
namespace pawn::roster
{
    // One member as the screen shows her
    struct Row
    {
        uint32               id        = 0;
        std::string          name;
        uint8                mainJob   = 0;
        uint8                mainLevel = 0;
        uint8                subJob    = 0;
        uint8                subLevel  = 0;
        cardian::party::Role role      = cardian::party::Role::None;
        bool                 byPlayer  = false; // he chose it; else the join rule gave it
        CCharEntity*         who       = nullptr; // the member herself, for a caller that shows more of her: good for this call only, never kept
    };

    // Every member of the player's party and her role: the player first,
    // then the party's other characters in the order the party holds them.
    // Settled afresh from the party as it stands, so a member who joined,
    // left or changed what she wears is judged as she is now. A role that
    // changed since the last call is said once in the map log.
    auto rolesOf(CCharEntity* PPlayer) -> std::vector<Row>;

    // A member's role as her party stands, for the gambit engine's tick:
    // None when she is with no player. Her party is settled at most every
    // two seconds for everyone in it, at once after a choice, and by the
    // same settle the screen reads; while she or her player is between
    // zones her last role holds for a minute
    auto roleOf(CCharEntity* PMember) -> cardian::party::Role;

    // The player's choice of a member's role, his from here on. CL_S_OK, or
    // CL_S_NOT_IN_PARTY when nobody by that charid is in his party.
    auto choose(CCharEntity* PPlayer, uint32 memberId, cardian::party::Role role) -> uint16;

    // The player takes his choice for a member back: the join rule decides
    // her role again. CL_S_OK, whether or not he had chosen for her, or
    // CL_S_NOT_IN_PARTY.
    auto release(CCharEntity* PPlayer, uint32 memberId) -> uint16;

    // A member out of her party (pawn::leftParty: she left or was removed, or
    // her body was taken down or faded; never a zone line, where she is only
    // off the party's list for a moment): the choices made for her are
    // forgotten, and her own when she is a player.
    void memberLeft(const CBattleEntity* PMember);
} // namespace pawn::roster
