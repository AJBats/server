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

#include "pull_math.h"

#include "common/cbasetypes.h"

#include <set>
#include <string>
#include <vector>

class CCharEntity;
class CMobEntity;

// The puller on the map (ROADMAP, Pulling; the rules are pull_math.h's): the
// call sound the player chose for her pull line, and her pulling kit -- worn
// while she holds the Puller seat, what she wore before put back once she
// leaves it, through the game's own equip (pawn::items::equip). A body of the
// world's kit is the census's (cardian_pull_kit, written by
// tools/world/census.py with her wardrobe), laid in her wardrobe beside her
// own gear; anyone with nothing to pull with is given the map's own (a
// coarse boomerang or pebbles for most), topped up while she holds the
// seat. A Bard wears no kit: she pulls with a song (pullSong). Beside the
// kit, what the Pull page reads: the kinds round the camp and the level
// their con is read at.
namespace pawn::pull
{
    // cardian_pull_kit as this build reads it: made at boot when missing, so a
    // database whose module SQL dbtool skipped still has it
    void ensureTable();

    // The census's pull kit for a body of the world, by item; none for anyone
    // else, or a job with no ranged weapon of its own to pull with
    auto plannedIds(const std::string& name) -> std::set<uint16>;

    // What a re-dress keeps of hers for pulling, whatever the plan wears: her
    // census kit and the map's own (pebbles and the rest)
    auto keptIds(const std::string& name) -> std::set<uint16>;

    // The census's kit for her read again at the next ask (a re-dress has
    // written a new one)
    void forget(uint32 charid);

    // A re-dress (redress.h): her census kit laid where her gear lives, each
    // piece she does not carry already -- a weapon once, ammunition by the
    // stack -- never worn. How many pieces were laid
    auto layKit(CCharEntity* PChar, uint8 location) -> uint32;

    // The player's call sound for the pull line (Settings, Pull call; the
    // Link's PREFS): <call0> to <call20>, or chat::kNoCall for none. In memory, his
    // addon sending it again at every bind
    void setCall(uint32 playerCharID, uint8 call);
    auto callOf(uint32 playerCharID) -> uint8;

    // Her pulling kit on her (chooseKit, pull_math.h): what she wears in her
    // ranged and ammunition slots changed to it when it is not on, the map's
    // own given first when it is that, and what she wore there remembered on
    // her character for putBack. `ready`: she can make a ranged attack now;
    // `said`: what changed, or why she cannot, for the map log
    struct Worn
    {
        bool        ready   = false;
        bool        changed = false;
        std::string said;
    };
    // `rangedToo` false: only her ammunition slot may change -- refilled in a
    // fight, which costs no TP -- and a kit that wants another ranged weapon
    // waits for the next free moment
    auto wearKit(CCharEntity* PChar, bool rangedToo = true) -> Worn;

    // Whether the kit put on by wearKit is still to be taken off
    auto wearsKit(CCharEntity* PChar) -> bool;

    // What she wore before her kit, back in her ranged and ammunition slots,
    // wherever it lies now; the memory let go. What she put back, or "" for
    // nothing
    auto putBack(CCharEntity* PChar) -> std::string;

    // The memory let go without a change: a re-dress has put her plan on her
    void forgetWorn(CCharEntity* PChar);

    // What she wears now makes a ranged attack: a bow or a gun with its
    // ammunition, a weapon thrown by itself, ammunition thrown by itself
    auto canShoot(CCharEntity* PChar) -> bool;

    // Her ammunition topped up to a stack once half of it has gone: the map's
    // own while she holds the seat (`puller`), and for a body of the world in
    // a real player's party (`world`) the ammunition the census planned for
    // her -- her own gear's and her pull kit's. How many she was given
    auto topUp(CCharEntity* PChar, bool puller, bool world) -> uint32;

    // A Bard's pulling song (pull_math.h kPullSongs): the first she has
    // learned and her jobs can sing now, 0 for none, or for anyone else
    auto pullSong(CCharEntity* PChar) -> uint16;

    // What her badge shows she pulls with: her song, else her ranged
    // weapon, else her ammunition
    struct Tool
    {
        uint16 item  = 0;
        uint16 spell = 0;
    };
    auto toolOf(CCharEntity* PChar) -> Tool;

    // The kind of mob a Pull page row names, the list's key: a beastman by
    // his race -- every Goblin of whatever job and level one row, "Goblins"
    // (the user, 2026-10-16: they spawn from pools of jobs, one family across
    // a range of levels) -- and any other mob by its name, so hares of two
    // level bands stay two rows
    auto kindOf(CMobEntity* PMob) -> std::string;

    // The way back (the user, 2026-10-16: plan it at the pick, the map's
    // knowledge of every mob to hand): the first idle member of the mob's
    // linking group that stands within its link radius -- with the tail the
    // chaser keeps behind her and the pull rule's clearance on top -- of the
    // straight run from `from` to `to`, as the game's link rule
    // (CMobController::TryLink) would call it while the mob chases her by;
    // nullptr when the way is clear
    auto wayBackKin(CMobEntity* PMob, const position_t& from, const position_t& to) -> CMobEntity*;

    // The level a con is read against for his party (pull_math.h conLevel):
    // the average of his party's members in his zone under Cardian's exp
    // rule (cardian.EXP_PARTY_AVERAGE), else his own
    auto conLevel(const CCharEntity* PPlayer) -> uint8;

    // The Pull page's kinds (pull_math.h kindsOf): every mob a party could
    // fight within kPullReach of his camp -- of him, with no camp -- by name,
    // the con read at conLevel, his list's order first
    struct Kinds
    {
        std::vector<cardian::pull::Kind> rows;
        uint8                            level  = 0;
        bool                             staked = false;
    };
    auto kindsAround(CCharEntity* PPlayer) -> Kinds;
} // namespace pawn::pull
