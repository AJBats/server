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

#include "pawn.h"

#include "common/database.h"
#include "common/logging.h"
#include "common/settings.h"

#include "entities/char_entity.h"
#include "lua/lua_base_entity.h"
#include "lua/luautils.h"
#include "party.h"
#include "utils/charutils.h"
#include "utils/moduleutils.h"
#include "utils/zoneutils.h"
#include "zone.h"

#include <memory>
#include <unordered_map>

namespace
{
    // charid -> owned pawn entity. The module is the lifetime owner, the way
    // MapSession owns a player's char; zones and viewers hold raw pointers.
    std::unordered_map<uint32, std::unique_ptr<CCharEntity>> pawns;
} // namespace

namespace pawn
{
    bool isEnabled()
    {
        return settings::get<bool>("pawn.ENABLE_PAWNS");
    }

    bool spawn(CCharEntity* PSummoner, const std::string& targetName)
    {
        if (!isEnabled() || PSummoner == nullptr || PSummoner->loc.zone == nullptr)
        {
            return false;
        }

        if (PSummoner->m_moghouseID != 0)
        {
            ShowWarningFmt("pawn: {} tried to spawn a pawn inside a mog house, refusing", PSummoner->getName());
            return false;
        }

        const uint32 targetCharID = charutils::getCharIdFromName(targetName);
        if (targetCharID == 0 || targetCharID == PSummoner->id || pawns.contains(targetCharID))
        {
            return false;
        }

        // Online anywhere in this map process (as a player or a pawn) -> refuse
        if (zoneutils::GetChar(targetCharID) != nullptr)
        {
            ShowWarningFmt("pawn: target {} ({}) is online, refusing spawn", targetName, targetCharID);
            return false;
        }

        // Same account only
        const auto rset = db::preparedStmt("SELECT t.charid FROM chars t "
                                           "JOIN chars s ON s.accid = t.accid "
                                           "WHERE t.charid = ? AND s.charid = ?",
                                           targetCharID, PSummoner->id);
        if (!rset || !rset->next())
        {
            ShowWarningFmt("pawn: target {} ({}) is not on {}'s account, refusing spawn", targetName, targetCharID, PSummoner->getName());
            return false;
        }

        auto PPawn = charutils::LoadChar(targetCharID);
        if (PPawn == nullptr)
        {
            ShowErrorFmt("pawn: LoadChar failed for {} ({})", targetName, targetCharID);
            return false;
        }

        // LoadChar queues self-packets (equip/status) that a real client would
        // clear during its login handshake; nobody is listening here.
        PPawn->clearPacketList();

        // Materialize beside the summoner, not at the char's saved position
        PPawn->loc.p = PSummoner->loc.p;
        PPawn->loc.p.x += 1.5f;
        PPawn->loc.destination = PSummoner->getZone();
        PPawn->loc.prevzone    = PSummoner->getZone();
        PPawn->m_moghouseID    = 0;

        // Visible from the very first spawn packet
        PPawn->status = xi::Status::Normal;

        charutils::loadDeathTimestamp(PPawn.get());

        // Assigns a real targid, inserts into the zone char list + spatial
        // grid (pushing ENTITY_SPAWN to everyone in range), runs CharZoneIn.
        // Deliberately no luautils::OnZoneIn/OnGameIn: those are login-
        // ceremony (cutscenes, zone locks) for real clients.
        PSummoner->loc.zone->IncreaseZoneCounter(PPawn.get());

        if (PPawn->loc.zone == nullptr)
        {
            ShowErrorFmt("pawn: zone insertion failed for {} ({})", targetName, targetCharID);
            return false; // PPawn destructs here; it never entered the zone
        }

        // CharZoneIn queued more packets (party reload etc.) -- discard
        PPawn->clearPacketList();
        PPawn->updatemask |= UPDATE_ALL_CHAR;

        ShowInfoFmt("pawn: spawned {} ({}) in zone {} beside {}", targetName, targetCharID, PSummoner->getZone(), PSummoner->getName());

        pawns[targetCharID] = std::move(PPawn);
        return true;
    }

    bool despawn(const std::string& targetName)
    {
        const uint32 targetCharID = charutils::getCharIdFromName(targetName);

        const auto it = pawns.find(targetCharID);
        if (it == pawns.end())
        {
            return false;
        }

        CCharEntity* PPawn = it->second.get();

        if (PPawn->PParty != nullptr)
        {
            PPawn->PParty->RemoveMember(PPawn);
        }

        if (PPawn->loc.zone != nullptr)
        {
            // Full observer/enmity/treasure-pool/grid unwind + ENTITY_DESPAWN
            // to every client that can see the pawn
            PPawn->loc.zone->DecreaseZoneCounter(PPawn);
        }

        ShowInfoFmt("pawn: despawned {} ({})", targetName, targetCharID);

        // Deliberately no persist::flush -- the pawn visit writes nothing back
        pawns.erase(it);
        return true;
    }

    void onZoneTick(CZone* PZone)
    {
        for (const auto& [charid, PPawn] : pawns)
        {
            if (PPawn->loc.zone == PZone)
            {
                // Nobody drains a session-less char's outbound queue; without
                // this it grows without bound
                PPawn->clearPacketList();
            }
        }
    }
} // namespace pawn

// Registers player:pawnSpawn('Charname') / player:pawnDespawn('Charname')
// without touching the core CLuaBaseEntity registration lists, and hooks the
// per-tick drain.
class PawnModule : public CPPModule
{
    void OnInit() override
    {
        lua["CBaseEntity"]["pawnSpawn"] = [](CLuaBaseEntity* PLuaBaseEntity, const std::string& targetName) -> bool
        {
            return pawn::spawn(dynamic_cast<CCharEntity*>(PLuaBaseEntity->GetBaseEntity()), targetName);
        };

        lua["CBaseEntity"]["pawnDespawn"] = [](CLuaBaseEntity* PLuaBaseEntity, const std::string& targetName) -> bool
        {
            std::ignore = PLuaBaseEntity;
            return pawn::despawn(targetName);
        };
    }

    void OnZoneTick(CZone* PZone) override
    {
        pawn::onZoneTick(PZone);
    }
};
REGISTER_CPP_MODULE(PawnModule);
