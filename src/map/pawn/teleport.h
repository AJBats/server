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

// The teleport shout (ROADMAP "The teleport shout"). The player shouts in the
// Party Finder for a White Mage to take his party to a crag; the finder's own
// shout (party_finder.h), heard only by the world's online White Mages who
// know the crag's Teleport, are at its level and have the MP for it, and
// answered by the few nearest of them free to come, each naming her price --
// 500 to 1,500 gil, the cheapest his to pick. The one he invites joins his
// party and comes straight to him -- a hire runs no town errands -- and says
// her price again. He pays it by trading her the exact gil (club.cpp plays
// her side of the game's trade window), and once the party is gathered round
// her she casts the game's own Teleport as an order of hers. Hired in town,
// she keeps to town until she casts: he leaves for a zone that is no city
// and she leaves his party. A hire that ends without the spell landing gives
// back what he paid. The spell decides who goes: a
// member without the crag's gate crystal, out of its reach, mounted or
// knocked out stays behind, and she says who and why in party chat. Then she
// thanks him and leaves the party -- on arrival, when she holds the crystal
// herself and goes too -- and the cardians left behind set out to meet him
// at the crag on the trek. The rules are teleport_rules.h.
//
// She takes a seat in his party, as the game's own rules have it: a full
// party is refused the shout and the invite as for any goal
// (CL_S_PARTY_FULL), and making room is his to do (the user, 2026-10-18).

#include "common/cbasetypes.h"
#include "teleport_rules.h"

#include <string>
#include <unordered_set>
#include <vector>

class CCharEntity;

namespace pawn::teleport
{
    // The shout's own checks for a crag, ahead of the finder's: CL_S_OK, or
    // CL_S_MALFORMED (no such crag), CL_S_NO_GATE_CRYSTAL (he holds none for
    // it, and the spell would leave him behind), CL_S_TELEPORT_HIRED (a White
    // Mage of his shout is on her way already). Casting it himself, or a
    // member of his party who does, refuses nothing: hiring one is his to
    // choose (the user, 2026-10-20). The finder's invite asks it again
    auto shoutRefusal(const CCharEntity* PPlayer, uint8 log) -> uint16;

    // The crags whose gate crystal he holds: the teleport goals the Party
    // Finder offers (the Link's GOALS)
    auto cragsHeld(const CCharEntity* PPlayer) -> std::vector<const Crag*>;

    // Who answers a teleport shout: made once per shout -- who of the world
    // knows the spell is read in one query -- then asked of each body the
    // yell reaches. A standing body is judged as she stands, her MP
    // included; a faded one by her rows, and stands whole
    class Responders
    {
    public:
        explicit Responders(uint8 log);
        auto answers(uint32 charid, const CCharEntity* PPawn, uint8 mjob, uint8 mlvl, uint8 sjob, uint8 slvl) const -> bool;

    private:
        const Crag*                crag_ = nullptr;
        std::unordered_set<uint32> knowers_;
    };

    // Her answer to a teleport shout, by a seed: the crag, never the price,
    // which the finder shows beside it
    auto answerLine(uint32 seed, uint8 log) -> std::string;

    // She joined his party on her yes to his teleport shout (`fresh`), at
    // the price she named, or on the open contract it left after a map
    // restart, which names neither crag nor price (party_finder.cpp
    // accepts): the hire begins. Her crag, her price and what he has paid
    // ride in her character variables, so a restart finds them again
    void hired(uint32 playerCharID, uint32 mageCharID, uint8 log, bool fresh, uint32 price);

    // She is hired for a teleport: she runs no town errands (town_errands.h)
    auto isHired(uint32 mageCharID) -> bool;

    // The gil a trade of his must hand her: her price while he has not paid
    // it and she has not cast, else 0 (club.cpp's trade window)
    auto priceAwaited(uint32 mageCharID, uint32 playerCharID) -> uint32;

    // His trade paid her price: she has him gather round
    void paid(uint32 mageCharID);

    // Every hire on: from the zone tick
    void tick();
} // namespace pawn::teleport
