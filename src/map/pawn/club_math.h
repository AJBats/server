// Cardian: the linkshell club's rules as plain functions (pawn/club.h,
// pawn/errands.h): the game's linkshell items and the shell a pearl belongs
// to, where a player buys a linkshell, what a recruit says on her way to him,
// who may be recruited (the pearl's lock) and what she answers, what she
// takes in the game's own trade window, and which of her nation's missions
// a rank catch-up takes her through.
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

    // ---- the pearl's lock: who may be recruited ---------------------------------

    // One of the world's is a recruit for his linkshell once her affinity
    // with him and the story missions the two have completed together reach
    // the lock (pawn.PEARL_AFFINITY, pawn.PEARL_MISSIONS)
    constexpr auto qualifies(const uint32_t affinity, const uint32_t missions, const uint32_t needAffinity, const uint32_t needMissions) -> bool
    {
        return affinity >= needAffinity && missions >= needMissions;
    }

    // Asked to join his linkshell, she thinks it over a beat, as one who
    // heard his shout does (milliseconds), then says yes in words of her
    // own, by her charid (the same recruit says the same thing): one who
    // qualifies never says no, whatever she is doing (OPEN_ISSUES #415)
    constexpr uint32_t kDecideMinMs = 1500;
    constexpr uint32_t kDecideMaxMs = 5200;

    constexpr std::array<std::string_view, 4> kJoinYes{
        "I'd be glad to!",
        "Count me in!",
        "I was hoping you'd ask!",
        "Of course! Let's do this.",
    };

    inline auto joinYes(const uint32_t charid) -> std::string_view
    {
        return kJoinYes[charid % kJoinYes.size()];
    }

    // Her tell when she never reaches him for the pearl: too long on the way
    inline auto joinLater() -> std::string_view
    {
        return "Something came up. Ask me again later!";
    }

    // ---- a recruit comes for her pearl (OPEN_ISSUES #415) -----------------------

    // Her yes carried out: she comes to him for the pearl in no party, a
    // trade needing none, once her fight is over. Standing within
    // kTrekZones zone lines of him she walks the way; faded, or farther,
    // her trip is out of sight and she comes in near him after
    // kTripSecondsPerZone a zone line, kTrekZones counted at most, until
    // travel between zones is built (#233)
    constexpr uint32_t kTrekZones          = 3;
    constexpr uint32_t kTripSecondsPerZone = 10;
    constexpr float    kVisitArrive        = 12.0f; // yalms from him one out of sight comes in at, behind him if the mesh allows
    constexpr float    kVisitRingNear      = 2.0f;  // yalms from him she waits at, by her charid,
    constexpr float    kVisitRingFar       = 3.4f;  //   on her side of him
    constexpr float    kVisitSpread        = 0.5f;  // radians round him a spot taken by another moves her
    constexpr float    kVisitApart         = 1.6f;  // yalms between two who wait at his side
    constexpr uint32_t kVisitWaitSeconds   = 180;   // at his side, how long she waits for his trade
    constexpr uint32_t kVisitGiveUpSeconds = 600;   // on her way, how long before something has come up
    constexpr float    kVisitLeaveTo       = 25.0f; // yalms from him one with no zone line to leave by walks off straight
    constexpr float    kVisitFadeAt        = 45.0f; // yalms from him one from elsewhere fades at, out of his sight
    constexpr uint32_t kVisitLeaveSeconds  = 90;    // or how long she walks off, whichever comes first

    // How far from him she waits, by her charid: the same recruit at the
    // same distance, each a little apart from the next
    inline auto visitRing(const uint32_t charid) -> float
    {
        constexpr uint32_t steps = 8;
        return kVisitRingNear + (kVisitRingFar - kVisitRingNear) * static_cast<float>(charid % steps) / static_cast<float>(steps - 1);
    }

    // At his side she greets him once: an emote at him, as the game shows
    // it, text and all (Emote's ids, enums/emote.h), and a tell, by her
    // charid (the same recruit greets the same way)
    constexpr std::array<uint8_t, 7> kGreetEmotes{
        8,  // wave
        12, // cheer
        43, // hurray
        11, // joy
        2,  // salute
        1,  // bow
        15, // smile
    };

    inline auto greetEmote(const uint32_t charid) -> uint8_t
    {
        return kGreetEmotes[charid % kGreetEmotes.size()];
    }

    constexpr std::array<std::string_view, 4> kImHere{
        "I'm here!",
        "Made it! Here I am.",
        "Here I am!",
        "There you are!",
    };

    inline auto imHere(const uint32_t charid) -> std::string_view
    {
        return kImHere[(charid / kGreetEmotes.size()) % kImHere.size()];
    }

    // Seconds her trip out of sight takes, by the zone lines between them
    // (0: the same zone; past kTrekZones, or no route, counts kTrekZones)
    inline auto tripSeconds(const uint32_t zones) -> uint32_t
    {
        return kTripSecondsPerZone * std::max<uint32_t>(1, std::min(zones, kTrekZones));
    }

    // Her goodbye when he has not traded her the pearl in time, by her
    // charid (the same recruit says the same thing)
    constexpr std::array<std::string_view, 3> kCatchYouLater{
        "I'll catch you later!",
        "Have to run. Ask me again sometime!",
        "Gotta go now. Catch you later!",
    };

    inline auto catchYouLater(const uint32_t charid) -> std::string_view
    {
        return kCatchYouLater[charid % kCatchYouLater.size()];
    }

    // ---- a pearl by the game's own trade -----------------------------------------

    constexpr uint8_t     kLsTypeLinkpearl = 3;
    constexpr std::size_t kTradeSlots      = 9; // the trade window's slots, gil's first

    // One slot of what he offers in the trade window, as the game shows it
    // to her (packet 0x023): the item, how many, and for a linkshell item its
    // shell and its kind
    struct Offered
    {
        uint16_t item   = 0;
        uint32_t qty    = 0;
        uint32_t lsid   = 0;
        uint8_t  lsType = 0;
    };

    // What she makes of his offer once he presses Trade: she takes exactly
    // one Linkpearl of a shell he holds, with nothing beside it, while she
    // wears no pearl; anything else she declines, saying why
    enum class Verdict : uint8_t
    {
        Take,
        NotTrading,       // not his to trade with: no member of his club and none of the world's
        TooSoon,          // one of the world's short of the pearl's lock with him
        NothingOffered,   // an empty window
        NotOnlyAPearl,    // something other than one Linkpearl, or something beside it
        NotHisShell,      // a Linkpearl of a shell he does not hold, or a broken one
        HasHisPearl,      // she wears a pearl of his shell already
        PearledElsewhere, // she wears a pearl of a shell he does not hold
    };

    // His trade request, from one she would trade with: every trade but the
    // pearl's handover is refused (the user, 2026-10-10), so wearing a pearl
    // already -- his shell's or another's -- she turns the request itself
    // down, with the reason she would give the trade. `his` and `wornLsid`
    // as for judgeOffer
    template <typename Shells>
    auto judgeRequest(const Shells& his, const uint32_t wornLsid) -> Verdict
    {
        if (wornLsid != 0)
        {
            return his.contains(wornLsid) ? Verdict::HasHisPearl : Verdict::PearledElsewhere;
        }
        return Verdict::Take;
    }

    // `his` answers whether he holds a shell (a set's contains); `wornLsid`
    // is the shell of the pearl she wears, 0 for none
    template <typename Shells>
    auto judgeOffer(const std::array<Offered, kTradeSlots>& slots, const Shells& his, const uint32_t wornLsid) -> Verdict
    {
        if (wornLsid != 0)
        {
            return his.contains(wornLsid) ? Verdict::HasHisPearl : Verdict::PearledElsewhere;
        }
        const Offered* only    = nullptr;
        std::size_t    offered = 0;
        for (const auto& slot : slots)
        {
            if (slot.qty > 0)
            {
                only = &slot;
                ++offered;
            }
        }
        if (offered == 0)
        {
            return Verdict::NothingOffered;
        }
        if (offered > 1 || only->item != kLinkpearl || only->qty != 1)
        {
            return Verdict::NotOnlyAPearl;
        }
        if (only->lsType != kLsTypeLinkpearl || only->lsid == 0 || !his.contains(only->lsid))
        {
            return Verdict::NotHisShell;
        }
        return Verdict::Take;
    }

    // Her word to one she has no reason to trade with -- a stranger, or one of
    // the world's short of the pearl's lock -- neutral, never cold: he may have
    // adventured with her a good while (the user, 2026-10-10). By her charid
    // and how many times he has tried, so a second try hears another line
    constexpr std::array<std::string_view, 8> kNoThanks{
        "Um, no thanks, I don't want any.",
        "Huh?",
        "No thanks...",
        "Oh! I'm alright, thanks.",
        "Hm? No, I'm good.",
        "That's okay, you keep it.",
        "Ah, no need, really.",
        "Thanks, but I'll pass.",
    };

    inline auto noThanks(const uint32_t charid, const uint32_t tries) -> std::string_view
    {
        return kNoThanks[(charid + tries) % kNoThanks.size()];
    }

    // Her word when she declines a trade (`tries`: how many he has had
    // declined by her for no reason of the trade's own)
    inline auto declineLine(const Verdict verdict, const uint32_t charid = 0, const uint32_t tries = 0) -> std::string_view
    {
        switch (verdict)
        {
            case Verdict::NotTrading:
            case Verdict::TooSoon:
                return noThanks(charid, tries);
            case Verdict::NothingOffered:
            case Verdict::NotOnlyAPearl:
                return "Thanks, but I'll only take a linkpearl of your linkshell.";
            case Verdict::NotHisShell:
                return "That linkpearl isn't from your linkshell.";
            case Verdict::HasHisPearl:
                return "I already have your linkpearl!";
            case Verdict::PearledElsewhere:
                return "Sorry, I'm already in another linkshell.";
            case Verdict::Take:
                break;
        }
        return {};
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
