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

#include "charswap.h"

#include "common/database.h"
#include "common/logging.h"
#include "common/settings.h"

#include "entities/char_entity.h"
#include "lua/lua_base_entity.h"
#include "lua/luautils.h"
#include "utils/charutils.h"
#include "utils/moduleutils.h"
#include "utils/zoneutils.h"

#include <unordered_map>

namespace
{
    struct StagedSwap
    {
        uint32 toCharID      = 0; // character to load at the re-login
        uint32 currentCharID = 0; // charid the session is bound to right now
    };

    // Keyed by the charid the CLIENT will claim in its re-login 0x00A -- its
    // original lobby-login charid, regardless of prior swaps.
    std::unordered_map<uint32, StagedSwap> stagedSwaps;

    // Session charid currently in play -> the claim id the client still
    // presents. Populated by swaps; an entry clears when its claim id next
    // loads unswapped (a fresh login means claim == identity again).
    std::unordered_map<uint32, uint32> activeClaims;
} // namespace

namespace charswap
{
    bool isEnabled()
    {
        return settings::get<bool>("cardian.ENABLE_CHARSWAP");
    }

    bool swapTo(CCharEntity* PChar, const std::string& targetName)
    {
        if (!isEnabled() || PChar == nullptr)
        {
            return false;
        }

        const uint32 targetCharID = charutils::getCharIdFromName(targetName);
        if (targetCharID == 0 || targetCharID == PChar->id)
        {
            return false;
        }

        if (zoneutils::GetChar(targetCharID) != nullptr)
        {
            ShowWarningFmt("charswap: target {} ({}) is online, refusing swap", targetName, targetCharID);
            return false;
        }

        // Same account only
        const auto rset = db::preparedStmt("SELECT t.charid FROM chars t "
                                           "JOIN chars s ON s.accid = t.accid "
                                           "WHERE t.charid = ? AND s.charid = ?",
                                           targetCharID, PChar->id);
        if (!rset || !rset->next())
        {
            ShowWarningFmt("charswap: target {} ({}) is not on {}'s account, refusing swap", targetName, targetCharID, PChar->getName());
            return false;
        }

        // The target takes over this client's screen position: land it exactly
        // where the swapping character is standing.
        db::preparedStmt("UPDATE chars "
                         "SET pos_zone = ?, pos_prevzone = ?, pos_rot = ?,"
                         "pos_x = ?, pos_y = ?, pos_z = ? "
                         "WHERE charid = ?",
                         PChar->getZone(),
                         PChar->loc.prevzone,
                         PChar->loc.p.rotation,
                         PChar->loc.p.x,
                         PChar->loc.p.y,
                         PChar->loc.p.z,
                         targetCharID);

        const auto   claimIt     = activeClaims.find(PChar->id);
        const uint32 claimCharID = claimIt != activeClaims.end() ? claimIt->second : PChar->id;

        stagedSwaps[claimCharID] = { targetCharID, PChar->id };

        ShowInfoFmt("charswap: staged {} ({}) -> {} ({}), client claim {}, forcing rezone", PChar->getName(), PChar->id, targetName, targetCharID, claimCharID);

        charutils::ForceRezone(PChar);
        return true;
    }

    uint32 resolve(uint32 claimCharID)
    {
        const auto it = stagedSwaps.find(claimCharID);
        if (it == stagedSwaps.end())
        {
            // A fresh, unswapped load under this claim: identity == claim again
            activeClaims.erase(claimCharID);
            return claimCharID;
        }

        const auto [toCharID, currentCharID] = it->second;
        stagedSwaps.erase(it);

        if (!isEnabled())
        {
            return claimCharID;
        }

        // Rebind the session row (session_key, client_addr and all) from the
        // character the session is currently bound to over to the target: the
        // client's crypto state continues seamlessly across the swap. Any
        // stale row for the target would collide with the rebind.
        if (toCharID != currentCharID)
        {
            db::preparedStmt("DELETE FROM accounts_sessions WHERE charid = ?", toCharID);
            db::preparedStmt("UPDATE accounts_sessions SET charid = ? WHERE charid = ?", toCharID, currentCharID);
        }

        if (toCharID == claimCharID)
        {
            activeClaims.erase(toCharID);
        }
        else
        {
            activeClaims[toCharID] = claimCharID;
        }

        ShowInfoFmt("charswap: session rebind {} -> {} at re-login (client claim {})", currentCharID, toCharID, claimCharID);
        return toCharID;
    }

    bool isClientClaimFor(uint32 currentCharID, uint32 claimedCharID)
    {
        const auto it = activeClaims.find(currentCharID);
        return it != activeClaims.end() && it->second == claimedCharID;
    }
} // namespace charswap

// Registers the player:swapTo('Charname') binding without touching the core
// CLuaBaseEntity registration lists.
class CharswapModule : public CPPModule
{
    void OnInit() override
    {
        lua["CBaseEntity"]["swapTo"] = [](CLuaBaseEntity* PLuaBaseEntity, const std::string& targetName) -> bool
        {
            return charswap::swapTo(dynamic_cast<CCharEntity*>(PLuaBaseEntity->GetBaseEntity()), targetName);
        };
    }
};
REGISTER_CPP_MODULE(CharswapModule);
