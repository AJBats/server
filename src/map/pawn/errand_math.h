// Cardian: the linkshell's errands (pawn/errands.h), their rules as plain
// functions: the kinds and whom each is for, the row's words, the clock an
// errand runs on and where it has her, what a call back keeps, the steps of
// gearing up in town, a level venture's bounds and the share of a level it
// keeps, and a finished quest or mission in her log.
#pragma once

#include <algorithm>
#include <charconv>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace cardian::errand
{
    // What she is sent to do: the Link's CL_ERRAND_* by value, and the row's
    // kind by name. Level is a level on one of her jobs, which the live
    // census levels while she is away (census.py LevelVentures). Money is a
    // venture at a profession of hers (professions.h)
    enum class Kind : uint8_t
    {
        None  = 0,
        Gear  = 1,
        Quest = 2,
        Level = 3,
        Money = 4,
        Rank  = 5,
    };

    // Where it stands: she is walking off (to a zone line, the auction house,
    // the guard, and back), or she is gone from the world. The Link's
    // CL_ERRAND_GOING and CL_ERRAND_AWAY by value
    enum class State : uint8_t
    {
        Going = 1,
        Away  = 2,
    };

    // What she is to the player: the Link's CL_CLUB_* by value
    enum class Member : uint8_t
    {
        Alt     = 0, // a character of his own account
        Owned   = 1, // a cardian his account owns
        Wild    = 2, // one of the world's wearing his pearl
        Recruit = 3, // one of the world's at the pearl's lock with him, no pearl: not a member
    };

    inline auto kindName(const Kind kind) -> std::string_view
    {
        switch (kind)
        {
            case Kind::Gear:
                return "gear";
            case Kind::Quest:
                return "quest";
            case Kind::Level:
                return "level";
            case Kind::Money:
                return "money";
            case Kind::Rank:
                return "rank";
            default:
                return "";
        }
    }

    inline auto kindOf(const std::string_view text) -> std::optional<Kind>
    {
        for (const auto kind : { Kind::Gear, Kind::Quest, Kind::Level, Kind::Money, Kind::Rank })
        {
            if (kindName(kind) == text)
            {
                return kind;
            }
        }
        return std::nullopt;
    }

    inline auto stateName(const State state) -> std::string_view
    {
        return state == State::Away ? "away" : "going";
    }
    inline auto stateOf(const std::string_view text) -> std::optional<State>
    {
        for (const auto state : { State::Going, State::Away })
        {
            if (stateName(state) == text)
            {
                return state;
            }
        }
        return std::nullopt;
    }

    // Out of the world: away
    constexpr auto gone(const State state) -> bool
    {
        return state == State::Away;
    }

    // A money venture's lengths: hours of the game clock, an Earth hour each
    // while unpaused (the user, 2026-10-10: the hourly rate a player thinks in)
    constexpr auto ventureHours(const uint32_t hours) -> bool
    {
        return hours == 1 || hours == 2;
    }

    // The kinds with behaviour behind them
    constexpr auto built(const Kind kind) -> bool
    {
        return kind == Kind::Gear || kind == Kind::Quest || kind == Kind::Rank || kind == Kind::Money || kind == Kind::Level;
    }

    // Whom a kind is for (the user, 2026-10-09, RESEARCH §11.13). Gear up is
    // the auction house's re-dress, which the census plans for the world's
    // adventurers alone, so it is for one of the world's wearing his pearl:
    // she gears and funds herself. A quest, a rank catch-up through her
    // nation's missions and a level are for every member. Earning money is
    // for his alts and owned cardians: the world's never earn him gil. A
    // recruit is no member
    constexpr auto forMember(const Kind kind, const Member member) -> bool
    {
        if (member == Member::Recruit)
        {
            return false;
        }
        switch (kind)
        {
            case Kind::Gear:
                return member == Member::Wild;
            case Kind::Quest:
            case Kind::Rank:
            case Kind::Level:
                return true;
            case Kind::Money:
                return member != Member::Wild;
            default:
                return false;
        }
    }

    // What the page may send her on: built, and for her
    constexpr auto offered(const Kind kind, const Member member) -> bool
    {
        return built(kind) && forMember(kind, member);
    }

    struct Errand
    {
        Kind     kind     = Kind::None;
        State    state    = State::Going;
        uint32_t started  = 0; // the game clock (Unix seconds) as she was sent
        uint32_t left     = 0; // the game clock as she left the world; 0 while she walks off
        uint32_t ends     = 0; // the game clock as she is back, once away on a clock; 0 off the clock
        uint32_t target   = 0; // a kind that ends on its target: what it is (a level)
        uint32_t progress = 0; // what she has made of it so far, in the kind's own measure
        std::vector<uint16_t> route; // the zones she crosses, in order (a quest's)
    };

    // Gone from the world now, back `seconds` later: the clock starts as she
    // leaves it, not as she is sent, since the walk off is seen. 0 seconds:
    // a kind that ends on its target runs off the clock
    inline auto goAway(Errand errand, const uint32_t now, const uint32_t seconds) -> Errand
    {
        errand.state = State::Away;
        errand.left  = now;
        errand.ends  = seconds > 0 ? now + seconds : 0;
        return errand;
    }

    // Her clock has run out
    inline auto due(const Errand& errand, const uint32_t now) -> bool
    {
        return errand.state == State::Away && errand.ends != 0 && now >= errand.ends;
    }

    // Seconds until she is back: 0 off the clock, and once due
    inline auto secondsLeft(const Errand& errand, const uint32_t now) -> uint32_t
    {
        if (errand.state != State::Away || errand.ends == 0 || now >= errand.ends)
        {
            return 0;
        }
        return errand.ends - now;
    }

    // The zone of her route she is crossing now: the route split evenly over
    // her time away -- the first zone until she is away, the last once due;
    // 0 with no route
    inline auto legAt(const Errand& errand, const uint32_t now) -> uint16_t
    {
        if (errand.route.empty())
        {
            return 0;
        }
        if (errand.state != State::Away || errand.ends == 0)
        {
            return errand.route.front();
        }
        const uint64_t whole   = errand.ends > errand.left ? static_cast<uint64_t>(errand.ends - errand.left) : 0;
        const uint64_t elapsed = now > errand.left ? std::min<uint64_t>(now - errand.left, whole) : 0;
        if (whole == 0 || elapsed >= whole)
        {
            return errand.route.back();
        }
        const auto index = static_cast<std::size_t>(elapsed * errand.route.size() / whole);
        return errand.route[std::min(index, errand.route.size() - 1)];
    }

    // What a call back keeps. Setting out, nothing is done yet. Away, a quest
    // is done whole or not at all: whole once its clock has run out (the call
    // came as she was coming back anyway), nothing before. A rank catch-up
    // keeps the missions her time away has covered (missionsDone), and a kind
    // that counts its progress keeps what it counted
    enum class Kept : uint8_t
    {
        Nothing,
        Progress,
        Whole,
    };

    inline auto keptOnCallBack(const Errand& errand, const uint32_t now) -> Kept
    {
        if (errand.state != State::Away)
        {
            return Kept::Nothing;
        }
        if (due(errand, now))
        {
            return Kept::Whole;
        }
        switch (errand.kind)
        {
            case Kind::Level:
            case Kind::Money:
                return errand.progress > 0 ? Kept::Progress : Kept::Nothing;
            case Kind::Rank:
                return Kept::Progress;
            default:
                return Kept::Nothing;
        }
    }

    // A rank catch-up's missions done by now: as many, in order, as her time
    // away covers, each its own minutes (on the game clock)
    inline auto missionsDone(const std::vector<uint16_t>& minutes, const uint64_t secondsAway) -> std::size_t
    {
        uint64_t    spent = 0;
        std::size_t done  = 0;
        for (const auto m : minutes)
        {
            spent += static_cast<uint64_t>(m) * 60;
            if (spent > secondsAway)
            {
                break;
            }
            ++done;
        }
        return done;
    }

    // A list of whole numbers as the args keep it: "20,30,45"; a piece that
    // is not one is left out
    inline auto numbersOf(const std::string_view text) -> std::vector<uint16_t>
    {
        std::vector<uint16_t> out;
        std::size_t           at = 0;
        while (at < text.size())
        {
            const auto end   = std::min(text.find(',', at), text.size());
            const auto piece = text.substr(at, end - at);
            uint16_t   value{};
            const auto res = std::from_chars(piece.data(), piece.data() + piece.size(), value);
            if (!piece.empty() && res.ec == std::errc{} && res.ptr == piece.data() + piece.size())
            {
                out.push_back(value);
            }
            at = end + 1;
        }
        return out;
    }

    inline auto numbersText(const std::vector<uint16_t>& numbers) -> std::string
    {
        std::string out;
        for (const auto n : numbers)
        {
            out += out.empty() ? "" : ",";
            out += std::to_string(n);
        }
        return out;
    }

    // ---- the row ------------------------------------------------------------

    // The kind's own parameters, as cardian_errands.args keeps them:
    // "key=value;key=value", keys sorted. A piece without '=' or with an
    // empty key is left out; a value holds no ';' or '='
    using Args = std::map<std::string, std::string, std::less<>>;

    inline auto parseArgs(const std::string_view text) -> Args
    {
        Args args;
        std::size_t at = 0;
        while (at <= text.size())
        {
            const auto end   = std::min(text.find(';', at), text.size());
            const auto piece = text.substr(at, end - at);
            const auto eq    = piece.find('=');
            if (eq != std::string_view::npos && eq > 0)
            {
                args[std::string(piece.substr(0, eq))] = std::string(piece.substr(eq + 1));
            }
            at = end + 1;
        }
        return args;
    }

    inline auto argsText(const Args& args) -> std::string
    {
        std::string out;
        for (const auto& [key, value] : args)
        {
            if (key.empty() || key.find_first_of(";=") != std::string::npos || value.find_first_of(";=") != std::string::npos)
            {
                continue;
            }
            out += out.empty() ? "" : ";";
            out += key;
            out += '=';
            out += value;
        }
        return out;
    }

    // A whole number in the args; nothing when absent or not a number
    inline auto numberArg(const Args& args, const std::string_view key) -> std::optional<uint32_t>
    {
        const auto it = args.find(key);
        if (it == args.end() || it->second.empty())
        {
            return std::nullopt;
        }
        uint32_t value{};
        const auto* first = it->second.data();
        const auto* last  = it->second.data() + it->second.size();
        const auto  res   = std::from_chars(first, last, value);
        if (res.ec != std::errc{} || res.ptr != last)
        {
            return std::nullopt;
        }
        return value;
    }

    // A route as the args keep it: zone ids, comma-separated. A piece that
    // is not a zone id is left out
    inline auto routeOf(const std::string_view text) -> std::vector<uint16_t>
    {
        std::vector<uint16_t> route;
        std::size_t           at = 0;
        while (at < text.size())
        {
            const auto end   = std::min(text.find(',', at), text.size());
            const auto piece = text.substr(at, end - at);
            uint16_t   zone{};
            const auto res = std::from_chars(piece.data(), piece.data() + piece.size(), zone);
            if (!piece.empty() && res.ec == std::errc{} && res.ptr == piece.data() + piece.size() && zone != 0)
            {
                route.push_back(zone);
            }
            at = end + 1;
        }
        return route;
    }

    inline auto routeText(const std::vector<uint16_t>& route) -> std::string
    {
        std::string out;
        for (const auto zone : route)
        {
            out += out.empty() ? "" : ",";
            out += std::to_string(zone);
        }
        return out;
    }

    // ---- gearing up in town -------------------------------------------------

    // The user, 2026-10-09: in town it plays like a maneuver. She walks to
    // the auction counter and waits there for the census's answer, walks to
    // her nation's guard and buys her scrolls, and comes back to where she
    // started -- or, in his party with him in her zone, takes her place
    // behind him again. A walk that does not arrive in time, or a wait past
    // its time, moves on: nothing holds her in town for ever
    enum class GearStep : uint8_t
    {
        ToCounter = 0,
        AtCounter = 1,
        ToGuard   = 2,
        AtGuard   = 3,
        Back      = 4,
        Done      = 5,
    };

    struct GearFacts
    {
        bool arrived  = false; // the step's walk is over: its end reached
        bool timedOut = false; // the step has run past its time
        bool dressed  = false; // the census's answer is on her, or none was needed
        bool hasGuard    = false; // a guard who sells stands in her zone
        bool returns     = false; // she walks back to where she started (else she rejoins him)
        bool toConsulate = false; // the guard turned her away, and her nation's consulate stands elsewhere in her city
    };

    inline auto nextGearStep(const GearStep step, const GearFacts& facts) -> GearStep
    {
        const auto afterCounter = facts.hasGuard ? GearStep::ToGuard : (facts.returns ? GearStep::Back : GearStep::Done);
        switch (step)
        {
            case GearStep::ToCounter:
                return facts.arrived ? GearStep::AtCounter : (facts.timedOut ? afterCounter : step);
            case GearStep::AtCounter:
                return facts.dressed || facts.timedOut ? afterCounter : step;
            case GearStep::ToGuard:
                return facts.arrived ? GearStep::AtGuard : (facts.timedOut ? (facts.returns ? GearStep::Back : GearStep::Done) : step);
            case GearStep::AtGuard:
                return facts.toConsulate ? GearStep::ToGuard : (facts.returns ? GearStep::Back : GearStep::Done);
            case GearStep::Back:
                return facts.arrived || facts.timedOut ? GearStep::Done : step;
            default:
                return GearStep::Done;
        }
    }

    // A step she walks in
    constexpr auto walks(const GearStep step) -> bool
    {
        return step == GearStep::ToCounter || step == GearStep::ToGuard || step == GearStep::Back;
    }

    // ---- a level venture ------------------------------------------------------

    // The jobs a level venture levels: Warrior to Puppetmaster, the era's,
    // which are the jobs the live census knows (census.py JOB_COLUMNS)
    constexpr uint8_t kFirstLevelJob = 1;
    constexpr uint8_t kLastLevelJob  = 18;

    // A job unlocked in char_jobs.unlocked's mask (bit 0 is the support job)
    constexpr auto jobUnlocked(const uint32_t mask, const uint8_t job) -> bool
    {
        return job < 32 && (mask & (1u << job)) != 0;
    }

    // Her level on a job she has unlocked: a job the game unlocked and she
    // has never played reads 0, and starts at 1
    constexpr auto levelOn(const uint8_t level) -> uint8_t
    {
        return level == 0 ? 1 : level;
    }

    // A level venture he may send her on: one of the jobs it levels,
    // unlocked, to a level above hers on it and at most his highest level on
    // any job of his (the user, 2026-10-18)
    constexpr auto levelFits(const uint32_t unlockedMask, const uint8_t job, const uint8_t herLevel, const uint32_t target, const uint8_t hisHighest) -> bool
    {
        return job >= kFirstLevelJob && job <= kLastLevelJob && jobUnlocked(unlockedMask, job) && target > levelOn(herLevel) && target <= hisHighest;
    }

    // Any job of hers a level venture could raise now, her levels by job id
    // as the Link carries them: the page offers Level while one can
    inline auto anyLevelFits(const uint32_t unlockedMask, const std::span<const uint8_t> levels, const uint8_t hisHighest) -> bool
    {
        for (uint8_t job = kFirstLevelJob; job <= kLastLevelJob && job < levels.size(); ++job)
        {
            if (levelFits(unlockedMask, job, levels[job], static_cast<uint32_t>(levelOn(levels[job])) + 1, hisHighest))
            {
                return true;
            }
        }
        return false;
    }

    // The share of a level she keeps as experience when he calls her back
    // mid-level (the census's count of her next level, cardian_venture's
    // progress): that share of the experience her next level asks, never the
    // whole of it -- a ding is the census's, or the game's as she earns it
    constexpr auto keptExp(const double share, const uint32_t toNext) -> uint32_t
    {
        if (toNext == 0 || !(share > 0.0))
        {
            return 0;
        }
        const double exp = share * static_cast<double>(toNext);
        return exp >= static_cast<double>(toNext - 1) ? toNext - 1 : static_cast<uint32_t>(exp);
    }

    // The share of her next level as the page shows it, in hundredths: 0 to 99
    constexpr auto sharePercent(const double share) -> uint8_t
    {
        if (!(share > 0.0))
        {
            return 0;
        }
        return static_cast<uint8_t>(std::clamp(static_cast<int>(share * 100.0), 0, 99));
    }

    // The conquest points a level venture earns her for experience behind
    // its levels, or kept toward her next: the server's rate under Signet
    // (conquest_system.cpp, census.py CP_RATE), all of it for his alt or his
    // own cardian (census.py pay_venture_points, on what the venture gained
    // her: ventureGain), the census's estimate of what a body still holds of
    // it for one of the world's, whose levels alone are paid (CP_KEPT,
    // pay_level)
    constexpr double kVentureCpRate = 0.10;
    constexpr double kVentureCpKept = 0.75;

    constexpr auto venturePoints(const uint64_t exp, const bool worlds) -> uint32_t
    {
        const double points = static_cast<double>(exp) * kVentureCpRate * (worlds ? kVentureCpKept : 1.0);
        return points >= 2147483647.0 ? 2147483647u : static_cast<uint32_t>(points);
    }

    // What a level venture gained her in experience on the job: the
    // experience behind the levels it gave her and any kept toward her next,
    // less what she already held toward her next as she set out. His alt's
    // and his own cardian's conquest points are paid on this alone, so a
    // call back pays nothing for experience she brought with her (census.py
    // venture.gained_exp)
    constexpr auto ventureGain(const uint64_t levelsExp, const uint64_t kept, const uint64_t had) -> uint64_t
    {
        const uint64_t total = levelsExp + kept;
        return total > had ? total - had : 0;
    }

    // ---- a level venture's banked levels --------------------------------------

    // A whole level in the census's count, give or take the floats (census.py
    // pace.DING_AT)
    constexpr double kWholeLevel = 1.0 - 1e-9;

    // The census's count of a level venture with the time it has not counted
    // yet added: `rate` levels an hour (cardian_venture.rate) over the real
    // seconds from `countedAt` -- the row's counted_at; 0 before the census
    // has run the count once, which adds nothing -- to `until`, at the census
    // clock's `speed`, and never past the levels left to her target
    constexpr auto ventureCount(const double progress, const double rate, const double countedAt, const double until, const double speed,
                                const uint32_t levelsLeft) -> double
    {
        double count = progress > 0.0 ? progress : 0.0;
        if (rate > 0.0 && speed > 0.0 && countedAt > 0.0 && until > countedAt)
        {
            count += rate * speed * (until - countedAt) / 3600.0;
        }
        return std::min(count, static_cast<double>(levelsLeft));
    }

    // The levels one ding of the venture lands: every whole level its count
    // holds, up to the levels left to her target; 0 while no whole level is
    // earned (census.py venture.levels_due)
    constexpr auto wholeLevels(const double count, const uint32_t levelsLeft) -> uint32_t
    {
        if (levelsLeft == 0 || !(count >= kWholeLevel))
        {
            return 0;
        }
        const double capped = std::min(count, static_cast<double>(levelsLeft));
        return std::clamp<uint32_t>(static_cast<uint32_t>(capped + (1.0 - kWholeLevel)), 1u, levelsLeft);
    }

    // A level venture he has called back is over only once what it banked
    // has landed: no whole level left in its count, and no ding of it planned
    // or being written in her rows
    constexpr auto recallWaits(const double count, const uint32_t levelsLeft, const bool dingOpen) -> bool
    {
        return dingOpen || wholeLevels(count, levelsLeft) > 0;
    }

    // A level venture's pace as people speak of it: experience an hour at
    // her level now, `levelsAnHour` times what her next level asks
    constexpr auto expPerHour(const double levelsAnHour, const uint32_t toNext) -> uint32_t
    {
        if (!(levelsAnHour > 0.0))
        {
            return 0;
        }
        const double exp = levelsAnHour * static_cast<double>(toNext);
        return exp >= 4294967295.0 ? 4294967295u : static_cast<uint32_t>(exp);
    }

    // The minutes her next level is still off: what is left of it at
    // `levelsAnHour`, rounded up; 0 with no pace
    constexpr auto minutesToNext(const double share, const double levelsAnHour) -> uint16_t
    {
        if (!(levelsAnHour > 0.0))
        {
            return 0;
        }
        const double left    = 1.0 - std::clamp(share, 0.0, 1.0);
        const double minutes = left / levelsAnHour * 60.0;
        const auto   whole   = static_cast<uint32_t>(minutes);
        return static_cast<uint16_t>(std::min<uint32_t>(65535u, whole + (static_cast<double>(whole) < minutes ? 1u : 0u)));
    }

    // Her skills as a level venture's report keeps them: "skill:tenths,..."
    inline auto skillsText(const std::map<uint16_t, uint16_t>& skills) -> std::string
    {
        std::string out;
        for (const auto& [skill, value] : skills)
        {
            out += (out.empty() ? "" : ",") + std::to_string(skill) + ":" + std::to_string(value);
        }
        return out;
    }

    // And read back; a malformed pair is skipped
    inline auto skillsOfText(const std::string_view text) -> std::map<uint16_t, uint16_t>
    {
        std::map<uint16_t, uint16_t> out;
        std::size_t                  at = 0;
        while (at < text.size())
        {
            const auto end   = std::min(text.find(',', at), text.size());
            const auto pair  = text.substr(at, end - at);
            const auto colon = pair.find(':');
            uint16_t   skill = 0;
            uint16_t   value = 0;
            if (colon != std::string_view::npos && std::from_chars(pair.data(), pair.data() + colon, skill).ec == std::errc{} &&
                std::from_chars(pair.data() + colon + 1, pair.data() + pair.size(), value).ec == std::errc{})
            {
                out[skill] = value;
            }
            at = end + 1;
        }
        return out;
    }

    // A skill a level venture raised: its id, and its value as she set out and now
    struct SkillRise
    {
        uint16_t skill = 0;
        uint16_t from  = 0;
        uint16_t to    = 0;

        constexpr auto operator==(const SkillRise&) const -> bool = default;
    };

    // The skills that rose between the two, by skill id
    inline auto skillsRisen(const std::map<uint16_t, uint16_t>& before, const std::map<uint16_t, uint16_t>& after) -> std::vector<SkillRise>
    {
        std::vector<SkillRise> out;
        for (const auto& [skill, value] : after)
        {
            const auto     it  = before.find(skill);
            const uint16_t was = it != before.end() ? it->second : 0;
            if (value > was)
            {
                out.push_back({ skill, was, value });
            }
        }
        return out;
    }

    // ---- her log --------------------------------------------------------------

    // The quest log's bits: an id past them is no quest of the log's
    constexpr uint16_t kQuestsPerLog   = 32 * 8;
    constexpr uint16_t kMissionsPerLog = 64;

    inline auto questDone(const uint8_t (&complete)[32], const uint16_t id) -> bool
    {
        return id < kQuestsPerLog && (complete[id / 8] & (1 << (id % 8))) != 0;
    }

    // A finished quest in her log as the game's completeQuest leaves it: off
    // the current list, on the completed one. False when it was done already
    // (nothing changes) or the id is past the log
    inline auto markQuestDone(uint8_t (&current)[32], uint8_t (&complete)[32], const uint16_t id) -> bool
    {
        if (id >= kQuestsPerLog || questDone(complete, id))
        {
            return false;
        }
        current[id / 8]  = static_cast<uint8_t>(current[id / 8] & ~(1 << (id % 8)));
        complete[id / 8] = static_cast<uint8_t>(complete[id / 8] | (1 << (id % 8)));
        return true;
    }

    inline auto missionDone(const bool (&complete)[64], const uint16_t id) -> bool
    {
        return id < kMissionsPerLog && complete[id];
    }

    // A finished mission as completeMission leaves it: done, and no longer
    // her current one if it was (`none`: the log's empty current, 65535 on a
    // nation's log, 0 past them). False when it was done already or the id is
    // past the log
    inline auto markMissionDone(uint16_t& current, bool (&complete)[64], const uint16_t id, const uint16_t none) -> bool
    {
        if (id >= kMissionsPerLog || complete[id])
        {
            return false;
        }
        complete[id] = true;
        if (current == id)
        {
            current = none;
        }
        return true;
    }
} // namespace cardian::errand
