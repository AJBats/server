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

#include "common/ipp.h"
#include "entities/char_entity.h"
#include "lua/lua_base_entity.h"
#include "lua/luautils.h"
#include "map_session.h"
#include "packets/s2c/0x00b_logout.h"
#include "utils/charutils.h"
#include "utils/moduleutils.h"
#include "utils/zoneutils.h"

#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace
{
    struct StagedSwap
    {
        uint32              toCharID      = 0; // character to take over at the re-login
        uint32              currentCharID = 0; // charid the session is bound to right now
        charswap::SwapHooks hooks;
    };

    // Keyed by the charid the CLIENT will claim in its re-login 0x00A -- its
    // original lobby-login charid, regardless of prior swaps.
    std::unordered_map<uint32, StagedSwap> stagedSwaps;

    // Adopt hooks of resolved swaps, waiting for the session to install the
    // incoming character. Keyed by that character's charid.
    std::unordered_map<uint32, std::function<std::unique_ptr<CCharEntity>()>> pendingAdoptions;

    // Characters installed alive and in place by adopt(); the 0x00A handler
    // takes each entry once.
    std::unordered_set<uint32> inPlaceHandovers;

    // Session charid currently in play -> the claim id the client still
    // presents. Populated by swaps; an entry clears when its claim id next
    // loads unswapped (a fresh login means claim == identity again).
    std::unordered_map<uint32, uint32> activeClaims;

    auto claimOf(const CCharEntity* PChar) -> uint32
    {
        const auto it = activeClaims.find(PChar->id);
        return it != activeClaims.end() ? it->second : PChar->id;
    }
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

        return stage(PChar, targetCharID, true, Rezone::ForceZoneOut, {});
    }

    bool stage(CCharEntity* PChar, const uint32 targetCharID, const bool moveTargetToPlayer, const Rezone rezone, SwapHooks hooks)
    {
        if (!isEnabled() || PChar == nullptr || targetCharID == 0 || targetCharID == PChar->id)
        {
            return false;
        }

        if (rezone == Rezone::ClientOnly && PChar->PSession == nullptr)
        {
            return false;
        }

        const auto ipp = IPP(zoneutils::GetZoneIPP(PChar->getZone()));
        if (rezone == Rezone::ClientOnly && ipp.getIP() == 0)
        {
            ShowErrorFmt("charswap: no map address for zone {}, cannot rezone {}'s client", static_cast<uint16>(PChar->getZone()), PChar->getName());
            return false;
        }

        if (moveTargetToPlayer)
        {
            // The target takes over this client's screen position: land it
            // exactly where the swapping character is standing.
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
        }

        const uint32 claimCharID = claimOf(PChar);
        stagedSwaps[claimCharID] = { targetCharID, PChar->id, std::move(hooks) };

        ShowInfoFmt("charswap: staged {} ({}) -> {}, client claim {}, {}", PChar->getName(), PChar->id, targetCharID, claimCharID,
                    rezone == Rezone::ClientOnly ? "client rezones in place" : "forcing zone-out");

        if (rezone == Rezone::ForceZoneOut)
        {
            charutils::ForceRezone(PChar);
            return true;
        }

        // The rezone packet alone: sending it makes the session increment its
        // key, go pending-zone and give up the character (surrender). Nothing
        // is queued for eviction, nothing changes status or position.
        PChar->PSession->zone_ipp = {};
        PChar->pushPacket<GP_SERV_COMMAND_LOGOUT>(GP_GAME_LOGOUT_STATE::ZONECHANGE, IPP(ipp));
        return true;
    }

    void surrender(const uint32 currentCharID, std::unique_ptr<CCharEntity>& PChar)
    {
        const auto claimIt     = activeClaims.find(currentCharID);
        const uint32 claimCharID = claimIt != activeClaims.end() ? claimIt->second : currentCharID;

        const auto it = stagedSwaps.find(claimCharID);
        if (it == stagedSwaps.end() || !it->second.hooks.surrender || PChar == nullptr || !isEnabled())
        {
            return;
        }

        ShowInfoFmt("charswap: {} ({}) handed over alive at the re-login", PChar->getName(), PChar->id);
        it->second.hooks.surrender(std::move(PChar));
    }

    uint32 resolve(const uint32 claimCharID, MapSession* PSession)
    {
        const bool zoning = PSession != nullptr && PSession->blowfish.status == BLOWFISH_PENDING_ZONE;

        const auto it = stagedSwaps.find(claimCharID);
        if (it == stagedSwaps.end())
        {
            // No swap staged. A swapped session that simply zones keeps the
            // character it is playing -- the client still claims its lobby
            // charid, but the session (and its accounts_sessions row) belongs
            // to the swapped-in character. A fresh login is the claim again:
            // the lobby just authenticated it and wrote its session row.
            for (auto claimIt = activeClaims.begin(); claimIt != activeClaims.end(); ++claimIt)
            {
                if (claimIt->second == claimCharID)
                {
                    if (zoning)
                    {
                        ShowInfoFmt("charswap: zoning keeps {} (client claim {})", claimIt->first, claimCharID);
                        return claimIt->first;
                    }
                    activeClaims.erase(claimIt);
                    break;
                }
            }
            return claimCharID;
        }

        auto staged = std::move(it->second);
        stagedSwaps.erase(it);

        if (!isEnabled())
        {
            return claimCharID;
        }

        const uint32 toCharID      = staged.toCharID;
        const uint32 currentCharID = staged.currentCharID;

        // Move the session's presence to the target without deleting or
        // renaming any live character's row (accounts_parties cascades on
        // session-row deletes). The outgoing row is parked under the
        // synthetic-account convention when a hook keeps the character
        // alive, or dropped when it goes offline; the target's existing row
        // (a pawn's) receives the real account, the client's address and the
        // current key, so the client's crypto state continues seamlessly.
        if (toCharID != currentCharID && PSession != nullptr)
        {
            uint32 accid      = 0;
            uint32 clientAddr = 0;
            if (const auto row = db::preparedStmt("SELECT accid, client_addr FROM accounts_sessions WHERE charid = ?", currentCharID); row && row->next())
            {
                accid      = row->get<uint32>("accid");
                clientAddr = row->get<uint32>("client_addr");
            }

            if (staged.hooks.surrender)
            {
                db::preparedStmt("UPDATE accounts_sessions SET accid = ?, client_addr = 0, client_port = 0 WHERE charid = ?", 0xC0000000u + currentCharID, currentCharID);
            }
            else
            {
                db::preparedStmt("DELETE FROM accounts_sessions WHERE charid = ?", currentCharID);
            }

            if (const auto existing = db::preparedStmt("SELECT charid FROM accounts_sessions WHERE charid = ?", toCharID); existing && existing->next())
            {
                db::preparedStmt("UPDATE accounts_sessions SET accid = ?, session_key = ?, client_addr = ? WHERE charid = ?",
                                 accid, PSession->blowfish.key, clientAddr, toCharID);
            }
            else
            {
                db::preparedStmt("INSERT INTO accounts_sessions (accid, charid, targid, session_key, client_addr) VALUES (?, ?, 0, ?, ?)",
                                 accid, toCharID, PSession->blowfish.key, clientAddr);
            }
        }

        // one identity per claim: the identity this claim was bound to
        // until now (any earlier swap in the chain) stops answering for it
        std::erase_if(activeClaims, [claimCharID](const auto& entry)
        {
            return entry.second == claimCharID;
        });
        if (toCharID != claimCharID)
        {
            activeClaims[toCharID] = claimCharID;
        }

        if (staged.hooks.adopt)
        {
            pendingAdoptions[toCharID] = std::move(staged.hooks.adopt);
        }

        ShowInfoFmt("charswap: session rebind {} -> {} at re-login (client claim {})", currentCharID, toCharID, claimCharID);
        return toCharID;
    }

    auto adopt(const uint32 charID) -> std::unique_ptr<CCharEntity>
    {
        const auto it = pendingAdoptions.find(charID);
        if (it == pendingAdoptions.end())
        {
            return nullptr;
        }

        auto hook = std::move(it->second);
        pendingAdoptions.erase(it);

        auto PChar = hook();
        if (PChar != nullptr)
        {
            if (PChar->loc.zone != nullptr)
            {
                inPlaceHandovers.insert(PChar->id);
            }
            ShowInfoFmt("charswap: {} ({}) installed alive{}, no database load", PChar->getName(), PChar->id, PChar->loc.zone != nullptr ? " in place" : "");
        }
        return PChar;
    }

    bool takeInPlaceHandover(const uint32 charID)
    {
        return inPlaceHandovers.erase(charID) != 0;
    }

    bool isClientClaimFor(const uint32 currentCharID, const uint32 claimedCharID)
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
