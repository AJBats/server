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
#include "errand_math.h"

#include <optional>
#include <string>
#include <utility>
#include <vector>

class CCharEntity;

// The linkshell club (RESEARCH §11.13, ROADMAP H) on the game's own items. The
// player's shell is a Linkshell he holds -- a New Linkshell bought from a
// linkshell vendor and made into one, the game's own way -- or a Pearlsack,
// and its pearls are the Linkpearls he makes from it in the game's menu. The
// characters of his own account and the cardians it owns are in his club
// whether they wear one or not; one of the world's adventurers is in it while
// she wears a pearl of his shell (cardian_club's rules, club_math.h).
//
// He trades her a pearl from his bags, within trading reach, and she puts it
// on herself; to one of the world's in his party that is the recruit, and she
// stays wild -- the census's, gearing and funding herself -- held for him as
// an open contract holds a body (party_finder.h): she signs in and out with
// him, his invite needs no shout, and the world's clocks leave her where he
// left her. Break pearl is the holder's kick: her pearl is broken as the game
// breaks it and thrown away, and one of the world's goes back to the wild.
// Nothing a body of the world's bags are cleared by keeps the pearl from her:
// the re-dress and the census keep a linkshell item where it lies, worn.
//
// Who wears which shell's pearl is read off the items (char_equip and
// char_inventory) into memory at boot, and again after every change and
// every look at the page. The Link's CLUB, CLUB_INVITE and PEARL answer the
// Linkshell page; its errands are errands.h's
namespace pawn::club
{
    using Member = cardian::errand::Member;

    void load();
    void registerHandlers();

    // A linkshell item someone wears: its shell, and the player holding that
    // shell's Linkshell, with his account
    struct Pearl
    {
        uint32 accid        = 0;
        uint32 playerCharID = 0;
        uint32 lsid         = 0;
    };
    auto pearlOf(uint32 charid) -> std::optional<Pearl>;

    // She wears a pearl of a shell somebody holds (the finder's open
    // contracts read it, and the world's clocks leave her be)
    auto isPearled(uint32 charid) -> bool;

    // Everyone wearing a pearl of a shell this player holds
    auto wearersOf(uint32 playerCharID) -> std::vector<uint32>;

    // What she is to this player: his alt, a cardian his account owns, or
    // one of the world's wearing a pearl of a shell he holds; nothing for
    // anyone else (a guest in his party is not a member)
    auto memberOf(const CCharEntity* PPlayer, uint32 charid) -> std::optional<Member>;

    // Every member of his club, by name, charid first
    auto membersOf(const CCharEntity* PPlayer) -> std::vector<std::pair<uint32, Member>>;

    // A linkshell item given her (the Trade page's GIVE): with no linkshell
    // worn, she puts it on, as she would a pearl traded from the Linkshell page
    void wearGiven(CCharEntity* PPawn);

    // A recruit who joins him from another zone tells him she is on her way
    void tellHeadingYourWay(const CCharEntity* PPawn, uint32 playerCharID);
} // namespace pawn::club
