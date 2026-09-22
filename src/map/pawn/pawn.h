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

#include "pawn_travel.h"
#include <array>
#include <string_view>

#include "common/cbasetypes.h"

#include <chrono>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

class CBattleEntity;
class CCharEntity;
class CMobEntity;
class CParty;
class CZone;

// Cardian pawns: session-less CCharEntity instances loaded from real DB
// character rows and inserted into a zone with no client attached. The pawn
// module owns each entity (mirroring how MapSession owns a player's char).
// Visibility, ticking, stats and gear all ride the normal character code;
// the module drains the outbound PacketList nobody will ever read and keeps
// pawns away from the session-only zone-change paths.
// Gated behind pawn.ENABLE_PAWNS.
namespace pawn
{
    bool isEnabled();

    // The account that owns what this character can summon or possess: the
    // session's lobby-authenticated account for a played character (which
    // may itself be a generated cardian on a generated account), else the
    // character's own.
    auto ownerAccountOf(const CCharEntity* PChar) -> uint32;

    // Every character the player could spawn as a cardian, by name: their
    // account's own alts and the generated cardians it owns, never the one
    // they are playing -- spawn()'s eligibility, as a list
    auto accountPawnNames(const CCharEntity* PChar) -> std::vector<std::string>;

    // Delete orphaned pawn session rows (client_addr = 0) left by a crash.
    // Called once at map boot.
    void cleanupStaleRows();

    // Mint a generated pawn: a real character (male Hume Warrior, defaults)
    // on its own generated account, registered in cardian_pawns and owned by
    // the summoner's account, kitted as a first login would be. The
    // character has never seen a lobby.
    bool create(CCharEntity* PSummoner, const std::string& targetName);

    // Load the named offline character (the summoner's own alt, or a
    // generated pawn owned by the summoner's account) and insert it into the
    // summoner's zone at the summoner's position. Returns false with no side
    // effects if the target is unknown, online, already a pawn, itself, or
    // not owned.
    bool spawn(CCharEntity* PSummoner, const std::string& targetName);

    // The club signs in with the player (ROADMAP H): every member of the
    // account (accountPawnNames) not online stands where the game saved
    // her, idling there until invited. The chat line, "Jevyak
    // (Northern San d'Oria), Zapp (...)", empty when nobody stood. spawn
    // stays the GM's tool
    auto signInClub(CCharEntity* PPlayer) -> std::string;

    // Recruit and release (ROADMAP H), the debug verbs behind the real
    // ones: recruiting a cardian is changing her owner -- the same
    // cardian_pawns row, the world's account for the player's -- and
    // taking her out of the census pool; releasing her puts both back.
    // A standing body changes hands on the spot. These skip the linkshell
    // and the pearl the real recruit verb will charge. "" on success,
    // else why not
    auto recruitCardian(CCharEntity* PPlayer, const std::string& targetName) -> std::string;
    auto releaseCardian(CCharEntity* PPlayer, const std::string& targetName) -> std::string;

    // The ladder's engine for an owned cardian (seats.cpp): stand her where
    // the game saved her, or beside her player for a summon
    bool standOwned(uint32 charid, uint32 ownerCharID);
    bool standBeside(uint32 charid, CCharEntity* PPlayer);

    // ...and signs out with her: every pawn under her name despawned where
    // she stands, position saved (charutils, at logout). How many
    auto signOutClub(const CCharEntity* PPlayer) -> uint32;

    // A party membership ended for good (CParty: a kick, a leave, a disband
    // -- never a zoning). A cardian no longer in the player's party has no
    // trek to make: hers ends when she is the one out, and every one of the
    // player's cardians in that party's ends when the player is. Her travel
    // order and her walk are dropped, and she idles where she stands
    void leftParty(const CBattleEntity* PMember, const CParty* PParty);

    // Her player is leaving his zone (charutils::SendToZone, the one gate
    // every client zone change passes): each cardian following him there
    // sets out for his destination now, not when he lands three or four
    // seconds later. The rule is the roam tick's own for a player in
    // another zone -- in his party, not waiting -- plus no trek of her own
    // under way, and only where a zone line leads there from where she
    // stands; a warp's followers keep today's path
    void playerZoning(const CCharEntity* PPlayer, xi::ZoneId destination);

    // A character to mint: the client's race enum (race and sex in one),
    // face 0-15, size 0-2, nation 0-2, main job and level. The census
    // (RESEARCH §11.2) speaks this model.
    struct CharSpec
    {
        std::string name;
        uint8       race   = 1; // HumeMale
        uint8       face   = 0;
        uint8       size   = 0;
        uint8       nation = 1; // Bastok
        uint8       mjob   = 1; // WAR
        uint8       level  = 1;
    };

    // Mint a character from a spec on its own generated account, owned by
    // ownerAccid and registered in cardian_pawns, and kit her (the creation
    // script, run on the loaded character); the charid, or 0. create() is
    // this with the default spec and the summoner's account. The world's
    // bodies are not made here: the census tool mints and finishes them
    // offline, and a stand is a load.
    auto createFromSpec(const CharSpec& spec, uint32 ownerAccid) -> uint32;

    // The account that owns the world's adventurers (login "cardianworld"),
    // made on first use; 0 if it cannot be
    auto worldAccountId() -> uint32;

    // Load the offline character and insert her into the zone at the point
    // (snapped to the navmesh), with no summoner and no party; her
    // controller runs as a world body (Mode::Roam). A load and nothing
    // else: her job, level, skills, gear and spells are what the census
    // tool wrote (world::ensureReady refuses a body it never finished).
    // False with no side effects if she is unknown, online or already a pawn.
    bool spawnAt(uint32 charid, CZone* PZone, const position_t& point, uint8 job);

    // Presence without a body: the session row (search, the lobby's
    // already-online check) and a position in a zone, written for an
    // offline character, with her census job and level where search reads
    // them. The slot tables (world.cpp) give every occupant presence at
    // boot; her body loads when a player arrives. markAbsent takes the
    // row away again.
    void markPresent(uint32 charid, uint16 zoneId, const position_t& point);
    void markAbsent(uint32 charid);
    // A session row with no body and no position written: the faded step
    // of the waterfall for an owned cardian, whose saved spot is her own
    void markOnline(uint32 charid);

    // Run xi.player.charCreate on a freshly minted pawn (implemented in
    // pawn_module.cpp so the sol2 cost stays out of pawn.cpp).
    void applyStarterKit(CCharEntity* PPawn);

    // Toggle hunt mode on the named live pawn (see CPawnController); the
    // usual summoner-only rule is the caller's (findManagedPawn).
    enum class Behavior : uint16;

    // Set a behaviour's unconditional gambit row (pawn_gambits.h): the
    // console commands' way in
    bool setBehaviorRow(CCharEntity* PPawn, Behavior behavior, uint16 arg);

    // Hunt mode: the party's strategy, a controller flag until the strategy
    // channel exists
    bool setHunting(CCharEntity* PPawn, bool on);

    // The party strategy channel (RESEARCH §8): not built, always 0
    auto partyStrategy(const CCharEntity* PPawn) -> uint16;

    // The party strategy channel (M3.9): one set of orders per player, read
    // by every cardian of theirs and every wild cardian in their party.
    // Strategy 0 = Hold, 1 = Pull (the hunters pull). Retreat is the "on me"
    // switch over it: nobody engages, nobody avoids aggro, hunting pauses,
    // until it clears. Orders live in memory; a map restart starts everyone
    // at Hold.
    constexpr uint16 kStrategyCount = 2;
    // Whose orders she follows: her summoner, or for a wild cardian the real
    // player in her party; 0 for nobody's
    auto ordersOwnerOf(const CCharEntity* PPawn) -> uint32;
    // Her retreat and hunt flags from the orders she follows, as she joins
    void applyOrdersTo(CCharEntity* PPawn);
    auto strategyName(uint16 strategy) -> std::string_view;
    auto strategyOf(uint32 ownerCharID) -> uint16;
    auto isRetreating(uint32 ownerCharID) -> bool;
    void setStrategy(CCharEntity* POwner, uint16 strategy);
    void setRetreat(CCharEntity* POwner, bool on);

    // The stake (RESEARCH §12.16): a place with a heading, one per player,
    // set where he stands and facing his way, held until dissolved. The
    // camp holds independently of Hold/Pull; Retreat suspends it. Every
    // set, move or break selects Hold, so changing camp never starts a
    // fight. Pulling from camp is deferred. It dissolves by command,
    // when the whole party has left its zone (stakeSweep, from the zone
    // tick) and at his sign-out. In memory, like the strategy.
    struct Stake
    {
        xi::ZoneId zone{};
        position_t at{}; // where, and facing: rotation is the heading
    };
    auto stakeOf(uint32 ownerCharID) -> std::optional<Stake>;
    auto setStake(CCharEntity* POwner) -> std::string; // "" when set or moved; otherwise why not
    auto clearStake(uint32 ownerCharID, std::string_view why) -> bool; // false when he had none
    void stakeSweep();

    // Every cardian of the owner's in the zone fights the entity with this
    // targid. "" when they go; otherwise why not, as the player reads it.
    auto partyEngage(CCharEntity* POwner, uint16 targid) -> std::string;

    // A mob nobody can hit right now: a worm underground (the game's own
    // test -- the worm roam flag with its name hidden), or anything the
    // server flags untargetable. Never pulled, never sent at, let go of.
    auto isUnderground(const CMobEntity* PMob) -> bool;

    // What the hunters pull -- the strategy's rules, per player, saved in
    // cardian_orders: the check band (EMobDifficulty), which end of it
    // first, and what company around a target is fair. The settings'
    // HUNT_CHECK_MIN/MAX and HUNT_CLEAN_PULLS are the defaults for a player
    // with no row yet.
    struct HuntRules
    {
        uint8 minCheck   = 3;
        uint8 maxCheck   = 5;
        uint8 pullFirst  = 1;     // 0 nearest, 1 easiest, 2 toughest
        bool  aggressive = false; // prey inside an aggressive mob's circle: allowed = that mob (the guard) is the pull, avoided = skipped, and no circle across the approach
        bool  links      = false; // pull with a linking family member near the target
        // A world body's home pull (ROADMAP D3, user): the farther she is from
        // her starting point, the more the errand favours prey that leads back
        // toward it. roam is the distance at which the pull weighs as much as
        // the walk itself; 0 = none, she roams anywhere. Drift is allowed
        position_t homeAt{};
        float      roam = 0.0f;
    };
    constexpr std::array<std::string_view, 3> kPullFirstNames{ "Nearest", "Easiest", "Toughest" };
    auto huntRulesOf(uint32 ownerCharID) -> HuntRules;
    // field: min | max | pull | aggressive | links. "" or the reason not.
    // A band end pushed past the other drags it along.
    auto setHuntRule(CCharEntity* POwner, std::string_view field, int value) -> std::string;


    // A dead pawn home points: revived the way a home point revives a
    // player (full HP/MP, no weakness) and moved to its home point -- the
    // player's, copied at this moment: PPlayer's when given, else her
    // summoner's, else the real player in her party's. Party membership is
    // untouched. false unless the pawn is dead and one of those players is
    // in the world.
    bool homePoint(CCharEntity* PPawn, const CCharEntity* PPlayer = nullptr);

    // A zone change the server meant to carry through the client protocol
    // -- a warp of her own (a scroll, Warp, Warp II on her) or a party
    // teleport she stood in range of -- is carried by the pawn transfer
    // instead: a warp to the party's home point, a teleport to where it
    // put her. Called each tick ahead of the zone's own check; true when
    // a carry was requested.
    bool carryZoning(CCharEntity* PPawn);

    // A stuck cardian teleports to her player's side: only from within
    // RESCUE_RANGE yalms (proximity is the anti-exploit -- no summoning
    // across the zone), on a RESCUE_COOLDOWN shared by all the player's
    // cardians. "" on success, else why not.
    auto rescue(CCharEntity* PPlayer, CCharEntity* PPawn) -> std::string;

    // Replace the pawn's gambits with the set xi.pawn.brain selects for it
    // (its job's default brain today). The controller calls this on its
    // first tick and whenever the pawn's job changes; !pawnbrain forces it.
    // Implemented in pawn_module.cpp.
    void loadBrain(CCharEntity* PPawn);

    // The saved gambit set (cardian_gambits, M3.85): the rows in the row
    // grammar, one "on spec" line each, and the master switch. Saved after
    // every edit; loaded at spawn instead of the defaults when present.
    void saveGambits(CCharEntity* PPawn);
    bool loadSavedGambits(CCharEntity* PPawn);
    void forgetGambits(CCharEntity* PPawn);
    bool reloadBrainByName(const std::string& targetName);
    bool reloadBrain(CCharEntity* PPawn);

    // Remove a pawn from its zone and destroy it. No character state is
    // written back to the DB (the pawn visit leaves no trace).
    bool despawn(const std::string& targetName);
    // Remove a live pawn from the world by charid (despawn() by name). With
    // keepOnline her session row stays, so search and the friend list
    // still find her in the zone she stood in: a world body that has
    // faded is out of the zone, not offline (RESEARCH §11.1)
    bool despawnById(uint32 charid, bool keepOnline = false);

    // True when the entity is a live pawn owned by this module.
    bool isPawn(const CCharEntity* PChar);

    // The real player she is in a party with, or nullptr: the one with a
    // live body, for whoever needs it -- the invite (which player invited
    // her), following, orders. Ownership is not consulted: a wild cardian
    // invited into the party is as much with the player as an alt is --
    // the rule the controller's GetLivePlayer uses. Blinks with his body:
    // for seconds at every zone line there is no such member. Ask
    // withRealPlayer when the question is whether she is his at all
    auto partyPlayer(const CCharEntity* PPawn) -> CCharEntity*;

    // She is a real player's, right now: the last real player seen live
    // in her party is still online (players::online -- his session, which
    // a zone line does not end). The ladder's inParty and the world's
    // clocks ask this, and neither blinks while he zones. Written on the
    // tick, erased only when the party ends for her or for him (leftParty)
    auto withRealPlayer(uint32 charid) -> bool;

    // The live pawn with this charid, or nullptr.
    auto findPawn(uint32 pawnCharID) -> CCharEntity*;

    // One city: two city zones of one capital's region (the invite rule,
    // ROADMAP H; the party finder's reach). The same zone is the caller's.
    auto sameCity(CZone* a, CZone* b) -> bool;

    // Two gates on the management surface (ROADMAP H: command yes, manage
    // no). Managed: the named live pawn, only if this character summoned
    // her -- her belongings, her gambits and her money are the summoner's.
    // Commandable: summoned, or in the player's party -- a wild cardian
    // invited along takes orders and shows what /check would show, and
    // nothing else.
    auto findManagedPawn(const CCharEntity* PSummoner, const std::string& targetName) -> CCharEntity*;
    auto findCommandablePawn(const CCharEntity* PPlayer, const std::string& targetName) -> CCharEntity*;

    // Names of every live pawn this character commands, sorted by name: the
    // roster the command window walks.
    auto commandablePawnNames(const CCharEntity* PPlayer) -> std::vector<std::string>;

    // Possession support --------------------------------------------------

    // Take a live pawn out of the module and out of its zone, restoring a
    // player's controller, pathing and speed, so a session can adopt it.
    // Party membership and every live stat, effect and timer stay as they
    // are; the entity is left the way a fresh load leaves it for the zone-in
    // handshake (out of zone, Disappear, destination = its own zone, standing
    // where it stood). Session rows are the swap's business, not this one's.
    auto release(uint32 pawnCharID) -> std::unique_ptr<CCharEntity>;

    // Make a live character that is currently out of its zone (a session has
    // just given it up) a pawn: inserted back into loc.destination at its own
    // position, driven by CPawnController, following summonerCharID. On
    // failure the entity is destroyed and false returned.
    bool adopt(std::unique_ptr<CCharEntity> PChar, uint32 summonerCharID);

    // Every pawn following fromCharID follows toCharID from now on.
    void reparent(uint32 fromCharID, uint32 toCharID);

    // The charid of the character that summoned this pawn; 0 when unknown.
    auto summonerOf(uint32 pawnCharID) -> uint32;

    // Queue a zone transfer, executed on the module tick. Without a hop the
    // pawn is delivered straight to its summoner's side (the escape hatch
    // for unroutable or unloaded destinations).
    void requestTransfer(uint32 pawnCharID, std::optional<TravelHop> hop);

    // Order the named pawn to travel to a zone, independent of its summoner.
    // The order takes precedence over follow behavior and clears on arrival.
    bool orderTravelByName(const std::string& targetName, uint16 zoneId);

    auto travelOrderOf(uint32 pawnCharID) -> std::optional<xi::ZoneId>;
    void clearTravelOrder(uint32 pawnCharID);

    // A walk order (ROADMAP C, direct control): a point in her zone she
    // walks to on the mesh, replacing any earlier one, given by the player
    // looking through her (`by`) and kept while he does -- arrived, she
    // stands on it; the order ends with his `off`, with his view of her
    // (camera off, addon gone, logout), with her leaving his party, or at a
    // point the mesh cannot reach. A steered walk is this order refreshed
    // every frame the ring moves. Follow, Wait and the approaches yield to
    // it; a fight does not (DoRoamTick is the idle tick), and she comes
    // back to it when the fight ends.
    // `laying`: the point is a stop on a route being laid (a paused maneuver,
    // docs/maneuvers.md): the point before it becomes a crumb to walk through
    // on the way, every yalm or so, and the order is walked crumb by crumb
    // (routeFront / popRoute) before its point
    void setWalkOrder(uint32 pawnCharID, const position_t& point, uint32 by, bool laying = false);
    auto walkOrderOf(uint32 pawnCharID) -> std::optional<position_t>;
    auto routeFront(uint32 pawnCharID) -> std::optional<position_t>; // the next crumb, none when the point is next
    void popRoute(uint32 pawnCharID);
    auto walkOrderedBy(uint32 pawnCharID) -> uint32; // 0 = no order
    void clearWalkOrder(uint32 pawnCharID);
    void forEachWalkOrder(const std::function<void(uint32)>& fn);

    // The maneuver a player has standing (docs/maneuvers.md,
    // CPawnController::BeginManeuver): the cardian he drives, one at a time.
    // The controller keeps the maneuver; this is the one-at-a-time index
    void setManeuver(uint32 playerCharID, uint32 pawnCharID);
    auto maneuverOf(uint32 playerCharID) -> uint32; // 0 = none
    void clearManeuver(uint32 playerCharID);

    // Record that a party invite reached this pawn (seen at OnPushPacket,
    // when InvitePending is already stamped on the entity). The pawn accepts
    // on the next zone tick through the same code path a real client's
    // Accept answer runs.
    void noteInvite(const CCharEntity* PPawn);

    // The game told a cardian something in a battle message (seen at
    // OnPushPacket): "too far away", "unable to see", "you must wait longer".
    // A client would show it; she has none, so the map log says it, and when
    // it comes on the heels of an order the player is told why it came to
    // nothing. `message` is a MsgBasic, `aboutIndex` the target it names.
    void noteBattleMessage(CCharEntity* PPawn, uint16 message, uint16 aboutIndex);

    // Formation latency instrumentation: the moment a played character's own
    // position packet (0x015) last arrived, so the pawn controller can show
    // how stale the packet path is next to the Cardian Link's stream.
    void notePositionPacket(const CCharEntity* PChar);
    auto positionPacketAge(uint32 charid) -> std::optional<std::chrono::milliseconds>;

    // Per-tick maintenance for pawns in this zone (called from OnZoneTick,
    // after all charTicks): answer pending invites, then discard queued
    // outbound packets.
    void onZoneTick(CZone* PZone);

    // The zone tick while the simulation is held (pause/pause.h): nothing steps, but
    // nobody else drains a session-less char's outbound queue, so that still happens.
    void onZoneTickHeld(CZone* PZone);
} // namespace pawn
