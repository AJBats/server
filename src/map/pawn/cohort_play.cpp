/*
===========================================================================

  Cardian: the cohorts in play, and the town sign-in. See cohort_play.h.

===========================================================================
*/

#include "cohort_play.h"

#include "cohort_play_math.h"
#include "players.h"
#include "seats.h"
#include "world.h"

#include "common/database.h"
#include "common/logging.h"
#include "common/settings.h"
#include "common/timer.h"
#include "entities/char_entity.h"
#include "utils/zoneutils.h"
#include "zone.h"

#include <magic_enum/magic_enum.hpp>

#include <algorithm>
#include <chrono>
#include <map>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace
{
    namespace math = cardian::cohorts;

    // The cohorts in play as of the last refresh
    std::unordered_set<uint32> activeNow;
    bool                       refreshed = false;

    // A cohort by its anchor, for the map log: "Jevyak's MNK"
    auto cohortName(const uint32 cohort) -> std::string
    {
        const auto job = static_cast<xi::Job>(cohort % 100);
        return fmt::format("{}'s {}", pawn::seats::nameOf(cohort / 100), magic_enum::enum_name(job));
    }

    auto cohortsText(const std::vector<uint32>& cohorts) -> std::string
    {
        std::string text;
        for (const auto cohort : cohorts)
        {
            text += fmt::format("{}{} ({})", text.empty() ? "" : ", ", cohort, cohortName(cohort));
        }
        return text.empty() ? std::string("no cohort") : text;
    }

    auto sorted(const std::unordered_set<uint32>& set) -> std::vector<uint32>
    {
        std::vector<uint32> out(set.begin(), set.end());
        std::ranges::sort(out);
        return out;
    }

    // -- the town sign-in's pacing and its line in the map log -----------------------------------

    realtime::time_point townAt{};
    constexpr auto       kTownEvery = std::chrono::milliseconds(250);
    // Sign-ins a run at most: the placement budget's bucket holds a quarter second's worth
    // (world.cpp placeBudget), so a run every quarter second spends about the budget's rate
    constexpr uint32 kSignInsPerRun = 5;
    // Sign-outs a run at most: a row each, and a cohort leaving play signs out hundreds
    constexpr uint32 kSignOutsPerRun = 10;

    math::Totals        said{};
    uint32              saidSignedIn = 0;
    std::vector<uint32> saidCohorts;

    std::unordered_map<uint16, bool> cityCache;

    auto isCity(const uint16 zone) -> bool
    {
        if (const auto it = cityCache.find(zone); it != cityCache.end())
        {
            return it->second;
        }
        auto*      PZone = zoneutils::GetZone(static_cast<xi::ZoneId>(zone));
        const bool city  = PZone != nullptr && (PZone->GetTypeMask() & xi::ZoneType::City) != xi::ZoneType::Unknown;
        cityCache.emplace(zone, city);
        return city;
    }
} // namespace

namespace pawn::cohorts
{
    void ensureTable()
    {
        db::preparedStmt("CREATE TABLE IF NOT EXISTS `cardian_cohort_rides` ("
                         "`charid` int(10) unsigned NOT NULL, "
                         "`job` tinyint(3) unsigned NOT NULL, "
                         "`cohort` int(10) unsigned NOT NULL, "
                         "PRIMARY KEY (`charid`, `job`)) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_general_ci");
    }

    void refresh()
    {
        // Each riding character's main job as saved, for one between bodies at a zone line: played
        // by his session, his body not loaded yet
        std::unordered_map<uint64, uint32> rides;
        std::unordered_map<uint32, uint8>  savedJob;
        if (const auto rset = db::preparedStmt("SELECT r.charid, r.job, r.cohort, COALESCE(s.mjob, 0) AS mjob FROM cardian_cohort_rides r "
                                               "LEFT JOIN char_stats s ON s.charid = r.charid");
            rset)
        {
            while (rset->next())
            {
                const auto charid = rset->get<uint32>("charid");
                rides[math::rideKey(charid, rset->get<uint8>("job"))] = rset->get<uint32>("cohort");
                savedJob[charid]                                      = rset->get<uint8>("mjob");
            }
        }
        // His live main job: the one he plays now, not the one last saved. Played is a client's session
        // under his charid; one of his characters standing as a cardian plays nothing
        std::vector<std::pair<uint32, uint8>> played;
        for (const auto& [charid, job] : savedJob)
        {
            if (!pawn::players::hasClient(charid))
            {
                continue;
            }
            const auto* PChar = zoneutils::GetChar(charid);
            played.emplace_back(charid, PChar != nullptr ? static_cast<uint8>(PChar->GetMJob()) : job);
        }
        auto now = math::activeOf(rides, played);
        if (!refreshed || now != activeNow)
        {
            const auto list = sorted(now);
            if (refreshed || !list.empty())
            {
                ShowInfoFmt("world: in play now: {}{}", cohortsText(list), refreshed ? fmt::format(" (was {})", cohortsText(sorted(activeNow))) : "");
            }
        }
        activeNow = std::move(now);
        refreshed = true;
    }

    auto active() -> const std::unordered_set<uint32>&
    {
        return activeNow;
    }

    auto inPlay(const std::string& anchor, const uint32 cohort, const uint32 cohort2) -> bool
    {
        return math::inPlay(anchor, cohort, cohort2, activeNow);
    }

    void townTick()
    {
        const auto now = realtime::now();
        if (townAt != realtime::time_point{} && now - townAt < kTownEvery)
        {
            return;
        }
        townAt = now;
        pawn::world::keepResidents();
        const double share = settings::get<float>("pawn.WORLD_COHORT_TOWN_SHARE");
        if (!pawn::world::isEnabled() || share <= 0.0)
        {
            return;
        }

        // The towns as the world holds them: each city's regulars and the cohorts in play online
        // there, and of those the sign-ins free to go; a sign-in whose cohort left play, wherever
        std::map<uint16, math::Town>             towns;
        std::map<uint16, std::vector<uint32>>    freeIn;
        std::vector<uint32>                      outOfPlay;
        uint32                                   signedIn = 0;
        for (const auto& p : pawn::world::presences())
        {
            if (p.signedIn && p.free && p.pool == pawn::world::Pool::Unplayed)
            {
                outOfPlay.push_back(p.charid);
                continue;
            }
            if (!isCity(p.zone))
            {
                continue;
            }
            auto& t = towns[p.zone];
            t.zone  = p.zone;
            if (p.pool == pawn::world::Pool::Regular)
            {
                ++t.regulars;
            }
            else if (p.pool == pawn::world::Pool::Cohort)
            {
                ++t.cohort;
                signedIn += p.signedIn ? 1 : 0;
                if (p.free)
                {
                    ++t.free;
                    freeIn[p.zone].push_back(p.charid);
                }
            }
        }
        const auto sizes = pawn::seats::zoneSizes();
        const auto cap   = pawn::seats::fadedCapNow();
        for (auto& [zone, t] : towns)
        {
            const auto   it   = sizes.find(zone);
            const uint32 used = it != sizes.end() ? it->second : 0;
            t.room            = cap > used ? cap - used : 0;
        }

        // Out: every sign-in whose cohort left play, then each town's excess past its target and the
        // slack, the newest minted first
        uint32 outs = 0;
        for (const auto charid : outOfPlay)
        {
            if (outs >= kSignOutsPerRun)
            {
                break;
            }
            pawn::world::signOutTown(charid);
            ++outs;
        }
        for (auto& [zone, t] : towns)
        {
            auto& mine   = freeIn[zone];
            auto  excess = math::excessOf(t, share);
            std::ranges::sort(mine);
            while (excess > 0 && !mine.empty() && outs < kSignOutsPerRun)
            {
                pawn::world::signOutTown(mine.back());
                mine.pop_back();
                --excess;
                --t.cohort;
                --t.free;
                --signedIn;
                ++t.room;
                ++outs;
            }
        }

        // In: once every seat waiting is placed, the cohorts in play's bodies the world holds nothing
        // of, each to the town short the most, within the placement budget
        std::vector<math::Town> list;
        list.reserve(towns.size());
        for (const auto& [zone, t] : towns)
        {
            list.push_back(t);
        }
        if (math::totalsOf(list, share).shortfall > 0 && !activeNow.empty() && !pawn::world::placing())
        {
            const auto candidates = pawn::world::signInCandidates();
            size_t     next       = 0;
            for (const auto zone : math::planSignIns(list, share, static_cast<uint32>(std::min<size_t>(candidates.size(), kSignInsPerRun))))
            {
                if (!pawn::world::takePlacement())
                {
                    break;
                }
                // one who cannot sign in (claimed since the read, never finished) gives the place to the next
                bool in = false;
                while (!in && next < candidates.size())
                {
                    in = pawn::world::signInTown(candidates[next++], zone);
                }
                if (!in)
                {
                    break;
                }
                auto& t = towns[zone];
                ++t.cohort;
                --t.room;
                ++signedIn;
            }
            list.clear();
            for (const auto& [zone, t] : towns)
            {
                list.push_back(t);
            }
        }

        // The map log's line, when the totals have moved enough to say, or the cohorts in play changed
        const auto totals  = math::totalsOf(list, share);
        const auto cohorts = sorted(activeNow);
        if (math::worthSaying(said, saidSignedIn, totals, signedIn) || cohorts != saidCohorts)
        {
            if (!cohorts.empty() || signedIn > 0 || saidSignedIn > 0)
            {
                ShowInfoFmt("world: town sign-in: {} signed in across {} towns for {}; the towns hold {} of the shared pools and {} of the cohorts in play "
                            "({} seated), against a target of {} ({} short)",
                            signedIn, totals.towns, cohortsText(cohorts), totals.regulars, totals.cohort, totals.cohort - std::min(totals.cohort, signedIn),
                            totals.target, totals.shortfall);
            }
            said         = totals;
            saidSignedIn = signedIn;
            saidCohorts  = cohorts;
        }
    }
} // namespace pawn::cohorts
