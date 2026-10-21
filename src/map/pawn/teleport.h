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
// Party Finder, in a town, for a White Mage to take his party to a crag; the
// finder's own shout (party_finder.h), heard only by the White Mages of his
// hub who know the crag's Teleport and Warp and are at their levels, and
// answered by the few nearest of them free to come, each naming her price --
// 500 to 1,500 gil, the cheapest his to pick. The one he invites joins his
// party and comes straight to him -- a hire runs no town errands, takes no
// order of his and runs no gambits -- and says her price again. He pays it
// by trading her the exact gil (club.cpp plays her side of the game's trade
// window), and once the party is gathered round her, or 20 seconds on, she
// casts the game's own Teleport as an order of hers. Paid is paid: only her
// own failure to cast gives back what he paid. She keeps to town until she
// casts: he leaves for a zone that is no town and she leaves his party. The
// spell decides who goes: a member without the crag's gate crystal, out of
// its reach, mounted or knocked out stays behind, and she says who and why
// in party chat. Then she thanks him and leaves the party -- on arrival, when
// she goes too -- casts Warp home, and the cardians left behind set out to
// meet him at the crag on the trek. The hire has no clock but the
// gathering's patience: it ends when the game takes either of them out of
// the party, never at a zone change. The rules are teleport_rules.h.
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
class CZone;

namespace pawn::teleport
{
    // The shout's own checks for a crag, ahead of the finder's: CL_S_OK, or
    // CL_S_MALFORMED (no such crag), CL_S_TELEPORT_IN_TOWN (he stands in no
    // city or town: a teleport is hired in town, never from the field, the
    // user, 2026-10-21), CL_S_NO_GATE_CRYSTAL (he holds none for it, and the
    // spell would leave him behind), CL_S_TELEPORT_HIRED (a White Mage of his
    // shout is hired already). Casting it himself, or a member of his party
    // who does, refuses nothing: hiring one is his to choose (the user,
    // 2026-10-20). The finder's invite asks it again
    auto shoutRefusal(const CCharEntity* PPlayer, uint8 log) -> uint16;

    // A White Mage of his shout is hired: no shout or invite of any goal goes
    // until the hire is over (the user, 2026-10-21: one contract at a time
    // while a teleport is under way)
    auto hiringFor(uint32 playerCharID) -> bool;

    // The crags whose gate crystal he holds: the teleport goals the Party
    // Finder offers (the Link's GOALS)
    auto cragsHeld(const CCharEntity* PPlayer) -> std::vector<const Crag*>;

    // Who answers a teleport shout: made once per shout -- who of the world
    // knows the spell, and who Warp, read in one query each -- then asked of
    // each body the yell reaches. A standing body is judged as she stands,
    // her MP included; a faded one by her rows, and stands whole
    class Responders
    {
    public:
        explicit Responders(uint8 log);
        auto answers(uint32 charid, const CCharEntity* PPawn, uint8 mjob, uint8 mlvl, uint8 sjob, uint8 slvl) const -> bool;

    private:
        const Crag*                crag_ = nullptr;
        std::unordered_set<uint32> knowers_;
        std::unordered_set<uint32> warpers_; // who knows Warp
    };

    // Her answer to a teleport shout, by a seed: the crag, never the price,
    // which the finder shows beside it
    auto answerLine(uint32 seed, uint8 log) -> std::string;

    // She joined his party on her yes to his teleport shout (`fresh`), at
    // the price she named (party_finder.cpp accepts): the hire begins, in
    // memory alone. Paid is paid (the user, 2026-10-21): what he pays goes
    // back only when she cannot get the spell off. Joined on an open
    // teleport contract instead, which nothing should leave, she is a
    // stray and leaves the party without a word
    void hired(uint32 playerCharID, uint32 mageCharID, uint8 log, bool fresh, uint32 price);

    // He signs out (pawn::signOutClub): his hire ends, and she leaves his
    // party, her contract with it. A teleport contract never outlives his
    // session (the user, 2026-10-21)
    void signOut(const CCharEntity* PPlayer);

    // The game took a character out of a party (pawn::leftParty, from the
    // party's RemoveMember and DisbandParty, never its zone change): her
    // hire ends when it is she or her player
    void noteLeftParty(uint32 charid);

    // The game refused her join after her yes (his party filled meanwhile):
    // her hire ends before it began
    void joinFailed(uint32 mageCharID);

    // A town for the teleport shout: a city or a town by the game's zone
    // type, and Nashmau, which the game types as the field
    auto isTown(CZone* PZone) -> bool;

    // His hub, whose White Mages answer him: a capital's city zones
    // together, by their region; Al Zahbi with Aht Urhgan Whitegate, save
    // while the beastmen attack it (Besieged); any other town on its own
    auto hubOf(CZone* PZone) -> uint32;

    // At the map's boot (pawn::cleanupStaleRows): no hire outlives the map,
    // so every teleport contract a stop left ends
    void endContractsAtBoot();

    // Every body of the world holds the six crags' gate crystals (the user, 2026-10-21): she never
    // teleports by herself, only at a player's word, and a player who could take her along would
    // think she holds them. Given as the map loads her to stand (pawn.cpp loadForStand), saved
    // with her; without one the game blanks the spell's animation on her, the caster included
    void giveCrystals(CCharEntity* PChar);

    // She is hired for a teleport: she runs no town errands (town_errands.h)
    // and takes no role in his party (party_roster.cpp)
    auto isHired(uint32 mageCharID) -> bool;

    // Hired and in his party, she is locked (the user, 2026-10-21): no
    // order of his reaches her (CPawnController::DoAction, the Link's
    // commanding handlers) and her gambits run none of their rows
    // (CGambits::Running), so she cures, buffs or casts nothing at his
    // word; her own order of the crag's Teleport alone goes (ownsCast)
    auto locked(const CCharEntity* PMage) -> bool;
    auto ownsCast(const CCharEntity* PMage, uint16 spellId) -> bool;

    // The gil a trade of his must hand her: her price while he has not paid
    // it and she has not cast, else 0 (club.cpp's trade window)
    auto priceAwaited(uint32 mageCharID, uint32 playerCharID) -> uint32;

    // His trade paid her price: she has him gather round
    void paid(uint32 mageCharID);

    // Every hire on: from the zone tick
    void tick();
} // namespace pawn::teleport
