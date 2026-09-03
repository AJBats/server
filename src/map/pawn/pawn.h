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

#include "common/cbasetypes.h"

#include <chrono>
#include <memory>
#include <optional>
#include <string>
#include <vector>

class CCharEntity;
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
    // the summoner's account. The character has never seen a lobby; spawn()
    // gives it the standard first-login starter kit on first spawn.
    bool create(CCharEntity* PSummoner, const std::string& targetName);

    // Load the named offline character (the summoner's own alt, or a
    // generated pawn owned by the summoner's account) and insert it into the
    // summoner's zone at the summoner's position. Returns false with no side
    // effects if the target is unknown, online, already a pawn, itself, or
    // not owned.
    bool spawn(CCharEntity* PSummoner, const std::string& targetName);

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
    // by every cardian of theirs. Strategy 0 = Off, 1 = Roam (the hunters
    // pull). Retreat is the "on me" switch over it: nobody engages, nobody
    // avoids aggro, hunting pauses, until it clears. Orders live in memory;
    // a map restart starts everyone at Off.
    constexpr uint16 kStrategyCount = 2;
    auto strategyName(uint16 strategy) -> std::string_view;
    auto strategyOf(uint32 ownerCharID) -> uint16;
    auto isRetreating(uint32 ownerCharID) -> bool;
    void setStrategy(CCharEntity* POwner, uint16 strategy);
    void setRetreat(CCharEntity* POwner, bool on);

    // Every cardian of the owner's in the zone fights the entity with this
    // targid. "" when they go; otherwise why not, as the player reads it.
    auto partyEngage(CCharEntity* POwner, uint16 targid) -> std::string;


    // A dead pawn home points: revived the way a home point revives a
    // player (full HP/MP, no weakness) and moved to its home point -- which
    // is always its summoner's, copied at this moment -- from where the
    // travel system walks it back to the party. Party membership is
    // untouched. false unless the pawn is dead and the summoner is in the
    // world.
    bool homePoint(CCharEntity* PPawn);

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

    // True when the entity is a live pawn owned by this module.
    bool isPawn(const CCharEntity* PChar);

    // The live pawn with this charid, or nullptr.
    auto findPawn(uint32 pawnCharID) -> CCharEntity*;

    // The named live pawn, but only if this character summoned it (the
    // management surface: gear and inventory belong to the summoner).
    auto findManagedPawn(const CCharEntity* PSummoner, const std::string& targetName) -> CCharEntity*;

    // Names of every live pawn this character summoned, sorted by name.
    auto managedPawnNames(uint32 summonerCharID) -> std::vector<std::string>;

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

    // Record that a party invite reached this pawn (seen at OnPushPacket,
    // when InvitePending is already stamped on the entity). The pawn accepts
    // on the next zone tick through the same code path a real client's
    // Accept answer runs.
    void noteInvite(const CCharEntity* PPawn);

    // Formation latency instrumentation: the moment a played character's own
    // position packet (0x015) last arrived, so the pawn controller can show
    // how stale the packet path is next to the Cardian Link's stream.
    void notePositionPacket(const CCharEntity* PChar);
    auto positionPacketAge(uint32 charid) -> std::optional<std::chrono::milliseconds>;

    // Per-tick maintenance for pawns in this zone (called from OnZoneTick,
    // after all charTicks): answer pending invites, then discard queued
    // outbound packets.
    void onZoneTick(CZone* PZone);
} // namespace pawn
