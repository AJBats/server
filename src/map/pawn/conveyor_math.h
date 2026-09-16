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

#pragma once

// The conveyor's arithmetic (RESEARCH §12.12 item 2, §12.14): a row or the
// role says "I would cast this", requests merge into needs, a cast in
// flight is a locked need whether or not anyone asked for it, and each
// need is assigned one caster, who reads it in her slot's order. Pure and
// entity-free, so cardian_conveyor_tests.cpp can hold it to account;
// conveyor.cpp feeds it and asks the game for the rest.

#include "fight_math.h"

#include "common/cbasetypes.h"

#include <fmt/format.h>

#include <algorithm>
#include <compare>
#include <cstddef>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace cardian::tactics
{
    // What merges into one need, the same kind on the same target: a cure
    // on a member, whatever the tier; a status spell by the family it
    // writes (Dia and Dia II are one need, Paralyze another); a -na by its
    // spell; damage and the rest never merge across casters, so their key
    // carries the caster and they are hers alone
    enum class NeedKind : uint8
    {
        Cure,
        Status,
        Na,
        Damage,
        Other,
    };

    struct NeedKey
    {
        NeedKind kind   = NeedKind::Other;
        uint32   arg    = 0; // the family, the spell, or the caster for damage and the rest
        uint32   target = 0;

        auto operator==(const NeedKey&) const -> bool = default;
    };

    // Who fed a request: a row names its mage; the role names its holder
    // but prefers nobody, and the bank assigns; the reflex is the role's
    // one shortcut, a member about to die
    enum class Source : uint8
    {
        Row,
        Role,
        Reflex,
    };

    struct Request
    {
        Source      source = Source::Row;
        uint32      caster = 0;   // the row's mage, or the holder who fed it
        uint32      row    = 0;   // the row's 1-based index: the order among her rows
        std::string rowId;        // the row's identifier, for the retry stamp
        uint16      spell  = 0;   // a specific spell; 0 leaves the tier to the bank
        double      fedAt  = 0.0;
        double      score  = 0.0; // the role's order among its own, lower first: a cure's HP margin, a debuff's cost less its worth
        double      landChance = 1.0; // the feeder's own chance to land it
        std::string why;          // "48 missing", "worth 12 MP against 6"

        // The same voice: a re-feed replaces it
        auto same(const Request& o) const -> bool
        {
            return source == o.source && caster == o.caster && row == o.row;
        }
    };

    struct Need
    {
        NeedKey              key;
        std::vector<Request> requests;

        // The schedule's word
        uint32 lockedBy = 0; // a cast in flight matches: nobody is assigned meanwhile
        uint32 assigned = 0;
        uint16 spell    = 0; // what the assigned caster casts

        // The caster the need prefers: the mage of the first row fed
        auto preferred() const -> uint32
        {
            for (const auto& r : requests)
            {
                if (r.source == Source::Row)
                {
                    return r.caster;
                }
            }
            return 0;
        }

        auto reflex() const -> bool
        {
            return std::any_of(requests.begin(), requests.end(), [](const Request& r)
                               {
                                   return r.source == Source::Reflex;
                               });
        }

        auto held() const -> bool
        {
            return lockedBy == 0 && assigned == 0;
        }

        auto fedBy(const uint32 caster) const -> bool
        {
            return std::any_of(requests.begin(), requests.end(), [&](const Request& r)
                               {
                                   return r.caster == caster;
                               });
        }

        auto rowFed() const -> bool
        {
            return preferred() != 0;
        }

        // The request that speaks for the need, one rule for every reader:
        // the reflex; else the preferred caster's lowest row; else another's
        // lowest row; else the role's best score. A need with no requests
        // (a cast in flight nobody asked for) speaks with an empty one
        auto lead() const -> const Request&
        {
            static const Request none;
            const Request*       best = nullptr;
            for (const auto& r : requests)
            {
                if (best == nullptr || rank(r) < rank(*best))
                {
                    best = &r;
                }
            }
            return best != nullptr ? *best : none;
        }

        // The spell the preferred caster's lowest row named; 0 when the
        // bank picks the tier
        auto askedSpell() const -> uint16
        {
            const uint32   who  = preferred();
            const Request* best = nullptr;
            for (const auto& r : requests)
            {
                if (r.source == Source::Row && r.caster == who && (best == nullptr || r.row < best->row))
                {
                    best = &r;
                }
            }
            return best != nullptr ? best->spell : 0;
        }

    private:
        auto rank(const Request& r) const -> std::pair<int, double>
        {
            switch (r.source)
            {
                case Source::Reflex:
                    return { 0, r.score };
                case Source::Row:
                    return { r.caster == preferred() ? 1 : 2, static_cast<double>(r.row) };
                case Source::Role:
                    return { 3, r.score };
            }
            return { 4, 0.0 };
        }
    };

    // --- the needs ------------------------------------------------------

    struct Needs
    {
        std::vector<Need> needs;

        auto find(const NeedKey& key) -> Need*
        {
            for (auto& n : needs)
            {
                if (n.key == key)
                {
                    return &n;
                }
            }
            return nullptr;
        }

        // A need for the key, opened if none: a cast in flight nobody asked
        // for is a need with no requests, held by its lock
        auto open(const NeedKey& key) -> Need&
        {
            if (Need* n = find(key); n != nullptr)
            {
                return *n;
            }
            needs.push_back(Need{ .key = key });
            return needs.back();
        }

        // Merge: the same voice re-feeding replaces its request, another
        // voice joins the need, a new key opens one
        auto feed(const NeedKey& key, Request r) -> Need&
        {
            Need& n = open(key);
            for (auto& have : n.requests)
            {
                if (have.same(r))
                {
                    have = std::move(r);
                    return n;
                }
            }
            n.requests.push_back(std::move(r));
            return n;
        }

        // A request not re-fed within its life is withdrawn; a need with
        // none left and no cast in flight is gone. A locked need keeps its
        // requests: the caster cannot re-feed while she casts, and the
        // rows that asked are owed their retry stamps when it lands
        void expire(const double now, const double life)
        {
            for (auto& n : needs)
            {
                if (n.lockedBy != 0)
                {
                    continue;
                }
                std::erase_if(n.requests, [&](const Request& r)
                              {
                                  return now - r.fedAt > life;
                              });
            }
            std::erase_if(needs, [](const Need& n)
                          {
                              return n.requests.empty() && n.lockedBy == 0;
                          });
        }

        void forget(const NeedKey& key)
        {
            std::erase_if(needs, [&](const Need& n)
                          {
                              return n.key == key;
                          });
        }
    };

    // --- the pick -------------------------------------------------------

    // One who could cast the need; the caller lists only the eligible: the
    // rows' mages, the holders who fed it, and for a need a row asked for,
    // every role holder
    struct Candidate
    {
        uint32 id         = 0;
        bool   open       = false; // free to cast it now: alive, not acting, past her last cast, no order queued, in range, the spell usable
        bool   kneeling   = false; // open, but after anyone standing (slice 5 owns the kneel; until then she casts from it as she always has)
        double landChance = 1.0;   // her chance to land it
        uint32 load       = 0;     // needs already assigned to her
        uint16 spell      = 0;     // what she would cast
    };

    // Among two who could: standing before kneeling, the preferred before
    // the rest, the better land chance, the lighter load, the lower id
    inline auto better(const Candidate& a, const Candidate& b, const uint32 preferred) -> bool
    {
        if (a.kneeling != b.kneeling)
        {
            return !a.kneeling;
        }
        if ((a.id == preferred) != (b.id == preferred))
        {
            return a.id == preferred;
        }
        if (a.landChance != b.landChance)
        {
            return a.landChance > b.landChance;
        }
        if (a.load != b.load)
        {
            return a.load < b.load;
        }
        return a.id < b.id;
    }

    // The best of the open candidates, null when none is: the need is held
    inline auto pickCaster(const Need& n, std::span<const Candidate> candidates) -> const Candidate*
    {
        const uint32     preferred = n.preferred();
        const Candidate* best      = nullptr;
        for (const auto& c : candidates)
        {
            if (c.open && (best == nullptr || better(c, *best, preferred)))
            {
                best = &c;
            }
        }
        return best;
    }

    // --- one caster's slot ----------------------------------------------

    // Where a need sits in one caster's slot: the reflex, then her own
    // rows in row order, then another's rows the bank handed her, then the
    // role's by score. That is "rows first, then the role"
    struct Rank
    {
        uint8  tier  = 4;
        double order = 0.0;

        auto operator<=>(const Rank&) const = default;
    };

    inline auto rankFor(const Need& n, const uint32 caster) -> Rank
    {
        Rank best;
        for (const auto& r : n.requests)
        {
            Rank rank;
            switch (r.source)
            {
                case Source::Reflex:
                    rank = { 0, r.score };
                    break;
                case Source::Row:
                    rank = { static_cast<uint8>(r.caster == caster ? 1 : 2), static_cast<double>(r.row) };
                    break;
                case Source::Role:
                    rank = { 3, r.score };
                    break;
            }
            best = std::min(best, rank);
        }
        return best;
    }

    // The needs assigned to her, as indices, in her slot's order; nothing
    // for nobody
    inline auto slotOrder(const std::vector<Need>& needs, const uint32 caster) -> std::vector<std::size_t>
    {
        std::vector<std::size_t> out;
        if (caster == 0)
        {
            return out;
        }
        for (std::size_t i = 0; i < needs.size(); ++i)
        {
            if (needs[i].assigned == caster && needs[i].lockedBy == 0)
            {
                out.push_back(i);
            }
        }
        std::stable_sort(out.begin(), out.end(), [&](const std::size_t a, const std::size_t b)
                         {
                             return rankFor(needs[a], caster) < rankFor(needs[b], caster);
                         });
        return out;
    }

    // --- the role's lines -----------------------------------------------

    // The HP a member keeps after the mob's biggest hit on record and what
    // she takes before a cure lands. Below zero, waiting is unsafe: the
    // reflex
    inline auto margin(const int32 hp, const double biggestHit, const double takenPerSecond, const double secondsToLand) -> double
    {
        return hp - biggestHit - takenPerSecond * secondsToLand;
    }

    // The role's efficiency line: the missing HP has piled up to what the
    // tier heals, so nothing overcures. In a fight the tier is the biggest
    // she has; between fights the smallest, a top-up
    inline auto cureWanted(const int32 missing, const int32 tierHeals) -> bool
    {
        return tierHeals > 0 && missing >= tierHeals;
    }

    // A mage's pace at a spot, one cycle per stretch of fighting: what the
    // fights cost her from the first open to the last close, against her
    // MP's net change from that close to the next open (rest, less what
    // she cast between). Behind once three cycles say spending beats
    // resting by a fifth; back on pace once resting beats spending. Said
    // once each way it turns
    struct Pace
    {
        Running spent;
        Running regained;
        int32   mpAtClose  = -1;
        bool    saidBehind = false;

        void opened(const int32 mp)
        {
            if (mpAtClose >= 0)
            {
                regained.fold(mp - mpAtClose);
            }
        }

        void closed(const int32 mp, const int32 spentThisCycle)
        {
            spent.fold(spentThisCycle);
            mpAtClose = mp;
        }

        auto measured() const -> bool
        {
            return spent.n >= 3 && regained.n >= 3;
        }

        auto behind() const -> bool
        {
            if (!measured())
            {
                return false;
            }
            return saidBehind ? spent.mean > regained.mean : spent.mean > regained.mean * 1.2 + 2.0;
        }

        // "Zapp: behind pace, ~30 MP a fight here, net -12 between fights"
        auto line(const std::string_view name) const -> std::string
        {
            return fmt::format("{}: {} pace, ~{:.0f} MP a fight here, net {:+.0f} between fights", name, behind() ? "behind" : "on", spent.mean, regained.mean);
        }
    };
} // namespace cardian::tactics
