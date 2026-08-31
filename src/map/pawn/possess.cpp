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

#include "possess.h"
#include "pawn.h"
#include "party_teach.h"

#include "charswap.h"
#include "common/database.h"
#include "common/logging.h"

#include "entities/char_entity.h"
#include "lua/lua_base_entity.h"
#include "lua/luautils.h"
#include "party.h"
#include "utils/charutils.h"
#include "utils/moduleutils.h"
#include "utils/zoneutils.h"

#include <memory>
#include <unordered_map>
#include <utility>

namespace
{
    // One possession in flight, shared by the swap hooks: the outgoing
    // character parks here between the session giving it up and the
    // incoming character being installed, when it becomes a pawn.
    struct Pending
    {
        uint32                       fromCharID = 0;
        uint32                       toCharID   = 0;
        bool                         fromLivePawn = false; // target was a live cardian
        std::unique_ptr<CCharEntity> outgoing;
    };

    bool ownsCharacter(const CCharEntity* PChar, const uint32 targetCharID)
    {
        // The player's own alt, or a generated pawn owned by the session's
        // account (the played character may be a generated cardian itself)
        const uint32 ownerAccid = pawn::ownerAccountOf(PChar);
        const auto   rset       = db::preparedStmt("SELECT c.charid FROM chars c "
                                                   "LEFT JOIN cardian_pawns p ON p.pawn_charid = c.charid "
                                                   "WHERE c.charid = ? AND (c.accid = ? OR p.owner_accid = ?)",
                                                   targetCharID, ownerAccid, ownerAccid);
        return rset && rset->next();
    }
} // namespace

namespace possess
{
    bool isEnabled()
    {
        return charswap::isEnabled() && pawn::isEnabled();
    }

    bool start(CCharEntity* PChar, const std::string& targetName)
    {
        if (!isEnabled() || PChar == nullptr || PChar->PSession == nullptr || PChar->loc.zone == nullptr)
        {
            return false;
        }

        const uint32 targetCharID = charutils::getCharIdFromName(targetName);
        if (targetCharID == 0 || targetCharID == PChar->id)
        {
            return false;
        }

        if (!ownsCharacter(PChar, targetCharID))
        {
            ShowWarningFmt("possess: {} ({}) is not owned by {}'s account, refusing", targetName, targetCharID, PChar->getName());
            return false;
        }

        CCharEntity* PTarget = pawn::findPawn(targetCharID);
        if (PTarget == nullptr && zoneutils::GetChar(targetCharID) != nullptr)
        {
            ShowWarningFmt("possess: {} ({}) is being played, refusing", targetName, targetCharID);
            return false;
        }

        if (PTarget != nullptr && PTarget->loc.zone != PChar->loc.zone)
        {
            ShowWarningFmt("possess: {} ({}) is in zone {}, not {}'s zone {}; bring them over first", targetName, targetCharID,
                           static_cast<uint16>(PTarget->getZone()), PChar->getName(), static_cast<uint16>(PChar->getZone()));
            return false;
        }

        auto pending          = std::make_shared<Pending>();
        pending->fromCharID   = PChar->id;
        pending->toCharID     = targetCharID;
        pending->fromLivePawn = PTarget != nullptr;

        charswap::SwapHooks hooks;

        // The session's re-login gives up the outgoing character before the
        // session row is rebound; it cannot become a pawn (own session row)
        // until that rebind has happened, so it parks until the adopt step.
        hooks.surrender = [pending](std::unique_ptr<CCharEntity> POutgoing)
        {
            pending->outgoing = std::move(POutgoing);
        };

        // Runs after the session rebind, before the 0x00A handler: the
        // outgoing character becomes a cardian following its successor and a
        // live target leaves pawn duty for the session (an offline target
        // loads from the database). Packets this emits to the new client are
        // wiped by the handshake, which is the point.
        hooks.adopt = [pending]() -> std::unique_ptr<CCharEntity>
        {
            if (pending->outgoing != nullptr)
            {
                if (!pawn::adopt(std::move(pending->outgoing), pending->toCharID))
                {
                    ShowErrorFmt("possess: {} could not stay behind as a cardian", pending->fromCharID);
                }
            }
            else
            {
                ShowWarningFmt("possess: no outgoing character was handed over for {}", pending->fromCharID);
            }

            pawn::reparent(pending->fromCharID, pending->toCharID);

            auto PIncoming = pending->fromLivePawn ? pawn::release(pending->toCharID) : nullptr;

            // The client's identity changes but its party does not; it must
            // be re-taught the structure after its handshake (see
            // party_teach.h). Party roles are left exactly as they were.
            partyteach::requestReteach(pending->toCharID);

            ShowInfoFmt("possess: {} takes the human seat; {} stays behind as a cardian", pending->toCharID, pending->fromCharID);
            return PIncoming;
        };

        if (!charswap::stage(PChar, targetCharID, PTarget == nullptr, charswap::Rezone::ClientOnly, std::move(hooks)))
        {
            return false;
        }

        ShowInfoFmt("possess: {} ({}) -> {} ({}) staged ({})", PChar->getName(), PChar->id, targetName, targetCharID,
                    PTarget != nullptr ? "live cardian keeps its position" : "offline character summoned to the player");
        return true;
    }

} // namespace possess

class PossessModule : public CPPModule
{
    void OnInit() override
    {
        lua["CBaseEntity"]["possess"] = [](CLuaBaseEntity* PLuaBaseEntity, const std::string& targetName) -> bool
        {
            return possess::start(dynamic_cast<CCharEntity*>(PLuaBaseEntity->GetBaseEntity()), targetName);
        };
    }


};
REGISTER_CPP_MODULE(PossessModule);
