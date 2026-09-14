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

// The party finder (ROADMAP H slice 3): the player names what they are
// recruiting for and shouts; up to eight of the world's adventurers in
// reach -- the player's zone or elsewhere in their city, the range an
// invite crosses on its own -- hear it and answer, each with a line of
// her own. The finder is the only way one of the world's adventurers
// joins a party: the invite takes a yes from the player's current shout
// for that same goal, and the game's own /invite at her is refused.
// Recruiting proper, the pearl, is a later verb; this is the party's door.

#include "common/cbasetypes.h"

#include <optional>
#include <string>
#include <vector>

class CCharEntity;

namespace pawn::finder
{
    // What the player is recruiting for, and once she joins, her contract
    // (the user, 2026-09-13). Experience points is the easy ask -- anyone
    // the player's fame reaches -- and her affinity grows with the exp she
    // gains beside them; a quest asks more and pays affinity at its
    // completion; a mission asks the most, her rank as well, and is the
    // only recruitment under which a mission completed together counts
    // (the pearl's lock). Under the wrong contract she complains and
    // nothing counts
    struct Goal
    {
        enum class Kind : uint8
        {
            Experience,
            Mission,
            Quest,
        };
        Kind  kind = Kind::Experience;
        uint8 log  = 0; // the mission log or quest area

        auto operator==(const Goal&) const -> bool = default;
    };

    auto goalFrom(const std::string& kind, int log) -> Goal;
    auto kindName(const Goal& goal) -> const char*; // "exp", "quest", "mission"

    // Where her mission log stands against the player's current mission in
    // the goal's log: not that far (a no), on that very mission (a big yes),
    // past it (she knows the way), or between missions and free to take it
    enum class MissionFit : uint8
    {
        Free,
        Behind,
        On,
        Done,
    };

    struct Answer
    {
        bool        yes = false;
        MissionFit  fit = MissionFit::Free; // Behind is a no of its own kind: she has not reached the mission
        std::string line;                   // hers: why she comes, or why not
    };

    struct Candidate
    {
        std::string name;
        uint8       job      = 0;
        uint8       level    = 0;
        uint32      affinity = 0; // hers toward this player, from the memory row
        std::string zone;         // the underscore name, as the roster line carries it
        std::string state;        // here (standing in the player's zone), standing (elsewhere in the city), busy (in a party, standing or camping faded), faded (online, no body), away
        Answer      answer;
    };

    // Everyone in reach with her answer: the yeses first, then here before
    // standing before the rest, then by level descending, then by name.
    // The `finder` verb's debugging list; the screen shouts instead
    auto candidates(const CCharEntity* PPlayer, const Goal& goal) -> std::vector<Candidate>;

    // The shout: up to eight of the adventurers in reach the world holds
    // (on the ladder or standing) hear it, picked at random -- enough
    // willing ones to fill the party when the crowd has them, the rest a
    // mix -- and answer in their own time: a reveal delay and a deciding
    // time each, drawn so the replies trickle in unevenly. Each answer is
    // the willingness question with a mood roll on its soft part, fixed
    // when the shout is made, so a marginal no can flip on a re-shout and
    // a hard no never does. A mission shout also carries up to three
    // extra nos from those who have not reached it, shuffled in with the
    // rest. A shout is a snapshot per player, good for SHOUT_LIFETIME:
    // asked again with `again` false it is the same shout, so the screen
    // can replay it; `again` true is a new one, refused inside
    // SHOUT_COOLDOWN (the current one comes back, its waitMs saying how
    // long). A shout with nobody in reach starts no cooldown. A no here
    // records nothing against her
    struct Responder
    {
        Candidate c;
        uint32    revealMs = 0; // from the shout, when she is heard
        uint32    decideMs = 0; // how long she deliberates before her answer shows
    };

    struct Shout
    {
        uint32                 id = 0;
        Goal                   goal;
        std::vector<Responder> rows;
        uint32                 waitMs = 0; // until the player may shout again, as of this reply
    };

    // nullptr with `why` set when the shout cannot be made at all (a full
    // party, a goal the player's log does not hold)
    auto shout(const CCharEntity* PPlayer, const Goal& goal, bool again, std::string& why) -> const Shout*;

    // A look at one of the shout's responders before the invite: her jobs,
    // nation and rank, her affinity, what she wears -- her body's gear when
    // she stands, her wardrobe as the census dressed her when she is faded
    // -- and her numbers: as she stands, or as she last stood at this level
    // (cardian_snapshot), or none. Only someone in the player's current shout
    struct Peek
    {
        std::string              name;
        uint8                    job      = 0;
        uint8                    level    = 0;
        uint8                    sjob     = 0;
        uint8                    slvl     = 0;
        uint8                    nation   = 0;
        uint8                    rank     = 1;
        bool                     standing = false;
        uint32                   affinity = 0;
        std::vector<std::string> gear;  // eqslot:itemid:invslot entries packed into chat-sized chunks, the gear line's format
        uint32                   hp      = 0;
        uint32                   maxhp   = 0;
        uint32                   mp      = 0;
        uint32                   maxmp   = 0;
        std::string              stats;  // the stats line's tokens: seven total:bonus, then att:def; empty when her numbers are unknown
    };

    auto peek(const CCharEntity* PPlayer, const std::string& name) -> std::optional<Peek>;

    // The stats line's tokens for a standing body: seven total:bonus, then
    // att:def -- the one producer the Equipment page and the peek share
    auto statsLine(CCharEntity* PPawn) -> std::string;

    // Her numbers as she stands, written as her body fades, for the peek
    // at her while faded
    void snapshot(CCharEntity* PPawn);

    // The party invite the player would send by hand, sent for them. She
    // must be a yes in the player's current shout for this same goal (the
    // shout is the only ask; a name nobody shouted for, or a yes to another
    // goal, is refused), then what stops her regardless of mood is asked
    // again -- a party, a fight, a camp, the level band, a mission she has
    // not reached -- then the packet handler's checks (a leader or
    // unpartied inviter, room in the party, an invitee alive, unpartied
    // and not already asked), then the solicit packet the pawn answers by
    // herself. A faded candidate stands first. "" on success, else the
    // reason
    auto invite(CCharEntity* PPlayer, const std::string& name, const Goal& goal) -> std::string;

    // She answered a shout that has not lapsed: the invite may still come,
    // so the world's clocks (a town seat's dwell) leave her where she is
    auto shoutedFor(const std::string& name) -> bool;

    // The pawn's answer to the solicit stamped on her: yes for an owned
    // cardian; for the world's, yes only when the finder asked her (invite
    // above) and the inviter is still there -- her contract with them
    // starts here, on the yes -- and a flat no otherwise, a solicit that
    // slipped past the hook
    auto accepts(CCharEntity* PPawn) -> bool;

    // Affinity grows on discrete shared events under the right contract:
    // a mission (counted for the pearl) or a quest completed together, or
    // AFFINITY_EXP of her own experience gained in the party. Logged; the
    // caller tells the player
    void bond(uint32 playerCharID, uint32 pawnCharID, const char* why, bool mission = false);

    // Her contract with this player: "exp", "quest", "mission", or "" for
    // none -- an owned cardian, nobody's party, or another player's
    // recruit. Set on her yes from the shout's goal, kept in her memory
    // row with that player across a map restart, ended when she leaves
    // the party (the party's leader named, so no other player's row is
    // touched)
    auto contractWith(uint32 charid, uint32 playerCharID) -> const char*;
    void noteLeft(uint32 charid, uint32 leaderCharID);

    // Exp she was granted in the party of the player who recruited her,
    // under an exp contract: banked toward her affinity, the player told
    // at each point
    void noteExp(const CCharEntity* PPawn, uint32 exp);

    // A faded invitee's stand, shared by the verb and the hook below: the
    // ladder must hold her as the world's or this player's own, she must
    // not camp with others (her stand would seat her in their party), and
    // the ladder must stand her when asked (seats::inviteStand). nullptr
    // when she stands, else the reason, logged
    auto standFaded(const CCharEntity* PPlayer, uint32 charid) -> const char*;

    // The game's own invite (/invite, the party menu), from the module's
    // incoming-packet hook ahead of the handler, for a party invite only.
    // The handler's own inviter checks come first (jail, a leader with
    // room) and are the game's to answer. Then: one of the world's
    // adventurers is refused flat -- the game's "invitation was declined"
    // and a line naming the Party Finder, and the packet dropped (true) --
    // since every recruitment goes through the finder, where the player
    // sees the intent she joins under (the user, 2026-09-13); a faded
    // cardian of the player's own is stood first so the handler finds
    // her; anyone else goes on as the game does it (false)
    auto interceptInvite(CCharEntity* PPlayer, uint32 charid) -> bool;
} // namespace pawn::finder
