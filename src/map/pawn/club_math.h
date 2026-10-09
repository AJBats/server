// Cardian: the linkshell club's rules as plain functions (pawn/club.h,
// pawn/errands.h): the game's linkshell items and the shell a pearl belongs
// to, where a player buys a linkshell, what a recruit says on her way to him,
// and which of her nation's missions a rank catch-up takes her through.
#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <optional>
#include <string_view>
#include <vector>

namespace cardian::club
{
    // ---- the game's linkshell items -----------------------------------------

    constexpr uint16_t kNewLinkshell = 512; // bought from a linkshell vendor, made into a Linkshell
    constexpr uint16_t kLinkshell    = 513; // the shell itself: its holder makes pearls from it
    constexpr uint16_t kPearlsack    = 514; // a pearl that can make pearls
    constexpr uint16_t kLinkpearl    = 515;

    // The equipment slots a linkshell item is worn in (SLOT_LINK1, SLOT_LINK2)
    constexpr uint8_t kLinkSlot1 = 16;
    constexpr uint8_t kLinkSlot2 = 17;

    // An item of a linkshell's: the shell, a sack, a pearl. A body of the
    // world's bags keep one where it lies, worn or not: it is his club's
    constexpr auto isLinkshellItem(const uint16_t itemId) -> bool
    {
        return itemId == kLinkshell || itemId == kPearlsack || itemId == kLinkpearl;
    }

    // The shell a linkshell item belongs to: the first four bytes of its
    // extra data (Exdata::Linkshell's GroupId, little-endian), as
    // char_inventory.extra keeps it; 0 when there are fewer
    inline auto lsidOf(const uint8_t* extra, const std::size_t size) -> uint32_t
    {
        if (extra == nullptr || size < 4)
        {
            return 0;
        }
        return static_cast<uint32_t>(extra[0]) | (static_cast<uint32_t>(extra[1]) << 8) | (static_cast<uint32_t>(extra[2]) << 16) |
               (static_cast<uint32_t>(extra[3]) << 24);
    }

    // The item's own kind in its extra data (Exdata::Linkshell's Flag, after
    // the shell's id, its key and its colour: the game's LSTYPE, 1 the shell,
    // 2 a sack, 3 a pearl, 4 broken), the ninth byte
    inline auto lsTypeOf(const uint8_t* extra, const std::size_t size) -> uint8_t
    {
        return extra != nullptr && size >= 9 ? extra[8] : 0;
    }
    constexpr uint8_t kLsTypeBroken = 4;

    // ---- where a linkshell is sold --------------------------------------------

    // A nation's city and its linkshell vendor (the guild shops that stock
    // the New Linkshell, scripts/data/guild_shops.lua): by the city's region
    // (San d'Oria 19, Bastok 20, Windurst 21). Jeuno and the towns sell none
    struct Vendor
    {
        std::string_view name; // for people
        std::string_view zone; // where the vendor stands, for people
    };

    inline auto vendorOf(const uint8_t region) -> std::optional<Vendor>
    {
        switch (region)
        {
            case 19:
                return Vendor{ "Paunelie", "Southern San d'Oria" };
            case 20:
                return Vendor{ "Ilita", "Port Bastok" };
            case 21:
                return Vendor{ "Khel Pahlhama", "Port Windurst" };
            default:
                return std::nullopt;
        }
    }

    // ---- a recruit on her way ---------------------------------------------------

    // What a recruit tells him as she sets out from another zone, by her
    // charid (the same recruit says the same thing)
    constexpr std::array<std::string_view, 4> kHeadingYourWay{
        "Hey, heading your way!",
        "On my way to you now!",
        "Heading your way. See you soon!",
        "Hey! Coming to you, hang tight.",
    };

    inline auto headingYourWay(const uint32_t charid) -> std::string_view
    {
        return kHeadingYourWay[charid % kHeadingYourWay.size()];
    }

    // ---- a rank catch-up --------------------------------------------------------

    // One of her nation's missions on the errand table: its id in the
    // nation's log, the rank step it belongs to (the rank she holds once
    // that step is done: a nation's 2-3 and its journeys are step 3), how
    // long it takes her and its zones. A step's last mission grants its rank
    struct Mission
    {
        uint16_t              id      = 0;
        uint8_t               step    = 0;
        uint16_t              minutes = 0;
        std::vector<uint16_t> zones;
    };

    // The catch-up to `target`: the missions of every step up to it that she
    // has not done, in the log's order, each its minutes and the rank it
    // grants (a step's last mission, 0 for the rest), and her route through
    // all their zones
    struct RankPlan
    {
        std::vector<uint16_t> missions;
        std::vector<uint16_t> minutes;
        std::vector<uint8_t>  grants;
        std::vector<uint16_t> route;
    };

    // `ladder`: her nation's missions in the log's order, by step; `done(id)`:
    // she has done that mission. Nothing when the target is not above her
    // rank, the ladder has no such step, or nothing of it is left to do
    template <typename Done>
    auto rankPlan(const std::vector<Mission>& ladder, const uint8_t rankNow, const uint8_t target, Done&& done) -> std::optional<RankPlan>
    {
        if (target <= rankNow || std::ranges::none_of(ladder, [&](const Mission& m) { return m.step == target; }))
        {
            return std::nullopt;
        }
        RankPlan plan;
        for (std::size_t i = 0; i < ladder.size(); ++i)
        {
            const auto& m = ladder[i];
            if (m.step > target)
            {
                break;
            }
            if (done(m.id))
            {
                continue;
            }
            const bool lastOfStep = i + 1 == ladder.size() || ladder[i + 1].step != m.step;
            plan.missions.push_back(m.id);
            plan.minutes.push_back(m.minutes);
            plan.grants.push_back(lastOfStep ? m.step : 0);
            for (const auto zone : m.zones)
            {
                if (plan.route.empty() || plan.route.back() != zone)
                {
                    plan.route.push_back(zone);
                }
            }
        }
        if (plan.missions.empty())
        {
            return std::nullopt;
        }
        return plan;
    }

    // The highest rank the catch-up offers: his own, as far as the ladder goes
    inline auto rankCap(const std::vector<Mission>& ladder, const uint8_t hisRank) -> uint8_t
    {
        uint8_t top = 0;
        for (const auto& m : ladder)
        {
            top = std::max(top, m.step);
        }
        return std::min(top, hisRank);
    }
} // namespace cardian::club
