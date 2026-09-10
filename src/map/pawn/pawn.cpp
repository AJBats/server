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
#include "pawn_items.h"
#include "pawn_loot.h"
#include "world.h"
#include "pawn_controller.h"
#include "pawn_gambits.h"
#include "gambit_text.h"

#include "common/database.h"
#include "common/logging.h"
#include "common/settings.h"
#include "common/utils.h"
#include "common/timer.h"
#include "common/xirand.h"

#include <cmath>

#include "ai/ai_container.h"
#include "ai/controllers/player_controller.h"
#include "ai/helpers/pathfind.h"
#include "entities/char_entity.h"
#include "entities/mob_entity.h"
#include "status_effect_container.h"
#include "map_session.h"
#include "navmesh/navmesh.h"
#include "login/login_helpers.h"
#include "packets/c2s/0x074_group_solicit_res.h"
#include "party.h"
#include "utils/battleutils.h"
#include "utils/charutils.h"
#include "utils/zoneutils.h"
#include "zone.h"

#include <bcrypt/BCrypt.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <magic_enum/magic_enum.hpp>
#include <limits>
#include <memory>
#include <optional>
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
    // The container is heap-allocated and never freed: ~CCharEntity writes
    // to the database, and the settings and database statics it relies on
    // are already destroyed by the time exit handlers would run a static
    // map's destructor. Pawns still alive at exit leave session rows that
    // cleanupStaleRows() reclaims on the next boot.
    auto& pawns = *new std::unordered_map<uint32, std::unique_ptr<CCharEntity>>();

    // Pawns with a party invite -> when to answer it. A human answers seconds
    // after the invite; answering on the next tick is a tell the client's
    // party UI may not expect (pawn.INVITE_ACCEPT_DELAY, milliseconds).
    std::unordered_map<uint32, timer::time_point> pendingInvites;

    // played charid -> arrival time of its last 0x015 position packet
    std::unordered_map<uint32, timer::time_point> lastPositionPacket;

    // pawn charid -> summoner charid
    std::unordered_map<uint32, uint32> summonerByPawn;

    // player charid -> the party's orders (the strategy channel). The hunt
    // rules load from cardian_orders on first use; strategy and retreat
    // are the session's
    struct PartyOrders
    {
        uint16          strategy = 0;
        bool            retreat  = false;
        pawn::HuntRules rules;
        bool            loaded = false;
    };
    std::unordered_map<uint32, PartyOrders> ordersByOwner;

    auto ordersFor(const uint32 ownerCharID) -> PartyOrders&
    {
        auto& o = ordersByOwner[ownerCharID];
        if (!o.loaded)
        {
            o.loaded           = true;
            o.rules.minCheck   = settings::get<uint8>("pawn.HUNT_CHECK_MIN");
            o.rules.maxCheck   = settings::get<uint8>("pawn.HUNT_CHECK_MAX");
            o.rules.aggressive = !settings::get<bool>("pawn.HUNT_CLEAN_PULLS");
            o.rules.links      = !settings::get<bool>("pawn.HUNT_CLEAN_PULLS");
            const auto rset    = db::preparedStmt("SELECT hunt_min, hunt_max, pull_first, aggressive, links FROM cardian_orders WHERE charid = ?", ownerCharID);
            if (rset && rset->next())
            {
                o.rules.minCheck   = rset->get<uint8>("hunt_min");
                o.rules.maxCheck   = rset->get<uint8>("hunt_max");
                o.rules.pullFirst  = rset->get<uint8>("pull_first");
                o.rules.aggressive = rset->get<uint8>("aggressive") != 0;
                o.rules.links      = rset->get<uint8>("links") != 0;
            }
        }
        return o;
    }

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

    // char_jobs columns by job id, for a level written to an offline character
    constexpr std::array<const char*, 23> kJobColumns = { "", "war", "mnk", "whm", "blm", "rdm", "thf", "pld", "drk", "bst", "brd", "rng",
                                                          "sam", "nin", "drg", "smn", "blu", "cor", "pup", "dnc", "sch", "geo", "run" };

    // Everything that turns a character standing in a zone into a pawn:
    // its own mover and brain, pawn speed, and the session row that gives
    // it presence (/sea, party queries, the lobby's already-online check;
    // client_addr stays 0 to mark the row as pawn-owned).
    void install(CCharEntity* PPawn)
    {
        // Chars are built with no pathfinder (clients move them); a pawn
        // moves itself
        PPawn->PAI->PathFind = std::make_unique<CPathFind>(PPawn);
        PPawn->PAI->SetController(std::make_unique<CPawnController>(PPawn));

        PPawn->baseSpeed = settings::get<uint8>("pawn.PAWN_SPEED");
        PPawn->UpdateSpeed();

        // Upsert, never delete-then-insert: accounts_parties cascades on a
        // session-row delete, and a character handed over mid-party keeps
        // its row (parked by charswap) and its party.
        db::preparedStmt("INSERT INTO accounts_sessions (accid, charid, targid, client_addr) VALUES (?, ?, ?, 0) "
                         "ON DUPLICATE KEY UPDATE accid = VALUES(accid), targid = VALUES(targid), client_addr = 0",
                         kPawnAccidBase + PPawn->id, PPawn->id, PPawn->targid);
        // The login server clears this on a real login; a pawn never passes
        // through it, and a link-dead flag left by an earlier possession
        // would show her as logging out in /sea for good
        db::preparedStmt("UPDATE char_flags SET disconnecting = 0 WHERE charid = ?", PPawn->id);
        savePawnPosition(PPawn);
    }

    void registerPawn(std::unique_ptr<CCharEntity> PPawn, const uint32 summonerCharID)
    {
        const uint32 charid    = PPawn->id;
        summonerByPawn[charid] = summonerCharID;
        pawns[charid]          = std::move(PPawn);
    }

    // Who the account holds besides the character being played: her own
    // alts and the cardians the account owns, id and name in one read.
    // The one place that eligibility is written -- spawn() and possess
    // ask it of a single charid, the roster and the club sign-in as a list
    auto accountMembers(const CCharEntity* PChar) -> std::vector<std::pair<uint32, std::string>>
    {
        std::vector<std::pair<uint32, std::string>> members;
        if (PChar == nullptr)
        {
            return members;
        }
        const uint32 ownerAccid = pawn::ownerAccountOf(PChar);
        const auto   rset       = db::preparedStmt("SELECT c.charid, c.charname FROM chars c "
                                                   "LEFT JOIN cardian_pawns p ON p.pawn_charid = c.charid "
                                                   "WHERE c.charid <> ? AND (c.accid = ? OR p.owner_accid = ?) ORDER BY c.charname",
                                                   PChar->id, ownerAccid, ownerAccid);
        while (rset && rset->next())
        {
            members.emplace_back(rset->get<uint32>("charid"), rset->get<std::string>("charname"));
        }
        return members;
    }

    // How far a saved spot may have drifted off the mesh and still be hers:
    // a character saved on a bridge or a stair the mesh models loosely
    constexpr float kSavedSpotSnap = 30.0f;

    // The nearest point of the zone's mesh to the one asked for, her
    // rotation kept; the point unchanged when the zone has no mesh, and
    // nothing when the mesh has none within the tolerance. Every stand sets
    // its own: a slot's point is authored and trusted at any distance, a
    // saved spot may have drifted, a spawn beside the player must be close
    auto snapToMesh(const CZone* PZone, const position_t& point, const float tolerance) -> std::optional<position_t>
    {
        const auto* navMesh = PZone != nullptr ? PZone->navMesh() : nullptr;
        if (navMesh == nullptr)
        {
            return point;
        }
        const auto snapped = navMesh->findClosestValidPoint(point);
        if (!snapped.has_value() || distance(*snapped, point) > tolerance)
        {
            return std::nullopt;
        }
        position_t landed = *snapped;
        landed.rotation   = point.rotation;
        return landed;
    }

    // The zone's own insert and what it leaves behind, for every way a pawn
    // arrives -- stood from offline, or walked in over a zone line. The
    // insert assigns a targid, puts her in the char list and the spatial
    // grid (pushing ENTITY_SPAWN to everyone in range) and runs CharZoneIn;
    // deliberately not the login ceremony (OnZoneIn/OnGameIn), which is
    // cutscenes and zone locks for real clients. Then the packets that
    // queued for a client nobody is holding, a full update mask, and every
    // viewer made to forget it saw her: a viewer whose own login handshake
    // is mid-flight has its queue cleared and would lose the spawn while
    // the server believed it sent, so the per-tick sync re-delivers it.
    // False when the zone refused her, and she never entered it
    bool enterZone(CCharEntity* PPawn, CZone* PZone)
    {
        PZone->IncreaseZoneCounter(PPawn);
        if (PPawn->loc.zone == nullptr)
        {
            ShowErrorFmt("pawn: zone insertion failed for {} ({}) into zone {}", PPawn->getName(), PPawn->id, static_cast<uint16>(PZone->GetID()));
            return false;
        }
        PPawn->clearPacketList();
        PPawn->updatemask |= UPDATE_ALL_CHAR;
        PZone->ForEachChar([&](CCharEntity* PViewer)
        {
            if (PViewer != PPawn)
            {
                PViewer->SpawnPCList.erase(PPawn->id);
            }
        });
        return true;
    }

    // A freshly loaded character stood in a zone: out of any Mog House,
    // visible from her first spawn packet, her death timer read, then the
    // insert and her own mover, brain and session row (install). Where she
    // lands and what shape she is in are the caller's business; this is the
    // plumbing under all of them
    bool placeInZone(CCharEntity* PPawn, CZone* PZone)
    {
        PPawn->loc.destination = PZone->GetID();
        PPawn->loc.prevzone    = PZone->GetID();
        PPawn->m_moghouseID    = 0;
        PPawn->status          = xi::Status::Normal;
        charutils::loadDeathTimestamp(PPawn);
        if (!enterZone(PPawn, PZone))
        {
            return false;
        }
        install(PPawn);
        return true;
    }

    // The offline character behind a stand: refused when she is already
    // standing or online anywhere in this process, and stripped of the
    // self-packets LoadChar queues for a client that is not there
    auto loadForStand(const uint32 charid) -> std::unique_ptr<CCharEntity>
    {
        if (charid == 0 || pawns.contains(charid) || zoneutils::GetChar(charid) != nullptr)
        {
            return nullptr;
        }
        auto PPawn = charutils::LoadChar(charid);
        if (PPawn == nullptr)
        {
            ShowErrorFmt("pawn: LoadChar failed for {}", charid);
            return nullptr;
        }
        PPawn->clearPacketList();
        return PPawn;
    }

    // Her first stand ever pays for the starter kit, once (cardian_pawns
    // remembers). The kit is a starting job's gear, so an advanced main is
    // stood down to Warrior 1 to take it -- which costs her her level, a
    // known bug (ROADMAP D6 follow-ups) kept in one place so it is fixed in
    // one place; today the census cuts basic jobs only, so it never fires
    void applyKitOnce(CCharEntity* PPawn)
    {
        const auto kitRset = db::preparedStmt("SELECT pawn_charid FROM cardian_pawns WHERE pawn_charid = ? AND kitted = 0", PPawn->id);
        if (!kitRset || !kitRset->next())
        {
            return;
        }
        if (static_cast<uint8>(PPawn->GetMJob()) > 6)
        {
            pawn::applyJobAndLevel(PPawn, static_cast<uint8>(xi::Job::WAR), 1);
        }
        pawn::applyStarterKit(PPawn);
        charutils::SaveCharStats(PPawn);
        charutils::SaveCharEquip(PPawn);
        db::preparedStmt("UPDATE cardian_pawns SET kitted = 1 WHERE pawn_charid = ?", PPawn->id);
        PPawn->clearPacketList();
    }

    // ...and an owned pawn's bag is kept stacked, however she was last
    // played. A world body's is not: fifty of them stand on one zone tick
    void kitAndTidy(CCharEntity* PPawn)
    {
        applyKitOnce(PPawn);
        if (const auto merges = pawn::items::tidyStacks(PPawn); merges > 0)
        {
            ShowInfoFmt("pawn: {} stacks her bag ({} merges)", PPawn->getName(), merges);
            PPawn->clearPacketList();
        }
    }

    // She stands where the game last saved her (ROADMAP H): her row's zone
    // and position, snapped to the mesh; her home point when that zone is
    // not here, she was in her Mog House, the mesh cannot place her, or she
    // was saved KO'd. In an ordered wait: she moves for an invite or a
    // gather, not for the player walking past. "Name (Zone)" for the chat
    // line, empty when she did not stand (online already, or nowhere to)
    auto standWhereLeft(const uint32 charid, const uint32 ownerCharID) -> std::string
    {
        auto PPawn = loadForStand(charid);
        if (PPawn == nullptr)
        {
            return {};
        }

        const auto&      home  = PPawn->profile.home_point;
        CZone*           PZone = zoneutils::GetZone(PPawn->loc.destination);
        std::string_view why;
        if (PZone == nullptr)
        {
            why = "her zone is not here";
        }
        else if (PPawn->m_moghouseID != 0)
        {
            why = "she was in her Mog House";
        }
        else if (PPawn->health.hp == 0)
        {
            why = "she was saved KO'd";
        }
        else if (const auto landed = snapToMesh(PZone, PPawn->loc.p, kSavedSpotSnap); landed.has_value())
        {
            PPawn->loc.p = *landed;
        }
        else
        {
            why = "the mesh has no place for her spot";
        }
        if (!why.empty())
        {
            PZone = zoneutils::GetZone(home.destination);
            if (PZone == nullptr)
            {
                ShowWarningFmt("pawn: {} ({}) cannot sign in: {}, and her home zone {} is not here either", PPawn->getName(), charid, why, static_cast<uint16>(home.destination));
                return {};
            }
            PPawn->loc.p     = home.p;
            PPawn->health.hp = PPawn->GetMaxHP();
            PPawn->health.mp = PPawn->GetMaxMP();
            PPawn->animation = xi::Animation::None;
        }
        if (!placeInZone(PPawn.get(), PZone))
        {
            return {};
        }
        kitAndTidy(PPawn.get());
        if (auto* PController = dynamic_cast<CPawnController*>(PPawn->PAI->GetController()); PController != nullptr)
        {
            PController->SetWaiting(true, true, "waits where she was left");
        }
        std::string zone = PZone->getName();
        std::ranges::replace(zone, '_', ' ');
        ShowInfoFmt("pawn: {} ({}) signs in at {} ({:.0f}, {:.0f}){}", PPawn->getName(), charid, zone, PPawn->loc.p.x, PPawn->loc.p.z,
                    why.empty() ? "" : fmt::format(", at her home point: {}", why));
        auto label = fmt::format("{} ({})", PPawn->getName(), zone);
        registerPawn(std::move(PPawn), ownerCharID);
        return label;
    }

    // One town: two city zones of one capital's region -- San d'Oria's,
    // Bastok's, Windurst's, Jeuno's (regions 19 to 22, never one
    // another's). Other towns sharing a region are not one street: Kazham
    // and Norg both sit in the Elshimo Lowlands with a jungle and a grotto
    // between them, so they hold. The same zone is settled before this
    auto sameCity(CZone* a, CZone* b) -> bool
    {
        const auto region = a->GetRegionID();
        if (region != b->GetRegionID() || region < REGION_TYPE::SANDORIA || region > REGION_TYPE::JEUNO)
        {
            return false;
        }
        return (a->GetTypeMask() & xi::ZoneType::City) != xi::ZoneType::Unknown &&
               (b->GetTypeMask() & xi::ZoneType::City) != xi::ZoneType::Unknown;
    }

    // Invited (ROADMAP H): in the player's zone she simply follows; from her
    // own city she runs to them (the wait ends, a travel order to their
    // zone); from anywhere else she holds where she stands until gathered
    // ("follow me", cardianWait off), a field route walking past aggro
    void gatherOrHold(CCharEntity* PPawn)
    {
        auto*              PController = dynamic_cast<CPawnController*>(PPawn->PAI->GetController());
        const CCharEntity* PSummoner   = zoneutils::GetChar(pawn::summonerOf(PPawn->id));
        if (PController == nullptr || PSummoner == nullptr || PSummoner->loc.zone == nullptr || PPawn->loc.zone == nullptr)
        {
            return;
        }
        if (PSummoner->loc.zone == PPawn->loc.zone)
        {
            PController->SetWaiting(false, false, "invited");
        }
        else if (sameCity(PPawn->loc.zone, PSummoner->loc.zone))
        {
            PController->SetWaiting(false, false, "invited from her own city");
            travelOrders[PPawn->id] = PSummoner->getZone();
            ShowInfoFmt("pawn: {} runs to {} in {} (her own city)", PPawn->getName(), PSummoner->getName(), PSummoner->loc.zone->getName());
        }
        else
        {
            PController->SetWaiting(true, true, "holds until gathered");
            travelOrders.erase(PPawn->id);
            ShowInfoFmt("pawn: {} holds in {} until {} gathers her ({} is not her city)", PPawn->getName(), PPawn->loc.zone->getName(), PSummoner->getName(), PSummoner->loc.zone->getName());
        }
    }
} // namespace

namespace pawn
{
    bool isEnabled()
    {
        return settings::get<bool>("pawn.ENABLE_PAWNS");
    }

    auto ownerAccountOf(const CCharEntity* PChar) -> uint32
    {
        if (PChar == nullptr)
        {
            return 0;
        }

        // A played character's session row carries the account the lobby
        // authenticated, and a swap rebinds only the row's charid; the core
        // fills PSession->accountID from chars.accid instead, which is the
        // generated account of a possessed generated cardian.
        if (PChar->PSession != nullptr)
        {
            const auto rset = db::preparedStmt("SELECT accid FROM accounts_sessions WHERE charid = ? AND client_addr <> 0", PChar->id);
            if (rset && rset->next())
            {
                return rset->get<uint32>("accid");
            }
        }
        return PChar->accid;
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

        const CharSpec spec{ .name = targetName };
        const uint32   charid = createFromSpec(spec, ownerAccountOf(PSummoner));
        if (charid != 0)
        {
            ShowInfoFmt("pawn: {} ({}) minted for {}", targetName, charid, PSummoner->getName());
        }
        return charid != 0;
    }

    auto createFromSpec(const CharSpec& spec, const uint32 ownerAccid) -> uint32
    {
        if (!isEnabled() || ownerAccid == 0)
        {
            return 0;
        }

        if (const auto invalidReason = loginHelpers::validateCharacterName(spec.name); invalidReason.has_value())
        {
            ShowWarningFmt("pawn: cannot create {}: {}", spec.name, *invalidReason);
            return 0;
        }

        uint32     accid   = 0;
        const auto accRset = db::preparedStmt("SELECT COALESCE(MAX(id), 0) AS max_id FROM accounts");
        if (!accRset || !accRset->next())
        {
            return 0;
        }
        accid = std::max<uint32>(1000, accRset->get<uint32>("max_id") + 1);

        // The generated account is never logged into; the credentials exist
        // only to keep the row shaped like every other account
        std::random_device rd;
        const auto         password = fmt::format("{:08x}{:08x}{:08x}{:08x}", rd(), rd(), rd(), rd());

        if (!db::preparedStmt("INSERT INTO accounts (id, login, password, timecreate) VALUES (?, ?, ?, NOW())",
                              accid, fmt::format("pawn{}", accid), BCrypt::generateHash(password)))
        {
            ShowErrorFmt("pawn: account creation failed for {}", spec.name);
            return 0;
        }

        uint32     charid   = 0;
        const auto charRset = db::preparedStmt("SELECT COALESCE(MAX(charid), 0) AS max_id FROM chars");
        if (!charRset || !charRset->next())
        {
            return 0;
        }
        charid = charRset->get<uint32>("max_id") + 1;

        // Her nation's starting city, and a spot in it the opening cutscene
        // would have left a new character at
        struct Start
        {
            xi::ZoneId zone;
            uint8      rotation;
            float      x, y, z;
        };
        static constexpr std::array<Start, 3> starts{ {
            { xi::ZoneId::SouthernSanDoria, 128, 93.0f, 0.0f, -57.0f },
            { xi::ZoneId::BastokMines, 192, -45.0f, 0.0f, 25.0f },
            { xi::ZoneId::WindurstWoods, 0, 106.0f, -5.0f, -23.0f },
        } };
        const auto& start = starts[std::min<uint8>(spec.nation, 2)];

        // The creation script kits the six starting jobs only; an advanced
        // main starts as a Warrior and takes her job at her first spawn
        constexpr uint8 kLastStartingJob = 6;
        char_mini       mini             = {
                          .m_name   = {},
                          .m_mjob   = spec.mjob <= kLastStartingJob ? spec.mjob : static_cast<uint8>(xi::Job::WAR),
            .m_zone   = start.zone,
            .m_nation = spec.nation,
        };

        mini.m_look.race = spec.race;
        mini.m_look.size = spec.size;
        mini.m_look.face = spec.face;

        std::strncpy(reinterpret_cast<char*>(mini.m_name), spec.name.c_str(), sizeof(mini.m_name) - 1);
        mini.m_name[sizeof(mini.m_name) - 1] = '\0';

        loginHelpers::saveCharacter(accid, charid, &mini);

        // Pawns never watch the opening cutscene; give them the city start
        // position and home point that cutscene would have assigned
        db::preparedStmt("DELETE FROM char_vars WHERE charid = ? AND varname = 'HQuest[newCharacterCS]notSeen'", charid);
        db::preparedStmt("UPDATE chars "
                         "SET pos_rot = ?, pos_x = ?, pos_y = ?, pos_z = ?,"
                         "home_zone = ?, home_rot = ?, home_x = ?, home_y = ?, home_z = ? "
                         "WHERE charid = ?",
                         start.rotation, start.x, start.y, start.z,
                         static_cast<uint16>(start.zone), start.rotation, start.x, start.y, start.z, charid);

        db::preparedStmt("INSERT INTO cardian_pawns (pawn_charid, owner_accid) VALUES (?, ?)", charid, ownerAccid);

        ShowInfoFmt("pawn: created {} ({}) on generated account {} for account {}", spec.name, charid, accid, ownerAccid);
        return charid;
    }

    auto worldAccountId() -> uint32
    {
        static uint32 cached = 0;
        if (cached != 0)
        {
            return cached;
        }
        if (const auto rset = db::preparedStmt("SELECT id FROM accounts WHERE login = 'cardianworld'"); rset && rset->next())
        {
            cached = rset->get<uint32>("id");
            return cached;
        }

        const auto accRset = db::preparedStmt("SELECT COALESCE(MAX(id), 0) AS max_id FROM accounts");
        if (!accRset || !accRset->next())
        {
            return 0;
        }
        const uint32       accid = std::max<uint32>(1000, accRset->get<uint32>("max_id") + 1);
        std::random_device rd;
        const auto         password = fmt::format("{:08x}{:08x}{:08x}{:08x}", rd(), rd(), rd(), rd());
        if (!db::preparedStmt("INSERT INTO accounts (id, login, password, timecreate) VALUES (?, 'cardianworld', ?, NOW())",
                              accid, BCrypt::generateHash(password)))
        {
            ShowErrorFmt("pawn: the world account could not be created");
            return 0;
        }
        ShowInfoFmt("pawn: the world account is {}", accid);
        cached = accid;
        return cached;
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

        // The player's own alt, or a generated pawn owned by their account.
        // The account is the session's (the one the lobby authenticated):
        // a possessed generated cardian lives on a generated account, and the
        // player's holdings must not shrink while they play it.
        const uint32 ownerAccid = ownerAccountOf(PSummoner);
        const auto   rset       = db::preparedStmt("SELECT c.charid FROM chars c "
                                                   "LEFT JOIN cardian_pawns p ON p.pawn_charid = c.charid "
                                                   "WHERE c.charid = ? AND (c.accid = ? OR p.owner_accid = ?)",
                                                   targetCharID, ownerAccid, ownerAccid);
        if (!rset || !rset->next())
        {
            ShowWarningFmt("pawn: target {} ({}) is not owned by {}'s account {}, refusing spawn", targetName, targetCharID, PSummoner->getName(), ownerAccid);
            return false;
        }

        auto PPawn = loadForStand(targetCharID);
        if (PPawn == nullptr)
        {
            return false;
        }

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

        // PPawn destructs on a refusal here; she never entered the zone
        if (!placeInZone(PPawn.get(), PSummoner->loc.zone))
        {
            return false;
        }
        kitAndTidy(PPawn.get());

        ShowInfoFmt("pawn: spawned {} ({}) in zone {} beside {}", targetName, targetCharID, PSummoner->getZone(), PSummoner->getName());

        registerPawn(std::move(PPawn), PSummoner->id);
        return true;
    }

    auto signInClub(CCharEntity* PPlayer) -> std::string
    {
        if (!isEnabled() || !settings::get<bool>("pawn.CLUB_SIGNIN") || PPlayer == nullptr)
        {
            return {};
        }
        std::vector<std::string> stood;
        for (const auto& [charid, name] : accountMembers(PPlayer))
        {
            if (auto label = standWhereLeft(charid, PPlayer->id); !label.empty())
            {
                stood.push_back(std::move(label));
            }
        }
        if (stood.empty())
        {
            return {};
        }
        auto line = fmt::format("{}", fmt::join(stood, ", "));
        ShowInfoFmt("pawn: {}'s club signs in: {}", PPlayer->getName(), line);
        return line;
    }

    auto signOutClub(const CCharEntity* PPlayer) -> uint32
    {
        if (PPlayer == nullptr)
        {
            return 0;
        }
        std::vector<uint32> hers;
        for (const auto& [charid, PPawn] : pawns)
        {
            if (summonerOf(charid) == PPlayer->id)
            {
                hers.push_back(charid);
            }
        }
        uint32 count = 0;
        for (const uint32 charid : hers)
        {
            if (despawnById(charid, false))
            {
                ++count;
            }
        }
        if (count > 0)
        {
            ShowInfoFmt("pawn: {}'s club signs out ({} despawned where they stood, positions saved)", PPlayer->getName(), count);
        }
        return count;
    }

    // Is any skill her job has under its ceiling for her level? True for a
    // body minted or caught up at a level her skills have not been capped for
    auto skillsBelowCap(const CCharEntity* PChar) -> bool
    {
        for (uint8 i = static_cast<uint8>(xi::SkillType::HandToHand); i <= static_cast<uint8>(xi::SkillType::Handbell); ++i)
        {
            const uint16 max = 10 * battleutils::GetMaxSkill(static_cast<xi::SkillType>(i), PChar->GetMJob(), PChar->GetMLevel());
            if (max > 0 && PChar->RealSkills.skill[i] < max)
            {
                return true;
            }
        }
        return false;
    }

    void markPresent(const uint32 charid, const uint16 zoneId, const position_t& point)
    {
        db::preparedStmt("INSERT INTO accounts_sessions (accid, charid, targid, client_addr) VALUES (?, ?, 0, 0) "
                         "ON DUPLICATE KEY UPDATE accid = VALUES(accid), client_addr = 0",
                         kPawnAccidBase + charid, charid);
        db::preparedStmt("UPDATE char_flags SET disconnecting = 0 WHERE charid = ?", charid);
        db::preparedStmt("UPDATE chars SET pos_zone = ?, pos_prevzone = ?, pos_rot = ?, pos_x = ?, pos_y = ?, pos_z = ? WHERE charid = ?",
                         zoneId, zoneId, point.rotation, point.x, point.y, point.z, charid);
        // Search reads her job and level from char_stats, where the census
        // tool's mint put them: nothing to write here
    }

    void markAbsent(const uint32 charid)
    {
        db::preparedStmt("DELETE FROM accounts_sessions WHERE charid = ? AND client_addr = 0", charid);
    }

    bool spawnAt(const uint32 charid, CZone* PZone, const position_t& point, const uint8 job)
    {
        if (!isEnabled() || PZone == nullptr || charid == 0 || pawns.contains(charid))
        {
            return false;
        }

        if (zoneutils::GetChar(charid) != nullptr)
        {
            ShowWarningFmt("pawn: character {} is online, refusing to stand her in the world", charid);
            return false;
        }

        auto PPawn = loadForStand(charid);
        if (PPawn == nullptr)
        {
            return false;
        }

        // A body that fell and faded stands whole again: the void takes her
        // death as it takes her drops
        if (PPawn->health.hp == 0)
        {
            PPawn->health.hp = PPawn->GetMaxHP();
            PPawn->health.mp = PPawn->GetMaxMP();
            PPawn->animation = xi::Animation::None;
            ShowInfoFmt("pawn: {} ({}) stands up whole after a KO", PPawn->getName(), charid);
        }

        // At the point, on the mesh: a slot's point is authored, so the
        // nearest mesh to it is taken however far off it turns out to be
        PPawn->loc.p = snapToMesh(PZone, point, std::numeric_limits<float>::max()).value_or(point);

        if (!placeInZone(PPawn.get(), PZone))
        {
            return false;
        }
        if (auto* PController = dynamic_cast<CPawnController*>(PPawn->PAI->GetController()); PController != nullptr)
        {
            PController->SetWorld(true);
        }

        applyKitOnce(PPawn.get());

        // Her job when it differs from the census's; her level is her own
        // (her character row's, set by the census tool's mint and catch-up
        // and by her own dings), never applied here. Her skills follow her
        // level at every stand
        if (job > 0 && static_cast<uint8>(PPawn->GetMJob()) != job)
        {
            applyJobAndLevel(PPawn.get(), job, PPawn->GetMLevel());
            PPawn->clearPacketList();
        }
        // The cap is 48 writes; taken only when a skill of her job's sits
        // under her level's ceiling (a body born or caught up at a level
        // her skills have not seen). A town standing fifty bodies at once
        // took the watchdog down when every stand paid it
        else if (skillsBelowCap(PPawn.get()))
        {
            capSkills(PPawn.get());
            PPawn->clearPacketList();
        }

        // Whatever her bag holds that her job and level can wear goes on
        if (const auto worn = items::dressFromBag(PPawn.get()); worn > 0)
        {
            ShowInfoFmt("pawn: {} dresses from her bag ({} pieces)", PPawn->getName(), worn);
            PPawn->clearPacketList();
        }

        const std::string name = PPawn->getName();
        ShowInfoFmt("pawn: {} ({}) stands in {} on her own, {} {}", name, charid, PZone->getName(), magic_enum::enum_name(PPawn->GetMJob()), PPawn->GetMLevel());
        registerPawn(std::move(PPawn), 0);
        return true;
    }

    bool setBehaviorRow(CCharEntity* PPawn, const Behavior behavior, const uint16 arg)
    {
        if (PPawn == nullptr)
        {
            return false;
        }

        if (auto* PController = dynamic_cast<CPawnController*>(PPawn->PAI->GetController()))
        {
            PController->Gambits().SetBehaviorRow(behavior, arg);
            return true;
        }
        return false;
    }

    bool setHunting(CCharEntity* PPawn, const bool on)
    {
        if (PPawn == nullptr)
        {
            return false;
        }
        if (auto* PController = dynamic_cast<CPawnController*>(PPawn->PAI->GetController()))
        {
            PController->SetHunting(on);
            return true;
        }
        return false;
    }

    auto partyStrategy(const CCharEntity* PPawn) -> uint16
    {
        return PPawn != nullptr ? strategyOf(summonerOf(PPawn->id)) : 0;
    }

    auto strategyName(const uint16 strategy) -> std::string_view
    {
        static constexpr std::array<std::string_view, kStrategyCount> names{ "Off", "Roam" };
        return strategy < names.size() ? names[strategy] : std::string_view("?");
    }

    auto strategyOf(const uint32 ownerCharID) -> uint16
    {
        const auto it = ordersByOwner.find(ownerCharID);
        return it != ordersByOwner.end() ? it->second.strategy : 0;
    }

    auto isRetreating(const uint32 ownerCharID) -> bool
    {
        const auto it = ordersByOwner.find(ownerCharID);
        return it != ordersByOwner.end() && it->second.retreat;
    }

    namespace
    {
        // The owner's orders reach every cardian of theirs that is out.
        // Orders are the other channel: they set controller flags and never
        // touch a gambit row -- who leads, who avoids, stays the list's call
        void applyOrders(const uint32 ownerCharID)
        {
            const auto orders = ordersByOwner[ownerCharID];
            const bool hunt   = orders.strategy == 1 && !orders.retreat;
            for (auto& [charid, PPawn] : pawns)
            {
                if (summonerOf(charid) != ownerCharID)
                {
                    continue;
                }
                if (auto* PController = dynamic_cast<CPawnController*>(PPawn->PAI->GetController()))
                {
                    PController->SetRetreat(orders.retreat);
                    PController->SetHunting(hunt);
                }
            }
        }
    } // namespace

    void setStrategy(CCharEntity* POwner, const uint16 strategy)
    {
        if (POwner == nullptr || strategy >= kStrategyCount)
        {
            return;
        }
        auto& orders = ordersByOwner[POwner->id];
        if (orders.strategy != strategy)
        {
            ShowInfoFmt("pawn: {} sets the party strategy to {}", POwner->getName(), strategyName(strategy));
        }
        orders.strategy = strategy;
        applyOrders(POwner->id);
    }

    void setRetreat(CCharEntity* POwner, const bool on)
    {
        if (POwner == nullptr)
        {
            return;
        }
        auto& orders = ordersByOwner[POwner->id];
        if (orders.retreat != on)
        {
            ShowInfoFmt("pawn: {} calls retreat {}", POwner->getName(), on ? "on" : "off");
        }
        orders.retreat = on;
        applyOrders(POwner->id);
    }

    auto huntRulesOf(const uint32 ownerCharID) -> HuntRules
    {
        return ordersFor(ownerCharID).rules;
    }

    auto setHuntRule(CCharEntity* POwner, const std::string_view field, const int value) -> std::string
    {
        if (POwner == nullptr)
        {
            return "no character";
        }
        auto&          o     = ordersFor(POwner->id);
        auto&          r     = o.rules;
        constexpr int  top   = static_cast<int>(EMobDifficulty::MAX) - 1;
        const auto     check = [&](const int v, const int lo, const int hi) { return v >= lo && v <= hi; };
        if (field == "min" && check(value, 0, top))
        {
            r.minCheck = static_cast<uint8>(value);
            r.maxCheck = std::max(r.maxCheck, r.minCheck);
        }
        else if (field == "max" && check(value, 0, top))
        {
            r.maxCheck = static_cast<uint8>(value);
            r.minCheck = std::min(r.minCheck, r.maxCheck);
        }
        else if (field == "pull" && check(value, 0, static_cast<int>(kPullFirstNames.size()) - 1))
        {
            r.pullFirst = static_cast<uint8>(value);
        }
        else if (field == "aggressive" && check(value, 0, 1))
        {
            r.aggressive = value != 0;
        }
        else if (field == "links" && check(value, 0, 1))
        {
            r.links = value != 0;
        }
        else
        {
            return "no such rule or value";
        }
        db::preparedStmt("INSERT INTO cardian_orders (charid, hunt_min, hunt_max, pull_first, aggressive, links) VALUES (?, ?, ?, ?, ?, ?) "
                         "ON DUPLICATE KEY UPDATE hunt_min = VALUES(hunt_min), hunt_max = VALUES(hunt_max), pull_first = VALUES(pull_first), "
                         "aggressive = VALUES(aggressive), links = VALUES(links)",
                         POwner->id, r.minCheck, r.maxCheck, r.pullFirst, r.aggressive ? 1 : 0, r.links ? 1 : 0);
        ShowInfoFmt("pawn: {} hunts {}..{}, {} first, aggressive company {}, links {}", POwner->getName(),
                    magic_enum::enum_name(static_cast<EMobDifficulty>(r.minCheck)), magic_enum::enum_name(static_cast<EMobDifficulty>(r.maxCheck)),
                    kPullFirstNames[r.pullFirst], r.aggressive ? "allowed" : "avoided", r.links ? "allowed" : "avoided");
        return "";
    }

    auto isUnderground(const CMobEntity* PMob) -> bool
    {
        if (PMob == nullptr)
        {
            return false;
        }
        const bool worm = (PMob->m_roamFlags & xi::RoamFlag::Worm) != xi::RoamFlag::None;
        return PMob->GetUntargetable() || (worm && PMob->IsNameHidden());
    }

    auto partyEngage(CCharEntity* POwner, const uint16 targid) -> std::string
    {
        if (POwner == nullptr || POwner->loc.zone == nullptr)
        {
            return "no zone";
        }
        if (targid == 0)
        {
            return "no target";
        }
        if (isRetreating(POwner->id))
        {
            return "retreating";
        }
        auto* PEntity = POwner->loc.zone->GetEntity(targid, TYPE_MOB | TYPE_PC);
        if (PEntity == nullptr)
        {
            return "no target";
        }
        if (auto* PChar = dynamic_cast<CCharEntity*>(PEntity); PChar != nullptr)
        {
            return isPawn(PChar) ? "talk comes later" : "that is a player";
        }
        auto* PMob = dynamic_cast<CMobEntity*>(PEntity);
        if (PMob == nullptr || PMob->isDead())
        {
            return "no target";
        }
        if (isUnderground(PMob))
        {
            return "underground";
        }

        uint32 sent = 0;
        for (auto& [charid, PPawn] : pawns)
        {
            if (summonerOf(charid) != POwner->id || PPawn->loc.zone != POwner->loc.zone || PPawn->isDead())
            {
                continue;
            }
            if (auto* PController = dynamic_cast<CPawnController*>(PPawn->PAI->GetController()))
            {
                PController->EngageOn(PMob);
                ++sent;
            }
        }
        ShowInfoFmt("pawn: {} sends {} cardian(s) at {}", POwner->getName(), sent, PMob->getName());
        return sent > 0 ? "" : "no cardians out";
    }

    auto rescue(CCharEntity* PPlayer, CCharEntity* PPawn) -> std::string
    {
        // By player, for this process's life: a restart forgives the cooldown
        static std::unordered_map<uint32, timer::time_point> lastRescue;

        if (PPawn == nullptr || !pawns.contains(PPawn->id))
        {
            return "no such cardian";
        }
        if (PPlayer == nullptr || PPlayer->loc.zone == nullptr || PPawn->loc.zone != PPlayer->loc.zone)
        {
            return "not in your zone";
        }
        if (PPawn->isDead())
        {
            return "KO'd";
        }

        const float range = settings::get<float>("pawn.RESCUE_RANGE");
        const float away  = distance(PPlayer->loc.p, PPawn->loc.p);
        if (away > range)
        {
            return fmt::format("too far ({:.0f} y; within {:.0f})", away, range);
        }

        const auto cooldown = std::chrono::seconds(static_cast<int64>(settings::get<float>("pawn.RESCUE_COOLDOWN")));
        const auto now      = timer::now();
        if (const auto it = lastRescue.find(PPlayer->id); it != lastRescue.end() && now - it->second < cooldown)
        {
            const auto left = std::chrono::duration_cast<std::chrono::seconds>(cooldown - (now - it->second)).count();
            return fmt::format("cooling down ({} s left)", left);
        }
        lastRescue[PPlayer->id] = now;

        // Beside the player, facing them; every path and hold she had goes
        // with it
        PPawn->PAI->PathFind->WarpTo(PPlayer->loc.p, 1.5f);
        PPawn->updatemask |= UPDATE_POS;

        ShowInfoFmt("pawn: {} rescued to {}'s side ({:.1f} y)", PPawn->getName(), PPlayer->getName(), away);
        return "";
    }

    bool homePoint(CCharEntity* PPawn)
    {
        if (PPawn == nullptr || !pawns.contains(PPawn->id) || !PPawn->isDead())
        {
            return false;
        }

        CCharEntity* PSummoner = zoneutils::GetChar(summonerOf(PPawn->id));
        if (PSummoner == nullptr)
        {
            return false;
        }

        // Cardians share their player's home point
        PPawn->profile.home_point = PSummoner->profile.home_point;
        const auto& home          = PPawn->profile.home_point;
        db::preparedStmt("UPDATE chars SET home_zone = ?, home_rot = ?, home_x = ?, home_y = ?, home_z = ? WHERE charid = ?",
                         static_cast<uint16>(home.destination), home.p.rotation, home.p.x, home.p.y, home.p.z, PPawn->id);

        PPawn->StatusEffectContainer->DelStatusEffectSilent(xi::StatusEffect::Weakness);
        PPawn->SetDeathTime(timer::time_point::min());
        PPawn->health.hp = PPawn->GetMaxHP();
        PPawn->health.mp = PPawn->GetMaxMP();
        PPawn->animation = xi::Animation::None;
        PPawn->updatemask |= UPDATE_HP;
        PPawn->PAI->Accept_Raise();

        ShowInfoFmt("pawn: {} home points to zone {}", PPawn->getName(), static_cast<uint16>(home.destination));
        requestTransfer(PPawn->id, TravelHop{ .destinationZone = home.destination, .walkTo = {}, .arriveAt = home.p });
        return true;
    }

    bool carryZoning(CCharEntity* PPawn)
    {
        if (PPawn == nullptr || !pawns.contains(PPawn->id))
        {
            return false;
        }

        // Carried alone, she waits where she lands; carried with her player
        // -- the same teleport, or a warp home on the player's heels -- she
        // arrives following
        const auto settle = [&](const xi::ZoneId destination)
        {
            const CCharEntity* PSummoner  = zoneutils::GetChar(summonerOf(PPawn->id));
            const bool         withPlayer = PSummoner != nullptr &&
                                    (PSummoner->getZone() == destination ||
                                     (PSummoner->requestedZoneChange && PSummoner->loc.destination == destination) ||
                                     (PSummoner->requestedWarp != WarpRequest::None && PSummoner->profile.home_point.destination == destination) ||
                                     PSummoner->StatusEffectContainer->HasStatusEffect(xi::StatusEffect::Teleport));
            if (auto* PController = dynamic_cast<CPawnController*>(PPawn->PAI->GetController()); PController != nullptr)
            {
                PController->Carried(withPlayer);
            }
        };

        // A warp: the party's home point, revived if it was the death timer's
        if (PPawn->requestedWarp != WarpRequest::None)
        {
            PPawn->requestedWarp = WarpRequest::None;
            if (PPawn->isDead())
            {
                return homePoint(PPawn);
            }

            CCharEntity* PSummoner = zoneutils::GetChar(summonerOf(PPawn->id));
            if (PSummoner != nullptr)
            {
                PPawn->profile.home_point = PSummoner->profile.home_point;
            }
            const auto& home = PPawn->profile.home_point;

            ShowInfoFmt("pawn: {} warps to zone {}", PPawn->getName(), static_cast<uint16>(home.destination));
            settle(home.destination);
            requestTransfer(PPawn->id, TravelHop{ .destinationZone = home.destination, .walkTo = {}, .arriveAt = home.p });
            return true;
        }

        // A teleport: setPos has already written where it put her and asked
        // for the zone; she goes there by transfer
        if (PPawn->requestedZoneChange)
        {
            PPawn->requestedZoneChange = false;
            if (PPawn->loc.destination != ZONE_NO_DESTINATION && PPawn->loc.destination != PPawn->getZone())
            {
                ShowInfoFmt("pawn: {} is carried to zone {}", PPawn->getName(), static_cast<uint16>(PPawn->loc.destination));
                settle(PPawn->loc.destination);
                requestTransfer(PPawn->id, TravelHop{ .destinationZone = PPawn->loc.destination, .walkTo = {}, .arriveAt = PPawn->loc.p });
                return true;
            }

            // Within the zone: setPos has moved her already, and only the
            // zone change it asked for is refused
            PPawn->status = xi::Status::Normal;
            PPawn->updatemask |= UPDATE_ALL_CHAR;
            if (PPawn->PAI->PathFind)
            {
                PPawn->PAI->PathFind->Clear();
            }
            ShowInfoFmt("pawn: {} is moved within zone {}", PPawn->getName(), static_cast<uint16>(PPawn->getZone()));
            return true;
        }
        return false;
    }

    namespace
    {
        auto gambitsOf(CCharEntity* PPawn) -> CGambits*
        {
            auto* PController = PPawn != nullptr ? dynamic_cast<CPawnController*>(PPawn->PAI->GetController()) : nullptr;
            return PController != nullptr ? &PController->Gambits() : nullptr;
        }
    } // namespace

    void saveGambits(CCharEntity* PPawn)
    {
        auto* PGambits = gambitsOf(PPawn);
        if (PGambits == nullptr)
        {
            return;
        }
        std::string blob;
        for (const auto& row : PGambits->Rows())
        {
            blob += row.enabled ? "1 " : "0 ";
            blob += text::formatRow(row.gambit);
            blob += '\n';
        }
        db::preparedStmt("INSERT INTO cardian_gambits (pawn_charid, set_id, master_on, set_rows) VALUES (?, 0, ?, ?) "
                         "ON DUPLICATE KEY UPDATE master_on = VALUES(master_on), set_rows = VALUES(set_rows)",
                         PPawn->id, static_cast<uint8>(PGambits->MasterOn() ? 1 : 0), blob);
    }

    bool loadSavedGambits(CCharEntity* PPawn)
    {
        auto* PGambits = gambitsOf(PPawn);
        if (PGambits == nullptr)
        {
            return false;
        }
        const auto rset = db::preparedStmt("SELECT master_on, set_rows FROM cardian_gambits WHERE pawn_charid = ? AND set_id = 0", PPawn->id);
        if (!rset || !rset->next())
        {
            return false;
        }

        PGambits->RemoveAllGambits();
        PGambits->SetMaster(rset->get<uint8>("master_on") != 0);

        const auto  blob  = rset->get<std::string>("set_rows");
        std::size_t count = 0;
        std::size_t bad   = 0;
        std::size_t start = 0;
        while (start < blob.size())
        {
            auto end = blob.find('\n', start);
            if (end == std::string::npos)
            {
                end = blob.size();
            }
            const std::string_view line(blob.data() + start, end - start);
            start = end + 1;
            if (line.size() < 3 || line[1] != ' ')
            {
                if (!line.empty())
                {
                    ++bad;
                }
                continue;
            }
            if (auto row = text::parseRow(line.substr(2)); row.has_value())
            {
                PGambits->AddGambit(std::move(*row), line[0] == '1');
                ++count;
            }
            else
            {
                ++bad;
            }
        }
        ShowInfoFmt("pawn: saved gambits loaded for {} ({} rows{})", PPawn->getName(), count, bad != 0 ? fmt::format(", {} malformed skipped", bad) : "");
        return true;
    }

    void forgetGambits(CCharEntity* PPawn)
    {
        if (PPawn != nullptr)
        {
            db::preparedStmt("DELETE FROM cardian_gambits WHERE pawn_charid = ? AND set_id = 0", PPawn->id);
        }
    }

    bool reloadBrain(CCharEntity* PPawn)
    {
        if (PPawn == nullptr || !pawns.contains(PPawn->id))
        {
            return false;
        }
        loadBrain(PPawn);
        return true;
    }

    bool reloadBrainByName(const std::string& targetName)
    {
        const auto it = pawns.find(charutils::getCharIdFromName(targetName));
        if (it == pawns.end())
        {
            return false;
        }
        loadBrain(it->second.get());
        return true;
    }

    bool despawn(const std::string& targetName)
    {
        return despawnById(charutils::getCharIdFromName(targetName));
    }

    bool despawnById(const uint32 targetCharID, const bool keepOnline)
    {
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

        if (!keepOnline)
        {
            db::preparedStmt("DELETE FROM accounts_sessions WHERE charid = ?", targetCharID);
        }

        ShowInfoFmt("pawn: despawned {} ({}){}", PPawn->getName(), targetCharID, keepOnline ? ", still online" : "");

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

    auto findPawn(const uint32 pawnCharID) -> CCharEntity*
    {
        const auto it = pawns.find(pawnCharID);
        return it != pawns.end() ? it->second.get() : nullptr;
    }

    auto findManagedPawn(const CCharEntity* PSummoner, const std::string& targetName) -> CCharEntity*
    {
        if (PSummoner == nullptr)
        {
            return nullptr;
        }

        const uint32 targetCharID = charutils::getCharIdFromName(targetName);
        if (targetCharID == 0 || summonerOf(targetCharID) != PSummoner->id)
        {
            return nullptr;
        }
        return findPawn(targetCharID);
    }

    auto accountPawnNames(const CCharEntity* PChar) -> std::vector<std::string>
    {
        std::vector<std::string> names;
        for (auto& [charid, name] : accountMembers(PChar))
        {
            names.emplace_back(std::move(name));
        }
        return names;
    }

    auto managedPawnNames(const uint32 summonerCharID) -> std::vector<std::string>
    {
        std::vector<std::string> names;
        for (const auto& [charid, PPawn] : pawns)
        {
            if (summonerOf(charid) == summonerCharID)
            {
                names.emplace_back(PPawn->getName());
            }
        }
        std::ranges::sort(names);
        return names;
    }

    auto release(const uint32 pawnCharID) -> std::unique_ptr<CCharEntity>
    {
        const auto it = pawns.find(pawnCharID);
        if (it == pawns.end())
        {
            return nullptr;
        }

        auto PChar = std::move(it->second);
        pawns.erase(it);
        summonerByPawn.erase(pawnCharID);
        pendingInvites.erase(pawnCharID);
        pendingTransfers.erase(pawnCharID);
        travelOrders.erase(pawnCharID);
        PChar->InvitePending.clean();

        // Nothing moves: the character keeps its zone, targid, position,
        // party, effects and fight. The client about to look through its
        // eyes re-runs the zone-in handshake in place, so it has to be shown
        // the world again (the party is re-taught after its handshake, see
        // party_teach.h).
        PChar->loc.destination     = PChar->getZone();
        PChar->loc.prevzone        = PChar->getZone();
        PChar->arrivedByZoning     = true;
        PChar->requestedZoneChange = false;
        PChar->SpawnPCList.clear();
        PChar->SpawnMOBList.clear();
        PChar->SpawnNPCList.clear();
        PChar->SpawnPETList.clear();
        PChar->SpawnTRUSTList.clear();

        // Back to a player's action surface: the stock controller, no
        // server-side pathing, stock speed
        PChar->PAI->SetController(std::make_unique<CPlayerController>(PChar.get()));
        PChar->PAI->PathFind.reset();
        PChar->baseSpeed = settings::get<uint8>("map.BASE_SPEED");
        PChar->UpdateSpeed();
        PChar->clearPacketList();

        ShowInfoFmt("pawn: released {} ({}) from pawn duty for a session, in place", PChar->getName(), PChar->id);
        return PChar;
    }

    bool adopt(std::unique_ptr<CCharEntity> PChar, const uint32 summonerCharID)
    {
        if (!isEnabled() || PChar == nullptr || pawns.contains(PChar->id))
        {
            return false;
        }

        if (PChar->loc.zone == nullptr)
        {
            ShowErrorFmt("pawn: cannot adopt {} ({}): not standing in a zone", PChar->getName(), PChar->id);
            return false;
        }

        // The session that gave this character up is no longer its owner;
        // the character itself does not move, leave, or re-enter anything
        PChar->PSession            = nullptr;
        PChar->requestedZoneChange = false;
        PChar->status              = xi::Status::Normal;
        PChar->clearPacketList();

        install(PChar.get());

        ShowInfoFmt("pawn: adopted {} ({}) as a pawn in place, zone {}, following {}", PChar->getName(), PChar->id, static_cast<uint16>(PChar->getZone()), summonerCharID);

        registerPawn(std::move(PChar), summonerCharID);
        return true;
    }

    void reparent(const uint32 fromCharID, const uint32 toCharID)
    {
        for (auto& [pawnCharID, summonerCharID] : summonerByPawn)
        {
            if (summonerCharID == fromCharID)
            {
                summonerCharID = toCharID;
            }
        }
    }

    void noteInvite(const CCharEntity* PPawn)
    {
        const auto delay = std::chrono::milliseconds(settings::get<uint32>("pawn.INVITE_ACCEPT_DELAY"));
        pendingInvites.insert_or_assign(PPawn->id, timer::now() + delay);
    }

    void notePositionPacket(const CCharEntity* PChar)
    {
        lastPositionPacket.insert_or_assign(PChar->id, timer::now());
    }

    auto positionPacketAge(uint32 charid) -> std::optional<std::chrono::milliseconds>
    {
        const auto it = lastPositionPacket.find(charid);
        if (it == lastPositionPacket.end())
        {
            return std::nullopt;
        }
        return std::chrono::duration_cast<std::chrono::milliseconds>(timer::now() - it->second);
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
        if (!enterZone(PPawn, PDestZone))
        {
            return;
        }
        if (PPawn->status == xi::Status::Disappear)
        {
            PPawn->status = xi::Status::Normal;
        }

        db::preparedStmt("UPDATE accounts_sessions SET targid = ? WHERE charid = ?", PPawn->targid, PPawn->id);
        savePawnPosition(PPawn);

        ShowInfoFmt("pawn: {} ({}) crossed into zone {}", PPawn->getName(), PPawn->id, static_cast<uint16>(destZoneId));
    }

    void onZoneTick(CZone* PZone)
    {
        const auto started   = std::chrono::steady_clock::now();
        uint32     pawnsHere = 0;
        for (const auto& [charid, PPawn] : pawns)
        {
            if (PPawn->loc.zone != PZone)
            {
                continue;
            }
            ++pawnsHere;

            loot::handOff(PPawn.get());

            if (const auto transferIt = pendingTransfers.find(charid); transferIt != pendingTransfers.end())
            {
                const auto hop = std::move(transferIt->second);
                pendingTransfers.erase(transferIt);
                executeTransfer(PPawn.get(), hop);
                PPawn->clearPacketList();
                continue;
            }

            const auto inviteIt = pendingInvites.find(charid);
            if (inviteIt != pendingInvites.end() && timer::now() >= inviteIt->second)
            {
                pendingInvites.erase(inviteIt);

                if (PPawn->InvitePending.UniqueNo != 0)
                {
                    GP_CLI_COMMAND_GROUP_SOLICIT_RES answer{};
                    answer.Res = std::to_underlying(GP_CLI_COMMAND_GROUP_SOLICIT_RES_RES::Accept);

                    if (answer.validate(nullptr, PPawn.get()).valid())
                    {
                        ShowInfoFmt("pawn: {} accepts the party invite", PPawn->getName());
                        answer.process(nullptr, PPawn.get());
                        gatherOrHold(PPawn.get());
                    }
                }
            }

            // Nobody drains a session-less char's outbound queue; without
            // this it grows without bound
            PPawn->clearPacketList();
        }

        world::onZoneTick(PZone);
        world::noteModuleTick(PZone, std::chrono::steady_clock::now() - started, pawnsHere);
    }
} // namespace pawn
