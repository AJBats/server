// Cardian: the cohorts in play and the town sign-in (pawn/cohort_play.h), their rules as plain
// functions: which cohorts are in play, whether a body is, and the towns' arithmetic -- each town's
// target, who is short, where the next sign-in goes and how many sign out.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace cardian::cohorts
{
    // A real character's job, as the rides table keys it (census.py cohort_rides)
    constexpr auto rideKey(const uint32_t charid, const uint8_t job) -> uint64_t
    {
        return (static_cast<uint64_t>(charid) << 8) | job;
    }

    // The cohorts in play: the one each job played now rides, a job being a real character online and
    // the main job he plays (census.py cohorts_in_play). A job at or under the newbie pool's levels
    // rides none
    inline auto activeOf(const std::unordered_map<uint64_t, uint32_t>& rides, std::span<const std::pair<uint32_t, uint8_t>> played)
        -> std::unordered_set<uint32_t>
    {
        std::unordered_set<uint32_t> active;
        for (const auto& [charid, job] : played)
        {
            if (const auto it = rides.find(rideKey(charid, job)); it != rides.end())
            {
                active.insert(it->second);
            }
        }
        return active;
    }

    // A body is in play while one of her cohorts is, her first job's or her second's (census.py
    // in_play); one of the shared pools always is
    inline auto inPlay(const std::string_view anchor, const uint32_t cohort, const uint32_t cohort2, const std::unordered_set<uint32_t>& active) -> bool
    {
        if ((anchor != "peer" && anchor != "rival") || cohort == 0)
        {
            return true;
        }
        return active.contains(cohort) || (cohort2 != 0 && active.contains(cohort2));
    }

    // -- the town sign-in -----------------------------------------------------------------------
    // Each city keeps the cohorts in play online there at its regulars -- the shared pools' bodies
    // online there -- times pawn.WORLD_COHORT_TOWN_SHARE: the seated (a town seat deals a cohort row
    // where no regular fits) and the signed in together. A town short of it signs bodies in; one more
    // than kSlack over it signs the excess out, of those the sign-in may sign out

    struct Town
    {
        uint16_t zone     = 0;
        uint32_t regulars = 0; // the shared pools' bodies online there
        uint32_t cohort   = 0; // the cohorts in play's bodies online there: seated and signed in
        uint32_t free     = 0; // of those, signed in and free to sign out: faded, in nobody's party, held for nobody, not away
        uint32_t room     = 0; // entries its faded cap still has room for
    };

    constexpr uint32_t kSlack = 5;

    inline auto targetOf(const uint32_t regulars, const double share) -> uint32_t
    {
        return share <= 0.0 ? 0 : static_cast<uint32_t>(std::lround(static_cast<double>(regulars) * share));
    }

    // Sign-ins the town takes now: up to its target, within its faded room
    inline auto shortfallOf(const Town& t, const double share) -> uint32_t
    {
        const auto target = targetOf(t.regulars, share);
        return target > t.cohort ? std::min(target - t.cohort, t.room) : 0;
    }

    // Sign-outs the town owes: what runs past its target and the slack, of the free
    inline auto excessOf(const Town& t, const double share) -> uint32_t
    {
        const auto line = targetOf(t.regulars, share) + kSlack;
        return t.cohort > line ? std::min(t.cohort - line, t.free) : 0;
    }

    // Where `count` sign-ins go, in order: each to the town with the largest shortfall then, a tie to
    // the lower zone id; fewer when every town is full
    inline auto planSignIns(std::vector<Town> towns, const double share, const uint32_t count) -> std::vector<uint16_t>
    {
        std::vector<uint16_t> out;
        while (out.size() < count)
        {
            Town*    best = nullptr;
            uint32_t most = 0;
            for (auto& t : towns)
            {
                const auto shortfall = shortfallOf(t, share);
                if (shortfall > most || (shortfall == most && shortfall > 0 && best != nullptr && t.zone < best->zone))
                {
                    best = &t;
                    most = shortfall;
                }
            }
            if (best == nullptr)
            {
                break;
            }
            out.push_back(best->zone);
            ++best->cohort;
            --best->room;
        }
        return out;
    }

    struct Totals
    {
        uint32_t towns     = 0;
        uint32_t regulars  = 0;
        uint32_t cohort    = 0;
        uint32_t target    = 0;
        uint32_t shortfall = 0;
    };

    inline auto totalsOf(std::span<const Town> towns, const double share) -> Totals
    {
        Totals out;
        for (const auto& t : towns)
        {
            out.towns += t.regulars > 0 || t.cohort > 0 ? 1 : 0;
            out.regulars += t.regulars;
            out.cohort += t.cohort;
            out.target += targetOf(t.regulars, share);
            out.shortfall += shortfallOf(t, share);
        }
        return out;
    }

    // The map log's line comes again once the sign-in's totals have moved this much since it last
    // said them, or a town's shortfall has opened or closed
    constexpr uint32_t kSayEvery = 25;

    inline auto worthSaying(const Totals& said, const uint32_t saidSignedIn, const Totals& now, const uint32_t signedIn) -> bool
    {
        const auto apart = [](const uint32_t a, const uint32_t b) { return a > b ? a - b : b - a; };
        return apart(saidSignedIn, signedIn) >= kSayEvery || apart(said.target, now.target) >= kSayEvery || apart(said.cohort, now.cohort) >= kSayEvery ||
               (said.shortfall == 0) != (now.shortfall == 0);
    }
} // namespace cardian::cohorts
