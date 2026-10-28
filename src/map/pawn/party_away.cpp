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

#include "party_away.h"

#include "cardian_link_messages.h"
#include "club.h"
#include "party_finder.h"
#include "pawn.h"
#include "teleport.h"

#include "common/database.h"
#include "common/logging.h"
#include "common/timer.h"
#include "entities/char_entity.h"
#include "party.h"
#include "utils/charutils.h"

#include <algorithm>
#include <cstring>
#include <ctime>
#include <optional>
#include <unordered_map>

namespace pawn::away
{
    namespace
    {
        // player charid -> his seats' members, in seat order; read from the
        // table the first time he is asked about
        std::unordered_map<uint32, std::vector<uint32>> seatsOf;

        // player charid -> member charid -> when a member who keeps no seat
        // is asked again: the party settles every two seconds, and asking is
        // a read of the database
        std::unordered_map<uint32, std::unordered_map<uint32, timer::time_point>> notYet;
        constexpr auto kAskAgain = std::chrono::seconds(60);

        auto seats(const uint32 player) -> std::vector<uint32>&
        {
            if (const auto it = seatsOf.find(player); it != seatsOf.end())
            {
                return it->second;
            }
            auto& list = seatsOf[player];
            if (const auto rset = db::preparedStmt("SELECT member FROM cardian_party_seats WHERE player = ? ORDER BY seat, member", player); rset)
            {
                while (rset->next())
                {
                    list.push_back(rset->get<uint32>("member"));
                }
            }
            return list;
        }

        // What she is to him, if she keeps a seat in his party
        auto kindOf(const CCharEntity* PPlayer, const uint32 charid) -> std::optional<Kind>
        {
            if (pawn::teleport::isHired(charid))
            {
                return std::nullopt; // hired for one service, which ends as she leaves
            }
            if (const auto member = pawn::club::memberOf(PPlayer, charid); member.has_value())
            {
                switch (*member)
                {
                    case pawn::club::Member::Alt:
                        return Kind::Alt;
                    case pawn::club::Member::Owned:
                        return Kind::Owned;
                    case pawn::club::Member::Wild:
                        return Kind::Pearl;
                    default:
                        break;
                }
            }
            if (const auto held = pawn::finder::openContractOf(charid); held.has_value() && held->playerCharID == PPlayer->id && !held->pearl)
            {
                return Kind::Contract;
            }
            return std::nullopt;
        }

        // The game's seeking-party flag on her body, as /invite sets a
        // player's: over her head and in /sea. The game takes it down as
        // she joins a party (CParty::AddMember)
        void seek(CCharEntity* PMember, const bool on)
        {
            if (PMember == nullptr || PMember->playerConfig.InviteFlg == on)
            {
                return;
            }
            PMember->playerConfig.InviteFlg = on;
            PMember->updatemask |= UPDATE_HP;
            charutils::SavePlayerSettings(PMember);
        }

        // Her seat opened, at the end of his seats
        void keep(const CCharEntity* PPlayer, const uint32 charid, const std::string& name)
        {
            seats(PPlayer->id).push_back(charid);
            db::preparedStmt("REPLACE INTO cardian_party_seats (player, member, seat) VALUES (?, ?, ?)", PPlayer->id, charid,
                             static_cast<uint32>(std::time(nullptr)));
            ShowInfoFmt("party: {} keeps a seat for {} in his party", PPlayer->getName(), name);
        }

        void forget(const uint32 player, const uint32 charid)
        {
            std::erase(seats(player), charid);
            db::preparedStmt("DELETE FROM cardian_party_seats WHERE player = ? AND member = ?", player, charid);
            if (auto* PMember = pawn::findPawn(charid); PMember != nullptr)
            {
                seek(PMember, false);
                return;
            }
            // No body: her saved settings, which a body she stands in later loads
            SAVE_CONF flag{};
            flag.InviteFlg = 1;
            uint32 mask    = 0;
            static_assert(sizeof(SAVE_CONF) >= sizeof(uint32));
            std::memcpy(&mask, &flag, sizeof(uint32));
            db::preparedStmt("UPDATE chars SET settings = settings & ? WHERE charid = ? LIMIT 1", ~mask, charid);
        }

        // Every open contract of his keeps a seat, joined or not yet: opened
        // before his seats are read, so a hire he kicks or leaves behind
        // before the party settles is closed as any other
        void keepContracts(const CCharEntity* PPlayer)
        {
            for (const auto& c : pawn::finder::openContracts(PPlayer->id))
            {
                const auto& held = seats(PPlayer->id);
                if (!c.pearl && !pawn::teleport::isHired(c.charid) && std::ranges::find(held, c.charid) == held.end())
                {
                    keep(PPlayer, c.charid, c.name);
                }
            }
        }

        // Her seat closed on purpose: a contract hire's contract with it
        void close(CCharEntity* PPlayer, const uint32 charid, const std::string_view why)
        {
            if (kindOf(PPlayer, charid) == Kind::Contract)
            {
                if (const auto status = pawn::finder::release(PPlayer, charid); status != CL_S_OK)
                {
                    ShowInfoFmt("party: {} closes {}'s seat ({}); her contract's release answers 0x{:04X}", PPlayer->getName(), charid, why, status);
                }
            }
            ShowInfoFmt("party: {} closes {}'s seat: {}", PPlayer->getName(), charid, why);
            forget(PPlayer->id, charid);
        }

        auto inParty(const CCharEntity* PPlayer, const uint32 charid) -> bool
        {
            return PPlayer->PParty != nullptr && std::ranges::any_of(PPlayer->PParty->members, [charid](const CBattleEntity* PMember)
                                                                     {
                                                                         return PMember->id == charid;
                                                                     });
        }

        // Her seat, if he holds one for her out of his party
        auto awaySeat(const CCharEntity* PPlayer, const uint32 charid) -> bool
        {
            const auto& list = seats(PPlayer->id);
            return std::ranges::find(list, charid) != list.end() && !inParty(PPlayer, charid);
        }
    } // namespace

    void ensureTable()
    {
        db::preparedStmt("CREATE TABLE IF NOT EXISTS `cardian_party_seats` ("
                         "`player` int(10) unsigned NOT NULL, "
                         "`member` int(10) unsigned NOT NULL, "
                         "`seat` int(10) unsigned NOT NULL, "
                         "PRIMARY KEY (`player`, `member`)) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_general_ci");
    }

    void note(const CCharEntity* PPlayer, const std::vector<CCharEntity*>& members)
    {
        if (PPlayer == nullptr || pawn::isPawn(PPlayer))
        {
            return;
        }
        auto&      list  = seats(PPlayer->id);
        auto&      later = notYet[PPlayer->id];
        const auto now   = timer::now();
        for (const auto* PMember : members)
        {
            if (PMember == PPlayer || !pawn::isPawn(PMember) || std::ranges::find(list, PMember->id) != list.end())
            {
                continue;
            }
            if (const auto it = later.find(PMember->id); it != later.end() && now < it->second)
            {
                continue;
            }
            if (!kindOf(PPlayer, PMember->id).has_value())
            {
                later[PMember->id] = now + kAskAgain;
                continue;
            }
            later.erase(PMember->id);
            keep(PPlayer, PMember->id, PMember->getName()); // the seat's order is when she joined him, on the real clock
        }
    }

    auto of(const CCharEntity* PPlayer) -> std::vector<Seat>
    {
        std::vector<Seat> out;
        if (PPlayer == nullptr)
        {
            return out;
        }
        keepContracts(PPlayer);
        const auto list = seats(PPlayer->id); // a copy: a seat let go below changes it
        for (const uint32 charid : list)
        {
            // hired to teleport, she is shown nowhere until the hire ends
            if (inParty(PPlayer, charid) || pawn::teleport::isHired(charid))
            {
                continue;
            }
            const auto kind = kindOf(PPlayer, charid);
            if (!kind.has_value())
            {
                ShowInfoFmt("party: {} lets go of a seat: {} is no longer his to invite", PPlayer->getName(), charid);
                forget(PPlayer->id, charid);
                continue;
            }
            Seat seat{ .charid = charid, .kind = *kind };
            if (auto* PPawn = pawn::findPawn(charid); PPawn != nullptr)
            {
                seek(PPawn, PPawn->PParty == nullptr);
                seat.name  = PPawn->getName();
                seat.job   = static_cast<uint8>(PPawn->GetMJob());
                seat.level = PPawn->GetMLevel();
                seat.sjob  = static_cast<uint8>(PPawn->GetSJob());
                seat.slvl  = PPawn->GetSLevel();
                seat.zone  = static_cast<uint16>(PPawn->getZone());
            }
            else if (const auto rset = db::preparedStmt("SELECT c.charname, c.pos_zone, s.mjob, s.mlvl, s.sjob, s.slvl FROM chars c "
                                                        "JOIN char_stats s ON s.charid = c.charid WHERE c.charid = ?",
                                                        charid);
                     rset && rset->next())
            {
                seat.name  = rset->get<std::string>("charname");
                seat.job   = rset->get<uint8>("mjob");
                seat.level = rset->get<uint8>("mlvl");
                seat.sjob  = rset->get<uint8>("sjob");
                seat.slvl  = rset->get<uint8>("slvl");
                seat.zone  = rset->get<uint16>("pos_zone");
            }
            else
            {
                continue;
            }
            out.push_back(std::move(seat));
        }
        return out;
    }

    auto invite(CCharEntity* PPlayer, const uint32 charid, std::string& line) -> uint16
    {
        if (PPlayer == nullptr || !awaySeat(PPlayer, charid))
        {
            return CL_S_NO_SEAT;
        }
        const auto kind = kindOf(PPlayer, charid);
        if (!kind.has_value())
        {
            forget(PPlayer->id, charid);
            return CL_S_NO_SEAT;
        }
        if (*kind == Kind::Contract)
        {
            const auto held = pawn::finder::openContractOf(charid);
            return pawn::finder::invite(PPlayer, charid, held.has_value() ? held->goal : pawn::finder::Goal{}, line);
        }
        return pawn::club::invite(PPlayer, charid, line);
    }

    auto release(CCharEntity* PPlayer, const uint32 charid) -> uint16
    {
        if (PPlayer == nullptr || !awaySeat(PPlayer, charid))
        {
            return CL_S_NO_SEAT;
        }
        if (kindOf(PPlayer, charid) == Kind::Contract)
        {
            if (const auto status = pawn::finder::release(PPlayer, charid); status != CL_S_OK)
            {
                return status;
            }
        }
        ShowInfoFmt("party: {} releases {}'s seat", PPlayer->getName(), charid);
        forget(PPlayer->id, charid);
        return CL_S_OK;
    }

    void end(const CCharEntity* PPlayer, const uint32 charid, const std::string_view why)
    {
        if (PPlayer == nullptr)
        {
            return;
        }
        const auto& list = seats(PPlayer->id);
        if (std::ranges::find(list, charid) != list.end())
        {
            ShowInfoFmt("party: {} closes {}'s seat: {}", PPlayer->getName(), charid, why);
            forget(PPlayer->id, charid);
        }
    }

    auto kick(CCharEntity* PPlayer, const std::string& name) -> bool
    {
        if (PPlayer == nullptr || PPlayer->PParty == nullptr)
        {
            return false;
        }
        auto* PVictim = dynamic_cast<CCharEntity*>(PPlayer->PParty->GetMemberByName(name));
        if (PVictim == nullptr)
        {
            return false;
        }
        if (PVictim == PPlayer)
        {
            leaveParty(PPlayer, "he kicks himself out of his party");
            return false;
        }
        keepContracts(PPlayer);
        const auto& list = seats(PPlayer->id);
        if (std::ranges::find(list, PVictim->id) == list.end())
        {
            return false;
        }
        // A contract hire's release takes her out of his party itself; the
        // game's own kick goes on for anyone it left in it
        close(PPlayer, PVictim->id, "his kick");
        return !inParty(PPlayer, PVictim->id);
    }

    void leaveParty(CCharEntity* PPlayer, const std::string_view why)
    {
        if (PPlayer == nullptr || pawn::isPawn(PPlayer))
        {
            return;
        }
        keepContracts(PPlayer);
        const auto list = seats(PPlayer->id); // a copy: each close changes it
        for (const uint32 charid : list)
        {
            close(PPlayer, charid, why);
        }
    }

    void left(const uint32 player, CCharEntity* PMember)
    {
        if (PMember == nullptr || player == 0)
        {
            return;
        }
        const auto& list = seats(player);
        if (std::ranges::find(list, PMember->id) != list.end())
        {
            seek(PMember, true);
        }
    }
} // namespace pawn::away
