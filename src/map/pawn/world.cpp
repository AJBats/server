/*
===========================================================================

  Cardian: the world's adventurers (ROADMAP D, RESEARCH.md §11). See world.h.

===========================================================================
*/

#include "world.h"
#include "pawn_items.h"

#include "pawn.h"

#include "common/database.h"
#include "common/logging.h"
#include "common/settings.h"

#include "data/datasets/zones/settings/dataset.h"
#include "data/loader.h"
#include "data/yaml/read.h"
#include "entities/char_entity.h"
#include "item_container.h"
#include "items/item.h"
#include "items/transactions/item_claim.h"
#include "login/login_helpers.h"
#include "utils/charutils.h"
#include "utils/itemutils.h"
#include "map_session.h"
#include "navmesh/navmesh.h"
#include "status_effect_container.h"
#include "utils/zoneutils.h"
#include "zone.h"

#include <sys/resource.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <magic_enum/magic_enum.hpp>
#include <cmath>
#include <numbers>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

using namespace std::chrono_literals;

namespace
{
    struct Body
    {
        uint32      charid  = 0;
        std::string name;
        uint16      zone    = 0;
        position_t  point{};
        bool        present = false;
        bool        pinned  = false;

        bool farming = false;

        // The zone slot she holds (index into the zone's table), -1 for none,
        // and its roam: the home pull's scale for the errand (0 = roams anywhere)
        int32 slot = -1;
        float roam = 0.0f;

        // KO'd: when she fell, so she fades after WORLD_KO_FADE
        std::optional<std::chrono::steady_clock::time_point> downSince;

        // Her bag as she stood: slot -> item and quantity. Anything beyond
        // it is a drop, and drops go to the void (sweepBag)
        struct Kept
        {
            uint16 itemId   = 0;
            uint32 quantity = 0;
        };
        std::unordered_map<uint8, Kept> kit;
        uint32                          sweepTick = 0;

        // Her census level and seed: the exp cap (sweepBag) is drawn from them
        uint8  censusLevel = 1;
        uint32 seed        = 0;
    };

    std::unordered_map<uint32, Body>        bodies; // by charid
    std::unordered_map<std::string, uint32> charidByName;
    std::unordered_map<uint16, uint32>      realPlayersLastTick;
    bool                                    debugRingPlaced = false;

    // Liveness: a zone is live while a real player is in it or, within
    // WORLD_LIVE_RADIUS zone lines, next to it; -1 keeps every zone live.
    // A zone that stops being live keeps its bodies WORLD_FADE_DELAY
    // seconds more, so a player popping out to shed aggro and straight
    // back finds the zone as they left it. Neighbours come from the
    // zone's own exits in its zone file, never a hand list.
    std::unordered_map<uint16, std::vector<uint16>>                    neighbourCache;
    std::unordered_map<uint16, std::chrono::steady_clock::time_point> lastLive;
    std::unordered_map<uint16, bool>                                   wasLive;

    auto neighboursOf(const uint16 zoneId) -> const std::vector<uint16>&
    {
        if (const auto it = neighbourCache.find(zoneId); it != neighbourCache.end())
        {
            return it->second;
        }
        auto& out = neighbourCache[zoneId];
        if (const auto zoneFile = xi::data::loadZoneFile<xi::data::datasets::zones::settings::Dataset>(static_cast<xi::ZoneId>(zoneId)); zoneFile)
        {
            for (const auto& line : zoneFile->ZoneLines)
            {
                const auto to = static_cast<uint16>(line.DestinationZone);
                if (to != zoneId && std::ranges::find(out, to) == out.end())
                {
                    out.push_back(to);
                }
            }
        }
        return out;
    }

    // The load line's counters, always on
    struct Load
    {
        std::chrono::steady_clock::time_point since{};
        rusage                                usage{};
        uint64                                brainTicks = 0;
        std::chrono::nanoseconds              brainTotal{};
        uint64                                moduleTicks = 0;
        std::chrono::nanoseconds              moduleTotal{};
        bool                                  started = false;
    };
    Load load;

    auto cpuSeconds(const rusage& ru) -> double
    {
        return static_cast<double>(ru.ru_utime.tv_sec + ru.ru_stime.tv_sec) + static_cast<double>(ru.ru_utime.tv_usec + ru.ru_stime.tv_usec) / 1e6;
    }

    auto rssMegabytes() -> long
    {
        std::ifstream statm("/proc/self/statm");
        long          pages = 0, resident = 0;
        statm >> pages >> resident;
        return resident * sysconf(_SC_PAGESIZE) / (1024 * 1024);
    }

    // Bodies waiting to stand, a few per zone tick: minting and loading a
    // character is hundreds of queries, and a ring of forty in one tick
    // would stall the map past its watchdog
    struct Pending
    {
        std::string name;
        uint16      zone = 0;
        position_t  point{};
        bool        pinned  = false;
        bool        farming = false;
        bool        presence = false; // a slot's occupant: presence now, her body when the zone is live
        int32       slot     = -1;
        float       roam     = 0.0f;
    };
    std::vector<Pending> pending;
    constexpr uint32     kStandPerTick = 2;

    struct CensusRow
    {
        uint32 charid = 0;
        uint8  race   = 1;
        uint8  face   = 0;
        uint8  size   = 0;
        uint8  nation = 0;
        uint8  job    = 1;
        uint8  level  = 1;
        uint32 seed   = 0;
    };

    auto readCensus(const std::string& name) -> std::optional<CensusRow>
    {
        const auto rset = db::preparedStmt("SELECT charid, race, face, size, nation, job, level, seed FROM cardian_census WHERE name = ?", name);
        if (!rset || !rset->next())
        {
            return std::nullopt;
        }
        return CensusRow{
            .charid = rset->get<uint32>("charid"),
            .race   = rset->get<uint8>("race"),
            .face   = rset->get<uint8>("face"),
            .size   = rset->get<uint8>("size"),
            .nation = rset->get<uint8>("nation"),
            .job    = rset->get<uint8>("job"),
            .level  = rset->get<uint8>("level"),
            .seed   = rset->get<uint32>("seed"),
        };
    }

    // Her charid, minting her on the world account the first time
    auto ensureMinted(const std::string& name, CensusRow& row) -> uint32
    {
        if (row.charid != 0)
        {
            return row.charid;
        }
        const uint32 owner = pawn::worldAccountId();
        if (owner == 0)
        {
            return 0;
        }
        const pawn::CharSpec spec{
            .name   = name,
            .race   = row.race,
            .face   = row.face,
            .size   = row.size,
            .nation = row.nation,
            .mjob   = row.job,
            .level  = row.level,
        };
        row.charid = pawn::createFromSpec(spec, owner);
        if (row.charid != 0)
        {
            db::preparedStmt("UPDATE cardian_census SET charid = ? WHERE name = ?", row.charid, name);
        }
        return row.charid;
    }

    auto realPlayersIn(CZone* PZone) -> uint32
    {
        uint32 count = 0;
        PZone->ForEachChar([&](CCharEntity* PChar)
        {
            if (PChar->PSession != nullptr && !pawn::isPawn(PChar))
            {
                ++count;
            }
        });
        return count;
    }

    auto isLive(CZone* PZone, const uint32 realHere) -> bool
    {
        const auto radius = settings::get<int32>("pawn.WORLD_LIVE_RADIUS");
        if (radius < 0 || realHere > 0)
        {
            return true;
        }
        if (radius == 0)
        {
            return false;
        }
        for (const auto neighbour : neighboursOf(static_cast<uint16>(PZone->GetID())))
        {
            if (auto* PNeighbour = zoneutils::GetZone(static_cast<xi::ZoneId>(neighbour)); PNeighbour != nullptr && realPlayersIn(PNeighbour) > 0)
            {
                return true;
            }
        }
        return false;
    }

    // The census name as written: first letter up, the rest down
    auto properName(std::string name) -> std::string
    {
        for (size_t i = 0; i < name.size(); ++i)
        {
            const auto c = static_cast<unsigned char>(name[i]);
            name[i]      = static_cast<char>(i == 0 ? std::toupper(c) : std::tolower(c));
        }
        return name;
    }

    void snapshotBag(Body& body)
    {
        body.kit.clear();
        const auto* PPawn   = pawn::findPawn(body.charid);
        const auto* storage = PPawn != nullptr ? PPawn->getStorage(LOC_INVENTORY) : nullptr;
        if (storage == nullptr)
        {
            return;
        }
        for (uint8 slot = 1; slot <= storage->GetSize(); ++slot)
        {
            if (const auto* PItem = storage->GetItem(slot); PItem != nullptr)
            {
                body.kit[slot] = Body::Kept{ .itemId = PItem->getID(), .quantity = PItem->getQuantity() };
            }
        }
    }

    // Skill-up notation for levels: 1.70 is level 1 at 70 % of the way to
    // 2. Her world cap is her census level plus a fraction her seed draws
    // between WORLD_EXP_CAP_MIN and WORLD_EXP_CAP_MAX, different per farmer
    // so none ding in step; it moves when the census moves
    auto capOf(const Body& body) -> float
    {
        const uint32 mix  = (body.seed * 2654435761u) ^ (static_cast<uint32>(body.censusLevel) * 40503u);
        const float  low  = settings::get<float>("pawn.WORLD_EXP_CAP_MIN") / 100.0f;
        const float  high = settings::get<float>("pawn.WORLD_EXP_CAP_MAX") / 100.0f;
        return static_cast<float>(body.censusLevel) + low + (high - low) * static_cast<float>(mix % 1000u) / 1000.0f;
    }

    auto progressOf(const uint8 level, const uint32 exp) -> float
    {
        const auto tnl = charutils::GetExpNEXTLevel(level);
        return static_cast<float>(level) + (tnl > 0 ? static_cast<float>(exp) / static_cast<float>(tnl) : 0.0f);
    }

    // The exp that puts a character of this level at the cap, or as near as
    // her level allows: none if she is already past the cap's level
    auto expAt(const float cap, const uint8 level) -> uint32
    {
        const float room = std::clamp(cap - static_cast<float>(level), 0.0f, 1.0f);
        return static_cast<uint32>(room * static_cast<float>(charutils::GetExpNEXTLevel(level)));
    }

    // A world body's kills pay her no drops: a solo pool hands one over on
    // arrival, so it is taken back here -- anything in her bag beyond what
    // she stood with is dropped (charutils::DropItem, the player's own
    // throw-away). Her exp is real, under her world cap (user 2026-09-07):
    // one number for where she is, one for the cap, one check. A big kill
    // at a low level can land her a level past the cap; that ding stands,
    // and the check holds her at that level's floor. The census owns her
    // level (RESEARCH §11.3); this is how it shows
    void sweepBag(Body& body)
    {
        auto* PPawn = pawn::findPawn(body.charid);
        if (PPawn == nullptr)
        {
            return;
        }
        // Her census level, re-read now and then so a raise reaches a
        // standing body: the cap moves and her next kills carry her over
        if (body.sweepTick % 75 == 0)
        {
            if (const auto rset = db::preparedStmt("SELECT level FROM cardian_census WHERE charid = ?", body.charid); rset && rset->next())
            {
                const auto now = rset->get<uint8>("level");
                if (now != body.censusLevel)
                {
                    body.censusLevel = now;
                    ShowInfoFmt("world: {}'s cap is {:.2f} now", body.name, capOf(body));
                }
            }
            // Signet lapses after three hours; a body standing that long
            // takes it again, as she does at every fade-in
            if (settings::get<bool>("pawn.WORLD_SIGNET") && !PPawn->StatusEffectContainer->HasStatusEffect(xi::StatusEffect::Signet))
            {
                PPawn->StatusEffectContainer->AddStatusEffect(xi::StatusEffect::Signet, static_cast<uint16>(xi::StatusEffect::Signet), 0, 0s, std::chrono::hours(3));
                PPawn->clearPacketList();
            }
        }
        const auto  job   = PPawn->GetMJob();
        const uint8 level = PPawn->GetMLevel();
        auto&       exp   = PPawn->jobs.exp[static_cast<uint8>(job)];
        const float cap   = capOf(body);
        if (progressOf(level, exp) > cap)
        {
            exp = expAt(cap, level);
            charutils::SaveCharExp(PPawn, job);
        }
        const auto* storage = PPawn->getStorage(LOC_INVENTORY);
        if (storage == nullptr)
        {
            return;
        }
        for (uint8 slot = 1; slot <= storage->GetSize(); ++slot)
        {
            const auto* PItem = storage->GetItem(slot);
            if (PItem == nullptr)
            {
                continue;
            }
            const auto kept   = body.kit.find(slot);
            uint32     excess = 0;
            if (kept == body.kit.end() || kept->second.itemId != PItem->getID())
            {
                excess = PItem->getQuantity();
            }
            else if (PItem->getQuantity() > kept->second.quantity)
            {
                excess = PItem->getQuantity() - kept->second.quantity;
            }
            if (excess == 0)
            {
                continue;
            }
            const uint16 itemId = PItem->getID();
            charutils::DropItem(PPawn, LOC_INVENTORY, slot, static_cast<int32>(excess), itemId);
            PPawn->clearPacketList();
            if (pawn::world::tickDebug())
            {
                const CItem* PKnown = xi::items::lookup(itemId);
                ShowInfoFmt("world: {}'s {} x{} goes to the void", body.name, PKnown != nullptr ? PKnown->getName() : std::to_string(itemId), excess);
            }
        }
    }

    // Her wardrobe as the census wrote it (RESEARCH §11.4): each piece put
    // in her bag if she lacks it, then worn. Read on every stand, so a census
    // that re-dressed her hands her the new pieces; a piece she takes off is
    // dropped (sold, as far as the world knows) so her bag never fills up
    auto dressFromWardrobe(const Body& body, CCharEntity* PPawn) -> uint32
    {
        std::vector<std::pair<uint8, uint16>> wardrobe;
        std::unordered_set<uint16>            wanted;
        if (const auto rset = db::preparedStmt("SELECT slot, itemid FROM cardian_wardrobe WHERE name = ? ORDER BY slot", body.name); rset)
        {
            while (rset->next())
            {
                wardrobe.emplace_back(rset->get<uint8>("slot"), rset->get<uint16>("itemid"));
                wanted.insert(wardrobe.back().second);
            }
        }
        auto*  bag  = PPawn->getStorage(LOC_INVENTORY);
        uint32 worn = 0;
        for (const auto& [equipSlot, itemId] : wardrobe)
        {
            const CItem* PWorn = PPawn->getEquip(static_cast<SLOTTYPE>(equipSlot));
            if (PWorn != nullptr && PWorn->getID() == itemId)
            {
                continue;
            }
            uint8 invSlot = bag != nullptr ? bag->SearchItem(itemId) : ERROR_SLOTID;
            if (invSlot == ERROR_SLOTID)
            {
                auto transaction = ItemClaimTransaction::start(PPawn);
                if (!transaction)
                {
                    break;
                }
                const uint32 quantity = equipSlot == SLOT_AMMO ? 99 : 1;
                const auto   landed   = transaction->give(LOC_INVENTORY, itemId, quantity, Silence::Yes);
                if (!landed.has_value() || !transaction->commit())
                {
                    ShowWarningFmt("world: {} cannot take item {} for slot {}", body.name, itemId, equipSlot);
                    continue;
                }
                invSlot = *landed;
            }
            if (const auto why = pawn::items::equip(PPawn, invSlot, equipSlot, LOC_INVENTORY); !why.empty())
            {
                ShowWarningFmt("world: {} cannot wear item {} in slot {}: {}", body.name, itemId, equipSlot, why);
                continue;
            }
            ++worn;
            if (PWorn != nullptr && bag != nullptr && !wanted.contains(PWorn->getID()))
            {
                for (uint8 slot = 1; slot <= bag->GetSize(); ++slot)
                {
                    if (bag->GetItem(slot) == PWorn)
                    {
                        charutils::DropItem(PPawn, LOC_INVENTORY, slot, static_cast<int32>(PWorn->getQuantity()), PWorn->getID());
                        break;
                    }
                }
            }
        }
        if (worn > 0)
        {
            charutils::SaveCharEquip(PPawn);
        }
        return worn;
    }

    bool fadeIn(Body& body)
    {
        auto* PZone = zoneutils::GetZone(static_cast<xi::ZoneId>(body.zone));
        if (PZone == nullptr)
        {
            return false;
        }
        const auto row = readCensus(body.name);
        if (!row.has_value() || row->charid == 0)
        {
            return false;
        }
        if (!pawn::spawnAt(row->charid, PZone, body.point, row->job, row->level))
        {
            return false;
        }
        body.present     = true;
        body.censusLevel = row->level;
        body.seed        = row->seed;
        if (auto* PPawn = pawn::findPawn(row->charid); PPawn != nullptr)
        {
            if (const auto worn = dressFromWardrobe(body, PPawn); worn > 0)
            {
                ShowInfoFmt("world: {} dresses in {} pieces", body.name, worn);
            }
        }
        snapshotBag(body);
        if (pawn::world::tickDebug())
        {
            if (const auto* PPawn = pawn::findPawn(row->charid); PPawn != nullptr)
            {
                ShowInfoFmt("world: {} stands at {:.2f} under a cap of {:.2f}", body.name,
                            progressOf(PPawn->GetMLevel(), PPawn->jobs.exp[static_cast<uint8>(PPawn->GetMJob())]), capOf(body));
            }
        }

        // Her nation's Signet, as the gate guard would give it: her kills in
        // a conquest region then count for her nation as any player's do,
        // and the farmers round the player feed real influence (ROADMAP F)
        if (settings::get<bool>("pawn.WORLD_SIGNET"))
        {
            if (auto* PPawn = pawn::findPawn(row->charid); PPawn != nullptr && !PPawn->StatusEffectContainer->HasStatusEffect(xi::StatusEffect::Signet))
            {
                PPawn->StatusEffectContainer->AddStatusEffect(xi::StatusEffect::Signet, static_cast<uint16>(xi::StatusEffect::Signet), 0, 0s, std::chrono::hours(3));
                PPawn->clearPacketList();
            }
        }
        // Her spellbook as the census wrote it: every spell her job casts at
        // her level whose scroll the book has opened (RESEARCH §11.4). Read
        // on every stand, so a census that grew hands her the new ones
        if (auto* PPawn = pawn::findPawn(row->charid); PPawn != nullptr)
        {
            uint32 learned = 0;
            if (const auto rset = db::preparedStmt("SELECT spellid FROM cardian_spells WHERE name = ?", body.name); rset)
            {
                while (rset->next())
                {
                    const auto spellId = rset->get<uint16>("spellid");
                    if (charutils::addSpell(PPawn, spellId) != 0)
                    {
                        charutils::SaveSpell(PPawn, spellId);
                        ++learned;
                    }
                }
            }
            if (learned > 0)
            {
                ShowInfoFmt("world: {} learns {} spells", body.name, learned);
            }
        }
        ShowInfoFmt("world: {} fades in at {} ({:.1f}, {:.1f}, {:.1f})", body.name, PZone->getName(), body.point.x, body.point.y, body.point.z);
        return true;
    }

    void fadeOut(Body& body, const std::string_view why)
    {
        if (!body.present)
        {
            return;
        }
        // Out of the zone, not offline: her session row stays, so search
        // still lists her where she stood, at her job and level
        pawn::despawnById(body.charid, true);
        body.present = false;
        body.downSince.reset();
        ShowInfoFmt("world: {} fades out ({}), still online", body.name, why);
    }

    struct Sample
    {
        uint32                   ticks = 0;
        std::chrono::nanoseconds moduleTotal{};
        std::chrono::nanoseconds moduleWorst{};
        uint32                   brainTicks = 0;
        std::chrono::nanoseconds brainTotal{};
        std::chrono::nanoseconds brainWorst{};
    };
    std::unordered_map<uint16, Sample> samples;
    constexpr uint32                   kReportEvery = 100;

    auto microseconds(const std::chrono::nanoseconds ns) -> long long
    {
        return std::chrono::duration_cast<std::chrono::microseconds>(ns).count();
    }
    // -- The slot tables (ROADMAP D3, RESEARCH §11.5) -----------------------------
    // A Cardian-owned YAML per zone, modules/cardian/world/<Zone>.yaml, says
    // what happens where, never who: an activity, a level band, a count, a
    // point and a spread. Filling a slot is a query over the census.
    struct SlotSpec
    {
        std::string          activity; // farm, stand, camp (a party; its members farm solo until D5 groups them)
        std::array<int32, 2> band{};
        uint32               count = 0;
        std::array<float, 3> at{};
        float                spread = 0.0f;
        float                roam   = 0.0f; // optional: the home pull's scale -- the farther out, the more the errand favours prey back toward the point
        uint32               party  = 1;    // camp: members per party; seats = count x party

        // A camp is recorded, not seated, until parties exist (D5): watching
        // its members farm solo says nothing, they only struggle and die (user)
        auto seats() const -> uint32
        {
            return activity == "camp" ? 0 : count;
        }
        auto farms() const -> bool
        {
            return activity == "farm" || activity == "camp";
        }
        bool operator==(const SlotSpec&) const = default;
    };
    // Unknown keys are mistakes; a missing one takes the default (roam, party)
    constexpr glz::opts kSlotYaml{ .error_on_unknown_keys = true, .error_on_missing_keys = false };
    struct SlotFile
    {
        std::vector<SlotSpec> slots;
    };
    struct ZoneSlots
    {
        bool                                  loaded = false;
        std::vector<SlotSpec>                 specs;
        std::vector<std::vector<std::string>> occupants; // names, by slot
        std::filesystem::file_time_type       written{}; // the file as read, so an edit is noticed
    };
    std::unordered_map<uint16, ZoneSlots> zoneSlots;
    std::unordered_set<uint16>            filledAtBoot;
    std::unordered_map<uint16, uint32>    slotPoll;
    constexpr uint32                      kSlotPollTicks = 25; // ~10 s of zone ticks between looks at the file

    auto slotPath(CZone* PZone) -> std::filesystem::path
    {
        return std::filesystem::path("modules/cardian/world") / fmt::format("{}.yaml", PZone->getName());
    }

    // The file as a table; nullopt when it does not parse (logged), so a
    // typo mid-edit never empties a zone. A missing or blank file is an
    // empty table
    auto parseSlots(const std::filesystem::path& path) -> std::optional<std::vector<SlotSpec>>
    {
        if (!std::filesystem::exists(path))
        {
            return std::vector<SlotSpec>{};
        }
        std::ifstream     in(path);
        const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        if (std::ranges::all_of(text, [](const unsigned char c) { return std::isspace(c) != 0; }))
        {
            return std::vector<SlotSpec>{};
        }
        SlotFile file{};
        if (const auto error = glz::read_yaml<kSlotYaml>(file, text); error)
        {
            ShowErrorFmt("world: {}: {}", path.string(), glz::format_error(error, text));
            return std::nullopt;
        }
        for (const auto& spec : file.slots)
        {
            if (spec.activity != "farm" && spec.activity != "stand" && spec.activity != "camp")
            {
                ShowErrorFmt("world: {}: activity {} is not farm, stand or camp", path.string(), spec.activity);
                return std::nullopt;
            }
        }
        return std::move(file.slots);
    }

    auto loadSlots(CZone* PZone) -> ZoneSlots&
    {
        const auto zoneId = static_cast<uint16>(PZone->GetID());
        auto&      table  = zoneSlots[zoneId];
        if (table.loaded)
        {
            return table;
        }
        table.loaded = true;
        const auto      path = slotPath(PZone);
        std::error_code ec;
        table.written = std::filesystem::exists(path, ec) ? std::filesystem::last_write_time(path, ec) : std::filesystem::file_time_type{};
        table.specs   = parseSlots(path).value_or(std::vector<SlotSpec>{});
        table.occupants.assign(table.specs.size(), {});
        // Bodies already placed keep their slots across a re-read: a slot is
        // appended to the file, never reordered
        for (const auto& [charid, body] : bodies)
        {
            if (body.zone == zoneId && body.slot >= 0 && static_cast<size_t>(body.slot) < table.specs.size())
            {
                table.occupants[body.slot].push_back(body.name);
            }
        }
        if (!table.specs.empty())
        {
            ShowInfoFmt("world: {} slot(s) for {} from {}", table.specs.size(), PZone->getName(), path.string());
        }
        return table;
    }

    auto nameHash(const std::string& name) -> uint32
    {
        uint32 h = 2166136261u;
        for (const unsigned char c : name)
        {
            h ^= c;
            h *= 16777619u;
        }
        return h;
    }

    // A slot's occupants: in the world, level in band, not recruited, not
    // placed anywhere already; cohort rows first (the recruitment pool is
    // who you should meet), then by a hash of zone, slot and seed, so a
    // visit tends to show the same faces
    auto chooseOccupants(const uint16 zoneId, const uint32 slot, const SlotSpec& spec, const size_t wanted) -> std::vector<std::string>
    {
        struct Candidate
        {
            std::string name;
            bool        shared = false;
            uint32      order  = 0;
        };
        std::vector<Candidate> candidates;
        if (const auto rset = db::preparedStmt("SELECT name, cohort, seed FROM cardian_census WHERE anchor <> 'bank' AND recruited = 0 AND level BETWEEN ? AND ?",
                                               spec.band[0], spec.band[1]);
            rset)
        {
            while (rset->next())
            {
                auto name = rset->get<std::string>("name");
                if (charidByName.contains(name) || std::ranges::any_of(pending, [&](const Pending& p) { return p.name == name; }))
                {
                    continue;
                }
                const uint32 seed  = rset->get<uint32>("seed");
                const uint32 order = (seed * 2654435761u) ^ (static_cast<uint32>(zoneId) * 40503u + (slot + 1) * 2654435761u);
                candidates.push_back(Candidate{ .name = std::move(name), .shared = rset->get<uint32>("cohort") == 0, .order = order });
            }
        }
        std::ranges::sort(candidates, [](const Candidate& a, const Candidate& b)
        {
            return std::tie(a.shared, a.order, a.name) < std::tie(b.shared, b.order, b.name);
        });
        std::vector<std::string> out;
        for (auto& c : candidates)
        {
            if (out.size() >= wanted)
            {
                break;
            }
            out.push_back(std::move(c.name));
        }
        return out;
    }

    // Her spot in the slot's spread, from her name, so she stands in the
    // same place each visit
    auto slotPoint(const SlotSpec& spec, const std::string& name) -> position_t
    {
        const uint32 h      = nameHash(name);
        const float  angle  = 2.0f * std::numbers::pi_v<float> * static_cast<float>(h % 1000) / 1000.0f;
        const float  radius = spec.spread * std::sqrt(static_cast<float>((h / 1000) % 1000) / 1000.0f);
        position_t   point{};
        point.x        = spec.at[0] + radius * std::cos(angle);
        point.y        = spec.at[1];
        point.z        = spec.at[2] + radius * std::sin(angle);
        point.rotation = static_cast<uint8>(h % 256);
        return point;
    }

    // Presence queued for what a slot is short of
    auto queueSlot(CZone* PZone, const uint32 slot) -> uint32
    {
        const auto  zoneId = static_cast<uint16>(PZone->GetID());
        auto&       table  = zoneSlots[zoneId];
        const auto& spec   = table.specs[slot];
        const auto  have   = table.occupants[slot].size();
        if (have >= spec.seats())
        {
            return 0;
        }
        uint32 queued = 0;
        for (const auto& name : chooseOccupants(zoneId, slot, spec, spec.seats() - have))
        {
            pending.push_back(Pending{ .name = name, .zone = zoneId, .point = slotPoint(spec, name), .pinned = false, .farming = spec.farms(), .presence = true, .slot = static_cast<int32>(slot), .roam = spec.roam });
            ++queued;
        }
        return queued;
    }

    auto fillZone(CZone* PZone) -> uint32
    {
        auto&  table  = loadSlots(PZone);
        uint32 queued = 0;
        for (uint32 slot = 0; slot < table.specs.size(); ++slot)
        {
            queued += queueSlot(PZone, slot);
        }
        return queued;
    }

    // Presence for a queued occupant: minted if need be, recorded as a body
    // of the zone, her session row and position written; her body at once
    // if the zone is live, else when a player arrives (the rising edge)
    bool placePresence(const Pending& item, CZone* PZone)
    {
        auto row = readCensus(item.name);
        if (!row.has_value())
        {
            return false;
        }
        if (row->charid == 0)
        {
            if (const auto why = loginHelpers::validateCharacterName(item.name); why.has_value())
            {
                ShowWarningFmt("world: census name {} cannot be minted: {}", item.name, *why);
                return false;
            }
        }
        const uint32 charid = ensureMinted(item.name, *row);
        if (charid == 0 || bodies.contains(charid))
        {
            return false;
        }
        const auto zoneId = static_cast<uint16>(PZone->GetID());
        Body&      body   = bodies[charid];
        body.charid       = charid;
        body.name         = item.name;
        body.zone         = zoneId;
        body.point        = item.point;
        body.pinned       = false;
        body.farming      = item.farming;
        body.slot         = item.slot;
        body.roam         = item.roam;
        body.present      = false;
        body.censusLevel  = row->level;
        body.seed         = row->seed;
        charidByName[item.name] = charid;
        if (auto& table = zoneSlots[zoneId]; item.slot >= 0 && static_cast<size_t>(item.slot) < table.occupants.size())
        {
            table.occupants[item.slot].push_back(item.name);
        }
        pawn::markPresent(charid, zoneId, item.point, row->job, row->level);
        ShowInfoFmt("world: {} ({} {}) holds slot {} in {}", item.name, magic_enum::enum_name(static_cast<xi::Job>(row->job)), row->level, item.slot, PZone->getName());
        if (wasLive[zoneId])
        {
            fadeIn(body);
        }
        return true;
    }

    // A seat's occupant returns to the pool: her body fades, her presence
    // goes, the seat is free to fill again
    void unseat(Body& body, const std::string_view why)
    {
        if (body.present)
        {
            fadeOut(body, why);
        }
        pawn::markAbsent(body.charid);
        if (auto tit = zoneSlots.find(body.zone); tit != zoneSlots.end() && body.slot >= 0 && static_cast<size_t>(body.slot) < tit->second.occupants.size())
        {
            std::erase(tit->second.occupants[body.slot], body.name);
        }
        charidByName.erase(body.name);
        bodies.erase(body.charid);
    }

    // The census moves (a recut, the player levelled): a seated body whose
    // level has left her slot's band gives the seat up, and the seat refills
    auto reseatOutgrown(CZone* PZone) -> uint32
    {
        const auto zoneId = static_cast<uint16>(PZone->GetID());
        const auto tit    = zoneSlots.find(zoneId);
        if (tit == zoneSlots.end() || tit->second.specs.empty())
        {
            return 0;
        }
        std::vector<uint32> outgrown;
        for (const auto& [charid, body] : bodies)
        {
            if (body.zone != zoneId || body.slot < 0 || static_cast<size_t>(body.slot) >= tit->second.specs.size())
            {
                continue;
            }
            const auto rset = db::preparedStmt("SELECT level FROM cardian_census WHERE charid = ?", charid);
            if (!rset || !rset->next())
            {
                continue;
            }
            const auto  level = rset->get<uint8>("level");
            const auto& band  = tit->second.specs[body.slot].band;
            if (level < band[0] || level > band[1])
            {
                ShowInfoFmt("world: {} is level {} now, outside slot {}'s band {}-{}; gives the seat up", body.name, level, body.slot, band[0], band[1]);
                outgrown.push_back(charid);
            }
        }
        std::unordered_set<uint32> seats;
        for (const auto charid : outgrown)
        {
            if (const auto it = bodies.find(charid); it != bodies.end())
            {
                seats.insert(static_cast<uint32>(it->second.slot));
                unseat(it->second, "she has outgrown the seat");
            }
        }
        uint32 queued = 0;
        for (const auto slot : seats)
        {
            queued += queueSlot(PZone, slot);
        }
        return queued;
    }

    // Everything the zone holds returns to the pool: bodies fade, presences
    // go, the table is dropped for a fresh read; the ring's pinned bodies stay
    auto clearZone(CZone* PZone) -> uint32
    {
        const auto zoneId  = static_cast<uint16>(PZone->GetID());
        uint32     cleared = 0;
        for (auto it = bodies.begin(); it != bodies.end();)
        {
            auto& body = it->second;
            if (body.zone != zoneId || body.pinned)
            {
                ++it;
                continue;
            }
            if (body.present)
            {
                fadeOut(body, "the zone refills");
            }
            pawn::markAbsent(body.charid);
            charidByName.erase(body.name);
            it = bodies.erase(it);
            ++cleared;
        }
        std::erase_if(pending, [&](const Pending& p) { return p.zone == zoneId; });
        zoneSlots.erase(zoneId);
        return cleared;
    }
} // namespace

namespace pawn::world
{
    bool isEnabled()
    {
        return pawn::isEnabled() && settings::get<bool>("pawn.WORLD_ENABLE");
    }

    auto isBody(const uint32 charid) -> bool
    {
        const auto it = bodies.find(charid);
        return it != bodies.end() && it->second.present;
    }

    auto tickDebug() -> bool
    {
        static const bool on = settings::get<bool>("pawn.WORLD_TICK_DEBUG");
        return on;
    }

    bool spawnByName(const std::string& rawName, CZone* PZone, const position_t& point, const bool pinned)
    {
        if (!isEnabled() || PZone == nullptr)
        {
            return false;
        }
        const auto name = properName(rawName);
        auto       row  = readCensus(name);
        if (!row.has_value())
        {
            ShowWarningFmt("world: {} is not in the census", name);
            return false;
        }
        const uint32 charid = ensureMinted(name, *row);
        if (charid == 0)
        {
            return false;
        }
        if (const auto it = bodies.find(charid); it != bodies.end() && it->second.present)
        {
            return false;
        }

        Body& body         = bodies[charid];
        body.charid        = charid;
        body.name          = name;
        body.zone          = static_cast<uint16>(PZone->GetID());
        body.point         = point;
        body.pinned        = pinned;
        body.farming       = false;
        body.downSince.reset();
        charidByName[name] = charid;
        if (!fadeIn(body))
        {
            bodies.erase(charid);
            charidByName.erase(name);
            return false;
        }
        return true;
    }

    auto despawnByName(const std::string& rawName) -> uint32
    {
        uint32 faded = 0;
        if (rawName == "all")
        {
            for (auto& [charid, body] : bodies)
            {
                if (body.present)
                {
                    fadeOut(body, "told to");
                    ++faded;
                }
            }
            bodies.clear();
            charidByName.clear();
            return faded;
        }

        const auto name = properName(rawName);
        if (const auto it = charidByName.find(name); it != charidByName.end())
        {
            if (const auto bit = bodies.find(it->second); bit != bodies.end())
            {
                if (bit->second.present)
                {
                    fadeOut(bit->second, "told to");
                    ++faded;
                }
                bodies.erase(bit);
            }
            charidByName.erase(it);
        }
        return faded;
    }

    auto ring(CZone* PZone, const position_t& centre, const uint32 count, const bool farming) -> uint32
    {
        if (!isEnabled() || PZone == nullptr || count == 0)
        {
            return 0;
        }

        // Every name up front: standing one runs queries of its own. A name
        // not yet minted is put through the lobby's own checks here, so one
        // the retail filter refuses costs a log line, not a hole in the ring
        // (the census tool runs the same filter over its bank; this is the
        // belt to its braces). Names in the bank are not in the world.
        std::vector<std::string> names;
        if (const auto rset = db::preparedStmt("SELECT name, charid FROM cardian_census WHERE anchor <> 'bank' ORDER BY seed"); rset)
        {
            while (rset->next())
            {
                const auto name = rset->get<std::string>("name");
                if (rset->get<uint32>("charid") == 0)
                {
                    if (const auto why = loginHelpers::validateCharacterName(name); why.has_value())
                    {
                        ShowWarningFmt("world: census name {} cannot be minted: {}", name, *why);
                        continue;
                    }
                }
                names.push_back(name);
            }
        }

        const float radius = 3.0f + 0.35f * static_cast<float>(count);
        uint32      queued = 0;
        for (const auto& name : names)
        {
            if (queued >= count)
            {
                break;
            }
            if (const auto it = charidByName.find(name); it != charidByName.end())
            {
                if (const auto bit = bodies.find(it->second); bit != bodies.end() && bit->second.present)
                {
                    continue;
                }
            }
            if (std::ranges::any_of(pending, [&](const Pending& p) { return p.name == name; }))
            {
                continue;
            }
            const float angle = 2.0f * std::numbers::pi_v<float> * static_cast<float>(queued) / static_cast<float>(count);
            position_t  point = centre;
            point.x           = centre.x + radius * std::cos(angle);
            point.z           = centre.z + radius * std::sin(angle);
            pending.push_back(Pending{ .name = name, .zone = static_cast<uint16>(PZone->GetID()), .point = point, .pinned = true, .farming = farming });
            ++queued;
        }
        return queued;
    }

    // Every present body the name means: one, or all of them
    template <typename Fn>
    auto forNamed(const std::string& rawName, Fn&& fn) -> uint32
    {
        uint32 n = 0;
        if (rawName == "all")
        {
            for (auto& [charid, body] : bodies)
            {
                if (body.present && fn(body))
                {
                    ++n;
                }
            }
            return n;
        }
        if (const auto it = charidByName.find(properName(rawName)); it != charidByName.end())
        {
            if (const auto bit = bodies.find(it->second); bit != bodies.end() && bit->second.present && fn(bit->second))
            {
                ++n;
            }
        }
        return n;
    }

    auto farm(const std::string& rawName, const bool on) -> uint32
    {
        return forNamed(rawName, [&](Body& body)
        {
            body.farming = on;
            ShowInfoFmt("world: {} farms {}", body.name, on ? "on" : "off");
            return true;
        });
    }

    auto isFarming(const uint32 charid) -> bool
    {
        const auto it = bodies.find(charid);
        return it != bodies.end() && it->second.farming;
    }

    auto homeOf(const uint32 charid) -> std::optional<std::pair<position_t, float>>
    {
        const auto it = bodies.find(charid);
        if (it == bodies.end() || it->second.roam <= 0.0f || it->second.slot < 0)
        {
            return std::nullopt;
        }
        // The slot's point, not her seat in its spread: home is the camp's
        const auto& table = zoneSlots[it->second.zone];
        if (static_cast<size_t>(it->second.slot) >= table.specs.size())
        {
            return std::nullopt;
        }
        const auto& at = table.specs[it->second.slot].at;
        position_t  anchor{};
        anchor.x = at[0];
        anchor.y = at[1];
        anchor.z = at[2];
        return std::make_pair(anchor, it->second.roam);
    }

    auto slots(CZone* PZone) -> std::vector<std::string>
    {
        std::vector<std::string> lines;
        if (!isEnabled() || PZone == nullptr)
        {
            return lines;
        }
        const auto& table = loadSlots(PZone);
        if (table.specs.empty())
        {
            lines.push_back(fmt::format("no slots for {} ({})", PZone->getName(), slotPath(PZone).string()));
            return lines;
        }
        for (size_t i = 0; i < table.specs.size(); ++i)
        {
            const auto& spec = table.specs[i];
            std::string who;
            for (const auto& name : table.occupants[i])
            {
                const auto it      = charidByName.find(name);
                const auto bit     = it != charidByName.end() ? bodies.find(it->second) : bodies.end();
                const bool present = bit != bodies.end() && bit->second.present;
                who += fmt::format("{}{}{}", who.empty() ? "" : ", ", name, present ? "" : "~");
            }
            lines.push_back(fmt::format("#{} {}{} L{}-{} x{} at ({:.0f}, {:.0f}, {:.0f}) spread {:.0f}{}: {}", i, spec.activity,
                                        spec.activity == "camp" ? fmt::format(" of {}", spec.party) : "", spec.band[0], spec.band[1],
                                        spec.count, spec.at[0], spec.at[1], spec.at[2], spec.spread, spec.roam > 0 ? fmt::format(" roam {:.0f}", spec.roam) : "",
                                        who.empty() ? (spec.activity == "camp" ? "unseated until parties (D5)" : "nobody yet") : who));
        }
        return lines;
    }

    auto fill(CZone* PZone) -> uint32
    {
        if (!isEnabled() || PZone == nullptr)
        {
            return 0;
        }
        clearZone(PZone);
        return fillZone(PZone);
    }

    auto addSlot(CZone* PZone, const std::string& activity, const uint8 low, const uint8 high, const uint8 count, const float spread, const position_t& at) -> bool
    {
        if (!isEnabled() || PZone == nullptr || (activity != "farm" && activity != "stand" && activity != "camp") || low == 0 || high < low || count == 0)
        {
            return false;
        }
        const auto path = slotPath(PZone);
        std::filesystem::create_directories(path.parent_path());
        const bool fresh = !std::filesystem::exists(path) || std::filesystem::file_size(path) == 0;
        std::ofstream out(path, std::ios::app);
        if (!out)
        {
            ShowErrorFmt("world: cannot write {}", path.string());
            return false;
        }
        if (fresh)
        {
            out << "# " << PZone->getName() << ": the slot table (RESEARCH.md §11.5) -- what happens where, never who.\n"
                << "# Written by !pawnworld slot; edit by hand and !pawnworld fill to re-read.\n"
                << "slots:\n";
        }
        out << fmt::format("  - activity: {}\n    band: [{}, {}]\n    count: {}\n    at: [{:.1f}, {:.1f}, {:.1f}]\n    spread: {:.0f}\n",
                           activity, low, high, count, at.x, at.y, at.z, spread);
        out.close();
        // The re-read keeps placed bodies on their slots, so only the new one fills
        zoneSlots.erase(static_cast<uint16>(PZone->GetID()));
        fillZone(PZone);
        return true;
    }

    void onZoneTick(CZone* PZone)
    {
        if (!isEnabled() || PZone == nullptr)
        {
            return;
        }
        const auto zoneId = static_cast<uint16>(PZone->GetID());

        // The slot tables: on a zone's first tick its occupants are chosen
        // and given presence, a couple a tick; their bodies come with a
        // player. An edited file is noticed within a few seconds and the
        // zone refills from it, so a table is authored with the zone live
        if (settings::get<bool>("pawn.WORLD_SLOTS"))
        {
            if (!filledAtBoot.contains(zoneId))
            {
                filledAtBoot.insert(zoneId);
                if (const auto queued = fillZone(PZone); queued > 0)
                {
                    ShowInfoFmt("world: {} fills {} seat(s)", PZone->getName(), queued);
                }
            }
            else if (++slotPoll[zoneId] % kSlotPollTicks == 0)
            {
                reseatOutgrown(PZone);
                if (const auto it = zoneSlots.find(zoneId); it != zoneSlots.end())
                {
                    const auto path = slotPath(PZone);
                    std::error_code ec;
                    const auto written = std::filesystem::exists(path, ec) ? std::filesystem::last_write_time(path, ec) : std::filesystem::file_time_type{};
                    if (!ec && written != it->second.written)
                    {
                        const auto parsed = parseSlots(path);
                        if (!parsed.has_value() || *parsed == it->second.specs)
                        {
                            it->second.written = written; // a bad edit, or a comment: the zone stands as it is
                        }
                        else
                        {
                            const auto cleared = clearZone(PZone);
                            const auto queued  = fillZone(PZone);
                            ShowInfoFmt("world: {}'s slot table changed: {} returned to the pool, {} seat(s) to fill", PZone->getName(), cleared, queued);
                        }
                    }
                }
            }
        }

        // The debug ring: once, in its own zone, from the first tick of any
        if (!debugRingPlaced)
        {
            debugRingPlaced = true;
            if (const auto count = settings::get<uint32>("pawn.WORLD_DEBUG_RING"); count > 0)
            {
                if (auto* PRingZone = zoneutils::GetZone(static_cast<xi::ZoneId>(settings::get<uint16>("pawn.WORLD_DEBUG_ZONE"))); PRingZone != nullptr)
                {
                    position_t centre{};
                    centre.x = settings::get<float>("pawn.WORLD_DEBUG_X");
                    centre.y = settings::get<float>("pawn.WORLD_DEBUG_Y");
                    centre.z = settings::get<float>("pawn.WORLD_DEBUG_Z");
                    const bool farming = settings::get<bool>("pawn.WORLD_DEBUG_FARM");
                    const auto queued  = ring(PRingZone, centre, count, farming);
                    ShowInfoFmt("world: debug ring of {} in {} ({} queued{})", count, PRingZone->getName(), queued, farming ? ", farming" : "");
                }
            }
        }

        // The queue: a few of this zone's pending bodies stand each tick
        uint32 stoodThisTick = 0;
        for (auto it = pending.begin(); it != pending.end() && stoodThisTick < kStandPerTick;)
        {
            if (it->zone != static_cast<uint16>(PZone->GetID()))
            {
                ++it;
                continue;
            }
            const Pending item = *it;
            it                 = pending.erase(it);
            if (item.presence)
            {
                if (placePresence(item, PZone))
                {
                    ++stoodThisTick;
                }
                continue;
            }
            if (spawnByName(item.name, PZone, item.point, item.pinned))
            {
                ++stoodThisTick;
                if (item.farming)
                {
                    farm(item.name, true);
                }
            }
        }

        const uint32 real           = realPlayersIn(PZone);
        realPlayersLastTick[zoneId] = real;

        const auto now  = std::chrono::steady_clock::now();

        // KO'd: she lies there WORLD_KO_FADE seconds, then fades; the next
        // fade-in stands her whole (spawnAt)
        for (auto& [charid, body] : bodies)
        {
            if (body.zone != zoneId || !body.present)
            {
                continue;
            }
            if (++body.sweepTick % 5 == 0)
            {
                sweepBag(body);
            }
            const auto* PPawn = pawn::findPawn(charid);
            if (PPawn == nullptr || !PPawn->isDead())
            {
                body.downSince.reset();
                continue;
            }
            if (!body.downSince.has_value())
            {
                body.downSince = now;
                ShowInfoFmt("world: {} is KO'd; fades in {} s", body.name, settings::get<uint32>("pawn.WORLD_KO_FADE"));
            }
            else if (now - *body.downSince >= std::chrono::seconds(settings::get<uint32>("pawn.WORLD_KO_FADE")))
            {
                fadeOut(body, "KO'd");
            }
        }
        const bool live = isLive(PZone, real);
        auto&      seen = lastLive.try_emplace(zoneId, now).first->second;
        if (live)
        {
            seen = now;
        }
        const bool before = wasLive[zoneId];
        wasLive[zoneId]   = live;

        if (live && !before)
        {
            for (auto& [charid, body] : bodies)
            {
                if (body.zone == zoneId && !body.present)
                {
                    fadeIn(body);
                }
            }
        }
        else if (!live && now - seen >= std::chrono::seconds(settings::get<uint32>("pawn.WORLD_FADE_DELAY")))
        {
            for (auto& [charid, body] : bodies)
            {
                if (body.zone == zoneId && body.present && !body.pinned)
                {
                    fadeOut(body, "the zone emptied");
                }
            }
        }

        reportLoad(now);
    }

    void reportLoad(const std::chrono::steady_clock::time_point now)
    {
        const auto every = settings::get<uint32>("pawn.WORLD_LOAD_REPORT");
        if (every == 0)
        {
            return;
        }
        if (!load.started)
        {
            load.started = true;
            load.since   = now;
            getrusage(RUSAGE_SELF, &load.usage);
            return;
        }
        if (now - load.since < std::chrono::seconds(every))
        {
            return;
        }

        rusage usage{};
        getrusage(RUSAGE_SELF, &usage);
        const double wall = std::chrono::duration<double>(now - load.since).count();
        const double cpu  = wall > 0 ? 100.0 * (cpuSeconds(usage) - cpuSeconds(load.usage)) / wall : 0.0;

        uint32                     present = 0;
        std::unordered_set<uint16> zonesWithBodies;
        for (const auto& [charid, body] : bodies)
        {
            if (body.present)
            {
                ++present;
                zonesWithBodies.insert(body.zone);
            }
        }
        uint32 liveZones = 0;
        for (const auto& [zoneId, isLiveNow] : wasLive)
        {
            liveZones += isLiveNow ? 1 : 0;
        }
        const auto brainAvg  = load.brainTicks > 0 ? microseconds(load.brainTotal / load.brainTicks) : 0LL;
        const auto moduleAvg = load.moduleTicks > 0 ? microseconds(load.moduleTotal / load.moduleTicks) : 0LL;
        ShowInfoFmt("world: load: {} bodies standing in {} zones ({} live); a body's tick {} us, the module's {} us; map {:.1f}% of one core, {} MB",
                    present, zonesWithBodies.size(), liveZones, brainAvg, moduleAvg, cpu, rssMegabytes());

        load.since       = now;
        load.usage       = usage;
        load.brainTicks  = 0;
        load.brainTotal  = {};
        load.moduleTicks = 0;
        load.moduleTotal = {};
    }

    void noteModuleTick(CZone* PZone, const std::chrono::nanoseconds elapsed, const uint32 pawnsInZone)
    {
        if (pawnsInZone > 0)
        {
            ++load.moduleTicks;
            load.moduleTotal += elapsed;
        }
        if (!tickDebug() || PZone == nullptr || pawnsInZone == 0)
        {
            return;
        }
        const auto zoneId = static_cast<uint16>(PZone->GetID());
        auto&      s      = samples[zoneId];
        ++s.ticks;
        s.moduleTotal += elapsed;
        s.moduleWorst = std::max(s.moduleWorst, elapsed);
        if (s.ticks >= kReportEvery)
        {
            const auto brainAvg = s.brainTicks > 0 ? microseconds(s.brainTotal / s.brainTicks) : 0LL;
            ShowInfoFmt("world: {} over {} ticks: {} pawns; brain {} us avg per pawn tick ({} us worst, {} ticks); module {} us avg ({} us worst); {} real players",
                        PZone->getName(), s.ticks, pawnsInZone, brainAvg, microseconds(s.brainWorst), s.brainTicks,
                        microseconds(s.moduleTotal / s.ticks), microseconds(s.moduleWorst), realPlayersLastTick[zoneId]);
            s = Sample{};
        }
    }

    void noteBrainTick(const uint16 zoneId, const std::chrono::nanoseconds elapsed)
    {
        ++load.brainTicks;
        load.brainTotal += elapsed;
        if (!tickDebug())
        {
            return;
        }
        auto& s = samples[zoneId];
        ++s.brainTicks;
        s.brainTotal += elapsed;
        s.brainWorst = std::max(s.brainWorst, elapsed);
    }
} // namespace pawn::world
