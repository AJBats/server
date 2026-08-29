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

class CCharEntity;

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
// Core integration is two calls: charswap::resolve() where the re-login
// path chooses which charid to load (map_networking.cpp), and
// charswap::isClientClaimFor() in the 0x00A validator. Everything else,
// including the player:swapTo() Lua binding (registered via CPPModule),
// lives here.
namespace charswap
{
    bool isEnabled();

    // Stage a swap from PChar to the named offline character on the same
    // account, move the target to PChar's current position, and force the
    // same-zone rezone that triggers the swap. Returns false with no side
    // effects if the target is unknown, online, itself, or on another account.
    bool swapTo(CCharEntity* PChar, const std::string& targetName);

    // Re-login remap: given the charid the client claimed in its 0x00A,
    // return the staged swap target (rebinding accounts_sessions to it), or
    // claimCharID unchanged when no swap is staged.
    uint32 resolve(uint32 claimCharID);

    // True when the session playing currentCharID belongs to a client that
    // (post-swap) still claims claimedCharID in its 0x00A.
    bool isClientClaimFor(uint32 currentCharID, uint32 claimedCharID);
} // namespace charswap
