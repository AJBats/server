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

#include "party_finder.h"

#include "pawn.h"
#include "seats.h"

#include "common/database.h"
#include "common/logging.h"
#include "common/settings.h"
#include "entities/char_entity.h"
#include "enums/party_kind.h"
#include "packets/s2c/0x0dc_group_solicit_req.h"
#include "party.h"
#include "utils/charutils.h"
#include "utils/zoneutils.h"
#include "zone.h"

#include <algorithm>

namespace pawn::finder
{
    namespace
    {
        // Her zone now: her body's when she stands, her row's otherwise
        auto zoneOf(const CCharEntity* PPawn, const uint16 rowZone) -> CZone*
        {
            return PPawn != nullptr ? PPawn->loc.zone : zoneutils::GetZone(static_cast<xi::ZoneId>(rowZone));
        }

        auto levelOf(const CCharEntity* PPawn, const uint8 rowLevel) -> uint8
        {
            return PPawn != nullptr ? PPawn->GetMLevel() : rowLevel;
        }

        auto inBand(const CCharEntity* PPlayer, const uint8 level) -> bool
        {
            const int band = settings::get<uint8>("pawn.FINDER_BAND");
            const int mine = PPlayer->GetMLevel();
            return level >= mine - band && level <= mine + band;
        }

        auto inReach(const CCharEntity* PPlayer, CZone* PHere) -> bool
        {
            return PHere != nullptr && (PHere == PPlayer->loc.zone || pawn::sameCity(PHere, PPlayer->loc.zone));
        }

        auto stateOf(const CCharEntity* PPlayer, const CCharEntity* PPawn, const uint32 charid, CZone* PHere) -> std::string
        {
            if (PPawn != nullptr)
            {
                return PPawn->PParty != nullptr ? "busy" : (PHere == PPlayer->loc.zone ? "here" : "standing");
            }
            return pawn::seats::has(charid) ? "faded" : "away";
        }
    } // namespace

    auto candidates(const CCharEntity* PPlayer) -> std::vector<Candidate>
    {
        std::vector<Candidate> out;
        if (PPlayer == nullptr || PPlayer->loc.zone == nullptr)
        {
            return out;
        }

        const int  band = settings::get<uint8>("pawn.FINDER_BAND");
        const int  mine = PPlayer->GetMLevel();
        const auto rset = db::preparedStmt("SELECT c.charid, c.charname, c.pos_zone, s.mjob, s.mlvl "
                                           "FROM cardian_census x "
                                           "JOIN chars c ON c.charid = x.charid "
                                           "JOIN char_stats s ON s.charid = c.charid "
                                           "WHERE x.recruited = 0 AND x.charid <> 0 AND s.mlvl BETWEEN ? AND ?",
                                           std::max(1, mine - band), mine + band);
        while (rset && rset->next())
        {
            const uint32 charid = rset->get<uint32>("charid");
            const auto*  PPawn  = pawn::findPawn(charid);
            auto*        PHere  = zoneOf(PPawn, rset->get<uint16>("pos_zone"));
            const uint8  level  = levelOf(PPawn, rset->get<uint8>("mlvl"));
            if (!inReach(PPlayer, PHere) || !inBand(PPlayer, level))
            {
                continue;
            }

            Candidate c;
            c.name  = rset->get<std::string>("charname");
            c.job   = rset->get<uint8>("mjob");
            c.level = level;
            c.zone  = PHere->getName();
            c.state = stateOf(PPlayer, PPawn, charid, PHere);
            c.rank  = c.state == "here" ? 0 : c.state == "standing" ? 1 : 2;
            out.push_back(std::move(c));
        }

        std::ranges::sort(out, [](const Candidate& a, const Candidate& b)
        {
            if (a.rank != b.rank)
            {
                return a.rank < b.rank;
            }
            if (a.level != b.level)
            {
                return a.level > b.level;
            }
            return a.name < b.name;
        });
        return out;
    }

    auto invite(CCharEntity* PPlayer, const std::string& name) -> std::string
    {
        if (PPlayer == nullptr || PPlayer->loc.zone == nullptr)
        {
            return "no such player";
        }
        auto* PPawn = pawn::findPawn(charutils::getCharIdFromName(name));
        if (PPawn == nullptr)
        {
            return "she is not standing anywhere";
        }

        // The same gate the list applies: an unrecruited census body, in
        // band, in the player's zone or city
        const auto rset = db::preparedStmt("SELECT recruited FROM cardian_census WHERE charid = ?", PPawn->id);
        if (!rset || !rset->next())
        {
            return "she is not one of the world's adventurers";
        }
        if (rset->get<uint8>("recruited") != 0)
        {
            return "she is spoken for";
        }
        if (!inReach(PPlayer, PPawn->loc.zone))
        {
            return "she is not in your city";
        }
        if (!inBand(PPlayer, PPawn->GetMLevel()))
        {
            return "she is not of your level";
        }

        if (PPlayer->PParty != nullptr && PPlayer->PParty->GetLeader() != PPlayer)
        {
            return "you are not the party leader";
        }
        if (PPlayer->PParty != nullptr && PPlayer->PParty->IsFull())
        {
            return "your party is full";
        }
        if (PPawn->isDead())
        {
            return "she is KO'd";
        }
        if (PPawn->PParty != nullptr)
        {
            return "she is in a party already";
        }
        if (PPawn->InvitePending.UniqueNo != 0)
        {
            return "she already has an invite";
        }

        PPawn->InvitePending.UniqueNo = PPlayer->id;
        PPawn->InvitePending.ActIndex = PPlayer->targid;
        PPawn->pushPacket<GP_SERV_COMMAND_GROUP_SOLICIT_REQ>(PPawn->id, PPawn->targid, PPlayer->getName(), PartyKind::Party);
        ShowInfoFmt("pawn: {} invites {} from the party finder", PPlayer->getName(), PPawn->getName());
        return "";
    }
} // namespace pawn::finder
