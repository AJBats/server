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

#include <functional>
#include <memory>
#include <string>

class CCharEntity;
struct MapSession;

// Cardian character swap: at a forced same-zone rezone, answer the client's
// re-login handshake with a different character on the same account. The
// session (socket + blowfish key) survives; only the character riding on it
// changes. Gated behind cardian.ENABLE_CHARSWAP.
//
// Client behavior (verified 2026-08-28): the retail client fully adopts the
// presented identity, but always claims the charid it originally logged in
// with at the lobby in every 0x00A -- so swaps are staged by that claim id,
// and the claim stays valid for the session's lifetime.
//
// Core integration is four calls in map_networking.cpp: surrender() where
// the zone-out destroys the outgoing character, resolve() where the re-login
// chooses which charid to load, adopt() in place of that load, and
// isClientClaimFor() in the 0x00A validator. Everything else,
// including the player:swapTo() Lua binding (registered via CPPModule),
// lives here.
namespace charswap
{
    bool isEnabled();

    // Entity handoffs riding on a staged swap (Cardian possession): the
    // outgoing character's live entity is handed to `surrender` instead of
    // being destroyed by the session, and `adopt` supplies the incoming
    // character's live entity instead of a database load. Either may be
    // empty; a plain swap uses neither.
    struct SwapHooks
    {
        std::function<void(std::unique_ptr<CCharEntity>)> surrender;
        std::function<std::unique_ptr<CCharEntity>()>     adopt;
    };

    // Stage a swap from PChar to the named offline character on the same
    // account, move the target to PChar's current position, and force the
    // same-zone rezone that triggers the swap. Returns false with no side
    // effects if the target is unknown, online, itself, or on another account.
    bool swapTo(CCharEntity* PChar, const std::string& targetName);

    enum class Rezone
    {
        // The outgoing character leaves the zone the way LSB zones every
        // player: evicted at zone-out, reloaded at the re-login.
        ForceZoneOut,
        // Only the client rezones. The outgoing character never leaves its
        // zone; the session lets go of it when the rezone packet is sent,
        // and the surrender hook must take it.
        ClientOnly,
    };

    // Stage a swap to targetCharID for PChar's client and send the client
    // its rezone. Validating the target is the caller's job.
    // moveTargetToPlayer lands the target where PChar stands (an offline
    // character joins the screen); a live target keeps its own position.
    bool stage(CCharEntity* PChar, uint32 targetCharID, bool moveTargetToPlayer, Rezone rezone, SwapHooks hooks);

    // The 0x00A handler asks once whether this character was installed by a
    // live in-place handover: already standing in its zone, so the handshake
    // must re-teach the client without inserting it again.
    bool takeInPlaceHandover(uint32 charID);

    // Zone-out handoff of the outgoing character: called with the session's
    // character (currentCharID = the charid being played) right before the
    // session destroys it for the rezone. A staged swap with a surrender
    // hook takes ownership (PChar is left empty).
    void surrender(uint32 currentCharID, std::unique_ptr<CCharEntity>& PChar);

    // Re-login remap: given the charid the client claimed in its 0x00A,
    // return the staged swap target, moving the session's accounts_sessions
    // presence to it. Rows are never deleted or renamed for a character that
    // stays alive: accounts_parties cascades on session-row deletes, so the
    // real session is written INTO the target's existing row and the
    // outgoing row is parked (accid 0xC0000000 + charid, the Cardian
    // synthetic-account convention) for whoever takes the character. With
    // nothing staged, a zoning session keeps the character it is playing; a
    // fresh login (the lobby just authenticated the claim) is the claim again.
    uint32 resolve(uint32 claimCharID, MapSession* PSession);

    // After resolve(): the live entity to install for the incoming
    // character, or nullptr when it should be loaded from the database.
    auto adopt(uint32 charID) -> std::unique_ptr<CCharEntity>;

    // True when the session playing currentCharID belongs to a client that
    // (post-swap) still claims claimedCharID in its 0x00A.
    bool isClientClaimFor(uint32 currentCharID, uint32 claimedCharID);
} // namespace charswap
