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

#include "conquest_math.h"
#include "world.h"

#include "common/cardian_conquest_clock.h"
#include "common/database.h"
#include "common/earth_time.h"
#include "common/logging.h"
#include "common/settings.h"
#include "common/xirand.h"
#include "conquest_system.h"
#include "utils/moduleutils.h"
#include "utils/zoneutils.h"
#include "zone.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <numbers>
#include <optional>
#include <random>
#include <string>
#include <utility>
#include <vector>

// The conquest simulation (RESEARCH §20, §20.11): an invisible thumb on the scale. Every
// Vana'diel hour of the game clock the map counts the seats the slot tables author in the
// field and the dungeons whose level band the world has reached -- the census's top level
// at or above the band's floor -- filled or not: the crowd is nobody in the world, not
// even faded, and no census body is looked at. Town seats earn nothing, since nothing is
// fought there. Each seat's nation is drawn every hour from its region's shares, which
// the dials alone set (strength, the tide, the home bonus and the next pull, a hand on the
// scale; conquest_math.h), with the period's campaigns (§20.12): the thumb favouring one
// nation round Jeuno for a few tallies, and each nation's focus on a third of the open outer
// regions, drawn anew each tally and growing from nothing at the tally. So a nation holds
// its home ground however many of its adventurers the census minted. The map sends what the game already understands: one
// M2W_AddInfluencePoints per nation per region, and the beastmen's Signet kills and
// deaths. xi_world's own tally decides, on the schedule of common/cardian_conquest_clock.h,
// with its own catch-up for the trailing nations (the user, 2026-10-09: it stays), and its
// own chat lines, outposts and merchants show the result. Each nation's home region and the
// region next to it are fought over from a fresh server's first day; an outer region opens
// once the world's top level reaches its own (the dials' `outer`), a far one with a crowd of
// the dials' own on top of what its tables seat. A region with no crowd -- not open, or no
// seat the world has reached -- gets one death a period, so it is the beastmen's. The world bodies' own Signet kills, and
// the player's, count on top of it as the game counts them (the user, 2026-10-09).
//
// The game clock is held by a pause, so the crowd earns nothing while the game stands.
// cardian.CONQUEST_SIMULATION switches it on; the dials are modules/cardian/conquest.yaml.

namespace conquest
{
// conquest_system.cpp's sender of M2W_AddInfluencePoints, which its header leaves out
void AddInfluencePoints(int points, unsigned int nation, REGION_TYPE region);
} // namespace conquest

namespace
{
    namespace cq = cardian::conquest;
    namespace cc = cardian::conquest_clock;

    const std::filesystem::path kDialsPath = std::filesystem::path("modules/cardian") / "conquest.yaml";

    constexpr std::array<std::string_view, cq::kRegions> kRegionTitles{
        "Ronfaure", "Zulkheim", "Norvallen", "Gustaberg", "Derfland", "Sarutabaruta", "Kolshushu",
        "Aragoneu", "Fauregandi", "Valdeaunia", "Qufim", "Li'Telor", "Kuzotz", "Vollbow",
        "Elshimo Lowlands", "Elshimo Uplands", "Tu'Lia", "Movalpolos", "Tavnazia",
    };

    // The step's state. The carry and the empty regions' deaths live for the process; the
    // tide is the one thing kept across a restart (cardian_conquest_tide)
    std::optional<int64>                 lastHour;
    std::optional<cq::Dials>             dials;
    std::filesystem::file_time_type      dialsWritten{};
    cq::Carry                            carry{};
    std::array<int64, cq::kRegions>      emptyDeathFor{};
    std::array<double, cq::kNations>     tide{};
    bool                                 tideLoaded = false;

    // The period's campaigns, set at every step. Each nation's focus is drawn anew from the
    // period itself, so a restart, a dial changed or a region the world opens mid-period gives
    // what the same draws give; the thumb is kept across a restart (cardian_conquest_thumb).
    // The last campaigns the log named, so it names them again only when they change
    cq::Campaign                  campaign{};
    cq::Thumb                     thumb{};
    bool                          thumbLoaded    = false;
    uint8                         loggedFavoured = cq::kNations;
    decltype(cq::Campaign::focus) loggedFocus{};

    auto isOn() -> bool
    {
        return settings::get<bool>("cardian.CONQUEST_SIMULATION");
    }

    // The game clock's Vana'diel hours since the epoch
    auto vanaHourNow() -> int64
    {
        return std::chrono::floor<std::chrono::milliseconds>(earth_time::game_now() - earth_time::vanadiel_epoch) / cc::kVanaHour;
    }

    // The dials, read again when the file has changed since the last read. A file that does
    // not parse, or names what is not a region or a nation, is said once and leaves the last
    // good dials; with none yet, the step waits for a good file
    void refreshDials()
    {
        std::error_code ec;
        const auto      written = std::filesystem::last_write_time(kDialsPath, ec);
        if (ec)
        {
            if (dialsWritten != std::filesystem::file_time_type{} || !dials.has_value())
            {
                ShowErrorFmt("conquest: {} cannot be read; the simulation {}", kDialsPath.string(), dials.has_value() ? "keeps its last dials" : "waits for it");
                dialsWritten = std::filesystem::file_time_type{};
            }
            return;
        }
        if (dials.has_value() && written == dialsWritten)
        {
            return;
        }
        dialsWritten = written;
        std::ifstream     in(kDialsPath, std::ios::binary);
        const std::string text{ std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>() };
        std::string       error;
        auto              file = cq::parseDials(text, error);
        auto              fresh = file.has_value() ? cq::toDials(*file, error) : std::nullopt;
        if (!fresh.has_value())
        {
            ShowErrorFmt("conquest: {}: {}; the simulation {}", kDialsPath.string(), error, dials.has_value() ? "keeps its last dials" : "waits for a good file");
            return;
        }
        ShowInfoFmt("conquest: dials read from {}", kDialsPath.string());
        dials = std::move(fresh);
    }

    // The tide's table: one row per nation, its strength's drift from its dial
    void ensureTideTable()
    {
        db::preparedStmt("CREATE TABLE IF NOT EXISTS `cardian_conquest_tide` ("
                         "`nation` tinyint(3) unsigned NOT NULL, " // 0 San d'Oria, 1 Bastok, 2 Windurst
                         "`drift` double NOT NULL DEFAULT 0, "     // the nation's strength less its dial, as a fraction of it
                         "PRIMARY KEY (`nation`)) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_general_ci");
    }

    void loadTide()
    {
        tideLoaded = true;
        if (const auto rset = db::preparedStmt("SELECT nation, drift FROM cardian_conquest_tide"); rset)
        {
            while (rset->next())
            {
                if (const auto nation = rset->get<uint8>("nation"); nation < cq::kNations)
                {
                    tide[nation] = rset->get<double>("drift");
                }
            }
        }
    }

    void saveTide()
    {
        db::preparedStmt("REPLACE INTO cardian_conquest_tide (nation, drift) VALUES (0, ?), (1, ?), (2, ?)", tide[0], tide[1], tide[2]);
    }

    // The thumb's table: one row, the nation it favours round Jeuno
    void ensureThumbTable()
    {
        db::preparedStmt("CREATE TABLE IF NOT EXISTS `cardian_conquest_thumb` ("
                         "`id` tinyint(3) unsigned NOT NULL, "             // always 0: the one row
                         "`nation` tinyint(3) unsigned NOT NULL, "         // 0 San d'Oria, 1 Bastok, 2 Windurst
                         "`period` bigint(20) NOT NULL, "                  // the tally closing the period it last counted, ms of game clock
                         "`tallies` int(10) unsigned NOT NULL DEFAULT 0, " // the tallies it has favoured that nation since
                         "PRIMARY KEY (`id`)) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_general_ci");
    }

    void loadThumb()
    {
        thumbLoaded = true;
        if (const auto rset = db::preparedStmt("SELECT nation, period, tallies FROM cardian_conquest_thumb WHERE id = 0"); rset && rset->next())
        {
            if (const auto nation = rset->get<uint8>("nation"); nation < cq::kNations)
            {
                thumb = cq::Thumb{ .favoured = nation, .period = rset->get<int64>("period"), .held = rset->get<uint32>("tallies") };
            }
        }
    }

    // The period as text: the statement binder carries no 64-bit integer, and the server converts exactly
    void saveThumb()
    {
        db::preparedStmt("REPLACE INTO cardian_conquest_thumb (id, nation, period, tallies) VALUES (0, ?, ?, ?)", thumb.favoured, std::to_string(*thumb.period),
                         thumb.held);
    }

    // One draw of a standard normal (Box-Muller)
    auto normalDraw() -> double
    {
        const double u1 = 1.0 - xirand::GetRandomNumber(0.0, 1.0); // (0, 1]: the logarithm needs it above 0
        const double u2 = xirand::GetRandomNumber(0.0, 1.0);
        return std::sqrt(-2.0 * std::log(u1)) * std::cos(2.0 * std::numbers::pi * u2);
    }

    // The world's top level as the census defines it (census.py world_top): the highest target
    // of an unrecruited census row, never under 18
    auto worldTopNow() -> uint8
    {
        uint8 highest = 0;
        if (const auto rset = db::preparedStmt("SELECT COALESCE(MAX(target), 0) AS top FROM cardian_census WHERE recruited = 0"); rset && rset->next())
        {
            highest = rset->get<uint8>("top");
        }
        return cq::worldTop(highest);
    }

    // The slots of the slot tables in a field or dungeon zone of a conquest region
    auto fieldSlots() -> std::vector<cq::Slot>
    {
        std::vector<cq::Slot> slots;
        for (const auto& slot : pawn::world::authoredSlots())
        {
            const auto zoneId = static_cast<xi::ZoneId>(slot.zone);
            const auto region = static_cast<uint8>(zoneutils::GetCurrentRegion(zoneId));
            if (region >= cq::kRegions)
            {
                continue;
            }
            if (auto* PZone = zoneutils::GetZone(zoneId); PZone == nullptr || (PZone->GetTypeMask() & xi::ZoneType::City) != xi::ZoneType::Unknown)
            {
                continue; // a town: Selbina, Mhaura, Rabao, Kazham, Norg
            }
            slots.push_back(cq::Slot{ .region = region, .low = slot.low, .high = slot.high, .seats = slot.seats });
        }
        return slots;
    }

    auto ownersNow() -> std::array<uint8, cq::kRegions>
    {
        std::array<uint8, cq::kRegions> owners{};
        for (uint8 r = 0; r < cq::kRegions; ++r)
        {
            owners[r] = conquest::GetRegionOwner(static_cast<REGION_TYPE>(r));
        }
        return owners;
    }

    // A period's length in Vana'diel hours
    auto periodHours(const uint32 days) -> int64
    {
        return std::chrono::floor<std::chrono::milliseconds>(cc::periodLength(days)) / cc::kVanaHour;
    }

    // The period's campaigns at this step: each nation's focus, drawn from the period itself
    // and the world's level now; the thumb, counting a tally from the period's second hour;
    // and how far the focus has grown. The log names them whenever they change
    void updateCampaigns(const int64 period, const int64 hoursIn, const uint8 top, const uint32 days)
    {
        std::mt19937_64                        rng(static_cast<uint64>(period));
        std::uniform_real_distribution<double> uniform(0.0, 1.0);
        campaign.focus = cq::drawFocus(*dials, top, [&] { return uniform(rng); });

        if (!thumbLoaded)
        {
            loadThumb();
        }
        if (cq::advanceThumb(thumb, period, hoursIn, dials->thumbTallies, ownersNow(), [] { return xirand::GetRandomNumber(0.0, 1.0); }))
        {
            saveThumb();
        }
        campaign.favoured = thumb.favoured;
        campaign.grown    = cq::focusGrown(hoursIn, periodHours(days), dials->focusRamp);

        if (campaign.favoured == loggedFavoured && campaign.focus == loggedFocus)
        {
            return;
        }
        loggedFavoured = campaign.favoured;
        loggedFocus    = campaign.focus;

        std::string open;
        for (const auto region : dials->outer)
        {
            if (cq::inPlay(*dials, region, top))
            {
                open += fmt::format("{}{}", open.empty() ? "" : ", ", kRegionTitles[region]);
            }
        }
        std::string line;
        for (uint8 n = 0; n < cq::kNations; ++n)
        {
            std::string picks;
            for (uint8 r = 0; r < cq::kRegions; ++r)
            {
                if (const double full = campaign.focus[n][r]; full > 0.0)
                {
                    picks += fmt::format("{}{} x{:.1f}", picks.empty() ? "" : ", ", kRegionTitles[r], full);
                }
            }
            line += fmt::format("; {} focuses on {}", cq::kNationNames[n], picks.empty() ? std::string("nothing") : picks);
        }
        const auto favoured = campaign.favoured < cq::kNations ? fmt::format("{} round Jeuno (its tally {} of {})", cq::kNationNames[campaign.favoured],
                                                                             thumb.held + 1, dials->thumbTallies)
                                                               : std::string("nobody yet");
        ShowInfoFmt("conquest: the campaigns, the world at level {}: outer regions open: {}; the thumb favours {}{}; each focus grows to its full pull {:.0f}% into the period",
                    top, open.empty() ? std::string("none") : open, favoured, line, dials->focusRamp * 100.0);
    }

    // `hours` Vana'diel hours of the crowd, sent; more than one when the step came late
    void step(const int64 hours)
    {
        refreshDials();
        if (!dials.has_value())
        {
            return;
        }
        if (!tideLoaded)
        {
            loadTide();
        }
        for (int64 h = 0; h < hours; ++h)
        {
            for (auto& drift : tide)
            {
                drift = cq::tideStep(drift, dials->tideAmplitude, dials->tidePeriodHours, normalDraw());
            }
        }
        saveTide();

        const auto  days    = cc::tallyDays();
        const auto  game    = earth_time::game_now();
        const int64 period  = std::chrono::floor<std::chrono::milliseconds>(cc::nextTally(game, days).time_since_epoch()).count();
        const auto  hoursIn = cc::hoursIntoPeriod(game, days);
        const auto  top     = worldTopNow();
        updateCampaigns(period, hoursIn, top, days);

        const auto seats = cq::fieldCrowd(fieldSlots(), *dials, top);
        auto       hour  = cq::sumHour(seats, *dials, ownersNow(), tide, [] { return xirand::GetRandomNumber(0.0, 1.0); }, campaign);
        cq::scale(hour, static_cast<double>(hours));
        const auto sends = cq::settle(hour, carry);

        std::string line;
        std::string empties;
        uint32      regionsSent = 0;
        for (uint8 r = 0; r < cq::kRegions; ++r)
        {
            const auto region = static_cast<REGION_TYPE>(r);
            // xi_world multiplies what a trailing nation is sent as it adds each message, so the
            // first nation sent into a region at zero gets less than the two after it: the order
            // is drawn anew each hour, never one nation always first
            std::array<uint8, cq::kNations> order{ 0, 1, 2 };
            for (uint8 i = cq::kNations - 1; i > 0; --i)
            {
                std::swap(order[i], order[xirand::GetRandomNumber<uint8>(0, static_cast<uint8>(i + 1))]);
            }
            for (const auto n : order)
            {
                if (sends.points[r][n] > 0)
                {
                    conquest::AddInfluencePoints(sends.points[r][n], n, region);
                }
            }
            if (sends.kills[r] > 0)
            {
                conquest::AddMobKills(sends.kills[r], region);
            }
            int32 deaths = sends.deaths[r];
            if (cq::owesEmptyDeath(hour.seats[r], period, emptyDeathFor[r], hoursIn))
            {
                emptyDeathFor[r] = period;
                ++deaths;
                empties += fmt::format("{}{}", empties.empty() ? "" : ", ", kRegionTitles[r]);
            }
            if (deaths > 0)
            {
                conquest::AddPlayerHomepoints(deaths, region);
            }
            if (hour.seats[r] > 0)
            {
                ++regionsSent;
                const auto& odds  = hour.odds[r];
                const auto& drawn = hour.drawn[r];
                line += fmt::format("; {} {} seats, shares {:.0f}/{:.0f}/{:.0f}%, drawn {}/{}/{}: {}/{}/{}, {} kills, {} deaths", kRegionTitles[r], hour.seats[r],
                                    odds[0] * 100.0, odds[1] * 100.0, odds[2] * 100.0, drawn[0], drawn[1], drawn[2],
                                    sends.points[r][0] / cq::kPointsPerInfluence, sends.points[r][1] / cq::kPointsPerInfluence,
                                    sends.points[r][2] / cq::kPointsPerInfluence, sends.kills[r], sends.deaths[r]);
            }
        }
        ShowInfoFmt("conquest: {} Vana'diel hour{} of the crowd, the world at level {}: {} seats in {} regions (San d'Oria/Bastok/Windurst){}; tide {:+.1f}%/{:+.1f}%/{:+.1f}%; focus grown {:.0f}%",
                    hours, hours == 1 ? "" : "s", top, seats.size(), regionsSent, line, tide[0] * 100.0, tide[1] * 100.0, tide[2] * 100.0, campaign.grown * 100.0);
        if (!empties.empty())
        {
            ShowInfoFmt("conquest: no crowd in {} -- not open, or no seat the world has reached: one death each for the beastmen this period", empties);
        }
    }
} // namespace

class ConquestSimModule : public CPPModule
{
    void OnInit() override
    {
        ensureTideTable();
        ensureThumbTable();
    }

    // The time server ticks every 2.4 s, a Vana'diel minute, held or not. A Vana'diel hour
    // of the game clock passed since the last is a step; its first look only sets the
    // clock, so the hours the server was off are never paid, and a game clock set back
    // pays nothing until it passes the last hour paid
    void OnTimeServerTick() override
    {
        if (!isOn())
        {
            lastHour.reset();
            return;
        }
        const auto now = vanaHourNow();
        if (!lastHour.has_value())
        {
            lastHour         = now;
            const auto days  = cc::tallyDays();
            const auto until = cc::nextTally(earth_time::game_now(), days) - earth_time::game_now();
            ShowInfoFmt("conquest: the crowd's simulation is on, every Vana'diel hour; the tally {}, the next in {} min of game clock",
                        days == 0 ? std::string("on the JST week") : fmt::format("every {} Vana'diel day{}", days, days == 1 ? "" : "s"),
                        std::chrono::duration_cast<std::chrono::minutes>(until).count());
            return;
        }
        if (now <= *lastHour)
        {
            return;
        }
        const auto hours = std::min<int64>(now - *lastHour, 25); // a real hour at most: a step that came very late pays no more
        lastHour         = now;
        step(hours);
    }
};

REGISTER_CPP_MODULE(ConquestSimModule);
