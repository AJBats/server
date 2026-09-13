/*
===========================================================================

  Cardian: the seat ladder (ROADMAP H, the seat waterfall).

  One sorted container of every cardian who could hold a body. The
  standing cap and the faded cap are two thresholds on its index, and the
  ladder is the only thing that stands or fades an entry: the rest of the
  code changes what a cardian IS (offer, touch, withdraw), and run()
  changes where she stands.

    Stored, pushed by the code that knows it changed:  zone, tier, owner, down
    Stamped by the ladder:                             seq (re-stamped on a change of tier or down, never of zone)
    Looked up on every run:                            live, near, inParty
    Owned by run():                                    step, want, retryAfter

  Each run refreshes the looked-up facts, sorts the whole vector with the
  comparator, reads the two thresholds and issues the difference through
  the engine callbacks. Fades and sign-outs all go; stands are rationed,
  since each is a character load. There is no sorted insert: a few
  thousand entries sort in well under a millisecond, twice a second, and
  a sort on the clock is the sweep that a zone-line crossing needs, with
  no event to miss.

  Pure: no engine types. xi_test drives it with recording lambdas
  (src/test/tests/cardian_ladder_tests.cpp).

===========================================================================
*/

#pragma once

#include "common/cbasetypes.h"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <functional>
#include <map>
#include <tuple>
#include <unordered_map>
#include <vector>

namespace pawn::seats
{
    enum class Tier : uint8
    {
        Crowd   = 0, // one of the world's own, never met
        Partied = 1, // a wild cardian the player has had in a party
        Owned   = 2, // a linkshell recruit: a cardian_pawns row the player owns
        Alt     = 3, // a character of the player's own account
    };

    enum class Step : uint8
    {
        Absent   = 0, // no session row: search does not find her
        Faded    = 1, // a session row, no body: search finds her, she costs no tick
        Standing = 2, // a body: a targid, drawn by the client, costing a tick
    };

    // What she is. Pushed by the code that knows it changed: a seat dealt,
    // a recruit, a release, the world's KO timer
    struct Facts
    {
        uint16 zone  = 0;           // where she would stand: her seat's zone, or where the game saved her
        Tier   tier  = Tier::Crowd;
        uint32 owner = 0;           // the player she stands under; 0 for one of the world's own
        bool   down  = false;       // KO'd past the world's delay; the world's timer sets and clears it

        bool operator==(const Facts&) const = default;
    };

    struct Entry
    {
        uint32 charid = 0;
        Facts  facts;
        int64  seq = 0; // offers count down from 0, touches count up: a total order, equals resolved the same way every run

        // looked up on every run
        bool live    = false; // she may hold a body now: not down, no stand of hers waiting on a retry, and a player in her zone or next door -- or she is owned, and her player's wherever she is
        bool near    = false; // a player in her zone itself
        bool inParty = false; // in a real player's party

        // owned by run()
        Step                                  step = Step::Absent;
        Step                                  want = Step::Absent;
        std::chrono::steady_clock::time_point retryAfter{};
    };

    // Where the players are, and who is with one: derivable from memory
    // right now, so never stored
    struct Lookup
    {
        std::function<bool(uint16 zone)>   playerNear; // a real player in this zone or one next door
        std::function<bool(uint16 zone)>   playerIn;   // a real player in this zone
        std::function<bool(uint32 charid)> inParty;    // she is in a real player's party
    };

    // The four things the ladder can do to a cardian, and the only place
    // any of them is done
    struct Engine
    {
        std::function<bool(uint32 charid)> stand;   // give her a body; false if she could not
        std::function<void(uint32 charid)> fade;    // take the body, keep the session row
        std::function<void(uint32 charid)> signIn;  // write the session row, no body
        std::function<void(uint32 charid)> signOut; // drop the session row
    };

    using Before = std::function<bool(const Entry&, const Entry&)>; // a sorts above b

    class Ladder
    {
    public:
        // The order the user settled 2026-09-12: live first (nobody stands
        // where no one can see her), then in the party, then tier, then
        // whether a player is in her own zone, then seq
        static bool defaultOrder(const Entry& a, const Entry& b)
        {
            return std::tuple(a.live, a.inParty, a.facts.tier, a.near, a.seq) >
                   std::tuple(b.live, b.inParty, b.facts.tier, b.near, b.seq);
        }

        Ladder(Before before, Lookup lookup, Engine engine, const uint32 standsPerRun = 3, const std::chrono::steady_clock::duration retryDelay = std::chrono::seconds(30))
        : before(std::move(before))
        , lookup(std::move(lookup))
        , engine(std::move(engine))
        , standsPerRun(standsPerRun)
        , retryDelay(retryDelay)
        {
        }

        // The two thresholds. Standing is server-wide; faded is per zone
        // and counts standing bodies too. Taken literally: 0 means nobody
        void setCaps(const uint32 standing, const uint32 faded)
        {
            standingCap = standing;
            fadedCap    = faded;
        }
        auto caps() const -> std::pair<uint32, uint32>
        {
            return { standingCap, fadedCap };
        }

        // ---- writes: what she is -------------------------------------------

        // A new entry lands below her equals. Unchanged facts are a no-op,
        // so a tick re-offering everyone never shuffles the order. A change
        // of tier or down re-enters her class at the bottom; a change of
        // zone or owner keeps her place, so a body walked over a zone line
        // by her player is not shed for it. Until the next run she sits
        // wherever she is in the vector
        void offer(const uint32 charid, const Facts& facts)
        {
            if (auto* e = find(charid); e != nullptr)
            {
                if (e->facts == facts)
                {
                    return;
                }
                const bool reranked = e->facts.tier != facts.tier || e->facts.down != facts.down;
                e->facts            = facts;
                if (reranked)
                {
                    e->seq = --lowSeq;
                }
                return;
            }
            Entry e;
            e.charid = charid;
            e.facts  = facts;
            e.seq    = --lowSeq;
            entries.push_back(e);
        }

        // To the front of her equals: a recall, an invite
        void touch(const uint32 charid)
        {
            if (auto* e = find(charid); e != nullptr)
            {
                e->seq = ++highSeq;
            }
        }

        // Out of the waterfall for good: unseated, or her player logged
        // out. The body and the row go through the engine on the way
        void withdraw(const uint32 charid)
        {
            const auto it = std::ranges::find(entries, charid, &Entry::charid);
            if (it == entries.end())
            {
                return;
            }
            lower(*it, Step::Absent);
            entries.erase(it);
        }

        // ---- the run: where she stands ------------------------------------

        void run(const std::chrono::steady_clock::time_point now)
        {
            refresh(now);
            std::stable_sort(entries.begin(), entries.end(), before);

            // The thresholds. Standing: her index among the live entries is
            // under the cap, and standing wins over the faded cap. Faded: her
            // index among her zone's entries is under the faded cap
            uint32                              liveSeen = 0;
            std::unordered_map<uint16, uint32> zoneSeen;
            for (auto& e : entries)
            {
                const bool stands = e.live && liveSeen < standingCap;
                liveSeen += e.live ? 1 : 0;
                const uint32 zi = zoneSeen[e.facts.zone]++;
                e.want          = stands ? Step::Standing : (zi < fadedCap ? Step::Faded : Step::Absent);
            }

            // Down first, worst first, all of them: every one frees budget
            for (std::size_t i = entries.size(); i-- > 0;)
            {
                if (entries[i].want < entries[i].step)
                {
                    lower(entries[i], entries[i].want);
                }
            }
            // Then up, best first. Sign-ins are a row and all go; stands are
            // a character load and are rationed. A stand that fails waits
            // retryDelay and drops below her equals, so a body that cannot
            // stand never holds the line against one that can
            uint32 attempts = 0;
            for (auto& e : entries)
            {
                if (e.want <= e.step)
                {
                    continue;
                }
                if (e.want == Step::Faded)
                {
                    engine.signIn(e.charid);
                    e.step = Step::Faded;
                    continue;
                }
                if (attempts >= standsPerRun)
                {
                    continue;
                }
                ++attempts;
                if (engine.stand(e.charid))
                {
                    e.step = Step::Standing;
                }
                else
                {
                    e.retryAfter = now + retryDelay;
                    e.seq        = --lowSeq;
                }
            }
        }

        // ---- reads: as of the last run --------------------------------------

        auto indexOf(const uint32 charid) const -> int
        {
            const auto it = std::ranges::find(entries, charid, &Entry::charid);
            return it == entries.end() ? -1 : static_cast<int>(it - entries.begin());
        }

        // Her zone's entries above her
        auto zoneIndexOf(const uint32 charid) const -> int
        {
            int zi = 0;
            for (const auto& e : entries)
            {
                if (e.charid == charid)
                {
                    return zi;
                }
                const auto* mine = find(charid);
                if (mine != nullptr && e.facts.zone == mine->facts.zone)
                {
                    ++zi;
                }
            }
            return -1;
        }

        auto stepOf(const uint32 charid) const -> Step
        {
            const auto* e = find(charid);
            return e != nullptr ? e->step : Step::Absent;
        }
        auto wantOf(const uint32 charid) const -> Step
        {
            const auto* e = find(charid);
            return e != nullptr ? e->want : Step::Absent;
        }
        auto entryOf(const uint32 charid) const -> const Entry*
        {
            return find(charid);
        }

        // This player's cardians, all of them, in order
        auto ownedBy(const uint32 owner) const -> std::vector<uint32>
        {
            std::vector<uint32> hers;
            for (const auto& e : entries)
            {
                if (e.facts.owner == owner)
                {
                    hers.push_back(e.charid);
                }
            }
            return hers;
        }

        // The recall list: this player's cardians without a body
        auto belowTheLine(const uint32 owner) const -> std::vector<uint32>
        {
            std::vector<uint32> hers;
            for (const auto& e : entries)
            {
                if (e.facts.owner == owner && e.step != Step::Standing)
                {
                    hers.push_back(e.charid);
                }
            }
            return hers;
        }

        auto size() const -> std::size_t
        {
            return entries.size();
        }
        auto standing() const -> uint32
        {
            return static_cast<uint32>(std::ranges::count(entries, Step::Standing, &Entry::step));
        }
        // Per zone: how many stand, how many are online (standing and faded)
        auto zoneCounts() const -> std::map<uint16, std::pair<uint32, uint32>>
        {
            std::map<uint16, std::pair<uint32, uint32>> counts;
            for (const auto& e : entries)
            {
                auto& [stood, online] = counts[e.facts.zone];
                stood += e.step == Step::Standing ? 1 : 0;
                online += e.step != Step::Absent ? 1 : 0;
            }
            return counts;
        }

    private:
        auto find(const uint32 charid) -> Entry*
        {
            const auto it = std::ranges::find(entries, charid, &Entry::charid);
            return it == entries.end() ? nullptr : &*it;
        }
        auto find(const uint32 charid) const -> const Entry*
        {
            const auto it = std::ranges::find(entries, charid, &Entry::charid);
            return it == entries.end() ? nullptr : &*it;
        }

        void refresh(const std::chrono::steady_clock::time_point now)
        {
            std::unordered_map<uint16, std::pair<bool, bool>> zones; // near, in
            for (auto& e : entries)
            {
                auto [it, fresh] = zones.try_emplace(e.facts.zone);
                if (fresh)
                {
                    it->second = { lookup.playerNear(e.facts.zone), lookup.playerIn(e.facts.zone) };
                }
                e.near    = it->second.second;
                e.live    = !e.facts.down && now >= e.retryAfter && (e.facts.owner != 0 || it->second.first);
                e.inParty = lookup.inParty(e.charid);
            }
        }

        // Standing to faded, faded to absent, or both at once
        void lower(Entry& e, const Step to)
        {
            if (e.step == Step::Standing && to < Step::Standing)
            {
                engine.fade(e.charid);
            }
            if (e.step != Step::Absent && to == Step::Absent)
            {
                engine.signOut(e.charid);
            }
            e.step = to;
        }

        Before                                before;
        Lookup                                lookup;
        Engine                                engine;
        uint32                                standsPerRun;
        std::chrono::steady_clock::duration   retryDelay;
        uint32                                standingCap = 0;
        uint32                                fadedCap    = 0;
        std::vector<Entry>                    entries;
        int64                                 lowSeq  = 0;
        int64                                 highSeq = 0;
    };
} // namespace pawn::seats
