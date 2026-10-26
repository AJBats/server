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

#include "chat_codes.h"
#include "rest_math.h"

#include "common/utils.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// The puller (ROADMAP, Pulling, P1): at a camp the member in the Puller seat
// brings in one at a time the kinds of mob the player's Pull page has on,
// with a ranged attack or a Bard's song, and hands each to the tank. Pure rules, over plain numbers, so xi_test pins them
// (cardian_pull_tests.cpp); pawn_pull.cpp reads the world and asks.
namespace cardian::pull
{
    // ---- who hunts ------------------------------------------------------

    // With the party's order Pull, free roaming, every member the party's hunt
    // reaches starts fights as before (engage_math.h huntsForParty); at a camp
    // only the member in the Puller seat does, and with nobody in the seat the
    // camp holds. Hold starts nobody's
    constexpr auto hunts(const bool pullOrder, const bool staked, const bool pullerSeat, const bool huntsForParty) -> bool
    {
        return pullOrder && (staked ? pullerSeat : huntsForParty);
    }

    // ---- the con ----------------------------------------------------------

    // The /check of a mob (charutils.h EMobDifficulty, 0 too weak to 7
    // incredibly tough), in the game's own words
    constexpr auto conWord(const uint8_t check) -> std::string_view
    {
        constexpr std::array<std::string_view, 8> words{ "Too Weak", "Incredibly Easy Prey", "Easy Prey", "Decent Challenge",
                                                         "Even Match", "Tough", "Very Tough", "Incredibly Tough" };
        return check < words.size() ? words[check] : std::string_view("?");
    }

    // The con's auto-translate phrase (group 4 of the game's dictionary): its
    // entry, or 0 for Incredibly Easy Prey, which has none
    constexpr uint8_t kConGroup = 4;
    constexpr auto    conPhrase(const uint8_t check) -> uint8_t
    {
        constexpr std::array<uint8_t, 8> entries{ 2, 0, 6, 5, 1, 4, 7, 3 };
        return check < entries.size() ? entries[check] : 0;
    }

    // ---- the pull line ----------------------------------------------------

    // A mob's name as people read it: the game's underscores as spaces
    inline auto mobWords(std::string_view name) -> std::string
    {
        std::string out(name);
        std::ranges::replace(out, '_', ' ');
        return out;
    }

    // What the puller says in party chat as she tags her mob: "Incoming!",
    // the mob, its con as the game's own phrase (plain words, in brackets,
    // for the con that has none), and the player's call sound when he chose
    // one ("Incoming! Forest Hare {Easy prey} <call14>")
    inline auto pullLine(const std::string_view mob, const uint8_t check, const uint8_t call) -> std::string
    {
        std::string line = "Incoming! " + mobWords(mob) + " ";
        if (const auto entry = conPhrase(check); entry != 0)
        {
            line += chat::autoTranslate(kConGroup, entry);
        }
        else
        {
            line += "(" + std::string(conWord(check)) + ")";
        }
        if (const auto sound = chat::callBytes(call); !sound.empty())
        {
            line += " " + sound;
        }
        return line;
    }

    // ---- the tagging spot -------------------------------------------------

    // She shoots from a little inside her ranged attack's range, so a step of
    // the mob's roam does not put it out of reach as she aims
    constexpr float kShotMargin = 2.0f;
    constexpr auto  shotReach(const float range) -> float
    {
        return std::max(1.0f, range - kShotMargin);
    }

    // The spots round a mob she may shoot it from, as far off as her shot
    // reaches (the user, 2026-10-25): rings at her reach and every kSpotStep
    // yalms inside it, down to kSpotNearest, kSpotDirections round each, at
    // the mob's height. The farthest ring first; each ring's spots nearest
    // `from`, where she stands, first. The caller sets each on the ground and
    // asks its sight and the danger map
    constexpr int   kSpotDirections = 16;
    constexpr float kSpotStep       = 4.0f;
    constexpr float kSpotNearest    = 6.0f;
    constexpr float kSpotRoam       = 2.0f; // the mob roamed this far from where it stood as her spot was chosen: chosen again
    inline auto shotRings(const position_t& mob, const float reach, const position_t& from) -> std::vector<std::vector<position_t>>
    {
        std::vector<std::vector<position_t>> rings;
        for (float r = reach; r > 0.0f && (rings.empty() || r >= kSpotNearest); r -= kSpotStep)
        {
            std::vector<position_t> ring;
            ring.reserve(kSpotDirections);
            for (int i = 0; i < kSpotDirections; ++i)
            {
                const float a = static_cast<float>(i) * 6.2831853f / static_cast<float>(kSpotDirections);
                ring.emplace_back(mob.x + r * std::cos(a), mob.y, mob.z + r * std::sin(a), 0, 0);
            }
            std::ranges::stable_sort(ring, [&](const position_t& a, const position_t& b) { return distance(a, from) < distance(b, from); });
            rings.push_back(std::move(ring));
        }
        return rings;
    }

    // ---- the pull list ----------------------------------------------------

    // The Pull page (ROADMAP, Pulling): the kinds of mob the puller may
    // bring in, by name as the game holds it ("Forest_Hare"), in the
    // player's order of preference, each on or off. She pulls only what is
    // on, the first kind on the list that stands within reach first
    constexpr float kPullReach = 200.0f; // the page lists, and the puller picks, within this of the camp

    struct ListRow
    {
        std::string name;
        bool        on = false;

        auto operator==(const ListRow&) const -> bool = default;
    };

    // A kind's place among the rows that are on: 0 the most wanted; none
    // for a kind off or not on the list
    inline auto rankOf(const std::span<const ListRow> list, const std::string_view name) -> std::optional<std::size_t>
    {
        std::size_t rank = 0;
        for (const auto& row : list)
        {
            if (!row.on)
            {
                continue;
            }
            if (row.name == name)
            {
                return rank;
            }
            ++rank;
        }
        return std::nullopt;
    }

    inline auto anyOn(const std::span<const ListRow> list) -> bool
    {
        return std::ranges::any_of(list, [](const ListRow& r) { return r.on; });
    }

    // The page's rows as the player left them, then the rows he kept from
    // before that the page did not show (kinds of another camp), in their
    // old order: a list carries his preferences from camp to camp
    inline auto mergeList(const std::span<const ListRow> sent, const std::span<const ListRow> before) -> std::vector<ListRow>
    {
        std::vector<ListRow> out;
        const auto           has = [&](const std::string_view name) { return std::ranges::any_of(out, [&](const ListRow& r) { return r.name == name; }); };
        for (const auto& row : sent)
        {
            if (!row.name.empty() && !has(row.name))
            {
                out.push_back(row);
            }
        }
        for (const auto& row : before)
        {
            if (!row.name.empty() && !has(row.name))
            {
                out.push_back(row);
            }
        }
        return out;
    }

    // The list as cardian_orders.pull_list keeps it: "Forest_Hare:1|Tunnel_Worm:0"
    // (a mob's name has neither ':' nor '|')
    inline auto listText(const std::span<const ListRow> list) -> std::string
    {
        std::string out;
        for (const auto& row : list)
        {
            if (!out.empty())
            {
                out += '|';
            }
            out += row.name;
            out += row.on ? ":1" : ":0";
        }
        return out;
    }

    inline auto parseList(const std::string_view text) -> std::vector<ListRow>
    {
        std::vector<ListRow> out;
        std::size_t          at = 0;
        while (at < text.size())
        {
            const auto end   = std::min(text.find('|', at), text.size());
            const auto field = text.substr(at, end - at);
            const auto colon = field.rfind(':');
            if (colon != std::string_view::npos && colon > 0)
            {
                out.push_back({ std::string(field.substr(0, colon)), field.substr(colon + 1) == "1" });
            }
            at = end + 1;
        }
        return out;
    }

    // ---- the kinds round the camp -----------------------------------------

    // One mob the page sees: its name, its /check against the party's level
    // and how far it stands from the player
    struct Seen
    {
        std::string name;
        uint8_t     check    = 0;
        float       toPlayer = 0.0f;
    };

    // One kind, as a row of the page: every mob of the name round the camp
    struct Kind
    {
        std::string name;
        uint8_t     conLow   = 0; // the easiest and the toughest of them
        uint8_t     conHigh  = 0;
        uint8_t     count    = 0;
        float       distance = 0.0f; // their average distance from the player
        bool        on       = false;
    };

    // The page's rows: the kinds on his list in its order, then the others
    // nearest first. A kind of his list with none round the camp is left
    // out; it stays on his list (mergeList)
    inline auto kindsOf(const std::span<const Seen> seen, const std::span<const ListRow> list) -> std::vector<Kind>
    {
        struct Sum
        {
            Kind   kind;
            double total = 0.0;
        };
        std::vector<Sum> sums;
        for (const auto& s : seen)
        {
            auto it = std::ranges::find_if(sums, [&](const Sum& k) { return k.kind.name == s.name; });
            if (it == sums.end())
            {
                sums.push_back({ Kind{ s.name, s.check, s.check, 0, 0.0f, false } });
                it = sums.end() - 1;
            }
            it->kind.conLow  = std::min(it->kind.conLow, s.check);
            it->kind.conHigh = std::max(it->kind.conHigh, s.check);
            it->kind.count   = static_cast<uint8_t>(std::min<int>(255, it->kind.count + 1));
            it->total += s.toPlayer;
        }
        for (auto& s : sums)
        {
            s.kind.distance = s.kind.count > 0 ? static_cast<float>(s.total / s.kind.count) : 0.0f;
        }

        std::vector<Kind> out;
        for (const auto& row : list)
        {
            if (const auto it = std::ranges::find_if(sums, [&](const Sum& k) { return k.kind.name == row.name; }); it != sums.end())
            {
                out.push_back(it->kind);
                out.back().on = row.on;
            }
        }
        std::vector<Kind> rest;
        for (const auto& s : sums)
        {
            if (std::ranges::none_of(list, [&](const ListRow& r) { return r.name == s.kind.name; }))
            {
                rest.push_back(s.kind);
            }
        }
        std::ranges::stable_sort(rest, [](const Kind& a, const Kind& b) { return a.distance < b.distance; });
        out.insert(out.end(), rest.begin(), rest.end());
        return out;
    }

    // The level a con is read against: with Cardian's exp rule the party's
    // average level, rounded down, as the rule pays it; else the player's
    inline auto conLevel(const bool partyAverage, const uint8_t playerLevel, const std::span<const uint8_t> partyLevels) -> uint8_t
    {
        if (!partyAverage || partyLevels.empty())
        {
            return playerLevel;
        }
        unsigned total = 0;
        for (const auto level : partyLevels)
        {
            total += level;
        }
        return static_cast<uint8_t>(total / partyLevels.size());
    }

    // ---- the pipeline (pull P2; the user, 2026-10-16) ----------------------

    // The puller keeps the camp busy with no downtime: she plans the party's
    // whole conveyor. The next mob lands as the party's mages, resting from
    // the last fight, are a few rest ticks from having the MP it will cost
    // them, the party's missing HP cured besides -- a mage stays down into a
    // fight until the tank first needs a cure (kRestInto), unless the last
    // fight linked -- and never before the current fight ends: one mob
    // on the party at a time. She plans backward from there: she tags it so
    // it lands then, and sets out early enough to stand ready to tag it,
    // leaving the fight before it ends when the way is long (switched off,
    // kLeavesFightsEarly). Tuned to err aggressive (the user: "make the
    // healers SWEAT"), to be turned back once played.
    constexpr double kRestInto     = 15.0; // seconds a mage keeps kneeling into the next fight before she stands to cure
    constexpr double kUnknownCost  = 0.3;  // a mage's cost of a fight here before one is measured: this share of her MP
    constexpr double kAimSeconds   = 3.0;  // the shot or the song, and the mob turning onto her
    constexpr double kWayFactor    = 1.2;  // the way walked, against the straight line
    constexpr double kSlackSeconds = 3.0;  // she sets out this much early, to stand ready at the spot
    constexpr double kHoldSeconds  = 90.0; // ready at the spot, a shot held this long is given up
    constexpr double kWaitLead     = 45.0; // a mob she will wait near (aggro holds it) is set out for this much early: half the hold, so a way that opens at once holds her shot no longer than half its limit
    constexpr double kKinPassOverSeconds = 15.0; // a mob left for another while kin stood on its way back: not picked again for this long
    constexpr float  kYalmsPerPlace = 30.0f; // the pick: a kind a place lower on the list must stand this much nearer

    // The pick's walk score, lower first: the yalms to the mob, and
    // kYalmsPerPlace for each place her kind sits below the list's first --
    // a rabbit at the camp beats an orc 80 yalms out, an orc beats a rabbit
    // as far (CPawnController::PickHuntTarget)
    constexpr auto pickScore(const float yalms, const std::size_t rank) -> float
    {
        return yalms + static_cast<float>(rank) * kYalmsPerPlace;
    }
    // Leaving a fight early for the next pull is off (the user, 2026-10-16:
    // an advanced technique, to be tuned with care): her going cuts the
    // party's damage, so the fight runs past its estimate and she stood idle
    // at the spot. She finishes her fight, then sets out (OPEN_ISSUES #514)
    constexpr bool   kLeavesFightsEarly = false;
    // What the pipeline reads of one mage: her MP, what the next fight costs
    // her (what her own rest readies her for, a fight and a link's reserve;
    // kUnknownCost of her pool before one is measured), her spending while
    // the current fight lasts, her share of curing the party's missing HP
    // (`debt`, counted against her MP, which it may take below nothing), and
    // her kneel (rest_math.h's ticks: one every `interval`, the first paying
    // at the second)
    struct Mage
    {
        double mp             = 0.0;
        double maxMp          = 0.0;
        double need           = 0.0;
        double spendPerSecond = 0.0;
        double debt           = 0.0;
        bool   kneeling       = false;
        int    ticks          = 0;
        double nextTick       = 0.0;
        double interval       = 10.0;
        int    clearMind      = 0;
        int    mpHeal         = 0;
    };

    // The MP each curer owes for the party's missing HP: converted at the
    // party's own cure efficiency (bank_math.h Exchange) and split evenly
    // among the members who cure (the user, 2026-10-18); nothing with nobody
    // to cure it
    inline auto hpDebtEach(const double missingHp, const double hpPerMp, const std::size_t curers) -> double
    {
        return curers == 0 || hpPerMp <= 0.0 || missingHp <= 0.0 ? 0.0 : missingHp / hpPerMp / static_cast<double>(curers);
    }

    // Seconds from now until she can stand with what the next fight costs
    // her, her share of the party's missing HP cured besides: kneeling, her
    // kneel goes on, fight or not; standing with a fight on, she spends
    // through it and kneels as it ends; standing between fights, she kneels
    // now
    inline auto readyAt(const Mage& m, const double fightLeft) -> double
    {
        if (m.interval <= 0.0)
        {
            return 0.0;
        }
        // Her need and her debt together, never more than she can hold: a
        // debt she could not rest off at full MP would hold the pull for good
        const double target = m.maxMp > 0.0 ? std::min(m.need + m.debt, m.maxMp) : m.need + m.debt;
        if (m.kneeling)
        {
            return rest::timeToReady(target - m.mp, m.ticks, m.nextTick, m.interval, m.clearMind, m.mpHeal);
        }
        if (fightLeft > 0.0)
        {
            const double atEnd = std::max(0.0, m.mp - m.spendPerSecond * fightLeft);
            return fightLeft + rest::kKneelSeconds + rest::timeToReady(target - atEnd, 0, m.interval, m.interval, m.clearMind, m.mpHeal);
        }
        const double missing = target - m.mp;
        return missing <= 0.0 ? 0.0 : rest::kKneelSeconds + rest::timeToReady(missing, 0, m.interval, m.interval, m.clearMind, m.mpHeal);
    }

    // When the next mob should land, seconds from now, and the mage it waits
    // on (-1: none, the fight's end alone). `restInto`: how long a mage keeps
    // kneeling into it -- kRestInto, and none after a fight that linked
    struct Arrival
    {
        double in    = 0.0;
        int    pacer = -1;
    };
    inline auto wantArrive(const std::span<const Mage> mages, const double fightLeft, const double restInto = kRestInto) -> Arrival
    {
        Arrival out{ std::max(0.0, fightLeft), -1 };
        for (std::size_t i = 0; i < mages.size(); ++i)
        {
            const double at = readyAt(mages[i], fightLeft) - restInto;
            if (at > out.in)
            {
                out = { at, static_cast<int>(i) };
            }
        }
        return out;
    }

    // Seconds to cover a way at her pace (yalms a second), the way's bends counted
    inline auto runSeconds(const float yalms, const double yalmsPerSecond) -> double
    {
        return yalmsPerSecond > 0.0 ? std::max(0.0f, yalms) * kWayFactor / yalmsPerSecond : 1e9;
    }

    // Seconds until she sets out for a mob whose walk out, from where she
    // stands to where she tags it, is `out` yalms, and whose run back, from
    // there to the landing point, `back`: the mob should land `arriveIn`
    // from now. At or under zero, she goes
    inline auto leaveIn(const double arriveIn, const float out, const float back, const double yalmsPerSecond) -> double
    {
        return arriveIn - (runSeconds(out, yalmsPerSecond) + kAimSeconds + runSeconds(back, yalmsPerSecond)) - kSlackSeconds;
    }

    // Ready at the spot: seconds until she tags it, so it lands on time
    inline auto shootIn(const double arriveIn, const float back, const double yalmsPerSecond) -> double
    {
        return arriveIn - kAimSeconds - runSeconds(back, yalmsPerSecond);
    }

    // ---- the badge -------------------------------------------------------

    // What the puller's badge beside the party list shows (the Link's
    // PULL_STATE): nothing off the seat or with the party not pulling at a
    // camp; else waiting for the party, out after a mob, or bringing it in
    enum class Badge : uint8_t
    {
        None     = 0,
        Waiting  = 1,
        Out      = 2,
        Bringing = 3,
    };

    // ---- the run --------------------------------------------------------

    constexpr double kReshootSeconds  = 6.0;  // a shot that brought nothing is fired again after this
    constexpr uint8_t kMaxShots       = 3;    // and given up after this many
    constexpr double kPassOverSeconds = 90.0; // a mob given up is not picked again for this long
    constexpr float  kHomeYalms       = 1.0f; // within this of the point past the landing (pastLanding) she is back
    constexpr float  kRunPast         = 3.0f; // she runs this far past the landing point, so the mob chasing her stops on it (the user, 2026-10-16)
    constexpr double kQuietSeconds    = 3.0;  // a look round the camp that found nothing is made again after this
    constexpr float  kLeftBehindReach = 25.0f; // a mob the party leaves be (a worm) holds her next pull back while she stands this near it: its spells' reach
    constexpr double kShootSeconds    = 20.0; // in reach and in sight, a first shot or song refused this long: she gives the mob up

    // How long the walk out may take, and the run back, for a mob this far
    // from the camp: a margin, and the way at a slow run's pace (mobs stand
    // up to kPullReach out). Past it she gives the mob up, or the camp takes
    // it where it is
    constexpr double kRunPace = 4.0; // yalms a second, slower than her run: the way bends round things
    constexpr auto   outSeconds(const float yalms) -> double
    {
        return 20.0 + yalms / kRunPace;
    }
    constexpr auto backSeconds(const float yalms) -> double
    {
        return 25.0 + yalms / kRunPace;
    }

    // At the tagging spot: what to do with the mob
    enum class Shot : uint8_t
    {
        Fire,   // a shot now
        Wait,   // the last one is still in the air, or its answer not yet due
        Coming, // the mob is on the party: run it in
        GiveUp, // shot enough, and nothing came
    };

    // `fired` shots so far, the last `sinceLast` seconds ago; `onParty`: the
    // mob fights a member of hers
    constexpr auto shot(const uint8_t fired, const double sinceLast, const bool onParty) -> Shot
    {
        if (onParty)
        {
            return Shot::Coming;
        }
        if (fired == 0)
        {
            return Shot::Fire;
        }
        if (sinceLast < kReshootSeconds)
        {
            return Shot::Wait;
        }
        return fired >= kMaxShots ? Shot::GiveUp : Shot::Fire;
    }

    // How near a point comes to the straight way from `a` to `b`, across the
    // ground (height left out): the run back's line, against a mob that
    // might link as the chaser passes it
    inline auto segmentDistance(const position_t& a, const position_t& b, const position_t& p) -> float
    {
        const float dx  = b.x - a.x;
        const float dz  = b.z - a.z;
        const float len = dx * dx + dz * dz;
        const float t   = len > 0.0f ? std::clamp(((p.x - a.x) * dx + (p.z - a.z) * dz) / len, 0.0f, 1.0f) : 0.0f;
        const float cx  = a.x + t * dx - p.x;
        const float cz  = a.z + t * dz - p.z;
        return std::sqrt(cx * cx + cz * cz);
    }

    // Where she runs to with the mob on her: kRunPast beyond the landing
    // point, away from `from`, the side the mob comes in from -- a mob
    // chasing her stops a few yalms behind her, on the landing point, not
    // short of it. The mob's side, not hers: tagging from inside the camp,
    // she stands behind the landing point already (the review, 2026-10-17)
    inline auto pastLanding(const position_t& landing, const position_t& from) -> position_t
    {
        const float dx  = landing.x - from.x;
        const float dz  = landing.z - from.z;
        const float len = std::sqrt(dx * dx + dz * dz);
        if (len < 0.01f)
        {
            return landing;
        }
        return position_t(landing.x + dx / len * kRunPast, landing.y, landing.z + dz / len * kRunPast, 0, 0);
    }

    // On the way back: what has become of the pull
    enum class Back : uint8_t
    {
        Run,    // on her: she runs on
        Handed, // it turned to another member as it came in -- the tank's Provoke: hers is done
        Home,   // she is back at the landing point: the camp takes it from here
        Lost,   // it gave up the chase: nothing to bring
        Late,   // the run has taken too long: the camp takes it where it is
    };

    // `limit`: the run's allowance (backSeconds)
    constexpr auto back(const bool fighting, const bool onHer, const bool onParty, const float toLanding, const double runSeconds, const double limit) -> Back
    {
        if (!fighting || !onParty)
        {
            return Back::Lost;
        }
        if (!onHer)
        {
            return Back::Handed;
        }
        if (toLanding <= kHomeYalms)
        {
            return Back::Home;
        }
        return runSeconds > limit ? Back::Late : Back::Run;
    }

    // ---- a Bard's songs ----------------------------------------------------

    // A Bard pulls with a song, never a ranged weapon: her ranged slot is her
    // instrument. Foe Requiem from level 7 (it does damage over time, so a
    // sleep breaks on it); from the first Elegy, at 39, an Elegy, which does
    // none. The game's spell ids, the first she can sing taken
    constexpr std::array<uint16_t, 9> kPullSongs{
        422,                               // Carnage Elegy
        421,                               // Battlefield Elegy
        374, 373, 372, 371, 370, 369, 368, // Foe Requiem VII to I
    };
    template <typename CanSing>
    auto pullSong(CanSing&& canSing) -> uint16_t
    {
        for (const auto id : kPullSongs)
        {
            if (canSing(id))
            {
                return id;
            }
        }
        return 0;
    }

    // ---- the kit ----------------------------------------------------------

    // The game's skills of what she pulls with (xi::SkillType)
    constexpr uint8_t kArchery      = 25;
    constexpr uint8_t kMarksmanship = 26;
    constexpr uint8_t kThrowing     = 27;

    // A ranged-slot weapon or a stack of ammunition she carries in her
    // inventory or a wardrobe, where gear is worn from
    struct Held
    {
        uint16_t id       = 0;
        bool     ammo     = false; // the ammunition slot's; else the ranged slot's
        uint8_t  skill    = 0;     // kArchery, kMarksmanship, kThrowing; anything else (an instrument) shoots nothing
        uint8_t  subskill = 0;     // a crossbow's bolts are not a gun's bullets
        uint8_t  level    = 0;
        uint32_t count    = 0;     // ammunition: how many, over all her stacks of it
        bool     wearable = false; // her job and her level allow it
        bool     worn     = false; // in its slot now
        bool     planned  = false; // the census's pull kit for her
    };

    // What the map gives a puller with nothing of her own, first that her job
    // wears: a ranged weapon (0 for none) and its ammunition, topped up while
    // she holds the seat (pebbles for most jobs)
    struct Fallback
    {
        uint16_t ranged   = 0;
        uint16_t ammo     = 0;
        bool     wearable = false;
    };

    // The map's own, given to a body of the world alone -- the player's alts
    // and his own cardians pull with what they carry (the user, 2026-10-18)
    // -- in its order: a coarse boomerang, which comes back, for a Warrior, a
    // Thief or a Beastmaster from level 6; pebbles, which twelve of the
    // eighteen jobs throw; darts (a Red Mage, a Corsair from level 7); a
    // shortbow and stone arrows (a Paladin, a Samurai). A Puppetmaster, a
    // Blue Mage under 15 and a Corsair under 7 wear none of them; a Bard
    // pulls with a song (kPullSongs)
    struct Gift
    {
        uint16_t ranged = 0;
        uint16_t ammo   = 0;
    };
    constexpr uint16_t           kCoarseBoomerang = 17290;
    constexpr uint16_t           kPebble          = 17296;
    constexpr uint16_t           kDart            = 17307;
    constexpr uint16_t           kShortbow        = 17152;
    constexpr uint16_t           kStoneArrow      = 17330;
    constexpr std::array<Gift, 4> kGifts{ { { kCoarseBoomerang, 0 }, { 0, kPebble }, { 0, kDart }, { kShortbow, kStoneArrow } } };

    // What a puller with nothing to throw, shoot or sing says in party chat:
    // as she takes the seat, every time pulling is switched on, and as she
    // runs out (the user, 2026-10-18), one picked at random each time
    inline constexpr std::array<std::string_view, 5> kNoKitLines{
        "I can't be puller, I have nothing to pull with!",
        "I can't pull, I've got nothing to throw or shoot!",
        "Nothing to pull with, so I can't be puller!",
        "No bow, no ammo, nothing to throw... I can't pull!",
        "I'd pull, but I have nothing to pull with!",
    };

    // The kit: the ranged slot's weapon (0: that slot bare, for ammunition
    // thrown by itself) and the ammunition slot's (0: left as the game leaves
    // it, under a weapon thrown by itself); `given`, the map's fallback
    struct Kit
    {
        uint16_t ranged = 0;
        uint16_t ammo   = 0;
        bool     given  = false;

        auto operator==(const Kit&) const -> bool = default;
    };

    namespace detail
    {
        constexpr auto shoots(const uint8_t skill) -> bool
        {
            return skill == kArchery || skill == kMarksmanship;
        }

        // Ammunition for a bow or a gun: its skill, and for marksmanship its
        // kind (bolts for a crossbow, bullets for a gun)
        constexpr auto fits(const Held& ranged, const Held& ammo) -> bool
        {
            return ammo.ammo && ammo.count > 0 && ammo.skill == ranged.skill && (ranged.skill == kArchery || ammo.subskill == ranged.subskill);
        }
    } // namespace detail

    // Her pulling kit, the first of:
    //  1. what she wears already shoots -- a bow or a gun with its ammunition,
    //     a weapon thrown by itself, or ammunition thrown with no ranged
    //     weapon on (a Ranger's own gear);
    //  2. the census's pull kit for her, the same shapes;
    //  3. the best bow or gun she carries with ammunition for it, the higher
    //     level first, the larger stack on a tie;
    //  4. a weapon thrown by itself she carries (a boomerang);
    //  5. ammunition thrown by itself she carries (shuriken, darts, pebbles);
    //  6. the first fallback her job wears, the map's to give.
    // Only what she may wear counts. None: she has nothing to pull with
    inline auto chooseKit(const std::span<const Held> held, const std::span<const Fallback> fallbacks) -> std::optional<Kit>
    {
        using detail::fits;
        using detail::shoots;
        const auto pick = [&](const auto& among) -> std::optional<Kit>
        {
            for (const auto& r : held)
            {
                if (!among(r) || r.ammo || !r.wearable)
                {
                    continue;
                }
                if (r.skill == kThrowing)
                {
                    return Kit{ r.id, 0 };
                }
                if (!shoots(r.skill))
                {
                    continue;
                }
                for (const auto& a : held)
                {
                    if (among(a) && a.wearable && fits(r, a))
                    {
                        return Kit{ r.id, a.id };
                    }
                }
            }
            const bool rangedOn = std::ranges::any_of(held, [&](const Held& r) { return !r.ammo && among(r) && r.worn; });
            for (const auto& a : held)
            {
                // thrown by itself: worn, only with no ranged weapon on that it would have to share the shot with
                if (among(a) && a.ammo && a.wearable && a.skill == kThrowing && a.count > 0 && !(a.worn && rangedOn))
                {
                    return Kit{ 0, a.id };
                }
            }
            return std::nullopt;
        };

        if (const auto worn = pick([](const Held& h) { return h.worn; }); worn.has_value())
        {
            return worn;
        }
        if (const auto planned = pick([](const Held& h) { return h.planned; }); planned.has_value())
        {
            return planned;
        }

        // 3. the best bow or gun with ammunition for it
        std::optional<Kit> best;
        uint8_t            bestLevel = 0;
        uint32_t           bestCount = 0;
        for (const auto& r : held)
        {
            if (r.ammo || !r.wearable || !shoots(r.skill))
            {
                continue;
            }
            for (const auto& a : held)
            {
                if (!a.wearable || !fits(r, a))
                {
                    continue;
                }
                if (!best.has_value() || r.level > bestLevel || (r.level == bestLevel && a.count > bestCount))
                {
                    best      = Kit{ r.id, a.id };
                    bestLevel = r.level;
                    bestCount = a.count;
                }
            }
        }
        if (best.has_value())
        {
            return best;
        }

        // 4. a weapon thrown by itself, the highest level
        const Held* thrown = nullptr;
        for (const auto& r : held)
        {
            if (!r.ammo && r.wearable && r.skill == kThrowing && (thrown == nullptr || r.level > thrown->level))
            {
                thrown = &r;
            }
        }
        if (thrown != nullptr)
        {
            return Kit{ thrown->id, 0 };
        }

        // 5. ammunition thrown by itself, the highest level, the larger stack on a tie
        const Held* stones = nullptr;
        for (const auto& a : held)
        {
            if (a.ammo && a.wearable && a.skill == kThrowing && a.count > 0 &&
                (stones == nullptr || a.level > stones->level || (a.level == stones->level && a.count > stones->count)))
            {
                stones = &a;
            }
        }
        if (stones != nullptr)
        {
            return Kit{ 0, stones->id };
        }

        // 6. the map's own
        for (const auto& f : fallbacks)
        {
            if (f.wearable)
            {
                return Kit{ f.ranged, f.ammo, true };
            }
        }
        return std::nullopt;
    }

    // Ammunition topped up -- a fallback's for a puller, a body of the
    // world's planned ammunition while she is in a real player's party -- to
    // a stack (at most 99) once half of it has gone: how many to give
    constexpr auto ammoTopUp(const uint32_t have, const uint32_t stackSize) -> uint32_t
    {
        const uint32_t stack = std::clamp<uint32_t>(stackSize, 1, 99);
        return have * 2 <= stack ? stack - have : 0;
    }
} // namespace cardian::pull
