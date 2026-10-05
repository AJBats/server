// Cardian: re-dressing a wild cardian at the auction house (pawn/redress.h),
// its decisions as plain rules: when to ask the census, what its answer
// raises, and which pieces leave her bag.
#pragma once

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

    // Item ids joined by commas: the pieces the census had issued her before
    // this plan (the row's `issued`)
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

    // A piece in her bag the re-dress takes away: one the census issued her,
    // which her new plan has no place for, and which she is not wearing. A
    // piece anyone else gave her stays
    inline auto dropsPiece(const uint16_t itemId, const bool worn, const std::set<uint16_t>& plan, const std::set<uint16_t>& issued) -> bool
    {
        return !worn && issued.contains(itemId) && !plan.contains(itemId);
    }
} // namespace cardian::redress
