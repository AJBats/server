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

#include "common/cbasetypes.h"

#include <glaze/yaml.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// The conquest simulation's arithmetic (RESEARCH §20.5-§20.7, §20.11): an invisible
// crowd -- nobody in the world, not even faded -- in every field and dungeon seat the
// slot tables author whose level band the world has reached, earns simulated
// experience under Signet, and one Vana'diel hour of it becomes what the game's own
// conquest messages carry: influence for each nation in each region, Signet kills and
// deaths for the beastmen. Each seat's nation is drawn every hour from the dials'
// shares for its region, never from the census, and the period's campaigns -- the thumb
// round Jeuno and each nation's focus -- move those shares. Everything here is pure, so the sum
// is tested with seats and dials in and influence out; the module that reads the
// tables and sends is conquest_sim.cpp.
//
// The dials are a Cardian file, modules/cardian/conquest.yaml, read again whenever it
// changes, so a dial turns on a running server. Its shape is files::DialFile; toDials
// checks every name in it.
namespace cardian::conquest
{

// The conquest regions, REGION_TYPE::RONFAURE (0) to TAVNAZIA (18), and the three nations,
// NATION_SANDORIA (0) to NATION_WINDURST (2), by the names the dials file uses
constexpr uint8 kRegions = 19;
constexpr uint8 kNations = 3;

constexpr std::array<std::string_view, kRegions> kRegionNames{
    "ronfaure", "zulkheim", "norvallen", "gustaberg", "derfland", "sarutabaruta", "kolshushu",
    "aragoneu", "fauregandi", "valdeaunia", "qufim", "litelor", "kuzotz", "vollbow",
    "elshimo_lowlands", "elshimo_uplands", "tulia", "movalpolos", "tavnazia",
};
constexpr std::array<std::string_view, kNations> kNationNames{ "sandoria", "bastok", "windurst" };

// Each nation's home region: Ronfaure, Gustaberg, Sarutabaruta
constexpr std::array<uint8, kNations> kHomeRegion{ 0, 3, 5 };

// A Vana'diel hour is a twenty-fifth of a real hour: the rates in the dials are per real hour
constexpr double kVanaHoursPerRealHour = 25.0;

// xi_world divides the points an M2W_AddInfluencePoints carries by ten before it adds them
// to the region (src/world/conquest_system.cpp), so a unit of influence is sent as ten points
constexpr int32 kPointsPerInfluence = 10;

inline auto regionByName(const std::string_view name) -> std::optional<uint8>
{
    for (uint8 r = 0; r < kRegions; ++r)
    {
        if (kRegionNames[r] == name)
        {
            return r;
        }
    }
    return std::nullopt;
}

inline auto nationByName(const std::string_view name) -> std::optional<uint8>
{
    for (uint8 n = 0; n < kNations; ++n)
    {
        if (kNationNames[n] == name)
        {
            return n;
        }
    }
    return std::nullopt;
}

} // namespace cardian::conquest

// The YAML reader reflects a file's struct by name, which MSVC allows only for a type
// with external linkage: the file's shape lives in a named namespace
namespace cardian::conquest::files
{
    struct Tide
    {
        double amplitude    = 0.20; // how far a nation's strength drifts from its dial, as a fraction
        double period_hours = 24.0; // how slowly: the drift's time constant, in real hours of game clock
    };
    // A hand on the scale: one nation's crowd in one region, times this
    struct Hand
    {
        std::string nation;
        std::string region;
        double      times = 1.0;
    };
    // The thumb round Jeuno: one nation favoured in these regions for `tallies` tallies, then
    // the next, the nation holding the fewest regions
    struct Thumb
    {
        std::vector<std::string> regions;
        double                   pull    = 1.0;
        uint32                   tallies = 4;
    };
    // Each nation's focus: `share` of the open outer regions, drawn anew every tally, each
    // pulled `pull` times, give or take `jitter` of it, growing from nothing at the tally to
    // its full pull at `ramp` of the period
    struct Focus
    {
        double share  = 0.0;
        double pull   = 1.0;
        double jitter = 0.0;
        double ramp   = 0.0;
    };
    // A crowd for the far regions, whose slot tables seat few or none: `seats` in each once
    // it is open, at the world's top level, on top of what its tables seat
    struct FarCrowd
    {
        std::vector<std::string> regions;
        uint32                   seats = 0;
    };
    struct DialFile
    {
        std::map<std::string, double>                   strength;           // by nation; a nation not named is 1
        double                                          home_bonus      = 2.0;
        double                                          next_pull       = 1.0;  // in the regions next to a nation's home
        std::map<std::string, std::vector<std::string>> next_regions;           // by nation: the regions next to its home
        std::map<std::string, uint32>                   outer;                  // the regions past them, by the world level that opens each
        Thumb                                           thumb;
        Focus                                           focus;
        FarCrowd                                        far_crowd;
        double                                          neighbour_bonus = 0.10; // for each bordering region the nation holds
        double                                          neighbour_cap   = 0.30;
        std::vector<Hand>                               hands;
        double                                          pressure = 0.05;    // simulated deaths per seat per real hour
        std::map<std::string, double>                   pressure_by_region; // a region's own, instead
        double                                          kills_per_seat_hour = 30.0;
        double                                          cp_rate             = 0.125; // the seats' conquest points, of their experience
        double                                          influence_share     = 0.5;   // of the conquest points, as influence
        Tide                                            tide;
        std::vector<std::array<double, 2>>              exp_per_hour;       // [level, experience a real hour], by level
        std::map<std::string, std::vector<std::string>> borders;            // land borders, from the zone lines
        std::vector<std::array<std::string, 2>>         sea;                // borders by ferry
    };
} // namespace cardian::conquest::files

namespace cardian::conquest
{

struct Dials
{
    std::array<double, kNations>                       strength{ 1.0, 1.0, 1.0 };
    double                                             homeBonus = 2.0;
    double                                             nextPull  = 1.0;
    std::array<std::array<bool, kRegions>, kNations>   nextRegion{}; // the regions next to each nation's home
    std::vector<uint8>                                 outer;        // the regions past them, in region order
    std::array<uint8, kRegions>                        opensAt{};    // an outer region's: the world level the nations start fighting there at
    std::array<bool, kRegions>                         thumbRegion{};
    double                                             thumbPull    = 1.0;
    uint32                                             thumbTallies = 4;
    double                                             focusShare   = 0.0; // of the open outer regions, each nation's picks a tally
    double                                             focusPull    = 1.0;
    double                                             focusJitter  = 0.0;
    double                                             focusRamp    = 0.0;
    std::vector<uint8>                                 farRegions;
    uint32                                             farSeats       = 0;
    double                                             neighbourBonus = 0.10;
    double                                             neighbourCap   = 0.30;
    std::array<std::array<double, kRegions>, kNations> hand{};     // 1 where no hand is set
    std::array<double, kRegions>                       pressure{}; // deaths per seat per real hour
    double                                             killsPerSeatHour = 30.0;
    double                                             cpRate           = 0.125;
    double                                             influenceShare   = 0.5;
    double                                             tideAmplitude    = 0.20;
    double                                             tidePeriodHours  = 24.0;
    std::vector<std::pair<uint8, double>>              expPerHour; // [level, experience a real hour], ascending
    std::array<std::vector<uint8>, kRegions>           borders{};  // land and sea, both ways
};

// Even dials: every multiplier 1, no hand, the given pressure everywhere, the borders empty
inline auto evenDials(const double pressure = 0.05) -> Dials
{
    Dials dials{};
    for (auto& row : dials.hand)
    {
        row.fill(1.0);
    }
    dials.pressure.fill(pressure);
    return dials;
}

inline void addBorder(Dials& dials, const uint8 a, const uint8 b)
{
    if (a == b)
    {
        return;
    }
    if (std::ranges::find(dials.borders[a], b) == dials.borders[a].end())
    {
        dials.borders[a].push_back(b);
    }
    if (std::ranges::find(dials.borders[b], a) == dials.borders[b].end())
    {
        dials.borders[b].push_back(a);
    }
}

// The most seats the far crowd may set in a region: each is a draw every hour, and the file
// is edited on a running server
constexpr uint32 kMostFarSeats = 1000;

// The file's text as the file's shape; nullopt, and why, when it does not parse
inline auto parseDials(const std::string_view text, std::string& error) -> std::optional<files::DialFile>
{
    constexpr glz::opts kDialYaml{ .error_on_unknown_keys = true, .error_on_missing_keys = false };
    files::DialFile     file{};
    if (const auto failed = glz::read_yaml<kDialYaml>(file, text); failed)
    {
        error = glz::format_error(failed, text);
        return std::nullopt;
    }
    return file;
}

// The file as dials; nullopt, and why, at the first name that is not a region or a
// nation, a negative rate, or an experience table out of level order
inline auto toDials(const files::DialFile& file, std::string& error) -> std::optional<Dials>
{
    Dials dials = evenDials(file.pressure);
    for (const auto& [name, value] : file.strength)
    {
        const auto nation = nationByName(name);
        if (!nation.has_value() || value < 0.0)
        {
            error = "strength: '" + name + "' is not a nation with a strength of 0 or more";
            return std::nullopt;
        }
        dials.strength[*nation] = value;
    }
    for (const auto& hand : file.hands)
    {
        const auto nation = nationByName(hand.nation);
        const auto region = regionByName(hand.region);
        if (!nation.has_value() || !region.has_value() || hand.times < 0.0)
        {
            error = "hands: '" + hand.nation + "' in '" + hand.region + "' is not a nation in a region, times 0 or more";
            return std::nullopt;
        }
        dials.hand[*nation][*region] = hand.times;
    }
    for (const auto& [name, value] : file.pressure_by_region)
    {
        const auto region = regionByName(name);
        if (!region.has_value() || value < 0.0)
        {
            error = "pressure_by_region: '" + name + "' is not a region with a pressure of 0 or more";
            return std::nullopt;
        }
        dials.pressure[*region] = value;
    }
    // A list of region names as regions; nullopt, and why, at the first that is not one
    const auto regionsOf = [&error](const std::string_view key, const std::vector<std::string>& names) -> std::optional<std::vector<uint8>>
    {
        std::vector<uint8> out;
        for (const auto& name : names)
        {
            const auto region = regionByName(name);
            if (!region.has_value())
            {
                error = std::string(key) + ": '" + name + "' is not a region";
                return std::nullopt;
            }
            if (std::ranges::find(out, *region) == out.end())
            {
                out.push_back(*region);
            }
        }
        return out;
    };
    for (const auto& [name, regions] : file.next_regions)
    {
        const auto nation = nationByName(name);
        if (!nation.has_value())
        {
            error = "next_regions: '" + name + "' is not a nation";
            return std::nullopt;
        }
        const auto listed = regionsOf("next_regions", regions);
        if (!listed.has_value())
        {
            return std::nullopt;
        }
        for (const auto region : *listed)
        {
            dials.nextRegion[*nation][region] = true;
        }
    }
    for (const auto& [name, level] : file.outer)
    {
        const auto region = regionByName(name);
        if (!region.has_value() || level > 255)
        {
            error = "outer: '" + name + "' is not a region opening at a level from 0 to 255";
            return std::nullopt;
        }
        dials.outer.push_back(*region);
        dials.opensAt[*region] = static_cast<uint8>(level);
    }
    std::ranges::sort(dials.outer);
    const auto thumbed = regionsOf("thumb", file.thumb.regions);
    if (!thumbed.has_value())
    {
        return std::nullopt;
    }
    const auto farCrowds = regionsOf("far_crowd", file.far_crowd.regions);
    if (!farCrowds.has_value())
    {
        return std::nullopt;
    }
    if (file.pressure < 0.0 || file.home_bonus < 0.0 || file.next_pull < 0.0 || file.thumb.pull < 0.0 || file.neighbour_bonus < 0.0 ||
        file.neighbour_cap < 0.0 || file.kills_per_seat_hour < 0.0 || file.cp_rate < 0.0 || file.influence_share < 0.0 || file.tide.amplitude < 0.0 ||
        file.tide.period_hours < 0.0)
    {
        error = "a rate, a bonus, a pull or the tide is below 0";
        return std::nullopt;
    }
    if (file.focus.share < 0.0 || file.focus.share > 1.0 || file.focus.pull <= 0.0 || file.focus.jitter < 0.0 || file.focus.jitter >= 1.0 ||
        file.focus.ramp < 0.0 || file.focus.ramp > 1.0)
    {
        error = "focus: share and ramp run from 0 to 1, jitter from 0 to under 1, and the pull is above 0";
        return std::nullopt;
    }
    if (file.thumb.tallies == 0 || file.far_crowd.seats > kMostFarSeats)
    {
        error = "thumb: tallies at least 1; far_crowd: seats at most " + std::to_string(kMostFarSeats);
        return std::nullopt;
    }
    dials.nextPull     = file.next_pull;
    dials.thumbPull    = file.thumb.pull;
    dials.thumbTallies = file.thumb.tallies;
    for (const auto region : *thumbed)
    {
        dials.thumbRegion[region] = true;
    }
    dials.focusShare       = file.focus.share;
    dials.focusPull        = file.focus.pull;
    dials.focusJitter      = file.focus.jitter;
    dials.focusRamp        = file.focus.ramp;
    dials.farRegions       = *farCrowds;
    dials.farSeats         = file.far_crowd.seats;
    dials.homeBonus        = file.home_bonus;
    dials.neighbourBonus   = file.neighbour_bonus;
    dials.neighbourCap     = file.neighbour_cap;
    dials.killsPerSeatHour = file.kills_per_seat_hour;
    dials.cpRate           = file.cp_rate;
    dials.influenceShare   = file.influence_share;
    dials.tideAmplitude    = file.tide.amplitude;
    dials.tidePeriodHours  = file.tide.period_hours;
    double lastLevel       = 0.0;
    for (const auto& [level, earned] : file.exp_per_hour)
    {
        if (level <= lastLevel || level > 255.0 || earned < 0.0)
        {
            error = "exp_per_hour: rows are [level, experience], levels rising from 1, experience 0 or more";
            return std::nullopt;
        }
        lastLevel = level;
        dials.expPerHour.emplace_back(static_cast<uint8>(level), earned);
    }
    for (const auto& [name, others] : file.borders)
    {
        const auto region = regionByName(name);
        if (!region.has_value())
        {
            error = "borders: '" + name + "' is not a region";
            return std::nullopt;
        }
        for (const auto& other : others)
        {
            const auto next = regionByName(other);
            if (!next.has_value())
            {
                error = "borders: '" + other + "', beside " + name + ", is not a region";
                return std::nullopt;
            }
            addBorder(dials, *region, *next);
        }
    }
    for (const auto& [a, b] : file.sea)
    {
        const auto ra = regionByName(a);
        const auto rb = regionByName(b);
        if (!ra.has_value() || !rb.has_value())
        {
            error = "sea: '" + a + "' and '" + b + "' are not two regions";
            return std::nullopt;
        }
        addBorder(dials, *ra, *rb);
    }
    return dials;
}

// The level the world's adventurers reach, as the census defines it (tools/world/census.py,
// world_top): the highest target of an unrecruited census row, and never under 18, the
// level the support job opens at
constexpr uint8 kWorldTopFloor = 18;

inline auto worldTop(const uint8 highestTarget) -> uint8
{
    return std::max(highestTarget, kWorldTopFloor);
}

// One slot of a slot table, as the crowd sees it: its region, its level band and how many
// seats it authors. Whether anybody sits in it does not matter
struct Slot
{
    uint8  region = 0;
    uint8  low    = 1;
    uint8  high   = 1;
    uint32 seats  = 0;
};

// The world has reached a slot once its top level stands at or above the slot's floor: the
// frontier follows the world's level
inline bool reached(const Slot& slot, const uint8 top)
{
    return slot.low <= top;
}

// The level its crowd earns at: the middle of its band, as far as the world has reached it
inline auto crowdLevel(const Slot& slot, const uint8 top) -> uint8
{
    const uint8 high = std::max(slot.low, std::min(slot.high, top));
    return static_cast<uint8>((slot.low + high) / 2);
}

// One seat of the crowd: where it sits and the level it earns at. Its nation is drawn each hour
struct Seat
{
    uint8 region = 0;
    uint8 level  = 1;
};

// The crowd of the slots the world has reached, a seat for each seat they author, in the
// conquest regions
inline auto crowd(const std::vector<Slot>& slots, const uint8 top) -> std::vector<Seat>
{
    std::vector<Seat> seats;
    for (const auto& slot : slots)
    {
        if (slot.region >= kRegions || !reached(slot, top))
        {
            continue;
        }
        seats.insert(seats.end(), slot.seats, Seat{ .region = slot.region, .level = crowdLevel(slot, top) });
    }
    return seats;
}

// Whether the nations fight over a region with the world at `top`: an outer region once the
// world's top level reaches the level that opens it; any other -- a home region and the region
// next to one -- always, from a fresh server's first day. A region not open has no crowd, so
// it is the beastmen's
inline auto inPlay(const Dials& dials, const uint8 region, const uint8 top) -> bool
{
    return std::ranges::find(dials.outer, region) == dials.outer.end() || top >= dials.opensAt[region];
}

// The crowd the nations field with the world at `top`: the seats of the slots it has reached
// in the regions open, and in each far region open, the far crowd at the world's top level
inline auto fieldCrowd(const std::vector<Slot>& slots, const Dials& dials, const uint8 top) -> std::vector<Seat>
{
    auto seats = crowd(slots, top);
    std::erase_if(seats, [&](const Seat& seat) { return !inPlay(dials, seat.region, top); });
    for (const auto region : dials.farRegions)
    {
        if (inPlay(dials, region, top))
        {
            seats.insert(seats.end(), dials.farSeats, Seat{ .region = region, .level = top });
        }
    }
    return seats;
}

// The experience one member of an even party at this level earns in a real hour, from the
// dials' table: straight lines between its rows, its first and last rows beyond them
inline auto expPerHour(const Dials& dials, const uint8 level) -> double
{
    const auto& table = dials.expPerHour;
    if (table.empty())
    {
        return 0.0;
    }
    if (level <= table.front().first)
    {
        return table.front().second;
    }
    for (size_t i = 1; i < table.size(); ++i)
    {
        if (level <= table[i].first)
        {
            const auto [lowLevel, lowExp]   = table[i - 1];
            const auto [highLevel, highExp] = table[i];
            const double t                  = static_cast<double>(level - lowLevel) / static_cast<double>(highLevel - lowLevel);
            return lowExp + t * (highExp - lowExp);
        }
    }
    return table.back().second;
}

// A period's campaigns (§20.12): the nation the thumb favours round Jeuno, each nation's
// focus -- the full pull of each outer region it picked this tally, 0 where it picked none
// -- and how far the focus has grown into the period, from 0 at the tally to 1. The
// default is no campaign at all
struct Campaign
{
    uint8                                              favoured = kNations; // none
    std::array<std::array<double, kRegions>, kNations> focus{};
    double                                             grown = 1.0;
};

// How far the focus has grown, hours into a period of periodHours: from nothing at the tally
// to its full pull at the ramp's fraction of the period, then held. A ramp of 0 is full at once
inline auto focusGrown(const int64 hoursInto, const int64 periodHours, const double ramp) -> double
{
    if (ramp <= 0.0 || periodHours <= 0)
    {
        return 1.0;
    }
    return std::clamp(static_cast<double>(hoursInto) / (ramp * static_cast<double>(periodHours)), 0.0, 1.0);
}

// How many open outer regions each nation focuses on: the dials' share of them, rounded, and
// at least one while any is open
inline auto focusPicks(const Dials& dials, const uint8 top) -> size_t
{
    const auto open = static_cast<size_t>(std::ranges::count_if(dials.outer, [&](const uint8 region) { return inPlay(dials, region, top); }));
    if (open == 0 || dials.focusShare <= 0.0)
    {
        return 0;
    }
    return std::clamp<size_t>(static_cast<size_t>(std::lround(dials.focusShare * static_cast<double>(open))), 1, open);
}

// Each nation's focus for a tally with the world at `top`: focusPicks of the open outer
// regions, apart from the other nations -- they may pick the same region or leave one to
// nobody -- each at the focus pull, give or take its jitter. Each nation ranks every outer
// region, open or not, in a random order of its own and takes the first open ones, so the
// same draws give the same picks, and a region the world opens mid-period takes its place
// in that order without reshuffling the rest. draw() answers a uniform number in [0, 1)
template <typename Draw>
inline auto drawFocus(const Dials& dials, const uint8 top, Draw&& draw) -> std::array<std::array<double, kRegions>, kNations>
{
    std::array<std::array<double, kRegions>, kNations> focus{};
    const auto                                         picks = focusPicks(dials, top);
    for (uint8 n = 0; n < kNations; ++n)
    {
        auto                         order = dials.outer;
        std::array<double, kRegions> jitter{};
        for (size_t i = 0; i < order.size(); ++i)
        {
            const auto left = order.size() - i;
            const auto j    = i + std::min(left - 1, static_cast<size_t>(draw() * static_cast<double>(left)));
            std::swap(order[i], order[j]);
            jitter[order[i]] = 1.0 + dials.focusJitter * (2.0 * draw() - 1.0);
        }
        size_t taken = 0;
        for (const auto region : order)
        {
            if (taken < picks && inPlay(dials, region, top))
            {
                focus[n][region] = dials.focusPull * jitter[region];
                ++taken;
            }
        }
    }
    return focus;
}

// Who the thumb favours next: of the nations it does not favour now, the one holding the
// fewest regions as of the last tally, a draw between equals. draw() answers [0, 1)
template <typename Draw>
inline auto nextFavoured(const uint8 favoured, const std::array<uint8, kRegions>& owners, Draw&& draw) -> uint8
{
    std::array<uint32, kNations> held{};
    for (const auto owner : owners)
    {
        if (owner < kNations)
        {
            ++held[owner];
        }
    }
    std::vector<uint8> fewest;
    for (uint8 n = 0; n < kNations; ++n)
    {
        if (n == favoured)
        {
            continue;
        }
        if (!fewest.empty() && held[n] < held[fewest.front()])
        {
            fewest.clear();
        }
        if (fewest.empty() || held[n] == held[fewest.front()])
        {
            fewest.push_back(n);
        }
    }
    return fewest[std::min(fewest.size() - 1, static_cast<size_t>(draw() * static_cast<double>(fewest.size())))];
}

// The thumb round Jeuno across tallies: the nation it favours (kNations: nobody yet), the
// period it last counted -- the tally closing it, as milliseconds of game clock -- and the
// tallies it has favoured that nation
struct Thumb
{
    uint8                favoured = kNations;
    std::optional<int64> period;
    uint32               held = 0;
};

// The thumb at a step `hoursInto` the period closing at `period`: a period it has not counted
// counts once, and after the dials' tallies the thumb passes on (nextFavoured), as a thumb
// that favours nobody yet takes its first. Only from the period's second hour: the owners
// it reads must be the tally's that opened the period, and xi_world's result can land a
// little after the map's first step. Answers whether it changed, so the caller saves it
template <typename Draw>
inline auto advanceThumb(Thumb& thumb, const int64 period, const int64 hoursInto, const uint32 tallies, const std::array<uint8, kRegions>& owners,
                         Draw&& draw) -> bool
{
    if (hoursInto < 1 || thumb.period == period)
    {
        return false;
    }
    if (thumb.favoured < kNations)
    {
        ++thumb.held;
    }
    if (thumb.favoured >= kNations || thumb.held >= tallies)
    {
        thumb.favoured = nextFavoured(thumb.favoured, owners, draw);
        thumb.held     = 0;
    }
    thumb.period = period;
    return true;
}

// A nation's weight in a region (§20.7, §20.12): its strength, drifted by the tide; the home
// bonus in its own home region, the next pull in a region next to it; the thumb's pull in
// a region round Jeuno while it favours the nation; its focus as far as it has grown; the
// neighbour bonus for each bordering region it holds (as of the last tally), to the cap;
// and any hand on the scale
inline auto weight(const Dials& dials, const uint8 nation, const uint8 region, const std::array<uint8, kRegions>& owners,
                   const std::array<double, kNations>& tide, const Campaign& campaign = {}) -> double
{
    double times = dials.strength[nation] * (1.0 + tide[nation]);
    if (kHomeRegion[nation] == region)
    {
        times *= dials.homeBonus;
    }
    if (dials.nextRegion[nation][region])
    {
        times *= dials.nextPull;
    }
    if (dials.thumbRegion[region] && campaign.favoured == nation)
    {
        times *= dials.thumbPull;
    }
    if (const double full = campaign.focus[nation][region]; full > 0.0)
    {
        times *= 1.0 + (full - 1.0) * campaign.grown;
    }
    uint32 held = 0;
    for (const auto other : dials.borders[region])
    {
        if (owners[other] == nation)
        {
            ++held;
        }
    }
    times *= 1.0 + std::min(dials.neighbourCap, static_cast<double>(held) * dials.neighbourBonus);
    return times * dials.hand[nation][region];
}

// The odds a seat of the region's crowd is of each nation this hour: each nation's weight
// over the three's sum. Even thirds when no nation has any weight
inline auto shares(const Dials& dials, const uint8 region, const std::array<uint8, kRegions>& owners, const std::array<double, kNations>& tide,
                   const Campaign& campaign = {}) -> std::array<double, kNations>
{
    std::array<double, kNations> out{};
    double                       total = 0.0;
    for (uint8 n = 0; n < kNations; ++n)
    {
        out[n] = weight(dials, n, region, owners, tide, campaign);
        total += out[n];
    }
    for (auto& share : out)
    {
        share = total > 0.0 ? share / total : 1.0 / kNations;
    }
    return out;
}

// The nation a draw in [0, 1) lands on under these shares
inline auto pick(const std::array<double, kNations>& odds, const double draw) -> uint8
{
    double below = 0.0;
    for (uint8 n = 0; n + 1 < kNations; ++n)
    {
        below += odds[n];
        if (draw < below)
        {
            return n;
        }
    }
    return static_cast<uint8>(kNations - 1);
}

// One Vana'diel hour of the crowd, by region: influence by nation in the conquest table's
// own units (before xi_world's catch-up for the trailing nations), and the beastmen's
// counters, Signet kills and deaths -- all fractions, which settle() carries over. With
// them, for the log: the seats, the shares they were drawn by and how the draw fell
struct Hour
{
    std::array<std::array<double, kNations>, kRegions> influence{};
    std::array<double, kRegions>                       kills{};
    std::array<double, kRegions>                       deaths{};
    std::array<uint32, kRegions>                       seats{};
    std::array<std::array<double, kNations>, kRegions> odds{};
    std::array<std::array<uint32, kNations>, kRegions> drawn{};
};

// Every seat earns a Vana'diel hour's share of a real hour's experience at its level; its
// conquest points at the seats' one flat rate, the influence share of them, go to the
// nation drawn for it this hour by its region's shares. Each seat also kills and dies at
// the dials' rates. draw() answers a uniform number in [0, 1), one per seat
template <typename Draw>
inline auto sumHour(const std::vector<Seat>& seats, const Dials& dials, const std::array<uint8, kRegions>& owners,
                    const std::array<double, kNations>& tide, Draw&& draw, const Campaign& campaign = {}) -> Hour
{
    Hour hour{};
    for (uint8 r = 0; r < kRegions; ++r)
    {
        hour.odds[r] = shares(dials, r, owners, tide, campaign);
    }
    for (const auto& seat : seats)
    {
        if (seat.region >= kRegions)
        {
            continue;
        }
        const uint8  nation = pick(hour.odds[seat.region], draw());
        const double earned = expPerHour(dials, seat.level) / kVanaHoursPerRealHour;
        hour.influence[seat.region][nation] += earned * dials.cpRate * dials.influenceShare;
        hour.kills[seat.region] += dials.killsPerSeatHour / kVanaHoursPerRealHour;
        hour.deaths[seat.region] += dials.pressure[seat.region] / kVanaHoursPerRealHour;
        ++hour.seats[seat.region];
        ++hour.drawn[seat.region][nation];
    }
    return hour;
}

// Several Vana'diel hours at once (a step that came late): the same hour, that many times
inline void scale(Hour& hour, const double times)
{
    for (uint8 r = 0; r < kRegions; ++r)
    {
        for (auto& value : hour.influence[r])
        {
            value *= times;
        }
        hour.kills[r] *= times;
        hour.deaths[r] *= times;
    }
}

// The fractions an hour leaves, carried to the next, so a small region's crowd still
// earns its share over the hours
struct Carry
{
    std::array<std::array<double, kNations>, kRegions> influence{};
    std::array<double, kRegions>                       kills{};
    std::array<double, kRegions>                       deaths{};
};

// What an hour sends: the points of each M2W_AddInfluencePoints (whole units of influence,
// ten points each) and whole kills and deaths for the beastmen's counters
struct Sends
{
    std::array<std::array<int32, kNations>, kRegions> points{};
    std::array<int32, kRegions>                       kills{};
    std::array<int32, kRegions>                       deaths{};
};

inline auto settle(const Hour& hour, Carry& carry) -> Sends
{
    Sends      sends{};
    // A billionth of a unit short is taken as whole: a sum of decimal rates lands a hair
    // under the unit it reaches (1.2 five times is 5.9999999999999998)
    const auto whole = [](const double value, double& left) -> int32
    {
        const double total = value + left;
        const double units = std::floor(total + 1e-9);
        left               = total - units;
        return static_cast<int32>(units);
    };
    for (uint8 r = 0; r < kRegions; ++r)
    {
        for (uint8 n = 0; n < kNations; ++n)
        {
            sends.points[r][n] = whole(hour.influence[r][n], carry.influence[r][n]) * kPointsPerInfluence;
        }
        sends.kills[r]  = whole(hour.kills[r], carry.kills[r]);
        sends.deaths[r] = whole(hour.deaths[r], carry.deaths[r]);
    }
    return sends;
}

// A region with no seat filled (§20.5, decision 4) gets one death a period and no kills,
// so it is the beastmen's until somebody fights there: a player's Signet kills count
// against that one death. It is sent from the period's second hour, so the tally that
// opened the period, which zeroes the counters, never lands after it
inline bool owesEmptyDeath(const uint32 seats, const int64 period, const int64 sentFor, const int64 hoursIntoPeriod)
{
    return seats == 0 && sentFor != period && hoursIntoPeriod >= 1;
}

// One Vana'diel hour of a nation's tide: its drift pulled back toward its dial with the
// tide's period as the time constant, plus a random step sized to keep the drift's spread
// at half the amplitude, never past the amplitude. normal is one draw of a standard
// normal. Amplitude 0 holds it still at the dial
inline auto tideStep(const double drift, const double amplitude, const double periodHours, const double normal) -> double
{
    if (amplitude <= 0.0 || periodHours <= 0.0)
    {
        return 0.0;
    }
    const double keep   = std::exp(-1.0 / (kVanaHoursPerRealHour * periodHours));
    const double spread = amplitude / 2.0;
    const double next   = drift * keep + spread * std::sqrt(1.0 - keep * keep) * normal;
    return std::clamp(next, -amplitude, amplitude);
}

} // namespace cardian::conquest
