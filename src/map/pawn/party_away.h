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

#include <string>
#include <string_view>
#include <vector>

class CCharEntity;

// His party's away members (the user, 2026-10-27 and 10-28): the Party
// screen is the one place a party is managed. A member's seat is opened as
// she joins him and closed only by something he does on purpose -- his kick
// (the game's /pcmd kick, or its party menu), his leaving or breaking up the
// party, a venture he sends her on, her Release. Out of his party any other
// way -- his logout, a crash, the server going down -- she keeps it, and the
// Party screen shows her greyed where she sat, with Invite and Release where
// her numbers stood, while she shows the game's seeking-party flag.
//
// Only one he can invite back with no shout keeps a seat: his alt, a cardian
// his account owns, one of the world's his contract holds or his linkshell's
// pearl holds (club.h, party_finder.h). A teleport hire, another player's
// cardian and another player never do. Every open contract of his has one,
// whether she has joined him yet or not, and closing a contract hire's seat
// releases her contract: a hire is her place in his party. A seat is a row
// of `cardian_party_seats`, its order when she first joined him; a seat
// closed is its row deleted. One no longer his to invite (her pearl broken,
// her contract ended) loses it too.
namespace pawn::away
{
    enum class Kind : uint8
    {
        Alt,
        Owned,
        Contract,
        Pearl,
    };

    struct Seat
    {
        uint32      charid = 0;
        std::string name;
        uint8       job   = 0;
        uint8       level = 0;
        uint8       sjob  = 0;
        uint8       slvl  = 0;
        Kind        kind  = Kind::Alt;
        uint16      zone  = 0; // where she is, 0 when not known
    };

    // The table, made as xi_map boots
    void ensureTable();

    // His party as the roles settle it (party_roster.cpp): each member who
    // keeps a seat and has none yet takes the next
    void note(const CCharEntity* PPlayer, const std::vector<CCharEntity*>& members);

    // His seats whose member is out of his party now, in seat order. A seat
    // whose member is no longer his to invite is let go here
    auto of(const CCharEntity* PPlayer) -> std::vector<Seat>;

    // Her Invite: the club's (club.h) for an alt, his own cardian or one
    // wearing his pearl, the contract's for one his contract holds.
    // CL_S_NO_SEAT for no away seat of his; CL_S_DECLINES with her words in
    // `line`; else as that invite answers
    auto invite(CCharEntity* PPlayer, uint32 charid, std::string& line) -> uint16;

    // Her Release: one his contract holds is released as Your contract
    // releases her; any other only leaves his party's seats. CL_S_OK, or
    // CL_S_NO_SEAT, or as the contract's release refuses
    auto release(CCharEntity* PPlayer, uint32 charid) -> uint16;

    // He ended her membership on purpose (a venture he sent her on): her
    // seat closes, and her flag comes down
    void end(const CCharEntity* PPlayer, uint32 charid, std::string_view why);

    // The game's own party commands, read before their handlers
    // (pawn_module.cpp): his kick of a member by name closes her seat, and
    // a contract hire's kick is her release, done here (true: the packet is
    // dropped); his kick of himself, his leave and his breakup close every
    // seat he holds
    auto kick(CCharEntity* PPlayer, const std::string& name) -> bool;
    void leaveParty(CCharEntity* PPlayer, std::string_view why);

    // She is out of her player's party (pawn::leftParty): kept a seat, she
    // raises the game's seeking-party flag
    void left(uint32 player, CCharEntity* PMember);
} // namespace pawn::away
