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
#include "pawn_controller.h"

#include "common/database.h"
#include "common/logging.h"
#include "common/settings.h"
#include "common/utils.h"
#include "common/xirand.h"

#include <cmath>

#include "ai/ai_container.h"
#include "ai/helpers/pathfind.h"
#include "entities/char_entity.h"
#include "navmesh/navmesh.h"
#include "login/login_helpers.h"
#include "packets/c2s/0x074_group_solicit_res.h"
#include "party.h"
#include "utils/charutils.h"
#include "utils/zoneutils.h"
#include "zone.h"

#include <bcrypt/BCrypt.hpp>

#include <memory>
#include <random>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace
{
    // Synthetic per-pawn account ids satisfy the schema's one-session-per-
    // account constraint while a pawn shares its real account with the player
    constexpr uint32 kPawnAccidBase = 0xC0000000;

    constexpr float kSpawnDistance = 3.0f;

    // charid -> owned pawn entity. The module is the lifetime owner, the way
    // MapSession owns a player's char; zones and viewers hold raw pointers.
    std::unordered_map<uint32, std::unique_ptr<CCharEntity>> pawns;

    // Pawns with a party invite awaiting their next-tick answer
    std::unordered_set<uint32> pendingInvites;

    // pawn charid -> summoner charid
    std::unordered_map<uint32, uint32> summonerByPawn;

    // pawn charid -> ordered travel destination
    std::unordered_map<uint32, xi::ZoneId> travelOrders;

    // Zone transfers awaiting execution on the module tick
    std::unordered_map<uint32, std::optional<pawn::TravelHop>> pendingTransfers;

    void savePawnPosition(const CCharEntity* PPawn)
    {
        db::preparedStmt("UPDATE chars "
                         "SET pos_zone = ?, pos_prevzone = ?, pos_rot = ?,"
                         "pos_x = ?, pos_y = ?, pos_z = ? "
                         "WHERE charid = ?",
                         PPawn->getZone(),
                         PPawn->loc.prevzone,
                         PPawn->loc.p.rotation,
                         PPawn->loc.p.x,
                         PPawn->loc.p.y,
                         PPawn->loc.p.z,
                         PPawn->id);
    }
} // namespace

namespace pawn
{
    bool isEnabled()
    {
        return settings::get<bool>("pawn.ENABLE_PAWNS");
    }

    void cleanupStaleRows()
    {
        // Pawn session rows are marked by client_addr = 0; a crash can orphan them
        db::preparedStmt("DELETE FROM accounts_sessions WHERE client_addr = 0");
    }

    bool create(CCharEntity* PSummoner, const std::string& targetName)
    {
        if (!isEnabled() || PSummoner == nullptr)
        {
            return false;
        }

        if (const auto invalidReason = loginHelpers::validateCharacterName(targetName); invalidReason.has_value())
        {
            ShowWarningFmt("pawn: cannot create {}: {}", targetName, *invalidReason);
            return false;
        }

        uint32     accid = 0;
        const auto accRset = db::preparedStmt("SELECT COALESCE(MAX(id), 0) AS max_id FROM accounts");
        if (!accRset || !accRset->next())
        {
            return false;
        }
        accid = std::max<uint32>(1000, accRset->get<uint32>("max_id") + 1);

        // The generated account is never logged into; the credentials exist
        // only to keep the row shaped like every other account
        std::random_device rd;
        const auto         password = fmt::format("{:08x}{:08x}{:08x}{:08x}", rd(), rd(), rd(), rd());

        if (!db::preparedStmt("INSERT INTO accounts (id, login, password, timecreate) VALUES (?, ?, ?, NOW())",
                              accid, fmt::format("pawn{}", accid), BCrypt::generateHash(password)))
        {
            ShowErrorFmt("pawn: account creation failed for {}", targetName);
            return false;
        }

        uint32     charid = 0;
        const auto charRset = db::preparedStmt("SELECT COALESCE(MAX(charid), 0) AS max_id FROM chars");
        if (!charRset || !charRset->next())
        {
            return false;
        }
        charid = charRset->get<uint32>("max_id") + 1;

        char_mini mini = {
            .m_name   = {},
            .m_mjob   = static_cast<uint8>(xi::Job::WAR),
            .m_zone   = xi::ZoneId::BastokMines,
            .m_nation = NATION_BASTOK,
        };

        mini.m_look.race = static_cast<uint8>(CharRace::HumeMale);
        mini.m_look.size = static_cast<uint16>(CharSize::Small);
        mini.m_look.face = static_cast<uint8>(CharFace::Face1A);

        std::strncpy(reinterpret_cast<char*>(mini.m_name), targetName.c_str(), sizeof(mini.m_name) - 1);
        mini.m_name[sizeof(mini.m_name) - 1] = '\0';

        loginHelpers::saveCharacter(accid, charid, &mini);

        // Pawns never watch the opening cutscene; give them the city start
        // position and home point that cutscene would have assigned
        db::preparedStmt("DELETE FROM char_vars WHERE charid = ? AND varname = 'HQuest[newCharacterCS]notSeen'", charid);
        db::preparedStmt("UPDATE chars "
                         "SET pos_rot = 192, pos_x = -45, pos_y = 0, pos_z = 25,"
                         "home_zone = ?, home_rot = 192, home_x = -45, home_y = 0, home_z = 25 "
                         "WHERE charid = ?",
                         static_cast<uint16>(xi::ZoneId::BastokMines), charid);

        db::preparedStmt("INSERT INTO cardian_pawns (pawn_charid, owner_accid) VALUES (?, ?)",
                         charid, PSummoner->accid);

        ShowInfoFmt("pawn: created {} ({}) on generated account {} for {}", targetName, charid, accid, PSummoner->getName());
        return true;
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

        // The summoner's own alt, or a generated pawn owned by their account
        const auto rset = db::preparedStmt("SELECT c.charid FROM chars c "
                                           "JOIN chars s ON s.charid = ? "
                                           "LEFT JOIN cardian_pawns p ON p.pawn_charid = c.charid "
                                           "WHERE c.charid = ? AND (c.accid = s.accid OR p.owner_accid = s.accid)",
                                           PSummoner->id, targetCharID);
        if (!rset || !rset->next())
        {
            ShowWarningFmt("pawn: target {} ({}) is not owned by {}'s account, refusing spawn", targetName, targetCharID, PSummoner->getName());
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

        // Materialize behind the summoner, queued by spawn order the way
        // trusts do; a bad spot self-heals once follow AI exists
        uint32 pawnsHere = 0;
        for (const auto& [id, P] : pawns)
        {
            if (P->loc.zone == PSummoner->loc.zone)
            {
                ++pawnsHere;
            }
        }
        // Behind the summoner by preference (trust convention), else around
        // the ring: the point must sit on the mesh near where we asked AND be
        // visible from the summoner, so a snap never lands across a wall.
        // Last resort is the summoner's own feet.
        const float ringDistance = kSpawnDistance * (pawnsHere + 1);
        PPawn->loc.p             = PSummoner->loc.p;

        const auto tryPlace = [&](const position_t& candidate) -> bool
        {
            const auto* navMesh = PSummoner->loc.zone->navMesh();
            if (navMesh == nullptr)
            {
                return false;
            }

            const auto snapped = navMesh->findClosestValidPoint(candidate);
            if (snapped.has_value() && distance(*snapped, candidate) < 2.0f && PSummoner->CanSeeTarget(*snapped))
            {
                PPawn->loc.p          = *snapped;
                PPawn->loc.p.rotation = PSummoner->loc.p.rotation;
                return true;
            }
            return false;
        };

        bool placed = tryPlace(nearPosition(PSummoner->loc.p, ringDistance, (float)M_PI));
        for (int attempt = 0; !placed && attempt < 6; ++attempt)
        {
            placed = tryPlace(nearPosition(PSummoner->loc.p, ringDistance, xirand::GetRandomNumber(2.0f * (float)M_PI)));
        }

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

        // Chars are built with no pathfinder (clients move them); a pawn
        // moves itself
        PPawn->PAI->PathFind = std::make_unique<CPathFind>(PPawn.get());
        PPawn->PAI->SetController(std::make_unique<CPawnController>(PPawn.get()));

        PPawn->baseSpeed = settings::get<uint8>("pawn.PAWN_SPEED");
        PPawn->UpdateSpeed();

        // accounts_sessions drives /sea, party membership queries and the
        // lobby's already-online check; client_addr stays 0 to mark the row
        // as pawn-owned
        db::preparedStmt("DELETE FROM accounts_sessions WHERE charid = ?", targetCharID);
        db::preparedStmt("INSERT INTO accounts_sessions (accid, charid, targid) VALUES (?, ?, ?)",
                         kPawnAccidBase + targetCharID, targetCharID, PPawn->targid);
        savePawnPosition(PPawn.get());

        const auto kitRset = db::preparedStmt("SELECT pawn_charid FROM cardian_pawns WHERE pawn_charid = ? AND kitted = 0", targetCharID);
        if (kitRset && kitRset->next())
        {
            applyStarterKit(PPawn.get());
            charutils::SaveCharStats(PPawn.get());
            charutils::SaveCharEquip(PPawn.get());
            db::preparedStmt("UPDATE cardian_pawns SET kitted = 1 WHERE pawn_charid = ?", targetCharID);
            PPawn->clearPacketList();
        }

        ShowInfoFmt("pawn: spawned {} ({}) in zone {} beside {}", targetName, targetCharID, PSummoner->getZone(), PSummoner->getName());

        summonerByPawn[targetCharID] = PSummoner->id;
        pawns[targetCharID]          = std::move(PPawn);
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

        savePawnPosition(PPawn);

        if (PPawn->PAI->IsEngaged())
        {
            PPawn->PAI->Internal_Disengage();
        }

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

        db::preparedStmt("DELETE FROM accounts_sessions WHERE charid = ?", targetCharID);

        ShowInfoFmt("pawn: despawned {} ({})", targetName, targetCharID);

        summonerByPawn.erase(targetCharID);
        pendingTransfers.erase(targetCharID);
        travelOrders.erase(targetCharID);
        pawns.erase(it);
        return true;
    }

    bool orderTravelByName(const std::string& targetName, const uint16 zoneId)
    {
        const uint32 targetCharID = charutils::getCharIdFromName(targetName);
        if (targetCharID == 0 || !pawns.contains(targetCharID))
        {
            return false;
        }

        const auto destination = static_cast<xi::ZoneId>(zoneId);
        if (zoneutils::GetZone(destination) == nullptr)
        {
            ShowWarningFmt("pawn: goto {}: zone {} is not loaded", targetName, zoneId);
            return false;
        }

        travelOrders[targetCharID] = destination;
        ShowInfoFmt("pawn: {} ordered to travel to zone {}", targetName, zoneId);
        return true;
    }

    auto travelOrderOf(const uint32 pawnCharID) -> std::optional<xi::ZoneId>
    {
        const auto it = travelOrders.find(pawnCharID);
        return it != travelOrders.end() ? std::optional{ it->second } : std::nullopt;
    }

    void clearTravelOrder(const uint32 pawnCharID)
    {
        travelOrders.erase(pawnCharID);
    }

    auto summonerOf(const uint32 pawnCharID) -> uint32
    {
        const auto it = summonerByPawn.find(pawnCharID);
        return it != summonerByPawn.end() ? it->second : 0;
    }

    void requestTransfer(const uint32 pawnCharID, std::optional<TravelHop> hop)
    {
        if (pawns.contains(pawnCharID))
        {
            pendingTransfers.insert_or_assign(pawnCharID, std::move(hop));
        }
    }

    bool isPawn(const CCharEntity* PChar)
    {
        return PChar != nullptr && pawns.contains(PChar->id);
    }

    void noteInvite(const CCharEntity* PPawn)
    {
        pendingInvites.insert(PPawn->id);
    }

    // Move a live pawn between zones same-process: the M2 despawn/spawn
    // machinery back to back. Party membership, treasure pool and viewer
    // packets are handled inside the two counter calls. A missing hop or an
    // unloaded destination delivers the pawn straight to its summoner.
    void executeTransfer(CCharEntity* PPawn, const std::optional<TravelHop>& hop)
    {
        CZone* POldZone = PPawn->loc.zone;
        if (POldZone == nullptr)
        {
            return;
        }

        xi::ZoneId destZoneId{};
        position_t arriveAt{};

        if (hop.has_value())
        {
            destZoneId = hop->destinationZone;
            arriveAt   = hop->arriveAt;
        }

        CZone* PDestZone = hop.has_value() ? zoneutils::GetZone(destZoneId) : nullptr;

        if (PDestZone == nullptr)
        {
            CCharEntity* PSummoner = zoneutils::GetChar(summonerOf(PPawn->id));
            if (PSummoner == nullptr || PSummoner->loc.zone == nullptr)
            {
                return;
            }
            destZoneId = PSummoner->getZone();
            arriveAt   = nearPosition(PSummoner->loc.p, kSpawnDistance, (float)M_PI);
            PDestZone  = PSummoner->loc.zone;
        }

        if (PPawn->PAI->IsEngaged())
        {
            PPawn->PAI->Internal_Disengage();
        }
        PPawn->InvitePending.clean();

        PPawn->loc.destination = destZoneId;
        POldZone->DecreaseZoneCounter(PPawn);

        PPawn->loc.p = arriveAt;
        PDestZone->IncreaseZoneCounter(PPawn);

        if (PPawn->loc.zone == nullptr)
        {
            ShowErrorFmt("pawn: transfer of {} ({}) into zone {} failed", PPawn->getName(), PPawn->id, static_cast<uint16>(destZoneId));
            return;
        }

        PPawn->clearPacketList();
        PPawn->updatemask |= UPDATE_ALL_CHAR;

        // The insert marked nearby viewers as having seen the pawn, but a
        // viewer whose login handshake is mid-flight has its packet queue
        // cleared, losing the spawn while the server believes it was sent.
        // Unmark everyone; the per-tick spawn sync re-delivers cleanly.
        PDestZone->ForEachChar([&](CCharEntity* PViewer)
        {
            if (PViewer != PPawn)
            {
                PViewer->SpawnPCList.erase(PPawn->id);
            }
        });

        db::preparedStmt("UPDATE accounts_sessions SET targid = ? WHERE charid = ?", PPawn->targid, PPawn->id);
        savePawnPosition(PPawn);

        ShowInfoFmt("pawn: {} ({}) crossed into zone {}", PPawn->getName(), PPawn->id, static_cast<uint16>(destZoneId));
    }

    void onZoneTick(CZone* PZone)
    {
        for (const auto& [charid, PPawn] : pawns)
        {
            if (PPawn->loc.zone != PZone)
            {
                continue;
            }

            if (const auto transferIt = pendingTransfers.find(charid); transferIt != pendingTransfers.end())
            {
                const auto hop = std::move(transferIt->second);
                pendingTransfers.erase(transferIt);
                executeTransfer(PPawn.get(), hop);
                PPawn->clearPacketList();
                continue;
            }

            if (pendingInvites.erase(charid) != 0 && PPawn->InvitePending.UniqueNo != 0)
            {
                GP_CLI_COMMAND_GROUP_SOLICIT_RES answer{};
                answer.Res = std::to_underlying(GP_CLI_COMMAND_GROUP_SOLICIT_RES_RES::Accept);

                if (answer.validate(nullptr, PPawn.get()).valid())
                {
                    ShowInfoFmt("pawn: {} accepts the party invite", PPawn->getName());
                    answer.process(nullptr, PPawn.get());
                }
            }

            // Nobody drains a session-less char's outbound queue; without
            // this it grows without bound
            PPawn->clearPacketList();
        }
    }
} // namespace pawn
