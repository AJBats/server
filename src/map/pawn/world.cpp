/*
===========================================================================

  Cardian: the world's adventurers (ROADMAP D, RESEARCH.md §11). See world.h.

===========================================================================
*/

#include "world.h"
#include "pawn_items.h"

#include "pawn.h"

#include "common/database.h"
#include "common/earth_time.h"
#include "common/logging.h"
#include "common/settings.h"
#include "common/vana_time.h"
#include "common/xirand.h"

#include "data/datasets/zones/settings/dataset.h"
#include "data/loader.h"
#include "data/yaml/read.h"
#include "data/enums/status_effect.h"
#include "entities/char_entity.h"
#include "item_container.h"
#include "items/item.h"
#include "items/transactions/item_claim.h"
#include "login/login_helpers.h"
#include "utils/charutils.h"
#include "utils/itemutils.h"
#include "map_session.h"
#include "navmesh/navmesh.h"
#include "party.h"
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
#include <map>
#include <string_view>
#include <tuple>
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

        // A camp seat: the slot is a party (ROADMAP D5); party is its size
        uint32 party = 1;

        // A town seat (ROADMAP D4) is a turnstile: she appears at an exit
        // point and walks to her seat, faces a point there, holds the seat
        // her dwell, then walks to an exit and fades, and the seat refills
        // with another face. The controller walks her (TownTick) and
        // reports the arrivals; the zone tick keeps the clock and unseats
        std::optional<position_t>                            cameFrom;
        std::optional<position_t>                            face;
        std::optional<position_t>                            exitAt;
        std::array<uint32, 2>                                dwell{};
        std::optional<std::chrono::steady_clock::time_point> leaveAt;
        std::string                                          pose;
        int32                                                seat    = -1; // her laid-out seat in a clustered slot
        std::vector<position_t>                              via;          // points walked in order on the way in, in reverse on the way out
        size_t                                               viaNext = 0;  // the next via point on the way in
        std::vector<position_t>                              wayOut;       // set when she leaves: the via points reversed, then the exit
        size_t                                               outNext = 0;
        bool                                                 atSeat  = false;
        bool                                                 leaving = false;
        bool                                                 gone    = false; // at the exit, or the walk given up

        // KO'd and faded: when she walks back to her seat (WORLD_KO_RETURN)
        std::optional<std::chrono::steady_clock::time_point> returnAt;

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
        uint32      party    = 1;

        // a town seat: where she walks in from, what she faces, her dwell, her pose,
        // and which laid-out seat of a clustered slot is hers (-1 = none)
        std::optional<position_t> cameFrom;
        std::optional<position_t> face;
        std::array<uint32, 2>     dwell{};
        std::string               pose;
        int32                     seat = -1;
        std::vector<position_t>   via;
    };
    std::vector<Pending> pending;
    constexpr uint32     kStandPerTick = 2;

    auto laneOf(const std::string& name) -> float;

    auto isHealer(const uint8 job) -> bool
    {
        return job == static_cast<uint8>(xi::Job::WHM) || job == static_cast<uint8>(xi::Job::RDM);
    }

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

    // -- Brains (ROADMAP D5): modules/cardian/world/brains.yaml ------------------------
    // Rows in the gambit grammar: common, then her job's, then her role's.
    struct BrainFile
    {
        std::vector<std::string>                        common;
        std::map<std::string, std::vector<std::string>> roles;
        std::map<std::string, std::vector<std::string>> jobs;
    };
    constexpr glz::opts kBrainYaml{ .error_on_unknown_keys = true, .error_on_missing_keys = false };
    const std::filesystem::path     kBrainPath = std::filesystem::path("modules/cardian/world") / "brains.yaml";
    BrainFile                       brainFile;
    std::filesystem::file_time_type brainWritten{};
    bool                            brainLoaded = false;

    // The readable row: "<who>: <condition> -> <action> [every <n>s]", compiled
    // to the numeric grammar the gambit engine and the saved sets speak.
    //   who        self | party | mob
    //   condition  always | hp < n | hp >= n | mp < n | mp >= n | tp < n | tp >= n |
    //              has <status> | lacks <status> | top enmity | not top enmity
    //   action     avoid aggro | rest with leader | rest | rest in battle | home point
    //              with leader | boost before weapon skills | formation <lead|flank left|flank right|
    //              rear left|rear right|behind> | cast best <spell> (the best of its
    //              family) | cast <spell> | cast random damage | ability <name> |
    //              best weapon skill | random weapon skill
    // Names are the game's own (spell_list, abilities, the status enum), spaces
    // for underscores. A row that will not compile is logged and left out.
    auto lower(std::string text) -> std::string
    {
        std::ranges::transform(text, text.begin(), [](const unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return text;
    }

    auto trim(const std::string_view text) -> std::string
    {
        const auto b = text.find_first_not_of(' ');
        const auto e = text.find_last_not_of(' ');
        return b == std::string_view::npos ? std::string() : std::string(text.substr(b, e - b + 1));
    }

    auto spellByName(const std::string& name) -> std::optional<std::pair<uint16, uint16>> // id, family
    {
        std::string key = lower(name);
        std::ranges::replace(key, ' ', '_');
        if (const auto rset = db::preparedStmt("SELECT spellid, family FROM spell_list WHERE name = ?", key); rset && rset->next())
        {
            return std::make_pair(rset->get<uint16>("spellid"), rset->get<uint16>("family"));
        }
        return std::nullopt;
    }

    auto abilityByName(const std::string& name) -> std::optional<uint16>
    {
        std::string key = lower(name);
        std::ranges::replace(key, ' ', '_');
        if (const auto rset = db::preparedStmt("SELECT abilityId FROM abilities WHERE name = ?", key); rset && rset->next())
        {
            return rset->get<uint16>("abilityId");
        }
        return std::nullopt;
    }

    auto statusByName(const std::string& name) -> std::optional<uint16>
    {
        // the table's names are snake_case ("sleep_i", "paralysis"); spaces and
        // underscores are dropped on both sides so "sleep ii" finds "sleep_ii"
        const auto squash = [](std::string text)
        {
            text = lower(std::move(text));
            std::erase_if(text, [](const unsigned char c) { return c == ' ' || c == '_'; });
            return text;
        };
        const std::string key = squash(name);
        for (const auto& [entryName, value] : xi::data::EnumTraits<xi::StatusEffect>::kEntries)
        {
            if (squash(std::string(entryName)) == key)
            {
                return static_cast<uint16>(value);
            }
        }
        if (key == "sleep") // the plain word means the first sleep
        {
            return statusByName("sleep i");
        }
        return std::nullopt;
    }

    auto compileRow(const std::string& text) -> std::optional<std::string>
    {
        const auto arrow = text.find("->");
        const auto colon = text.find(':');
        if (arrow == std::string::npos || colon == std::string::npos || colon > arrow)
        {
            ShowErrorFmt("world: brains: '{}' is not '<who>: <condition> -> <action>'", text);
            return std::nullopt;
        }
        const auto who  = lower(trim(text.substr(0, colon)));
        const auto cond = lower(trim(text.substr(colon + 1, arrow - colon - 1)));
        auto       act  = lower(trim(text.substr(arrow + 2)));
        uint32     retry = 0;
        if (const auto every = act.rfind(" every "); every != std::string::npos && act.ends_with('s'))
        {
            retry = static_cast<uint32>(std::atoi(act.substr(every + 7).c_str()));
            act   = trim(act.substr(0, every));
        }
        static const std::unordered_map<std::string, int> whoIds{ { "self", 0 }, { "party", 1 }, { "mob", 2 } };
        const auto whoIt = whoIds.find(who);
        if (whoIt == whoIds.end())
        {
            ShowErrorFmt("world: brains: '{}': who is self, party or mob", text);
            return std::nullopt;
        }

        // the condition
        std::string condSpec;
        const auto  number = [&](const std::string& s) -> std::optional<int>
        {
            const auto t = trim(s);
            return !t.empty() && std::ranges::all_of(t, [](const unsigned char c) { return std::isdigit(c) != 0; }) ? std::optional(std::atoi(t.c_str())) : std::nullopt;
        };
        static const std::vector<std::pair<std::string, int>> compare{ { "hp <", 1 }, { "hp >=", 2 }, { "mp <", 3 }, { "mp >=", 4 }, { "tp <", 5 }, { "tp >=", 6 } };
        if (cond == "always")
        {
            condSpec = "0:0";
        }
        else if (cond == "top enmity")
        {
            condSpec = "12:0";
        }
        else if (cond == "not top enmity")
        {
            condSpec = "13:0";
        }
        else if (cond.starts_with("has ") || cond.starts_with("lacks "))
        {
            const bool has  = cond.starts_with("has ");
            const auto name = trim(cond.substr(has ? 4 : 6));
            const auto id   = statusByName(name);
            if (!id.has_value())
            {
                ShowErrorFmt("world: brains: '{}': no status called '{}'", text, name);
                return std::nullopt;
            }
            condSpec = fmt::format("{}:{}", has ? 9 : 10, *id);
        }
        else
        {
            for (const auto& [prefix, id] : compare)
            {
                if (cond.starts_with(prefix))
                {
                    if (const auto n = number(cond.substr(prefix.size())); n.has_value())
                    {
                        condSpec = fmt::format("{}:{}", id, *n);
                    }
                    break;
                }
            }
            if (condSpec.empty())
            {
                ShowErrorFmt("world: brains: '{}': cannot read the condition '{}'", text, cond);
                return std::nullopt;
            }
        }

        // the action
        std::string actSpec;
        static const std::unordered_map<std::string, std::string> switches{
            { "avoid aggro", "100:1:1" }, { "rest with leader", "100:6:1" }, { "home point with leader", "100:7:1" },
            { "rest", "100:8:1" }, { "boost before weapon skills", "100:9:1" }, { "rest in battle", "100:10:1" }
        };
        static const std::unordered_map<std::string, int> seats{
            { "lead", 1 }, { "flank left", 2 }, { "flank right", 3 }, { "rear left", 4 }, { "rear right", 5 }, { "behind", 6 }
        };
        if (const auto it = switches.find(act); it != switches.end())
        {
            actSpec = it->second;
        }
        else if (act.starts_with("formation "))
        {
            const auto seat = seats.find(trim(act.substr(10)));
            if (seat == seats.end())
            {
                ShowErrorFmt("world: brains: '{}': no seat called '{}'", text, trim(act.substr(10)));
                return std::nullopt;
            }
            actSpec = fmt::format("100:4:{}", seat->second);
        }
        else if (act == "cast random damage")
        {
            actSpec = "2:3:0";
        }
        else if (act.starts_with("cast best "))
        {
            const auto spell = spellByName(trim(act.substr(10)));
            if (!spell.has_value())
            {
                ShowErrorFmt("world: brains: '{}': no spell called '{}'", text, trim(act.substr(10)));
                return std::nullopt;
            }
            actSpec = fmt::format("2:0:{}", spell->second);
        }
        else if (act.starts_with("cast "))
        {
            const auto spell = spellByName(trim(act.substr(5)));
            if (!spell.has_value())
            {
                ShowErrorFmt("world: brains: '{}': no spell called '{}'", text, trim(act.substr(5)));
                return std::nullopt;
            }
            actSpec = fmt::format("2:2:{}", spell->first);
        }
        else if (act.starts_with("ability "))
        {
            const auto ability = abilityByName(trim(act.substr(8)));
            if (!ability.has_value())
            {
                ShowErrorFmt("world: brains: '{}': no ability called '{}'", text, trim(act.substr(8)));
                return std::nullopt;
            }
            actSpec = fmt::format("3:2:{}", *ability);
        }
        else if (act == "best weapon skill")
        {
            actSpec = "4:0:0";
        }
        else if (act == "random weapon skill")
        {
            actSpec = "4:3:0";
        }
        else
        {
            ShowErrorFmt("world: brains: '{}': cannot read the action '{}'", text, act);
            return std::nullopt;
        }
        return fmt::format("{}|{}|{}|{}", whoIt->second, condSpec, actSpec, retry);
    }

    auto brains() -> const BrainFile&
    {
        std::error_code ec;
        const auto      written = std::filesystem::exists(kBrainPath, ec) ? std::filesystem::last_write_time(kBrainPath, ec) : std::filesystem::file_time_type{};
        if (brainLoaded && written == brainWritten)
        {
            return brainFile;
        }
        brainLoaded  = true;
        brainWritten = written;
        if (!std::filesystem::exists(kBrainPath))
        {
            ShowWarningFmt("world: {} missing; world bodies run the bare defaults", kBrainPath.string());
            brainFile = {};
            return brainFile;
        }
        std::ifstream     in(kBrainPath);
        const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        BrainFile         parsed{};
        if (const auto error = glz::read_yaml<kBrainYaml>(parsed, text); error)
        {
            ShowErrorFmt("world: {}: {} (the last good brains stand)", kBrainPath.string(), glz::format_error(error, text));
            return brainFile;
        }
        // Compile every row once; a row that will not compile is left out
        const auto compile = [](std::vector<std::string>& rows)
        {
            std::vector<std::string> out;
            for (const auto& text : rows)
            {
                if (const auto spec = compileRow(text); spec.has_value())
                {
                    out.push_back(*spec);
                }
            }
            rows = std::move(out);
        };
        compile(parsed.common);
        for (auto& [role, rows] : parsed.roles)
        {
            compile(rows);
        }
        for (auto& [job, rows] : parsed.jobs)
        {
            compile(rows);
        }
        brainFile = std::move(parsed);
        ShowInfoFmt("world: brains read from {} ({} common, {} roles, {} jobs)", kBrainPath.string(), brainFile.common.size(), brainFile.roles.size(), brainFile.jobs.size());
        return brainFile;
    }

    // Her role: the tank is the highest Warrior of her party (ties by name),
    // any other Warrior and the fighters are melee, the casters mages
    auto isMage(const xi::Job job) -> bool
    {
        return job == xi::Job::WHM || job == xi::Job::BLM || job == xi::Job::RDM || job == xi::Job::SMN || job == xi::Job::BRD || job == xi::Job::SCH;
    }

    auto roleOf(const CCharEntity* PPawn) -> std::string
    {
        if (isMage(PPawn->GetMJob()))
        {
            return "mage";
        }
        if (PPawn->GetMJob() == xi::Job::WAR || PPawn->GetMJob() == xi::Job::PLD)
        {
            if (PPawn->PParty == nullptr)
            {
                return "tank";
            }
            for (const auto* PMember : PPawn->PParty->members)
            {
                const auto* PChar = dynamic_cast<const CCharEntity*>(PMember);
                if (PChar == nullptr || PChar == PPawn || PChar->loc.zone != PPawn->loc.zone || (PChar->GetMJob() != xi::Job::WAR && PChar->GetMJob() != xi::Job::PLD))
                {
                    continue;
                }
                if (std::make_tuple(PChar->GetMLevel(), PChar->name) > std::make_tuple(PPawn->GetMLevel(), PPawn->name))
                {
                    return "melee"; // a higher Warrior holds the tank's place
                }
            }
            return "tank";
        }
        return "melee";
    }

    // -- Camps (ROADMAP D5): a camp slot's occupants are a party ----------------------
    auto campMates(const Body& body) -> std::vector<Body*>
    {
        std::vector<Body*> out;
        if (body.party <= 1 || body.slot < 0)
        {
            return out;
        }
        for (auto& [charid, other] : bodies)
        {
            if (other.zone == body.zone && other.slot == body.slot && other.party > 1)
            {
                out.push_back(&other);
            }
        }
        return out;
    }

    // The camp's leader among those standing: the tank (the highest Warrior)
    // first, then the highest fighter, then the highest of all; ties by
    // name. Nobody when nobody stands
    auto campLeader(const Body& body) -> Body*
    {
        Body* best    = nullptr;
        auto  keyOf   = [](const Body& b) -> std::tuple<int, int, std::string>
        {
            const auto* PPawn = pawn::findPawn(b.charid);
            if (PPawn == nullptr || PPawn->isDead())
            {
                return { -1, 0, b.name };
            }
            // the tank, then a fighter, then a mage who is not the healer, then the healer
            const auto role = roleOf(PPawn);
            const int  rank = role == "tank" ? 3 : role == "melee" ? 2 : isHealer(static_cast<uint8>(PPawn->GetMJob())) ? 0 : 1;
            return { rank, PPawn->GetMLevel(), b.name };
        };
        for (Body* mate : campMates(body))
        {
            if (!mate->present)
            {
                continue;
            }
            if (best == nullptr || keyOf(*mate) > keyOf(*best))
            {
                best = mate;
            }
        }
        return best != nullptr && std::get<0>(keyOf(*best)) >= 0 ? best : nullptr;
    }

    // Standing in a camp, she joins its party: the one a standing mate is
    // in, or a new one under that mate. Leaving it is despawnById's
    void joinCampParty(Body& body)
    {
        if (body.party <= 1)
        {
            return;
        }
        auto* PPawn = pawn::findPawn(body.charid);
        if (PPawn == nullptr || PPawn->PParty != nullptr)
        {
            return;
        }
        CCharEntity* PWith = nullptr;
        for (Body* mate : campMates(body))
        {
            if (mate == &body || !mate->present)
            {
                continue;
            }
            if (auto* PMate = pawn::findPawn(mate->charid); PMate != nullptr && !PMate->isDead())
            {
                if (PMate->PParty != nullptr)
                {
                    PWith = PMate;
                    break;
                }
                if (PWith == nullptr)
                {
                    PWith = PMate;
                }
            }
        }
        if (PWith == nullptr)
        {
            return; // first of her camp to stand: the next one forms the party under her
        }
        if (PWith->PParty == nullptr)
        {
            PWith->PParty = new CParty(PWith);
            PWith->clearPacketList();
        }
        PWith->PParty->AddMember(PPawn);
        PPawn->clearPacketList();
        PWith->clearPacketList();
        ShowInfoFmt("world: {} joins {}'s party ({} of {})", body.name, PWith->getName(), PWith->PParty->members.size(), body.party);
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
        // A town body not yet at her seat comes in at her exit point and walks
        // (TownTick); anyone else stands where she stood
        const bool        walksIn = body.cameFrom.has_value() && !body.atSeat && !body.leaving;
        const position_t& at      = walksIn ? *body.cameFrom : body.point;
        if (!pawn::spawnAt(row->charid, PZone, at, row->job, row->level))
        {
            return false;
        }
        if (walksIn)
        {
            body.viaNext = 0; // from the gate again: the whole way in, doorway included
        }
        body.present     = true;
        body.censusLevel = row->level;
        body.seed        = row->seed;
        body.returnAt.reset();
        if (auto* PPawn = pawn::findPawn(row->charid); PPawn != nullptr)
        {
            if (const auto worn = dressFromWardrobe(body, PPawn); worn > 0)
            {
                ShowInfoFmt("world: {} dresses in {} pieces", body.name, worn);
            }
        }
        snapshotBag(body);
        joinCampParty(body);
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
        ShowInfoFmt("world: {} fades in at {} ({:.1f}, {:.1f}, {:.1f}){}", body.name, PZone->getName(), at.x, at.y, at.z,
                    walksIn ? fmt::format(" and walks to her seat ({:.1f}, {:.1f}, {:.1f})", body.point.x, body.point.y, body.point.z) : "");
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
        std::string          activity; // farm, stand, camp (a party)
        std::array<int32, 2> band{};
        uint32               count = 0;
        std::array<float, 3> at{};
        float                spread = 0.0f;
        float                roam   = 0.0f; // optional: the home pull's scale -- the farther out, the more the errand favours prey back toward the point
        uint32               party  = 1;    // members per party; seats = count x party (a camp, or a stand that comes and goes as a group)

        // A town seat (ROADMAP D4), stand only. She faces `face`. With a
        // `dwell` (seconds, min and max) the seat is a turnstile: she holds
        // it that long, walks to an exit and fades, and the seat refills
        // with another face after a gap. `enter` and `exit` name the
        // zone's exits she arrives from and leaves by -- "nearest" (the
        // default) and "any" are words too. `hours` is the player's local
        // clock, `vhours` Vana'diel's (the guilds keep it), `holiday` a
        // Vana'diel weekday the seat stands empty. `prefer: sellers` fills
        // the seat from the names in the player's own auction history.
        // `pose: kneel` kneels her at her seat
        std::optional<std::array<float, 3>> face;
        std::array<uint32, 2>               dwell{};
        std::string                         enter;
        std::string                         exit;
        std::optional<std::array<int32, 2>> hours;
        std::optional<std::array<int32, 2>> vhours;
        std::string                         holiday;
        std::string                         prefer;
        std::string                         pose;
        // `cliques: [min, max]` lays the seats out as little groups of that
        // many, each a circle its members face the middle of (the user:
        // "little clusters of people facing each other"); a body alone
        // faces a way of her own
        std::array<uint32, 2>               cliques{};
        // `via`: points walked in order on the way in and in reverse on the
        // way out -- a doorway the mesh's shortest line would miss (the
        // Tanners' Guild: the mesh leaks through a wall)
        std::vector<std::array<float, 3>>   via;

        auto seats() const -> uint32
        {
            return count * std::max<uint32>(1, party);
        }
        auto clustered() const -> bool
        {
            return cliques[1] > 0;
        }
        auto isCamp() const -> bool
        {
            return activity == "camp";
        }
        auto farms() const -> bool
        {
            return activity == "farm" || activity == "camp";
        }
        auto turnstile() const -> bool
        {
            return dwell[1] > 0;
        }
        // On the town's clock: a dwell that runs out, or hours that close
        auto timed() const -> bool
        {
            return turnstile() || hours.has_value() || vhours.has_value() || !holiday.empty();
        }
        // Anything the controller's town walk must know about
        auto town() const -> bool
        {
            return turnstile() || face.has_value() || !pose.empty() || clustered() || !via.empty();
        }
        bool operator==(const SlotSpec&) const = default;
    };
    // Where a town body appears and leaves: the gates, the Mog House door
    struct ExitSpec
    {
        std::string          name;
        std::array<float, 3> at{};
        bool                 operator==(const ExitSpec&) const = default;
    };
    // Unknown keys are mistakes; a missing one takes the default (roam, party)
    constexpr glz::opts kSlotYaml{ .error_on_unknown_keys = true, .error_on_missing_keys = false };
    struct SlotFile
    {
        std::vector<ExitSpec> exits;
        std::vector<SlotSpec> slots;
        bool                  operator==(const SlotFile&) const = default;
    };
    // One seat of a clustered slot: where she stands and what she faces
    struct Seat
    {
        position_t                at{};
        std::optional<position_t> face;
    };
    struct ZoneSlots
    {
        bool                                               loaded = false;
        std::vector<ExitSpec>                              exits;      // as authored (the re-read compares against these)
        std::vector<position_t>                            exitPoints; // the same, on the mesh: where bodies appear and fade
        std::vector<SlotSpec>                              specs;
        std::vector<std::vector<std::string>>              occupants; // names, by slot
        std::vector<uint32>                                turns;     // a turnstile's departures so far: the next face differs
        std::vector<std::chrono::steady_clock::time_point> refillAt;  // a turnstile refills no sooner than this
        std::vector<std::vector<std::string>>              recent;    // the last few faces a turnstile showed
        std::vector<std::vector<Seat>>                     layout;    // a clustered slot's seats, laid out once
        std::vector<std::vector<std::string>>              holders;   // who holds each laid-out seat ("" = free)
        std::filesystem::file_time_type                    written{}; // the file as read, so an edit is noticed
    };
    std::unordered_map<uint16, ZoneSlots> zoneSlots;

    auto toPosition(const std::array<float, 3>& at) -> position_t;
    auto flatDistance(const position_t& a, const position_t& b) -> float;

    // A small seeded generator, so a layout is the same every visit
    struct Dice
    {
        uint32 state;
        auto   next() -> uint32
        {
            state ^= state << 13;
            state ^= state >> 17;
            state ^= state << 5;
            return state;
        }
        auto unit() -> float
        {
            return static_cast<float>(next() % 100000u) / 100000.0f;
        }
        auto between(const uint32 lo, const uint32 hi) -> uint32
        {
            return hi <= lo ? lo : lo + next() % (hi - lo + 1);
        }
    };

    // The seats of a clustered slot (`cliques`). Group sizes are drawn in
    // the range until the seats are covered. Group centres are spread over
    // the slot by best-candidate sampling -- each new centre the farthest of
    // twenty random tries from the ones placed -- so the groups do not pile
    // up. A group is a conversation circle: its members on a ring round
    // the centre, a yalm out for a pair and wider as the group grows,
    // evenly spaced with a little jitter, each facing the middle. A body
    // alone faces a heading of her own. Drawn from the zone, the slot and
    // a fixed seed, so the arrangement is the same every visit
    auto layoutCliques(const uint16 zoneId, const uint32 slot, const SlotSpec& spec) -> std::vector<Seat>
    {
        Dice dice{ (static_cast<uint32>(zoneId) * 40503u) ^ ((slot + 1) * 2654435761u) ^ 0x5bd1e995u };
        if (dice.state == 0)
        {
            dice.state = 1;
        }
        const uint32 wanted = spec.seats();
        const uint32 lo     = std::max<uint32>(1, std::min(spec.cliques[0], spec.cliques[1]));
        const uint32 hi     = std::max(lo, spec.cliques[1]);
        std::vector<uint32> sizes;
        for (uint32 covered = 0; covered < wanted;)
        {
            const uint32 n = std::min(dice.between(lo, hi), wanted - covered);
            sizes.push_back(n);
            covered += n;
        }
        const auto centre = toPosition(spec.at);
        const auto inDisc = [&]() -> position_t
        {
            const float angle  = 2.0f * std::numbers::pi_v<float> * dice.unit();
            const float radius = std::max(0.0f, spec.spread - 1.0f) * std::sqrt(dice.unit());
            position_t  p      = centre;
            p.x += radius * std::cos(angle);
            p.z += radius * std::sin(angle);
            return p;
        };
        std::vector<position_t> centres;
        for (size_t g = 0; g < sizes.size(); ++g)
        {
            position_t best     = inDisc();
            float      bestGap  = -1.0f;
            for (int tries = 0; tries < 20; ++tries)
            {
                const position_t candidate = tries == 0 ? best : inDisc();
                float            gap       = std::numeric_limits<float>::max();
                for (const auto& c : centres)
                {
                    gap = std::min(gap, flatDistance(candidate, c));
                }
                if (centres.empty() || gap > bestGap)
                {
                    best    = candidate;
                    bestGap = gap;
                }
                if (centres.empty())
                {
                    break;
                }
            }
            centres.push_back(best);
        }
        std::vector<Seat> seats;
        for (size_t g = 0; g < sizes.size(); ++g)
        {
            const uint32 n = sizes[g];
            if (n == 1)
            {
                // alone: a heading of her own, held as a point to face
                const float heading = 2.0f * std::numbers::pi_v<float> * dice.unit();
                Seat        seat{};
                seat.at   = centres[g];
                seat.face = centres[g];
                seat.face->x += 4.0f * std::cos(heading);
                seat.face->z += 4.0f * std::sin(heading);
                seats.push_back(seat);
                continue;
            }
            const float ring = 0.9f + 0.3f * static_cast<float>(n - 2);
            const float base = 2.0f * std::numbers::pi_v<float> * dice.unit();
            for (uint32 i = 0; i < n; ++i)
            {
                const float angle = base + 2.0f * std::numbers::pi_v<float> * static_cast<float>(i) / static_cast<float>(n) + (dice.unit() - 0.5f) * 0.5f;
                Seat        seat{};
                seat.at = centres[g];
                seat.at.x += ring * std::cos(angle);
                seat.at.z += ring * std::sin(angle);
                seat.face = centres[g];
                seats.push_back(seat);
            }
        }
        return seats;
    }

    auto weekdayByName(const std::string& name) -> std::optional<uint32>
    {
        static const std::array<std::string_view, 8> days{ "firesday", "earthsday", "watersday", "windsday", "iceday", "lightningday", "lightsday", "darksday" };
        std::string key = name;
        std::ranges::transform(key, key.begin(), [](const unsigned char c) { return static_cast<char>(std::tolower(c)); });
        for (uint32 i = 0; i < days.size(); ++i)
        {
            if (days[i] == key)
            {
                return i;
            }
        }
        return std::nullopt;
    }

    // Is the hour inside [from, to)? Equal ends mean always; to before
    // from wraps midnight (18 - 2)
    auto inWindow(const int32 hour, const int32 from, const int32 to) -> bool
    {
        if (from == to)
        {
            return true;
        }
        return from < to ? (hour >= from && hour < to) : (hour >= from || hour < to);
    }

    // Is the seat held at this hour: the player's clock, Vana'diel's, the holiday
    auto seatOpen(const SlotSpec& spec) -> bool
    {
        if (spec.hours.has_value() && !inWindow(static_cast<int32>(earth_time::local::get_hour()), (*spec.hours)[0], (*spec.hours)[1]))
        {
            return false;
        }
        if (spec.vhours.has_value() && !inWindow(static_cast<int32>(vanadiel_time::get_hour()), (*spec.vhours)[0], (*spec.vhours)[1]))
        {
            return false;
        }
        if (const auto day = weekdayByName(spec.holiday); day.has_value() && *day == vanadiel_time::get_weekday())
        {
            return false;
        }
        return true;
    }

    auto toPosition(const std::array<float, 3>& at) -> position_t
    {
        position_t p{};
        p.x = at[0];
        p.y = at[1];
        p.z = at[2];
        return p;
    }

    auto flatDistance(const position_t& a, const position_t& b) -> float
    {
        return std::hypot(a.x - b.x, a.z - b.z);
    }

    // The nearest walkable point to an authored one: a table is written by
    // eye and by !pos, and a point a few yalms above or below the mesh
    // (the lower level under the auction house, the gate arch) would never
    // be reached. The point itself when there is no mesh or nothing near
    auto snapToMesh(CZone* PZone, const position_t& point) -> position_t
    {
        const auto* navMesh = PZone != nullptr ? PZone->navMesh() : nullptr;
        if (navMesh == nullptr)
        {
            return point;
        }
        if (const auto snapped = navMesh->findClosestValidPoint(point); snapped.has_value())
        {
            position_t out = *snapped;
            out.rotation   = point.rotation;
            return out;
        }
        return point;
    }

    // The authoring aid: say once when a table's point moved to reach the
    // mesh, so the user knows which spot to walk again
    void reportSnap(CZone* PZone, const std::string& what, const position_t& authored, const position_t& snapped)
    {
        if (std::hypot(authored.x - snapped.x, authored.y - snapped.y, authored.z - snapped.z) > 1.0f)
        {
            ShowInfoFmt("world: {}: {} ({:.1f}, {:.1f}, {:.1f}) is off the mesh; bodies use ({:.1f}, {:.1f}, {:.1f}) -- walk it again",
                        PZone->getName(), what, authored.x, authored.y, authored.z, snapped.x, snapped.y, snapped.z);
        }
    }

    // The exit a word means from a point: a name, "any" (a random one, not
    // `notThis` when there is another), else the nearest. Nothing when the
    // zone has no exits
    auto exitPoint(const ZoneSlots& table, const std::string& how, const position_t& from, const std::optional<position_t>& notThis) -> std::optional<position_t>
    {
        if (table.exits.empty() || table.exitPoints.size() != table.exits.size())
        {
            return std::nullopt;
        }
        if (!how.empty() && how != "nearest" && how != "any")
        {
            for (size_t i = 0; i < table.exits.size(); ++i)
            {
                if (table.exits[i].name == how)
                {
                    return table.exitPoints[i];
                }
            }
        }
        if (how == "any")
        {
            std::vector<const position_t*> others;
            for (const auto& p : table.exitPoints)
            {
                if (!notThis.has_value() || flatDistance(p, *notThis) > 1.0f)
                {
                    others.push_back(&p);
                }
            }
            if (others.empty())
            {
                others.push_back(&table.exitPoints.front());
            }
            return *others[xirand::GetRandomNumber(others.size())];
        }
        const position_t* best = nullptr;
        for (const auto& p : table.exitPoints)
        {
            if (best == nullptr || flatDistance(p, from) < flatDistance(*best, from))
            {
                best = &p;
            }
        }
        return *best;
    }
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
    auto parseSlots(const std::filesystem::path& path) -> std::optional<SlotFile>
    {
        if (!std::filesystem::exists(path))
        {
            return SlotFile{};
        }
        std::ifstream     in(path);
        const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        if (std::ranges::all_of(text, [](const unsigned char c) { return std::isspace(c) != 0; }))
        {
            return SlotFile{};
        }
        SlotFile file{};
        if (const auto error = glz::read_yaml<kSlotYaml>(file, text); error)
        {
            ShowErrorFmt("world: {}: {}", path.string(), glz::format_error(error, text));
            return std::nullopt;
        }
        const auto bad = [&](const std::string& why)
        {
            ShowErrorFmt("world: {}: {}", path.string(), why);
            return std::nullopt;
        };
        for (const auto& e : file.exits)
        {
            if (e.name.empty() || e.name == "nearest" || e.name == "any" || std::ranges::count_if(file.exits, [&](const ExitSpec& o) { return o.name == e.name; }) > 1)
            {
                return bad(fmt::format("exit '{}' needs a name of its own (not nearest or any, not twice)", e.name));
            }
        }
        const auto knownExit = [&](const std::string& how)
        {
            return how.empty() || how == "nearest" || how == "any" || std::ranges::any_of(file.exits, [&](const ExitSpec& e) { return e.name == how; });
        };
        const auto goodHours = [](const std::optional<std::array<int32, 2>>& h)
        {
            return !h.has_value() || ((*h)[0] >= 0 && (*h)[0] <= 23 && (*h)[1] >= 0 && (*h)[1] <= 24);
        };
        for (const auto& spec : file.slots)
        {
            if (spec.activity != "farm" && spec.activity != "stand" && spec.activity != "camp")
            {
                return bad(fmt::format("activity {} is not farm, stand or camp", spec.activity));
            }
            if (spec.activity != "stand" && (spec.town() || !spec.enter.empty() || !spec.exit.empty() || spec.hours.has_value() || spec.vhours.has_value() || !spec.holiday.empty() || !spec.prefer.empty()))
            {
                return bad(fmt::format("a {} slot takes no town keys (face, dwell, enter, exit, hours, vhours, holiday, prefer, pose, cliques, via)", spec.activity));
            }
            if (spec.dwell[0] > spec.dwell[1])
            {
                return bad(fmt::format("dwell [{}, {}] is min then max", spec.dwell[0], spec.dwell[1]));
            }
            if (!knownExit(spec.enter) || !knownExit(spec.exit))
            {
                return bad(fmt::format("enter '{}' / exit '{}' is not an exit of this zone, nearest or any", spec.enter, spec.exit));
            }
            if (spec.turnstile() && file.exits.empty())
            {
                return bad("a turnstile (a dwell) needs the zone's exits: where she comes from and goes");
            }
            if (!goodHours(spec.hours) || !goodHours(spec.vhours))
            {
                return bad("hours are [from, to] on a 24-hour clock");
            }
            if (!spec.holiday.empty() && !weekdayByName(spec.holiday).has_value())
            {
                return bad(fmt::format("holiday '{}' is not a Vana'diel weekday", spec.holiday));
            }
            if (!spec.prefer.empty() && spec.prefer != "sellers")
            {
                return bad(fmt::format("prefer '{}' is not sellers", spec.prefer));
            }
            if (!spec.pose.empty() && spec.pose != "kneel")
            {
                return bad(fmt::format("pose '{}' is not kneel", spec.pose));
            }
            if (spec.cliques[0] > spec.cliques[1] || spec.cliques[1] > 8)
            {
                return bad(fmt::format("cliques [{}, {}] is min then max, up to 8", spec.cliques[0], spec.cliques[1]));
            }
        }
        return file;
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
        auto file     = parseSlots(path).value_or(SlotFile{});
        table.exits   = std::move(file.exits);
        table.specs   = std::move(file.slots);
        table.occupants.assign(table.specs.size(), {});
        table.turns.assign(table.specs.size(), 0);
        table.refillAt.assign(table.specs.size(), std::chrono::steady_clock::time_point{});
        table.recent.assign(table.specs.size(), {});
        table.layout.assign(table.specs.size(), {});
        table.holders.assign(table.specs.size(), {});
        for (size_t i = 0; i < table.specs.size(); ++i)
        {
            if (table.specs[i].clustered())
            {
                table.layout[i] = layoutCliques(zoneId, static_cast<uint32>(i), table.specs[i]);
                for (auto& seat : table.layout[i])
                {
                    seat.at = snapToMesh(PZone, seat.at);
                }
                table.holders[i].assign(table.layout[i].size(), std::string());
            }
        }
        // Exits and town points onto the mesh, said once when they moved
        table.exitPoints.clear();
        for (const auto& e : table.exits)
        {
            const auto authored = toPosition(e.at);
            const auto snapped  = snapToMesh(PZone, authored);
            reportSnap(PZone, fmt::format("exit {}", e.name), authored, snapped);
            table.exitPoints.push_back(snapped);
        }
        for (size_t i = 0; i < table.specs.size(); ++i)
        {
            if (table.specs[i].town())
            {
                const auto authored = toPosition(table.specs[i].at);
                reportSnap(PZone, fmt::format("slot #{}'s point", i), authored, snapToMesh(PZone, authored));
            }
        }
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

    // Every world body stepped on the same zone tick down the same
    // corner-hugging line, so the mesh sent them down a street in single
    // file (the user, 2026-09-07). Her lane: a sideways offset her town
    // walks keep to. (Her run cycle's phase is the controller's business:
    // the walk-step jitter, TownTick)
    auto laneOf(const std::string& name) -> float
    {
        return -1.6f + 3.2f * static_cast<float>((nameHash(name) / 13u) % 1000u) / 1000.0f;
    }

    // The names in the player's own auction history, newest first: who they
    // bought from (a crowd listing, seller 0, taken by a real buyer) and who
    // bought from them (a real listing sold to the crowd, buyer 0). The
    // market writes the crowd's names from the census, so these are people
    // of the world
    auto recentCounterparties() -> std::vector<std::string>
    {
        std::vector<std::string> names;
        const auto               take = [&](const char* sql)
        {
            if (const auto rset = db::preparedStmt(sql); rset)
            {
                while (rset->next())
                {
                    auto who = rset->get<std::string>("who");
                    if (!who.empty() && std::ranges::find(names, who) == names.end())
                    {
                        names.push_back(std::move(who));
                    }
                }
            }
        };
        take("SELECT seller_name AS who FROM auction_house WHERE seller = 0 AND buyer <> 0 AND buyer_name IS NOT NULL ORDER BY sell_date DESC LIMIT 20");
        take("SELECT buyer_name AS who FROM auction_house WHERE seller <> 0 AND buyer = 0 AND buyer_name IS NOT NULL ORDER BY sell_date DESC LIMIT 20");
        return names;
    }

    // A slot's occupants: in the world, level in band, not recruited, not
    // placed anywhere already; the preferred names first (a town seat that
    // prefers the player's sellers), then cohort rows (the recruitment pool
    // is who you should meet), then by a hash of zone, slot and seed, so a
    // visit tends to show the same faces. A turnstile mixes its turn into
    // the hash and passes over the faces it showed last, so the next one
    // differs. A camp party is dealt healer first when the band has one,
    // then the rest; a solo seat takes anyone
    auto chooseOccupants(const uint16 zoneId, const uint32 slot, const SlotSpec& spec, const size_t wanted, const bool wantHealer,
                         const uint32 turn = 0, const std::vector<std::string>& recent = {}) -> std::vector<std::string>
    {
        struct Candidate
        {
            std::string name;
            bool        preferred = false;
            bool        shared    = false;
            bool        healer    = false;
            uint32      order     = 0;
        };
        const std::vector<std::string> preferred = spec.prefer == "sellers" ? recentCounterparties() : std::vector<std::string>{};
        std::vector<Candidate>         candidates;
        std::vector<Candidate>         shown; // recent faces, taken only when nobody else fits
        if (const auto rset = db::preparedStmt("SELECT name, cohort, seed, job FROM cardian_census WHERE anchor <> 'bank' AND recruited = 0 AND level BETWEEN ? AND ?",
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
                const uint32 seed      = rset->get<uint32>("seed");
                const uint32 order     = (seed * 2654435761u) ^ (static_cast<uint32>(zoneId) * 40503u + (slot + 1) * 2654435761u) ^ (turn * 0x9E3779B9u);
                const bool   isPreferred = std::ranges::find(preferred, name) != preferred.end();
                const bool   wasShown    = std::ranges::find(recent, name) != recent.end();
                Candidate    c{ .name = std::move(name), .preferred = isPreferred, .shared = rset->get<uint32>("cohort") == 0, .healer = isHealer(rset->get<uint8>("job")), .order = order };
                (wasShown ? shown : candidates).push_back(std::move(c));
            }
        }
        if (candidates.size() < wanted)
        {
            candidates.insert(candidates.end(), shown.begin(), shown.end());
        }
        std::ranges::sort(candidates, [](const Candidate& a, const Candidate& b)
        {
            return std::make_tuple(!a.preferred, a.shared, a.order, a.name) < std::make_tuple(!b.preferred, b.shared, b.order, b.name);
        });
        std::vector<std::string> out;
        if (wantHealer && spec.isCamp())
        {
            if (const auto it = std::ranges::find_if(candidates, [](const Candidate& c) { return c.healer; }); it != candidates.end())
            {
                out.push_back(it->name);
                candidates.erase(it);
            }
        }
        for (auto& c : candidates)
        {
            if (out.size() >= wanted)
            {
                break;
            }
            if (spec.isCamp() && c.healer && !out.empty())
            {
                continue; // one healer a camp; the rest fight
            }
            out.push_back(std::move(c.name));
        }
        // a camp short of fighters takes whoever is left, healers included
        for (auto& c : candidates)
        {
            if (out.size() >= wanted)
            {
                break;
            }
            if (std::ranges::find(out, c.name) == out.end())
            {
                out.push_back(std::move(c.name));
            }
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

    // Presence queued for what a slot is short of. A town seat fills only
    // in its hours, and a turnstile no sooner than its refill time
    auto queueSlot(CZone* PZone, const uint32 slot) -> uint32
    {
        const auto  zoneId = static_cast<uint16>(PZone->GetID());
        auto&       table  = zoneSlots[zoneId];
        const auto& spec   = table.specs[slot];
        const auto  have   = table.occupants[slot].size();
        if (have >= spec.seats() || !seatOpen(spec) || (spec.turnstile() && std::chrono::steady_clock::now() < table.refillAt[slot]))
        {
            return 0;
        }
        // a camp wants a healer unless one already holds a seat
        bool hasHealer = false;
        for (const auto& name : table.occupants[slot])
        {
            if (const auto it = charidByName.find(name); it != charidByName.end())
            {
                if (const auto* PPawn = pawn::findPawn(it->second); PPawn != nullptr && isHealer(static_cast<uint8>(PPawn->GetMJob())))
                {
                    hasHealer = true;
                }
            }
        }
        // A clustered slot deals its laid-out seats in order, skipping the
        // held ones and the ones already promised to a pending body
        std::vector<size_t> freeSeats;
        if (spec.clustered())
        {
            for (size_t i = 0; i < table.layout[slot].size(); ++i)
            {
                const bool promised = std::ranges::any_of(pending, [&](const Pending& p) { return p.zone == zoneId && p.slot == static_cast<int32>(slot) && p.seat == static_cast<int32>(i); });
                if (table.holders[slot][i].empty() && !promised)
                {
                    freeSeats.push_back(i);
                }
            }
        }
        uint32 queued = 0;
        for (const auto& name : chooseOccupants(zoneId, slot, spec, spec.seats() - have, !hasHealer && spec.isCamp(), table.turns[slot], table.recent[slot]))
        {
            Pending item{ .name = name, .zone = zoneId, .point = slotPoint(spec, name), .pinned = false, .farming = spec.farms(), .presence = true, .slot = static_cast<int32>(slot), .roam = spec.roam, .party = std::max<uint32>(1, spec.party) };
            if (spec.clustered())
            {
                if (queued >= freeSeats.size())
                {
                    break; // no laid-out seat left
                }
                const auto& seat = table.layout[slot][freeSeats[queued]];
                item.seat        = static_cast<int32>(freeSeats[queued]);
                item.point       = seat.at;
                item.face        = seat.face;
            }
            else if (spec.town())
            {
                item.point = snapToMesh(PZone, item.point); // her seat, on the mesh, so the walk can end
            }
            if (spec.face.has_value() && !item.face.has_value())
            {
                item.face = toPosition(*spec.face);
            }
            for (const auto& v : spec.via)
            {
                item.via.push_back(snapToMesh(PZone, toPosition(v)));
            }
            if (spec.turnstile())
            {
                item.dwell    = spec.dwell;
                // she comes in from the exit nearest her first via point, else her seat
                item.cameFrom = exitPoint(table, spec.enter, item.via.empty() ? item.point : item.via.front(), std::nullopt);
            }
            item.pose = spec.pose;
            pending.push_back(std::move(item));
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
        body.party        = item.party;
        body.present      = false;
        body.censusLevel  = row->level;
        body.seed         = row->seed;
        body.face         = item.face;
        body.dwell        = item.dwell;
        body.pose         = item.pose;
        body.cameFrom     = item.cameFrom;
        body.seat         = item.seat;
        body.via          = item.via;
        charidByName[item.name] = charid;
        if (auto& table = zoneSlots[zoneId]; item.slot >= 0 && static_cast<size_t>(item.slot) < table.occupants.size())
        {
            table.occupants[item.slot].push_back(item.name);
            if (item.seat >= 0 && static_cast<size_t>(item.seat) < table.holders[item.slot].size())
            {
                table.holders[item.slot][item.seat] = item.name;
            }
        }
        pawn::markPresent(charid, zoneId, item.point, row->job, row->level);
        ShowInfoFmt("world: {} ({} {}) holds slot {} in {}", item.name, magic_enum::enum_name(static_cast<xi::Job>(row->job)), row->level, item.slot, PZone->getName());
        if (wasLive[zoneId])
        {
            fadeIn(body);
        }
        else if (item.dwell[1] > 0)
        {
            // Nobody to see her walk in: she is at her seat already, and the
            // town's clock runs unseen -- she leaves it on time all the same
            body.atSeat  = true;
            body.viaNext = body.via.size();
            body.leaveAt = std::chrono::steady_clock::now() + std::chrono::seconds(xirand::GetRandomNumber(item.dwell[0], item.dwell[1] + 1));
        }
        return true;
    }

    // A seat's occupant returns to the pool: her body fades, her presence
    // goes, the seat is free to fill again. A turnstile counts the turn,
    // remembers the face and waits its gap before the next one
    void unseat(Body& body, const std::string_view why)
    {
        if (body.present)
        {
            fadeOut(body, why);
        }
        pawn::markAbsent(body.charid);
        if (auto tit = zoneSlots.find(body.zone); tit != zoneSlots.end() && body.slot >= 0 && static_cast<size_t>(body.slot) < tit->second.occupants.size())
        {
            auto&      table = tit->second;
            const auto slot  = static_cast<size_t>(body.slot);
            std::erase(table.occupants[slot], body.name);
            if (body.seat >= 0 && static_cast<size_t>(body.seat) < table.holders[slot].size() && table.holders[slot][body.seat] == body.name)
            {
                table.holders[slot][body.seat].clear();
            }
            if (table.specs[slot].turnstile())
            {
                ++table.turns[slot];
                table.refillAt[slot] = std::chrono::steady_clock::now() + std::chrono::seconds(xirand::GetRandomNumber(settings::get<uint32>("pawn.WORLD_TOWN_GAP_MIN"), settings::get<uint32>("pawn.WORLD_TOWN_GAP_MAX") + 1));
                auto& recent         = table.recent[slot];
                std::erase(recent, body.name);
                recent.push_back(body.name);
                if (recent.size() > 6)
                {
                    recent.erase(recent.begin());
                }
            }
        }
        charidByName.erase(body.name);
        bodies.erase(body.charid);
    }

    // -- Town seats (ROADMAP D4): the turnstile's clock --------------------------
    // She leaves when her dwell is up or her hours close: standing, she
    // walks to her exit and fades there (the controller reports the
    // arrival, `gone`); faded, she simply goes. A group leaves together.
    // Then the seat waits its gap and refills with another face
    // Her way out: the via points in reverse (those she has passed), then
    // the exit -- the one nearest her last via point, else her seat
    void setWayOut(Body& body, const ZoneSlots& table, const std::optional<position_t>& sharedExit)
    {
        const auto& spec = table.specs[body.slot];
        body.wayOut.clear();
        body.outNext = 0;
        for (size_t i = std::min(body.viaNext, body.via.size()); i > 0; --i)
        {
            body.wayOut.push_back(body.via[i - 1]);
        }
        const position_t& from = body.via.empty() ? body.point : body.via.front();
        body.exitAt            = sharedExit.has_value() ? sharedExit : exitPoint(table, spec.exit, from, body.cameFrom);
        if (body.exitAt.has_value())
        {
            body.wayOut.push_back(*body.exitAt);
        }
    }

    void leaveSeat(Body& body, ZoneSlots& table, const std::string_view why)
    {
        body.leaving = true;
        body.leaveAt.reset();
        setWayOut(body, table, std::nullopt);
        if (body.present)
        {
            ShowInfoFmt("world: {} leaves her seat ({}){}", body.name, why,
                        body.exitAt.has_value() ? fmt::format(", for ({:.0f}, {:.0f}, {:.0f}){}", body.exitAt->x, body.exitAt->y, body.exitAt->z, body.wayOut.size() > 1 ? " by way of the door" : "") : "");
        }
        else
        {
            body.gone = true;
        }
        for (Body* mate : campMates(body))
        {
            if (mate != &body && !mate->leaving)
            {
                mate->leaving = true;
                mate->leaveAt.reset();
                setWayOut(*mate, table, body.exitAt);
                mate->gone = !mate->present;
            }
        }
    }

    void tickTown(CZone* PZone, const std::chrono::steady_clock::time_point now, const bool poll)
    {
        const auto zoneId = static_cast<uint16>(PZone->GetID());
        const auto tit    = zoneSlots.find(zoneId);
        if (tit == zoneSlots.end() || tit->second.specs.empty())
        {
            return;
        }
        auto&               table = tit->second;
        std::vector<uint32> gone;
        std::vector<uint32> due;
        for (auto& [charid, body] : bodies)
        {
            if (body.zone != zoneId || body.slot < 0 || static_cast<size_t>(body.slot) >= table.specs.size() || !table.specs[body.slot].timed())
            {
                continue;
            }
            if (body.gone || (body.leaving && !body.present))
            {
                gone.push_back(charid);
            }
            else if (!body.leaving && ((body.leaveAt.has_value() && now >= *body.leaveAt) || (poll && !seatOpen(table.specs[body.slot]))))
            {
                due.push_back(charid);
            }
        }
        for (const auto charid : due)
        {
            if (const auto it = bodies.find(charid); it != bodies.end() && !it->second.leaving)
            {
                leaveSeat(it->second, table, seatOpen(table.specs[it->second.slot]) ? "her time is up" : "the hours are over");
            }
        }
        for (const auto charid : gone)
        {
            if (const auto it = bodies.find(charid); it != bodies.end())
            {
                unseat(it->second, "gone her way");
            }
        }
        if (poll)
        {
            for (uint32 slot = 0; slot < table.specs.size(); ++slot)
            {
                if (table.specs[slot].timed())
                {
                    queueSlot(PZone, slot);
                }
            }
        }
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

    auto hasBody(const uint32 charid) -> bool
    {
        return bodies.contains(charid);
    }

    auto isFarming(const uint32 charid) -> bool
    {
        const auto it = bodies.find(charid);
        return it != bodies.end() && it->second.farming;
    }

    auto brainRows(const CCharEntity* PPawn) -> std::vector<std::pair<std::string, bool>>
    {
        const auto& file = brains();
        std::vector<std::pair<std::string, bool>> rows;
        const auto add = [&](const std::vector<std::string>& specs)
        {
            for (const auto& spec : specs)
            {
                rows.emplace_back(spec, true);
            }
        };
        add(file.common);
        if (const auto it = file.jobs.find(std::string(magic_enum::enum_name(PPawn->GetMJob()))); it != file.jobs.end())
        {
            add(it->second);
        }
        const auto role = roleOf(PPawn);
        if (const auto it = file.roles.find(role); it != file.roles.end())
        {
            add(it->second);
        }
        return rows;
    }

    auto roleName(const uint32 charid) -> std::string
    {
        const auto* PPawn = pawn::findPawn(charid);
        return PPawn != nullptr ? roleOf(PPawn) : std::string();
    }

    auto campLeaderOf(const uint32 charid) -> uint32
    {
        const auto it = bodies.find(charid);
        if (it == bodies.end() || it->second.party <= 1)
        {
            return 0;
        }
        const Body* leader = campLeader(it->second);
        return leader != nullptr ? leader->charid : 0;
    }

    auto campSizeOf(const uint32 charid) -> uint32
    {
        const auto it = bodies.find(charid);
        return it != bodies.end() ? std::max<uint32>(1, it->second.party) : 1;
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

    auto townOrder(const uint32 charid) -> std::optional<TownOrder>
    {
        const auto it = bodies.find(charid);
        if (it == bodies.end() || !it->second.present)
        {
            return std::nullopt;
        }
        const Body& body = it->second;
        if (body.dwell[1] == 0 && !body.face.has_value() && body.pose.empty() && body.via.empty() && !body.leaving)
        {
            return std::nullopt; // a field seat: the farmer's own
        }
        TownOrder order{};
        order.face    = body.face;
        order.atSeat  = body.atSeat && !body.leaving;
        order.leaving = body.leaving;
        order.kneel   = body.pose == "kneel";
        if (body.leaving)
        {
            order.goal = body.outNext < body.wayOut.size() ? body.wayOut[body.outNext] : body.point;
        }
        else
        {
            order.goal = body.viaNext < body.via.size() ? body.via[body.viaNext] : body.point;
        }
        return order;
    }

    auto laneOf(const uint32 charid) -> float
    {
        const auto it = bodies.find(charid);
        return it != bodies.end() ? ::laneOf(it->second.name) : 0.0f;
    }

    // She reached the point she was walking to: the next via point, her
    // seat (the dwell starts), or the last of her way out (gone)
    void noteReached(const uint32 charid)
    {
        const auto it = bodies.find(charid);
        if (it == bodies.end())
        {
            return;
        }
        Body& body = it->second;
        if (body.leaving)
        {
            if (body.outNext + 1 < body.wayOut.size())
            {
                ++body.outNext;
            }
            else
            {
                body.gone = true;
            }
            return;
        }
        if (body.viaNext < body.via.size())
        {
            ++body.viaNext;
            return;
        }
        if (body.atSeat)
        {
            return;
        }
        body.atSeat = true;
        if (body.dwell[1] > 0)
        {
            const auto seconds = xirand::GetRandomNumber(body.dwell[0], body.dwell[1] + 1);
            body.leaveAt       = std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
            ShowInfoFmt("world: {} takes her seat for {} s", body.name, seconds);
        }
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
            std::string town;
            if (spec.turnstile())
            {
                town += fmt::format(" dwell {}-{}s turn {}", spec.dwell[0], spec.dwell[1], table.turns[i]);
            }
            if (spec.hours.has_value())
            {
                town += fmt::format(" hours {}-{}", (*spec.hours)[0], (*spec.hours)[1]);
            }
            if (spec.vhours.has_value())
            {
                town += fmt::format(" vhours {}-{}", (*spec.vhours)[0], (*spec.vhours)[1]);
            }
            if (!seatOpen(spec))
            {
                town += " (closed now)";
            }
            lines.push_back(fmt::format("#{} {}{} L{}-{} x{} at ({:.0f}, {:.0f}, {:.0f}) spread {:.0f}{}{}: {}", i, spec.activity,
                                        spec.party > 1 ? fmt::format(" of {}", spec.party) : "", spec.band[0], spec.band[1],
                                        spec.count, spec.at[0], spec.at[1], spec.at[2], spec.spread, spec.roam > 0 ? fmt::format(" roam {:.0f}", spec.roam) : "", town,
                                        who.empty() ? "nobody now" : who));
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
        const auto now    = std::chrono::steady_clock::now();

        // The slot tables: on a zone's first tick its occupants are chosen
        // and given presence, a couple a tick; their bodies come with a
        // player. An edited file is noticed within a few seconds and the
        // zone refills from it, so a table is authored with the zone live.
        // The town's turnstiles turn every tick; their refills come with
        // the poll
        if (settings::get<bool>("pawn.WORLD_SLOTS"))
        {
            bool poll = false;
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
                poll = true;
                reseatOutgrown(PZone);
                if (const auto it = zoneSlots.find(zoneId); it != zoneSlots.end())
                {
                    const auto path = slotPath(PZone);
                    std::error_code ec;
                    const auto written = std::filesystem::exists(path, ec) ? std::filesystem::last_write_time(path, ec) : std::filesystem::file_time_type{};
                    if (!ec && written != it->second.written)
                    {
                        const auto parsed = parseSlots(path);
                        if (!parsed.has_value() || (parsed->slots == it->second.specs && parsed->exits == it->second.exits))
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
            tickTown(PZone, now, poll);
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
                if (body.slot >= 0)
                {
                    body.returnAt = now + std::chrono::seconds(settings::get<uint32>("pawn.WORLD_KO_RETURN"));
                }
            }
        }
        // A KO'd seat-holder walks back to her seat once the return has
        // passed, as long as somebody is there to see her
        for (auto& [charid, body] : bodies)
        {
            if (body.zone == zoneId && !body.present && body.returnAt.has_value() && now >= *body.returnAt && wasLive[zoneId])
            {
                ShowInfoFmt("world: {} is back at her seat after her KO", body.name);
                fadeIn(body);
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
