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

#include "drill.h"

#include "pawn.h"

#include "common/logging.h"
#include "common/settings.h"
#include "common/timer.h"
#include "common/utils.h"

#include "data/yaml/read.h"
#include "entities/char_entity.h"
#include "pause/pause.h"
#include "utils/charutils.h"
#include "utils/zoneutils.h"
#include "zone.h"

#include <algorithm>
#include <array>
#include <filesystem>
#include <fstream>
#include <optional>
#include <regex>
#include <string>
#include <vector>

namespace pawn::drill::files
{
    // One drill of a run file. Every key but `minutes` and `level` is needed
    struct Spec
    {
        std::string                         name;
        std::string                         character;
        uint16                              zone = 0; // where she starts
        std::optional<std::array<float, 3>> at;       // ... the spot, set on the mesh
        uint16                              to      = 0;     // the zone she walks to
        float                               minutes = 30.0f; // her time limit
        uint8                               level   = 0;     // her main job's level for the drill; 0, her own
    };

    // A run: its id, written on top by dev-servers.ps1, and its drills
    struct Run
    {
        std::string       run;
        std::vector<Spec> drills;
    };
} // namespace pawn::drill::files

namespace
{
    using pawn::drill::files::Run;
    using pawn::drill::files::Spec;

    constexpr glz::opts         kRunYaml{ .error_on_unknown_keys = true, .error_on_missing_keys = false };
    const std::filesystem::path kRunPath     = std::filesystem::path("log") / "drill" / "run.yaml";
    const std::filesystem::path kResultsPath = std::filesystem::path("log") / "drill" / "results.jsonl";
    constexpr auto              kTickEvery   = std::chrono::milliseconds(500);
    constexpr auto              kLostAfter   = std::chrono::seconds(60); // nowhere in the world this long, where a zone change takes seconds
    constexpr float             kTeleport    = 50.0f;                    // a move this long between two looks is no walk

    // A drill under way, and what it has seen
    struct Watch
    {
        Spec                      spec;
        uint32                    charid = 0;
        uint8                     ownLevel = 0; // her own level, put back as the drill ends; 0 when the drill kept it
        realtime::time_point      began{};
        realtime::time_point      seenAt{}; // last found in a zone
        std::optional<position_t> last;     // where she stood at the last look, in the zone she is in
        float                     walked = 0.0f;
        std::vector<uint16>       zones;
        int32                     maxHp           = 0;
        int32                     lastHp          = -1;
        int32                     hpLost          = 0; // every HP she lost, summed
        int                       hits            = 0;
        int                       plans           = 0;
        int                       stretches       = 0;
        double                    planMs          = 0.0;
        double                    planMsMax       = 0.0;
        int64                     points          = 0;
        int64                     sightTests      = 0; // in her plans
        int64                     checkSightTests = 0; // in the once-a-second checks of her route
        int                       holds           = 0;
        int                       blocked         = 0; // stuck by aggro at a choke
        int                       chases          = 0;
        int                       stuck           = 0; // "I'm stuck!": no walk at all
        int                       rests           = 0; // across a zone line
        bool                      over            = false;
    };

    std::string                                     runId;
    std::vector<Watch>                              watches;
    std::optional<std::filesystem::file_time_type> mapStarted; // a run file written before the map's first look is a stale one
    std::filesystem::file_time_type                 seenWritten{};
    realtime::time_point                            lastTick{};
    std::optional<realtime::time_point>             heldAt; // a pause holding since: the drills' clocks stand still

    auto watchOf(const uint32 charid) -> Watch*
    {
        for (auto& w : watches)
        {
            if (!w.over && w.charid == charid)
            {
                return &w;
            }
        }
        return nullptr;
    }

    // Lines are only ever added: dev-servers.ps1 reads its own run's by id
    void writeLine(const std::string& line)
    {
        std::error_code ec;
        std::filesystem::create_directories(kResultsPath.parent_path(), ec);
        std::ofstream out(kResultsPath, std::ios::app);
        out << line << '\n';
    }

    // A JSON string, for the plain names and reasons a run holds
    auto jsonText(const std::string& text) -> std::string
    {
        std::string out = "\"";
        for (const char c : text)
        {
            if (c == '"' || c == '\\')
            {
                out += '\\';
            }
            out += (c == '\n' || c == '\r') ? ' ' : c;
        }
        return out + "\"";
    }

    // A run refused whole: why, and that it is done
    void refuse(const std::string& id, const std::string& why)
    {
        ShowWarningFmt("drill: run {} refused: {}", id, why);
        writeLine(fmt::format("{{\"run\":{},\"error\":{}}}", jsonText(id), jsonText(why)));
        writeLine(fmt::format("{{\"run\":{},\"done\":true}}", jsonText(id)));
    }

    // A drill over: its line in the results, her own level back, her trek
    // called off and her body taken out of the world; the run's last says
    // the run is done
    void finish(Watch& w, const std::string& result)
    {
        w.over                = true;
        const double seconds = std::chrono::duration<double>(realtime::now() - w.began).count();
        std::string  zones;
        for (const auto zone : w.zones)
        {
            zones += fmt::format("{}{}", zones.empty() ? "" : ",", zone);
        }
        writeLine(fmt::format("{{\"run\":{},\"name\":{},\"character\":{},\"level\":{},\"result\":{},\"seconds\":{:.1f},\"walked\":{:.1f},\"zones\":[{}],"
                              "\"hits\":{},\"hpLost\":{},\"maxHp\":{},\"chases\":{},\"holds\":{},\"blocked\":{},\"stuck\":{},\"rests\":{},\"plans\":{},"
                              "\"stretches\":{},\"planMsTotal\":{:.2f},\"planMsMax\":{:.2f},\"points\":{},\"sightTests\":{},\"checkSightTests\":{}}}",
                              jsonText(runId), jsonText(w.spec.name), jsonText(w.spec.character), w.spec.level, jsonText(result), seconds, w.walked, zones,
                              w.hits, w.hpLost, w.maxHp, w.chases, w.holds, w.blocked, w.stuck, w.rests, w.plans, w.stretches, w.planMs, w.planMsMax, w.points,
                              w.sightTests, w.checkSightTests));
        ShowInfoFmt("drill {}: {} -- {} after {:.0f} s, {:.0f}y walked, {} plans, the worst {:.2f} ms", w.spec.name, result, w.spec.character, seconds, w.walked,
                    w.plans, w.planMsMax);
        if (w.charid != 0)
        {
            if (w.ownLevel != 0)
            {
                pawn::setDrillLevel(w.charid, w.ownLevel, true);
            }
            pawn::clearTravelOrder(w.charid);
            pawn::despawnById(w.charid);
        }
        if (std::ranges::all_of(watches, [](const Watch& o) { return o.over; }))
        {
            writeLine(fmt::format("{{\"run\":{},\"done\":true}}", jsonText(runId)));
            ShowInfoFmt("drill: run {} done", runId);
        }
    }

    void startRun(const Run& run)
    {
        for (auto& w : watches)
        {
            if (!w.over)
            {
                finish(w, "cut short by the next run");
            }
        }
        watches.clear();
        runId = run.run;

        float minutes = 0.0f;
        for (const auto& spec : run.drills)
        {
            minutes = std::max(minutes, spec.minutes);
        }
        writeLine(fmt::format("{{\"run\":{},\"started\":true,\"drills\":{},\"minutes\":{:.1f}}}", jsonText(runId), run.drills.size(), minutes));
        ShowInfoFmt("drill: run {}, {} drills", runId, run.drills.size());

        const auto now = realtime::now();
        for (const auto& spec : run.drills)
        {
            Watch w;
            w.spec   = spec;
            w.began  = now;
            w.seenAt = now;
            watches.push_back(std::move(w));
        }
        for (auto& w : watches)
        {
            const auto&      spec   = w.spec;
            auto*            PZone  = zoneutils::GetZone(static_cast<xi::ZoneId>(spec.zone));
            const uint32     charid = charutils::getCharIdFromName(spec.character);
            const position_t at((*spec.at)[0], (*spec.at)[1], (*spec.at)[2], 0, 0);
            if (charid == 0)
            {
                finish(w, "no such character");
                continue;
            }
            if (!pawn::standForDrill(charid, PZone, at))
            {
                finish(w, "she could not be stood up (online, or out already)");
                continue;
            }
            w.charid = charid;
            if (spec.level != 0)
            {
                if (const auto* PChar = zoneutils::GetChar(charid); PChar != nullptr && PChar->GetMLevel() != spec.level)
                {
                    w.ownLevel = PChar->GetMLevel();
                    pawn::setDrillLevel(charid, spec.level, false);
                }
            }
            if (!pawn::orderTravel(charid, spec.to, 0))
            {
                finish(w, "her travel order was refused");
                continue;
            }
            ShowInfoFmt("drill {}: {} sets out from zone {} for zone {}{}, {} minutes at most", spec.name, spec.character, spec.zone, spec.to,
                        spec.level != 0 ? fmt::format(" at level {}", spec.level) : std::string(), spec.minutes);
        }
        if (run.drills.empty())
        {
            writeLine(fmt::format("{{\"run\":{},\"done\":true}}", jsonText(runId)));
        }
    }

    // A zone this map runs, by id: never 0, the placeholder every map keeps
    auto served(const uint16 zone) -> bool
    {
        auto* PZone = zone != 0 ? zoneutils::GetZone(static_cast<xi::ZoneId>(zone)) : nullptr;
        return PZone != nullptr && PZone->GetIP() != 0;
    }

    void readRun()
    {
        std::ifstream     in(kRunPath);
        const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());

        // The id first, by hand, so a file that does not parse is still
        // answered under its run's id
        std::smatch      match;
        const std::regex idLine(R"(^run:\s*'([^']*)')");
        const std::string id = std::regex_search(text, match, idLine) ? match[1].str() : std::string();
        if (id.empty())
        {
            ShowErrorFmt("drill: {} has no run id: not run", kRunPath.string());
            return;
        }

        Run run{};
        if (const auto error = glz::read_yaml<kRunYaml>(run, text); error)
        {
            refuse(id, glz::format_error(error, text));
            return;
        }
        for (const auto& spec : run.drills)
        {
            std::string why;
            if (spec.name.empty() || spec.character.empty())
            {
                why = "a drill without a name or a character";
            }
            else if (!spec.at.has_value())
            {
                why = fmt::format("{}: no `at`", spec.name);
            }
            else if (!served(spec.zone) || !served(spec.to))
            {
                why = fmt::format("{}: zone {} or {} is not a zone this map runs", spec.name, spec.zone, spec.to);
            }
            else if (spec.minutes <= 0.0f)
            {
                why = fmt::format("{}: no time to walk", spec.name);
            }
            if (!why.empty())
            {
                refuse(id, why);
                return;
            }
        }
        startRun(run);
    }

    void watch(Watch& w, const realtime::time_point now)
    {
        auto* PChar = zoneutils::GetChar(w.charid);
        if (PChar == nullptr || PChar->loc.zone == nullptr)
        {
            if (now - w.seenAt > kLostAfter)
            {
                finish(w, "lost: nowhere in the world for a minute");
            }
            return;
        }
        if (!pawn::isPawn(PChar))
        {
            w.charid = 0; // a player in her now: nothing of hers is the drill's to touch
            finish(w, "lost: she is a player's now, not the drill's");
            return;
        }
        w.seenAt        = now;
        const auto zone = static_cast<uint16>(PChar->getZone());
        if (w.zones.empty() || w.zones.back() != zone)
        {
            w.zones.push_back(zone);
            w.last.reset();
        }
        if (w.last.has_value())
        {
            const float step = distance(*w.last, PChar->loc.p);
            w.walked += step < kTeleport ? step : 0.0f;
        }
        w.last        = PChar->loc.p;
        const auto hp = static_cast<int32>(PChar->health.hp);
        w.maxHp       = std::max(w.maxHp, static_cast<int32>(PChar->GetMaxHP()));
        if (w.lastHp >= 0 && hp < w.lastHp)
        {
            ++w.hits;
            w.hpLost += w.lastHp - hp;
        }
        w.lastHp = hp;

        const auto limit = std::chrono::duration<float, std::ratio<60>>(w.spec.minutes);
        if (zone == w.spec.to)
        {
            finish(w, "arrived");
        }
        else if (PChar->isDead())
        {
            finish(w, fmt::format("fell in zone {}", zone));
        }
        else if (!pawn::travelOrderOf(w.charid).has_value())
        {
            finish(w, fmt::format("her trek gave up in zone {}", zone));
        }
        else if (now - w.began > std::chrono::duration_cast<realtime::duration>(limit))
        {
            finish(w, fmt::format("timed out in zone {}", zone));
        }
    }
} // namespace

namespace pawn::drill
{
    void tick()
    {
        if (!settings::get<bool>("cardian.DRILLS"))
        {
            return;
        }
        const auto now = realtime::now();
        if (now - lastTick < kTickEvery)
        {
            return;
        }
        lastTick = now;
        if (!mapStarted.has_value())
        {
            mapStarted = std::filesystem::file_time_type::clock::now();
        }

        std::error_code ec;
        const auto      written = std::filesystem::last_write_time(kRunPath, ec);
        if (!ec && written != seenWritten && written >= *mapStarted)
        {
            seenWritten = written;
            readRun();
        }

        // Held, the walkers stand still and so do their clocks
        if (cardian::pause::isHeld())
        {
            if (!heldAt.has_value())
            {
                heldAt = now;
            }
            return;
        }
        if (heldAt.has_value())
        {
            const auto held = now - *heldAt;
            heldAt.reset();
            for (auto& w : watches)
            {
                w.began += held;
                w.seenAt += held;
            }
        }
        for (auto& w : watches)
        {
            if (!w.over && w.charid != 0)
            {
                watch(w, now);
            }
        }
    }

    void notePlan(const uint32 charid, const std::chrono::microseconds took, const int searched, const int sightTests, const bool stretch)
    {
        if (auto* w = watchOf(charid); w != nullptr)
        {
            const double ms = static_cast<double>(took.count()) / 1000.0;
            ++w->plans;
            w->stretches += stretch ? 1 : 0;
            w->planMs += ms;
            w->planMsMax = std::max(w->planMsMax, ms);
            w->points += searched;
            w->sightTests += sightTests;
        }
    }

    void noteCheck(const uint32 charid, const int sightTests)
    {
        if (auto* w = watchOf(charid); w != nullptr)
        {
            w->checkSightTests += sightTests;
        }
    }

    void noteHold(const uint32 charid)
    {
        if (auto* w = watchOf(charid); w != nullptr)
        {
            ++w->holds;
        }
    }

    void noteBlocked(const uint32 charid)
    {
        if (auto* w = watchOf(charid); w != nullptr)
        {
            ++w->blocked;
        }
    }

    void noteChase(const uint32 charid)
    {
        if (auto* w = watchOf(charid); w != nullptr)
        {
            ++w->chases;
        }
    }

    void noteStuck(const uint32 charid)
    {
        if (auto* w = watchOf(charid); w != nullptr)
        {
            ++w->stuck;
        }
    }

    void noteRest(const uint32 charid)
    {
        if (auto* w = watchOf(charid); w != nullptr)
        {
            ++w->rests;
        }
    }
} // namespace pawn::drill
