/*
===========================================================================

  Cardian: the party finder's rules (party_finder.h, RESEARCH.md §18.6),
  as plain functions: who hears a shout, how one who hears it answers,
  which of those in reach are heard, and in what order they speak.

  Pure: no engine types. xi_test pins them
  (src/test/tests/cardian_finder_tests.cpp).

===========================================================================
*/

#pragma once

#include "common/cbasetypes.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <vector>

namespace pawn::finder
{
    // What the player is recruiting for, and once she joins, her contract
    // (the user, 2026-09-13). Experience points is the easy ask -- anyone
    // the player's fame reaches -- and her affinity grows with the exp she
    // gains beside them; a quest asks more and pays affinity at its
    // completion; a mission draws whoever is on it, stranger or not, and is
    // the only recruitment under which a mission completed together counts
    // (the pearl's tally). Under the wrong contract nothing counts, and an
    // exp recruit hints in party chat that exp is what she came for
    struct Goal
    {
        enum class Kind : uint8
        {
            Experience,
            Mission,
            Quest,
        };
        Kind  kind = Kind::Experience;
        uint8 log  = 0; // the mission log or quest area

        auto operator==(const Goal&) const -> bool = default;
    };

    // Where her mission log stands against the player's current mission in
    // the goal's log: not that far (a no), on that very mission (a yes), past
    // it (she knows the way), or between missions and free to take it
    enum class MissionFit : uint8
    {
        Free,
        Behind,
        On,
        Done,
    };

    namespace rules
    {
        // ---- who hears ------------------------------------------------------

        // The nation logs are the mission logs 0 to 2 (MissionLog::Sandoria,
        // Bastok, Windurst), each its nation's number
        constexpr uint8 kLastNationLog = 2;

        // The yell reaches every city and town, wherever the player shouts
        // from (the user, 2026-10-02): a body there hears it, and one in the
        // field or a dungeon is busy and does not. A nation's own missions
        // are for its own people: nobody from another nation hears a shout
        // for them
        inline auto hears(const bool inCityOrTown, const uint8 nation, const Goal& goal) -> bool
        {
            if (!inCityOrTown)
            {
                return false;
            }
            return goal.kind != Goal::Kind::Mission || goal.log > kLastNationLog || nation == goal.log;
        }

        // ---- how she answers ------------------------------------------------

        // The level band: her level within FINDER_BAND of the player's main
        // level. Below it -1, inside 0, above it +1
        inline auto bandSide(const int playerLevel, const int herLevel, const int band) -> int
        {
            if (herLevel < playerLevel - band)
            {
                return -1;
            }
            if (herLevel > playerLevel + band)
            {
                return 1;
            }
            return 0;
        }

        // Her disposition is who the player is to the world, never who he is
        // to her: his fame in her nation (the game's level, 1 to 9) and his
        // rank in it against hers, plus a shout's mood (kMoodMin to kMoodMax,
        // rolled when the shout is made) against her seeded threshold.
        // Having partied with him and her affinity for him are not in it
        // (the user, 2026-10-02: new faces); they keep the friend seat and
        // the affinity seat (pickHeard). Calibrated so that someone the
        // player's fame has not reached (fame 1, his rank hers) says yes to
        // experience points one time in two, and to a quest about one in
        // three (13 in 40)
        constexpr int    kMoodMin       = -10;
        constexpr int    kMoodMax       = 10;
        constexpr uint32 kSeedSpread    = 40; // her own threshold, seed % this
        constexpr int    kThresholdBase = 16;
        constexpr int    kQuestAsk      = 7;

        inline auto score(const uint8 fame, const int rankDiff, const int mood) -> int
        {
            return 30 + mood + static_cast<int>(fame) * 5 + std::clamp(rankDiff * 5, -10, 15);
        }

        inline auto threshold(const uint32 seed, const int ask) -> int
        {
            return kThresholdBase + ask + static_cast<int>(seed % kSeedSpread);
        }

        // What the goal asks of her: a quest more than experience points; a
        // mission she has done, or is free to take, as little as experience
        // points (on the mission itself she needs no asking: willing)
        inline auto askOf(const Goal& goal) -> int
        {
            return goal.kind == Goal::Kind::Quest ? kQuestAsk : 0;
        }

        // Her yes or no, once nothing stops her outright (a party, a fight, a
        // camp, the band): on the player's very mission she comes whatever
        // his standing (the user, 2026-10-02: missions are open to
        // strangers); behind it she cannot; anything else is the score
        // against her threshold
        inline auto willing(const Goal& goal, const MissionFit fit, const uint8 fame, const int rankDiff, const int mood, const uint32 seed) -> bool
        {
            if (goal.kind == Goal::Kind::Mission)
            {
                if (fit == MissionFit::On)
                {
                    return true;
                }
                if (fit == MissionFit::Behind)
                {
                    return false;
                }
            }
            return score(fame, rankDiff, mood) >= threshold(seed, askOf(goal));
        }

        // ---- who is heard, and in what order --------------------------------

        // How many of those in reach hear one shout: a number drawn between
        // these, so a re-shout brings a crowd of its own. Of them at most
        // kCannotMax are people who cannot come whatever their mood -- out of
        // the level band, or short of the mission -- so the shout shows why
        // not without filling up with it: the whole world's towns hold far
        // more people out of the band than in it
        constexpr std::size_t kHeardMin  = 16;
        constexpr std::size_t kHeardMax  = 20;
        constexpr std::size_t kCannotMax = 3;

        // One in reach, as the seats see her
        struct Seat
        {
            std::size_t index    = 0;     // the caller's
            bool        yes      = false; // her answer to this shout
            bool        cannot   = false; // a no that is no matter of mood: out of the band, or short of the mission
            bool        friendly = false; // partied with the player before, or holds affinity for him
            uint32      affinity = 0;
            uint32      hops     = std::numeric_limits<uint32>::max(); // zone lines from the player; max when no line reaches
            int64       heardAt  = std::numeric_limits<int64>::min();  // when she last heard his shout; min for never
        };

        struct Heard
        {
            std::vector<Seat>          seats;        // in the order they speak: nearest first
            std::optional<std::size_t> friendSeat;   // the caller's index of the friend seat's holder
            std::optional<std::size_t> affinitySeat; // the same for the affinity seat
        };

        // The shout's crowd (the user, 2026-09-13 and 2026-10-02). `heard` of
        // the pool hear it, drawn at random, with two seats kept: one for a
        // friend -- the one longest unheard, so friendship shows on every
        // shout without crowding out new faces -- and one for whoever holds
        // the most affinity for the player after her; for either, one who
        // could come goes before one who cannot. Enough willing ones to fill
        // the party (`need`) when the pool has them, so one shout can form a
        // party; up to kCannotMax who cannot come, the kept seats included;
        // the rest a mix of those who could. Then nearest first, by zone lines from the player: the
        // nearer voices are heard first. A kept seat with nobody for it is
        // filled like the rest
        template <class Rng>
        auto pickHeard(std::vector<Seat> pool, const std::size_t heard, const std::size_t need, Rng& rng) -> Heard
        {
            Heard out;
            std::shuffle(pool.begin(), pool.end(), rng);

            // The best of the pool by `before`, among those `eligible`; the
            // shuffle settles ties
            const auto best = [&](const auto& eligible, const auto& before)
            {
                auto found = pool.end();
                for (auto it = pool.begin(); it != pool.end(); ++it)
                {
                    if (eligible(*it) && (found == pool.end() || before(*it, *found)))
                    {
                        found = it;
                    }
                }
                return found;
            };
            const auto take = [&](const std::vector<Seat>::iterator it)
            {
                out.seats.push_back(*it);
                pool.erase(it);
            };

            // The friend seat: a friend, longest unheard
            if (out.seats.size() < heard)
            {
                const auto it = best([](const Seat& s) { return s.friendly; },
                                     [](const Seat& a, const Seat& b)
                                     {
                                         return a.cannot != b.cannot ? !a.cannot : a.heardAt < b.heardAt;
                                     });
                if (it != pool.end())
                {
                    out.friendSeat = it->index;
                    take(it);
                }
            }

            // The affinity seat: the most affinity of the rest, longest unheard among equals
            if (out.seats.size() < heard)
            {
                const auto it = best([](const Seat& s) { return s.affinity > 0; },
                                     [](const Seat& a, const Seat& b)
                                     {
                                         if (a.cannot != b.cannot)
                                         {
                                             return !a.cannot;
                                         }
                                         return a.affinity != b.affinity ? a.affinity > b.affinity : a.heardAt < b.heardAt;
                                     });
                if (it != pool.end())
                {
                    out.affinitySeat = it->index;
                    take(it);
                }
            }

            // Takes from the pool, in the shuffle's order, up to `limit` that
            // `wanted` picks, never past the shout's size
            const auto takeWhile = [&](std::size_t limit, const auto& wanted)
            {
                for (auto it = pool.begin(); it != pool.end() && limit > 0 && out.seats.size() < heard;)
                {
                    if (wanted(*it))
                    {
                        out.seats.push_back(*it);
                        it = pool.erase(it);
                        --limit;
                    }
                    else
                    {
                        ++it;
                    }
                }
            };

            // Enough yeses to fill the party, the kept seats' own yeses counted
            const auto yesSoFar = static_cast<std::size_t>(std::count_if(out.seats.begin(), out.seats.end(), [](const Seat& s) { return s.yes; }));
            takeWhile(need > yesSoFar ? need - yesSoFar : 0, [](const Seat& s) { return s.yes; });

            // Those who cannot come, a few, a kept seat that went to one of
            // them counted among the few
            const auto cannotSoFar = static_cast<std::size_t>(std::count_if(out.seats.begin(), out.seats.end(), [](const Seat& s) { return s.cannot; }));
            takeWhile(kCannotMax > cannotSoFar ? kCannotMax - cannotSoFar : 0, [](const Seat& s) { return s.cannot; });

            // The rest a mix of those who could, from a fresh shuffle: the
            // sure yeses were the first in the old order, so what came
            // before them there is all nos
            std::shuffle(pool.begin(), pool.end(), rng);
            takeWhile(heard, [](const Seat& s) { return !s.cannot; });

            // Nearest first; among equals the shuffle's order, so the kept
            // seats and the sure yeses do not always speak first
            std::shuffle(out.seats.begin(), out.seats.end(), rng);
            std::stable_sort(out.seats.begin(), out.seats.end(), [](const Seat& a, const Seat& b) { return a.hops < b.hops; });
            return out;
        }
    } // namespace rules
} // namespace pawn::finder
