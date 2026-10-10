// Cardian: re-dressing a wild cardian at the auction house (pawn/redress.h),
// its decisions as plain rules: when to ask the census, what its answer
// raises, which pieces leave her bag, and what her support job takes. And
// the live census's dings out of sight (redress.h, cardian_ding): what the
// map does with a ding the census has planned.
#pragma once

#include "club_math.h"

#include <algorithm>
#include <charconv>
#include <cstdint>
#include <optional>
#include <set>
#include <string_view>
#include <system_error>
#include <vector>

namespace cardian::redress
{
    // cardian_redress.state: the map has asked, the census has answered with
    // her plan for the level, the map has put it on her
    enum class State
    {
        Asked,
        Ready,
        Done,
    };

    inline auto stateOf(const std::string_view text) -> std::optional<State>
    {
        if (text == "asked")
        {
            return State::Asked;
        }
        if (text == "ready")
        {
            return State::Ready;
        }
        if (text == "done")
        {
            return State::Done;
        }
        return std::nullopt;
    }

    // Her row: the level asked for, or once done the level she was dressed for
    struct Record
    {
        uint8_t level = 0;
        State   state = State::Done;
    };

    // Ask the census for her plan at the level she has now: never dressed at
    // a counter (no row), or dressed there for a lower level -- and nothing
    // already on its way (a row asked or ready waits for its own end)
    inline auto shouldAsk(const uint8_t levelNow, const std::optional<Record>& record) -> bool
    {
        if (levelNow == 0)
        {
            return false;
        }
        if (!record.has_value())
        {
            return true;
        }
        return record->state == State::Done && levelNow > record->level;
    }

    namespace detail
    {
        template <typename T>
        auto number(const std::string_view text) -> std::optional<T>
        {
            T value{};
            const auto* first = text.data();
            const auto* last  = text.data() + text.size();
            const auto  res   = std::from_chars(first, last, value);
            if (text.empty() || res.ec != std::errc{} || res.ptr != last)
            {
                return std::nullopt;
            }
            return value;
        }

        // Each piece of a list the census writes, split by `sep`
        template <typename Fn>
        void eachPiece(std::string_view text, const char sep, Fn&& fn)
        {
            while (!text.empty())
            {
                const auto at    = text.find(sep);
                const auto piece = text.substr(0, at);
                if (!piece.empty())
                {
                    fn(piece);
                }
                if (at == std::string_view::npos)
                {
                    break;
                }
                text.remove_prefix(at + 1);
            }
        }
    } // namespace detail

    // A skill and its value in the game's tenths (char_skills.value)
    struct SkillValue
    {
        uint8_t  skill = 0;
        uint16_t value = 0;

        auto operator==(const SkillValue&) const -> bool = default;
    };

    // The census's skill values for her level, as it writes them into the row:
    // "skillid:value" pairs joined by commas. A piece that does not read is
    // left out
    inline auto parseSkills(const std::string_view text) -> std::vector<SkillValue>
    {
        std::vector<SkillValue> out;
        detail::eachPiece(text, ',', [&](const std::string_view piece)
        {
            const auto colon = piece.find(':');
            if (colon == std::string_view::npos)
            {
                return;
            }
            const auto skill = detail::number<uint8_t>(piece.substr(0, colon));
            const auto value = detail::number<uint16_t>(piece.substr(colon + 1));
            if (skill.has_value() && value.has_value())
            {
                out.push_back({ *skill, *value });
            }
        });
        return out;
    }

    // What the census's values raise: each skill above what she has now, to
    // the census's value. Never lowered -- a skill the game raised past it
    // stands. have(skill) is her value now, in tenths
    template <typename Have>
    auto raises(const std::vector<SkillValue>& wanted, Have&& have) -> std::vector<SkillValue>
    {
        std::vector<SkillValue> out;
        for (const auto& w : wanted)
        {
            if (w.value > have(w.skill))
            {
                out.push_back(w);
            }
        }
        return out;
    }

    // Item ids joined by commas: the pieces and the food the census had
    // issued her before this plan (the row's `issued`)
    inline auto parseIds(const std::string_view text) -> std::set<uint16_t>
    {
        std::set<uint16_t> out;
        detail::eachPiece(text, ',', [&](const std::string_view piece)
        {
            if (const auto id = detail::number<uint16_t>(piece); id.has_value() && *id != 0)
            {
                out.insert(*id);
            }
        });
        return out;
    }

    // What a re-dress leaves in a body of the world's bags, by the census's own
    // rule for her character tables (census.py fill_bags): Mog Wardrobe 1
    // holds the census's gear alone, so a piece there the plan has no place
    // for goes, worn or not; her inventory keeps her gil, what the plan wants
    // (her food, and her pieces where her gear lives in it), the reraise
    // and warp items she buys for herself (`kept`: items::usableOnWild) and
    // a linkshell's items, the pearl she wears for her linkshell above all
    // (club.h; worn, a re-dress never takes it off), and nothing else --
    // loot, old food, a piece an older plan gave her
    inline auto keepsItem(const uint16_t itemId, const bool inInventory, const bool gil, const bool kept, const std::set<uint16_t>& wanted) -> bool
    {
        return wanted.contains(itemId) || (inInventory && (gil || kept || cardian::club::isLinkshellItem(itemId)));
    }

    // The census's answer is put on her only at the level it was planned
    // for. One planned for another level -- she dinged after asking, or the
    // live census dinged her out of sight while it waited for her next
    // stand -- is set aside, gear, spells, skills and support job alike, and
    // she asks again at the next counter: its support job could take back
    // the one the census gave her
    inline auto answerFits(const uint8_t answerLevel, const uint8_t levelNow) -> bool
    {
        return answerLevel != 0 && answerLevel == levelNow;
    }

    // The support job the census planned for the level and the job's own
    // level as she levelled it, ahead of the half the game shows (the row's
    // `sub` and `sublevel`; job 0 for none)
    struct SubPlan
    {
        uint8_t job   = 0;
        uint8_t level = 0;
    };

    // What she has of it now: her main and support jobs, the planned job's
    // own level in her job table, and her unlocked jobs (char_jobs.unlocked:
    // bit 0 the support job itself, then one bit per job id)
    struct SubNow
    {
        uint8_t  mainJob  = 0;
        uint8_t  subJob   = 0;
        uint8_t  jobLevel = 0;
        uint32_t unlocked = 0;
    };

    // What the re-dress writes: the unlocks with the support job's and the
    // job's own bits added, and the job's level raised to the plan's, never
    // lowered. The game gives the support job the lesser of that level and
    // half her main (SetSLevel). `switchJob` when she carries another support
    // job, or none: that takes the game's own job change, as her player's Mog
    // House would. The same support job at a higher level takes only what
    // the game's own level change does, and nothing a job change would
    struct SubChange
    {
        uint32_t unlocked  = 0;
        uint8_t  jobLevel  = 0;
        bool     switchJob = false;

        auto operator==(const SubChange&) const -> bool = default;
    };

    // The highest job id a support job may be: the Rune Fencer's, the last
    // job a character holds (Monstrosity's 23 is no job of hers)
    constexpr uint8_t kLastJob = 22;

    // Her support job as the census plans it: nothing when it plans none --
    // a support job is never taken away -- when the plan names her main or
    // no real job, or when she already carries it at that level, unlocked
    inline auto subChange(const SubPlan plan, const SubNow now) -> std::optional<SubChange>
    {
        if (plan.job == 0 || plan.level == 0 || plan.job > kLastJob || plan.job == now.mainJob)
        {
            return std::nullopt;
        }
        const SubChange change{ now.unlocked | 1u | (1u << plan.job), std::max(now.jobLevel, plan.level), now.subJob != plan.job };
        if (!change.switchJob && change.unlocked == now.unlocked && change.jobLevel == now.jobLevel)
        {
            return std::nullopt;
        }
        return change;
    }
    // -- The live census (RESEARCH §11.11): a body of the world levels out of sight

    // cardian_ding.state: the census has planned a ding for her (or, in a
    // city, her gear at the level she has); the map has claimed a body it
    // does not hold, for the census to write in her rows; the map has put
    // the ding on a body it holds; it is over
    enum class DingState
    {
        Planned,
        Claimed,
        Applied,
        Done,
    };

    inline auto dingStateOf(const std::string_view text) -> std::optional<DingState>
    {
        if (text == "planned")
        {
            return DingState::Planned;
        }
        if (text == "claimed")
        {
            return DingState::Claimed;
        }
        if (text == "applied")
        {
            return DingState::Applied;
        }
        if (text == "done")
        {
            return DingState::Done;
        }
        return std::nullopt;
    }

    // What the census planned: a level on `job` (her main after it), or a
    // dress at the level she has (`dress`); `gear` when her wardrobe, spells
    // and food for it are planned too, which go on only in a city
    struct DingPlan
    {
        bool    dress = false;
        uint8_t job   = 0;
        uint8_t level = 0;
        bool    gear  = false;
    };

    // Where she is now, as the map sees her
    struct DingNow
    {
        bool    standing   = false; // the map holds her body
        bool    withPlayer = false; // in a real player's party, or held for one by her contract
        bool    seen       = false; // standing in a zone a real player is in
        bool    busy       = false; // down, fighting, in an event, or carrying out an order: not now
        bool    farming    = false; // her seat farms: standing, she earns the experience she kills for
        bool    inCity     = false; // standing in a city
        uint8_t mainJob    = 0;     // the job she plays now
        uint8_t jobLevel   = 0;     // her level on the plan's job
    };

    enum class DingStep
    {
        Wait,     // not now: she is seen, with a player, busy, farming, or out of a city for a city's change
        Apply,    // the map puts it on the body it holds
        Claim,    // the map holds no body of hers: the census writes it in her rows, and she stands for nobody until then
        SetAside, // she has the level already: nothing to do
    };

    // A ding reaches her only out of sight: never in a real player's party
    // or held for him, never standing where a real player is. Standing out
    // of sight she takes it when she is free, unless her seat farms -- she
    // earns her own experience then, and keeps it -- and a dress, or a
    // change of the job she plays, waits for a city; a level she already
    // has is set aside. A body the map does not hold is the census's to
    // write
    inline auto dingStep(const DingPlan& plan, const DingNow& now) -> DingStep
    {
        if (now.withPlayer)
        {
            return DingStep::Wait;
        }
        if (!now.standing)
        {
            return DingStep::Claim;
        }
        if (now.seen || now.busy || now.farming)
        {
            return DingStep::Wait;
        }
        const bool switches = plan.job != 0 && plan.job != now.mainJob;
        if (!plan.dress && now.jobLevel >= plan.level)
        {
            return DingStep::SetAside;
        }
        if ((plan.dress || switches) && !now.inCity)
        {
            return DingStep::Wait;
        }
        return DingStep::Apply;
    }

    // Her gear, spells and food go on with a ding only where it was planned
    // and in a city
    inline auto dressesNow(const DingPlan& plan, const DingNow& now) -> bool
    {
        return plan.gear && now.inCity;
    }

    // A claim the census has not finished in this long is taken back: the
    // watcher is down or stuck, and she may stand again. Her ding waits for
    // the next look
    constexpr uint32_t kClaimLapseSeconds = 120;

    inline auto claimLapsed(const uint32_t secondsHeld) -> bool
    {
        return secondsHeld >= kClaimLapseSeconds;
    }

    // A body's census row as far as her target goes (cardian_census): her
    // first job's, her second's, and the later jobs her support windows had
    // her take up, "job:target" pairs joined by commas (census.py take_up)
    struct CareerTargets
    {
        uint8_t          job     = 0;
        uint8_t          target  = 0;
        uint8_t          job2    = 0;
        uint8_t          target2 = 0;
        std::string_view later;
    };

    // Her target on the job she plays (census.py target_on): her cap while a
    // player is near. A job not of her career reads her first job's
    inline auto targetOn(const CareerTargets& row, const uint8_t playing) -> uint8_t
    {
        if (playing == row.job || playing == 0)
        {
            return row.target;
        }
        if (row.job2 != 0 && playing == row.job2)
        {
            return row.target2;
        }
        std::string_view rest = row.later;
        while (!rest.empty())
        {
            const auto comma = rest.find(',');
            const auto piece = rest.substr(0, comma);
            rest             = comma == std::string_view::npos ? std::string_view{} : rest.substr(comma + 1);
            const auto colon = piece.find(':');
            if (colon == std::string_view::npos)
            {
                continue;
            }
            unsigned job    = 0;
            unsigned target = 0;
            const auto jobText    = piece.substr(0, colon);
            const auto targetText = piece.substr(colon + 1);
            if (std::from_chars(jobText.data(), jobText.data() + jobText.size(), job).ec != std::errc{} ||
                std::from_chars(targetText.data(), targetText.data() + targetText.size(), target).ec != std::errc{})
            {
                continue;
            }
            if (job == playing)
            {
                return static_cast<uint8_t>(std::min(target, 255u));
            }
        }
        return row.target;
    }
} // namespace cardian::redress
