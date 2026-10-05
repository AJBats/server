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

#include "account_wide.h"

#include "common/database.h"
#include "common/logging.h"
#include "common/lua.h"
#include "common/settings.h"
#include "entities/char_entity.h"
#include "enums/packet_s2c.h"
#include "lua/lua_base_entity.h"
#include "map_session.h"
#include "packets/basic.h"
#include "packets/s2c/0x01b_job_info.h"
#include "packets/s2c/0x055_scenarioitem.h"
#include "utils/charutils.h"
#include "utils/moduleutils.h"
#include "utils/zoneutils.h"
#include "zone.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

// A player's major progression is his account's, not one character's (the
// user, 2026-10-05; ROADMAP N): what one character of an account has earned,
// every character of it has -- the support job, the level cap the limit
// breaks raise, the gate crystals and Limit Breaker, and the outposts' warps.
// It is copied: each character holds it as if earned, so it stays with the
// character whatever happens later, and cardian.ACCOUNT_WIDE_PROGRESSION (a
// server setting) switches the copying off.
//
// The maps are different: they are shared without being copied. A map any
// character of the account holds is shown to the others in the key item
// packet as it leaves the server (OnPushPacket), and never written to their
// own key items, so a player who turns the addon's Shared maps off sees only
// the maps his character earned (setSharedMaps).
//
// When: at the grant. Six entity calls write these fields once a character is
// loaded -- unlockJob, changeJob and changesJob (the job bits), setLevelCap
// (the level cap), addKeyItem (key items) and addTeleport (the outposts) --
// and modules/cardian/lua/account_wide.lua tells the account after each
// (cardianAccountGranted), so a character's new progression reaches the rest
// of the account at once. A real player's zone-in, his login included, brings
// the account together again as a safety net, for whatever came by another
// path: what it copies is logged as a warning, since the grant should have.
// An account is its characters in `chars.accid`; one in a zone is read and
// written in memory (and saved), the others in the database.
namespace
{
    // The account's lists, by name in the game's own key item table
    // (scripts/enum/key_item.codegen.lua, xi.keyItem), read once Lua has it:
    // copied, every *_GATE_CRYSTAL and LIMIT_BREAKER; shared as a view, every
    // MAP_OF_*. Logged as read, so the list can be checked against the game's
    struct Lists
    {
        bool                read = false;
        std::vector<uint16> shared;
        std::vector<uint16> maps;
    };

    auto lists() -> const Lists&
    {
        static Lists l;
        if (l.read)
        {
            return l;
        }
        const auto table = ::lua["xi"]["keyItem"].get<sol::optional<sol::table>>();
        if (!table.has_value())
        {
            return l; // not loaded yet: asked again next time
        }
        std::vector<std::pair<uint16, std::string>> named;
        for (const auto& [key, value] : *table)
        {
            if (!key.is<std::string>() || !value.is<uint16>())
            {
                continue;
            }
            const auto name = key.as<std::string>();
            const auto id   = value.as<uint16>();
            if (name.ends_with("_GATE_CRYSTAL") || name == "LIMIT_BREAKER")
            {
                l.shared.push_back(id);
                named.emplace_back(id, name);
            }
            else if (name.starts_with("MAP_OF_"))
            {
                l.maps.push_back(id);
            }
        }
        std::ranges::sort(l.shared);
        std::ranges::sort(l.maps);
        std::ranges::sort(named);
        std::string names;
        for (const auto& [id, name] : named)
        {
            names += fmt::format("{}{} ({})", names.empty() ? "" : ", ", name, id);
        }
        l.read = true;
        ShowInfoFmt("account: copied across an account: the support job, the level cap, the outposts' warps, {}", names);
        ShowInfoFmt("account: {} maps shared across an account as a view", l.maps.size());
        return l;
    }

    constexpr std::size_t kKeyTables = std::tuple_size_v<decltype(keyitems_t::tables)>;

    // One character of an account as the sync sees it
    struct Member
    {
        uint32       charid = 0;
        std::string  name;
        CCharEntity* PChar    = nullptr; // in a zone: read and written in memory
        uint32       unlocked = 0;
        uint8        genkai   = 0;
        uint32       outposts[3]{};
        keyitems_t   keys{};
        bool         hasUnlocks = false; // a char_unlocks row to write the outposts to
    };

    auto readAccount(const uint32 accid) -> std::vector<Member>
    {
        std::vector<Member> members;
        const auto rset = db::preparedStmt("SELECT c.charid, c.charname, c.keyitems, j.unlocked, j.genkai, u.charid AS ucharid, "
                                           "COALESCE(u.outpost_sandy, 0) AS os, COALESCE(u.outpost_bastok, 0) AS ob, COALESCE(u.outpost_windy, 0) AS ow "
                                           "FROM chars c JOIN char_jobs j ON j.charid = c.charid LEFT JOIN char_unlocks u ON u.charid = c.charid "
                                           "WHERE c.accid = ?",
                                           accid);
        if (!rset)
        {
            return members;
        }
        while (rset->next())
        {
            Member m;
            m.charid     = rset->get<uint32>("charid");
            m.name       = rset->get<std::string>("charname");
            m.hasUnlocks = !rset->isNull("ucharid");
            m.PChar      = zoneutils::GetChar(m.charid);
            if (m.PChar != nullptr)
            {
                m.unlocked    = m.PChar->jobs.unlocked;
                m.genkai      = m.PChar->jobs.genkai;
                m.outposts[0] = m.PChar->teleport.outpostSandy;
                m.outposts[1] = m.PChar->teleport.outpostBastok;
                m.outposts[2] = m.PChar->teleport.outpostWindy;
                m.keys        = m.PChar->keys;
            }
            else
            {
                m.unlocked    = rset->get<uint32>("unlocked");
                m.genkai      = rset->get<uint8>("genkai");
                m.outposts[0] = rset->get<uint32>("os");
                m.outposts[1] = rset->get<uint32>("ob");
                m.outposts[2] = rset->get<uint32>("ow");
                if (!rset->isNull("keyitems"))
                {
                    db::extractFromBlob(rset, "keyitems", m.keys);
                }
            }
            members.push_back(std::move(m));
        }
        return members;
    }

    auto hasKey(const keyitems_t& keys, const uint16 id) -> bool
    {
        return (id >> 9) < kKeyTables && keys.tables[id >> 9].keyList.test(id & 511);
    }

    // An account's maps, by key item table: the bytes of the key item
    // packet's flags, every map any of its characters holds set
    using MapBytes = std::array<std::array<uint8, 64>, kKeyTables>;
    std::unordered_map<uint32, MapBytes> accountMaps;

    // The players who turned the addon's Shared maps off, by charid
    std::unordered_set<uint32> mapsOff;

    auto mapsOf(const std::vector<Member>& members) -> MapBytes
    {
        MapBytes maps{};
        for (const auto& m : members)
        {
            for (const auto id : lists().maps)
            {
                if (hasKey(m.keys, id))
                {
                    maps[id >> 9][(id & 511) / 8] |= static_cast<uint8>(1 << (id % 8));
                }
            }
        }
        return maps;
    }

    // Who brought the account together, and why: a grant, or the safety net
    struct Cause
    {
        bool        safetyNet = false;
        std::string by; // "Misha's unlockJob", "Misha's zone-in"
    };

    void tell(const Cause& cause, const std::string& line)
    {
        if (cause.safetyNet)
        {
            ShowWarningFmt("account: the safety net caught, at {}: {}", cause.by, line);
        }
        else
        {
            ShowInfoFmt("account: {}: {}", cause.by, line);
        }
    }

    // Every character of the account brought up to what any of them has, and
    // the account's maps noted for the key item packet. Says whether the maps
    // changed, so the clients can be shown them again
    auto syncAccount(const uint32 accid, const Cause& cause) -> bool
    {
        auto members = readAccount(accid);
        if (members.empty())
        {
            return false;
        }

        if (settings::get<bool>("cardian.ACCOUNT_WIDE_PROGRESSION"))
        {
            // What the account has
            bool                       subjob = false;
            uint8                      genkai = 0;
            uint32                     outposts[3]{};
            std::string                subjobFrom;
            std::string                genkaiFrom;
            std::unordered_set<uint16> shared;
            for (const auto& m : members)
            {
                if ((m.unlocked & 1) != 0 && !subjob)
                {
                    subjob     = true;
                    subjobFrom = m.name;
                }
                if (m.genkai > genkai)
                {
                    genkai     = m.genkai;
                    genkaiFrom = m.name;
                }
                for (std::size_t n = 0; n < 3; ++n)
                {
                    outposts[n] |= m.outposts[n];
                }
                for (const auto id : lists().shared)
                {
                    if (hasKey(m.keys, id))
                    {
                        shared.insert(id);
                    }
                }
            }

            // Every character brought up to it
            for (auto& m : members)
            {
                auto* PChar = m.PChar;
                if (subjob && (m.unlocked & 1) == 0)
                {
                    tell(cause, fmt::format("{} gets the support job, as {} has it", m.name, subjobFrom));
                    if (PChar != nullptr)
                    {
                        PChar->jobs.unlocked |= 1;
                        charutils::SaveCharJob(PChar, xi::Job::WAR); // as upstream's unlockJob(0) saves it
                        if (PChar->PSession != nullptr)
                        {
                            PChar->pushPacket<GP_SERV_COMMAND_JOB_INFO>(PChar);
                        }
                    }
                    else
                    {
                        db::preparedStmt("UPDATE char_jobs SET unlocked = unlocked | 1 WHERE charid = ? LIMIT 1", m.charid);
                    }
                }
                if (m.genkai < genkai)
                {
                    tell(cause, fmt::format("{}'s level cap rises to {}, as {}'s is", m.name, genkai, genkaiFrom));
                    if (PChar != nullptr)
                    {
                        PChar->jobs.genkai = genkai;
                    }
                    db::preparedStmt("UPDATE char_jobs SET genkai = ? WHERE charid = ? LIMIT 1", genkai, m.charid);
                }
                const TELEPORT_TYPE kinds[3]  = { TELEPORT_TYPE::OUTPOST_SANDY, TELEPORT_TYPE::OUTPOST_BASTOK, TELEPORT_TYPE::OUTPOST_WINDY };
                const char*         column[3] = { "outpost_sandy", "outpost_bastok", "outpost_windy" };
                for (std::size_t n = 0; n < 3; ++n)
                {
                    if ((m.outposts[n] | outposts[n]) == m.outposts[n] || (PChar == nullptr && !m.hasUnlocks))
                    {
                        continue; // nothing new, or nowhere to write it
                    }
                    tell(cause, fmt::format("{} gets the outpost warps {:#x} ({}), as the account has them", m.name, outposts[n] & ~m.outposts[n], column[n]));
                    if (PChar != nullptr)
                    {
                        uint32* fields[3] = { &PChar->teleport.outpostSandy, &PChar->teleport.outpostBastok, &PChar->teleport.outpostWindy };
                        *fields[n] |= outposts[n];
                        charutils::SaveTeleport(PChar, kinds[n]);
                    }
                    else
                    {
                        db::preparedStmt(fmt::format("UPDATE char_unlocks SET {0} = {0} | ? WHERE charid = ? LIMIT 1", column[n]), outposts[n], m.charid);
                    }
                }
                std::unordered_set<uint8> tables;
                for (const auto id : shared)
                {
                    if (hasKey(m.keys, id))
                    {
                        continue;
                    }
                    tell(cause, fmt::format("{} gets key item {}, as the account has it", m.name, id));
                    m.keys.tables[id >> 9].keyList.set(id & 511);
                    tables.insert(static_cast<uint8>(id >> 9));
                }
                if (!tables.empty())
                {
                    if (PChar != nullptr)
                    {
                        PChar->keys = m.keys;
                        charutils::SaveKeyItems(PChar);
                        if (PChar->PSession != nullptr)
                        {
                            for (const auto table : tables)
                            {
                                PChar->pushPacket<GP_SERV_COMMAND_SCENARIOITEM>(PChar, table);
                            }
                        }
                    }
                    else
                    {
                        db::preparedStmt("UPDATE chars SET keyitems = ? WHERE charid = ? LIMIT 1", m.keys, m.charid);
                    }
                }
            }
        }

        const auto maps    = mapsOf(members);
        const auto old     = accountMaps.find(accid);
        const bool changed = old == accountMaps.end() || old->second != maps;
        accountMaps[accid] = maps;
        return changed;
    }

    // The key item tables that hold maps, shown again to one player (the
    // packet hook adds the account's maps as each leaves)
    void showMapsTo(CCharEntity* PChar)
    {
        std::unordered_set<uint8> tables;
        for (const auto id : lists().maps)
        {
            tables.insert(static_cast<uint8>(id >> 9));
        }
        for (const auto table : tables)
        {
            PChar->pushPacket<GP_SERV_COMMAND_SCENARIOITEM>(PChar, table);
        }
    }

    // ... and to every character of the account with a client
    void showMaps(const uint32 accid)
    {
        zoneutils::ForEachZone(
            [&](CZone* PZone)
            {
                PZone->ForEachChar(
                    [&](CCharEntity* PChar)
                    {
                        if (PChar->accid == accid && PChar->PSession != nullptr)
                        {
                            showMapsTo(PChar);
                        }
                    });
            });
    }

    // GP_SERV_COMMAND_SCENARIOITEM's flags as bytes of the packet: the held
    // ones, the seen ones and the table's index, after the header
    using KeyItemPacket = GP_SERV_COMMAND_SCENARIOITEM::PacketData;
    constexpr std::size_t kHeld  = sizeof(GP_SERV_HEADER) + offsetof(KeyItemPacket, GetItemFlag);
    constexpr std::size_t kSeen  = sizeof(GP_SERV_HEADER) + offsetof(KeyItemPacket, LookItemFlag);
    constexpr std::size_t kTable = sizeof(GP_SERV_HEADER) + offsetof(KeyItemPacket, TableIndex);
    static_assert(sizeof(KeyItemPacket::GetItemFlag) == 64 && sizeof(KeyItemPacket::LookItemFlag) == 64, "a key item table is 512 flags");
    static_assert(sizeof(keyitems_table_t::keyList) == 64, "the packet's flags are a copy of the table's bytes");
} // namespace

namespace pawn::accountwide
{
    void setSharedMaps(CCharEntity* PChar, const bool on)
    {
        if (PChar == nullptr)
        {
            return;
        }
        const bool was = !mapsOff.contains(PChar->id);
        if (on)
        {
            mapsOff.erase(PChar->id);
        }
        else
        {
            mapsOff.insert(PChar->id);
        }
        if (was != on && PChar->PSession != nullptr)
        {
            ShowInfoFmt("account: {} turns Shared maps {}", PChar->getName(), on ? "on" : "off");
            showMapsTo(PChar);
        }
    }
} // namespace pawn::accountwide

class AccountWideModule : public CPPModule
{
    void OnInit() override
    {
        // modules/cardian/lua/account_wide.lua, after a grant: the account
        // brought together at once. what: the grant's name, for the log
        ::lua["CBaseEntity"]["cardianAccountGranted"] = [](CLuaBaseEntity* PLuaBaseEntity, const std::string& what)
        {
            auto* PChar = dynamic_cast<CCharEntity*>(PLuaBaseEntity->GetBaseEntity());
            if (PChar == nullptr || PChar->accid == 0)
            {
                return;
            }
            if (syncAccount(PChar->accid, Cause{ .safetyNet = false, .by = fmt::format("{}'s {}", PChar->getName(), what) }))
            {
                showMaps(PChar->accid);
            }
        };
    }

    // The safety net: a real player arriving (his login included) brings his
    // account together; the world's bodies are never looked at
    void OnCharZoneIn(CCharEntity* PChar) override
    {
        if (PChar != nullptr && PChar->PSession != nullptr && PChar->accid != 0)
        {
            syncAccount(PChar->accid, Cause{ .safetyNet = true, .by = fmt::format("{}'s zone-in", PChar->getName()) });
        }
    }

    // The account's maps, added to each key item table as it leaves for a
    // client, marked seen where his own list lacks them; never written to
    // the character. Only read here: the account's maps are noted by a grant
    // or a zone-in, and read from the database the first time if neither has
    void OnPushPacket(CCharEntity* PChar, const std::unique_ptr<CBasicPacket>& packet) override
    {
        if (packet->getType() != std::to_underlying(PacketS2C::GP_SERV_COMMAND_SCENARIOITEM) || PChar->PSession == nullptr ||
            mapsOff.contains(PChar->id))
        {
            return;
        }
        auto it = accountMaps.find(PChar->accid);
        if (it == accountMaps.end())
        {
            it = accountMaps.emplace(PChar->accid, mapsOf(readAccount(PChar->accid))).first;
        }
        const auto table = packet->ref<uint16>(kTable);
        if (table >= kKeyTables)
        {
            return;
        }
        const auto& bytes = it->second[table];
        for (std::size_t i = 0; i < bytes.size(); ++i)
        {
            const auto added = static_cast<uint8>(bytes[i] & ~packet->ref<uint8>(kHeld + i));
            packet->ref<uint8>(kHeld + i) |= added;
            packet->ref<uint8>(kSeen + i) |= added;
        }
    }
};

REGISTER_CPP_MODULE(AccountWideModule);
