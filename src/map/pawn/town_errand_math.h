// Cardian: a cardian's own errands in town (pawn/town_errands.h), their rules
// as plain functions: who goes and when she looks at her needs, what she
// needs, the order she sees to them in, her lines, and her errands going on
// when the player leaves her zone.
#pragma once

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

namespace cardian::town
{
    // Whose errands they are: a member of the player's party in a city's
    // zone, wherever he is -- joined in another zone, or crossing her city on
    // her way to him, she gets ready where she stands rather than coming to
    // him and going back (the user, 2026-10-17) -- or one of the world's at
    // her own hangout, the town seat she holds, out of any party
    enum class Who : uint8_t
    {
        Nobody,
        Party,
        Hangout,
    };

    struct WhoFacts
    {
        bool standing   = false; // up, in a zone, in no event
        bool inCity     = false; // her zone is a city's
        bool elsewhere  = false; // something else has her: the linkshell's errand, a trek (but her way to him), a walk someone else gave her, her way out of town
        bool inHisParty = false; // a real player's party
        bool worlds     = false; // one of the world's, in the wild: with no player, held for none
        bool atHangout  = false; // holding her town seat, not on her way out
        bool seen       = false; // a real player in her zone: out of sight the census sees to her
    };

    constexpr auto whoGoes(const WhoFacts& f) -> Who
    {
        if (!f.standing || !f.inCity || f.elsewhere)
        {
            return Who::Nobody;
        }
        if (f.inHisParty)
        {
            return Who::Party;
        }
        return f.worlds && f.atHangout && f.seen ? Who::Hangout : Who::Nobody;
    }

    // What keeps her from her errands for now
    struct Busy
    {
        bool fighting = false; // engaged, walking in on or attending a fight, a fight anywhere in her party
        bool ordered  = false; // the player's order: his hold, his attack, a maneuver, his walk, an order in her line, his rest, a retreat
        bool acting   = false; // a spell, an ability or an item under way
        bool resting  = false; // kneeling to rest of her own accord
    };

    // A fight or an order of the player's suspends her errands; she takes them
    // up afresh once she is free in the city again
    constexpr auto interrupted(const Busy& b) -> bool
    {
        return b.fighting || b.ordered;
    }

    // She sets out now: nothing interrupts her, and nothing she is in the
    // middle of -- an action, a rest she knelt for -- keeps her a moment
    // longer. The world's at a hangout kneel by the seat's pose, and a walk
    // stands them, so their kneel keeps them from nothing
    constexpr auto setsOutNow(const Busy& b, const Who who) -> bool
    {
        return !interrupted(b) && !b.acting && (!b.resting || who == Who::Hangout);
    }

    // She looks at her needs as she arrives in a zone of a city and as she
    // becomes free there -- out of a fight or an order, at her hangout, in
    // his party now -- and not again while she stays free, so a need nothing
    // here can meet does not send her round and round
    struct Watch
    {
        uint16_t zone = 0;
        bool     free = false;       // at the last look: someone's, and nothing interrupting her
        Who      who  = Who::Nobody; // whose she was: one of the world's invited is his party's now
    };

    constexpr auto looksNow(const Watch& before, const uint16_t zone, const bool free, const Who who) -> bool
    {
        return free && (zone != before.zone || !before.free || who != before.who);
    }

    // What she needs, a bit each
    enum Need : uint8_t
    {
        kRedress = 1, // the auction counter: the census dresses her for her level
        kScrolls = 2, // a gate guard: an Instant Warp, an Instant Reraise
        kJobs    = 4, // a Mog House door or a Nomad Moogle: her career's jobs
    };

    struct NeedFacts
    {
        bool    worlds       = false; // one of the world's: her gear and her jobs are the census's (one of his own's are his to manage)
        bool    guest        = false; // in his party: she plays the job he took her on in
        bool    risen        = false; // risen past the level she was last dressed for
        bool    lacksScrolls = false; // lacks a scroll she can pay for and carry
        bool    careerJobs   = false; // her career wants another main or support job now
        uint8_t seenTo       = 0;     // the needs whose stop she reached in this zone already: not again until she comes back
    };

    // One of the world's sees to all three, and is dressed again for the jobs
    // her career gives her -- but in his party she changes no job, so the role
    // he took her on in stays hers; one of his own buys her scrolls alone
    constexpr auto needsOf(const NeedFacts& f) -> uint8_t
    {
        uint8_t needs = f.lacksScrolls ? kScrolls : 0;
        if (f.worlds)
        {
            if (f.careerJobs && !f.guest)
            {
                needs |= kJobs | kRedress;
            }
            if (f.risen)
            {
                needs |= kRedress;
            }
        }
        return static_cast<uint8_t>(needs & ~f.seenTo);
    }

    // Where she sees to a need: a consulate is her nation's guard in another
    // zone of her city
    enum class Stop : uint8_t
    {
        Counter,
        Guard,
        Consulate,
        MogHouse,
    };

    constexpr auto needOf(const Stop stop) -> uint8_t
    {
        switch (stop)
        {
            case Stop::Counter:
                return kRedress;
            case Stop::Guard:
            case Stop::Consulate:
                return kScrolls;
            case Stop::MogHouse:
                return kJobs;
        }
        return 0;
    }

    constexpr auto stopName(const Stop stop) -> std::string_view
    {
        switch (stop)
        {
            case Stop::Counter:
                return "the auction counter";
            case Stop::Guard:
                return "the gate guard";
            case Stop::Consulate:
                return "her nation's consulate";
            case Stop::MogHouse:
                return "the Mog House";
        }
        return "";
    }

    struct Spot
    {
        Stop  stop      = Stop::Counter;
        float x         = 0.0f;
        float y         = 0.0f;
        float z         = 0.0f;
        bool  elsewhere = false; // in another zone of her city
    };

    // The order she visits her stops in, as indices into `spots`: the nearest
    // next, from where she stands and then from each stop; the counter waits
    // for the Mog House, since the census dresses her for the jobs she wears,
    // and a stop in another zone goes last
    inline auto visitOrder(const std::vector<Spot>& spots, float x, float y, float z) -> std::vector<std::size_t>
    {
        std::vector<std::size_t> order;
        std::vector<bool>        taken(spots.size(), false);
        const auto               left = [&](const auto& is)
        {
            for (std::size_t i = 0; i < spots.size(); ++i)
            {
                if (!taken[i] && is(spots[i]))
                {
                    return true;
                }
            }
            return false;
        };
        while (order.size() < spots.size())
        {
            const bool here = left([](const Spot& s) { return !s.elsewhere; });
            const bool mog  = left([](const Spot& s) { return s.stop == Stop::MogHouse; });
            std::size_t best    = spots.size();
            float       nearest = 0.0f;
            for (std::size_t i = 0; i < spots.size(); ++i)
            {
                const auto& s = spots[i];
                if (taken[i] || (s.elsewhere && here) || (s.stop == Stop::Counter && mog))
                {
                    continue;
                }
                const float away = std::sqrt((s.x - x) * (s.x - x) + (s.y - y) * (s.y - y) + (s.z - z) * (s.z - z));
                if (best == spots.size() || away < nearest)
                {
                    best    = i;
                    nearest = away;
                }
            }
            if (best == spots.size())
            {
                break;
            }
            taken[best] = true;
            order.push_back(best);
            x = spots[best].x;
            y = spots[best].y;
            z = spots[best].z;
        }
        return order;
    }

    // ---- her lines ------------------------------------------------------------

    // Said in party chat, in his party only, and nothing at a stop (the user,
    // 2026-10-16: the fewer lines the better)
    enum class Line : uint8_t
    {
        SetsOut,  // as she sets out
        Ready,    // every stop done
        NotReady, // he has left her zone while her errands run
    };

    inline constexpr std::array<std::string_view, 5> kSetsOut{
        "Getting ready before we set out.",
        "Let me get ready before we head out.",
        "A few things to see to before we go.",
        "One moment, getting ready to set out.",
        "Getting my things in order before we leave.",
    };
    inline constexpr std::array<std::string_view, 5> kReady{
        "I'm ready to go!",
        "All set, ready to go!",
        "Ready when you are!",
        "All done. Ready to set out!",
        "Ready to go!",
    };
    inline constexpr std::array<std::string_view, 4> kNotReady{
        "Not ready yet, I'll join you when I am.",
        "Still getting ready. I'll catch up!",
        "Go on ahead, I'll join you when I'm ready.",
        "Not quite ready yet. I'll be right behind you.",
    };

    inline auto linesOf(const Line line) -> std::span<const std::string_view>
    {
        switch (line)
        {
            case Line::SetsOut:
                return kSetsOut;
            case Line::Ready:
                return kReady;
            case Line::NotReady:
                return kNotReady;
        }
        return {};
    }

    // One of the kind's lines by a roll, as its index: a line said a moment
    // ago (`recent`, a bit per line) is passed over while another is left, so
    // a party setting out together does not say one line twice
    inline auto pickLine(const Line line, const uint32_t roll, const uint32_t recent) -> std::size_t
    {
        const auto lines = linesOf(line);
        std::vector<std::size_t> fresh;
        for (std::size_t i = 0; i < lines.size(); ++i)
        {
            if ((recent & (1u << i)) == 0)
            {
                fresh.push_back(i);
            }
        }
        if (fresh.empty())
        {
            return lines.empty() ? 0 : roll % lines.size();
        }
        return fresh[roll % fresh.size()];
    }

    // Her set-out line: in his party, the first time she sets out in a zone;
    // her errands taken up again after an interruption go on without a word
    constexpr auto saysSetOut(const Who who, const bool setOutHere) -> bool
    {
        return who == Who::Party && !setOutHere;
    }

    // ---- going on --------------------------------------------------------------

    struct OnFacts
    {
        bool standing   = false; // up, in a zone
        bool inHisParty = false; // the party of the player she set out with, he in a zone or crossing between two
        bool worlds     = false; // one of the world's, in the wild
        bool sameZone   = false; // in the zone she set out in
        bool onTheWay   = false; // on her way to her consulate: in another zone of her city, or the line between
    };

    // Her errands go on while she is still whose they were. The player's
    // party member keeps going when he leaves her zone (the user,
    // 2026-10-16): she follows him once she is ready, by the usual rule --
    // a walk through a zone line is followed on a trek, a warp holds her
    // where she is (warp_hold.h). One of the world's keeps to her own zone
    constexpr auto goesOn(const Who who, const OnFacts& f) -> bool
    {
        if (!f.standing || !(f.sameZone || f.onTheWay))
        {
            return false;
        }
        switch (who)
        {
            case Who::Party:
                return f.inHisParty;
            case Who::Hangout:
                return f.worlds && f.sameZone;
            default:
                return false;
        }
    }

    // He has gone on while her errands run -- to a zone neither hers nor the
    // one he stood in as she set out: once he is there, where he hears her,
    // she says so -- once
    constexpr auto saysNotReady(const Who who, const bool heLeft, const bool heHears, const bool said) -> bool
    {
        return who == Who::Party && heLeft && heHears && !said;
    }
} // namespace cardian::town
