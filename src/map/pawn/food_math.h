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

#include "food_weights.h"
#include "party_roles.h"

#include "common/cbasetypes.h"
#include "data/enums/job.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// Food (RESEARCH §19): the cardians eat with the player. Her party role
// says what she eats, and her "Self -> Eat with the player" row eats it:
// every 30 seconds between fights, when the player has a food effect on and
// she has none. A Healer whose food is a cookie (a short MP food) eats it
// instead as she kneels for MP, armed by the player's food. A body of the
// world eats the census's pick for her (cardian_food, tools/world/census.py)
// and is kept topped up while she is in the player's party; an owned cardian
// eats the best food for her role that her own bags hold.
//
// A food is ranked as gear is (RESEARCH §19.6, tools/world/food_rank.py):
// every stat it gives her now, weighed by the gear scorer's weights for her
// seat (food_weights.h, a copy of tools/world/gear_roles.py's), the
// food-only percentages turned into the points they add at her own attack,
// defence, accuracy and the rest.
//
// Pure, so xi_test pins it (cardian_food_tests.cpp): a food's facts as its
// item script states them, read by the rule tools/economy/gamedata.py
// parse_food reads them by -- the two must agree -- what a food gives her
// and its score, the owned cardian's pick, and the moments she eats.
namespace cardian::food
{
    constexpr uint32 kLongSeconds = 1800; // a food lasting this long is eaten with the player; a shorter MP food is a cookie
    constexpr int32  kHealerFloor = 3;    // MP recovered while healing: the least a Healer's food gives
    constexpr uint32 kStock       = 12;   // a body of the world's stock of each food, as the census lays it (FOOD_STOCK)
    constexpr auto   kCheckEvery  = std::chrono::seconds(30);

    // Who may eat it (xi.itemUtils.foodOnItemCheck): raw fish a Mithra,
    // raw meat a Galka, or anyone with the mod that lets them
    enum class Kind : uint8
    {
        Basic,
        RawFish,
        RawMeat,
    };

    // One stat a food adds, by its xi.mod name
    struct Mod
    {
        std::string name;
        int32       value = 0;

        auto operator==(const Mod&) const -> bool = default;
    };

    // A food as its script states it: how long its effect lasts, who may eat
    // it, and every mod its onEffectGain adds, summed by name. A script whose
    // onEffectGain branches or loops (by race, by party size) is conditional:
    // its mods are left out and no rule picks it
    struct Facts
    {
        uint32           duration    = 0; // seconds
        Kind             kind        = Kind::Basic;
        bool             conditional = false;
        std::vector<Mod> mods;

        auto mod(const std::string_view name) const -> int32
        {
            for (const auto& m : mods)
            {
                if (m.name == name)
                {
                    return m.value;
                }
            }
            return 0;
        }

        void add(const std::string_view name, const int32 value)
        {
            for (auto& m : mods)
            {
                if (m.name == name)
                {
                    m.value += value;
                    return;
                }
            }
            mods.push_back({ std::string(name), value });
        }
    };

    namespace detail
    {
        constexpr auto isWordChar(const char c) -> bool
        {
            return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
        }

        constexpr auto isSpace(const char c) -> bool
        {
            return c == ' ' || c == '\t' || c == '\r' || c == '\n';
        }

        constexpr auto skipSpace(const std::string_view text, std::size_t at) -> std::size_t
        {
            while (at < text.size() && isSpace(text[at]))
            {
                ++at;
            }
            return at;
        }

        // `word` at `at`, a whole word: no word character either side
        constexpr auto wordAt(const std::string_view text, const std::size_t at, const std::string_view word) -> bool
        {
            return text.substr(at, word.size()) == word && (at == 0 || !isWordChar(text[at - 1])) &&
                   (at + word.size() >= text.size() || !isWordChar(text[at + word.size()]));
        }

        // A run of digits at `at`, with a leading minus when `sign`; the
        // number and where it ends, or nullopt
        struct Number
        {
            int32       value = 0;
            std::size_t end   = 0;
        };

        constexpr auto numberAt(const std::string_view text, std::size_t at, const bool sign) -> std::optional<Number>
        {
            bool negative = false;
            if (sign && at < text.size() && text[at] == '-')
            {
                negative = true;
                ++at;
            }
            const std::size_t first = at;
            int64             value = 0;
            while (at < text.size() && text[at] >= '0' && text[at] <= '9')
            {
                value = std::min<int64>(value * 10 + (text[at] - '0'), 1000000000);
                ++at;
            }
            if (at == first)
            {
                return std::nullopt;
            }
            return Number{ static_cast<int32>(negative ? -value : value), at };
        }

        // The braces after the first `xi.effect.FOOD, {` -- the food effect's
        // parameters as onItemUse puts them on -- or nullopt
        constexpr auto effectParams(const std::string_view text) -> std::optional<std::string_view>
        {
            constexpr std::string_view kEffect = "xi.effect.FOOD";
            for (std::size_t at = text.find(kEffect); at != std::string_view::npos; at = text.find(kEffect, at + 1))
            {
                std::size_t next = at + kEffect.size();
                if (next < text.size() && isWordChar(text[next]))
                {
                    continue;
                }
                next = skipSpace(text, next);
                if (next >= text.size() || text[next] != ',')
                {
                    continue;
                }
                next = skipSpace(text, next + 1);
                if (next >= text.size() || text[next] != '{')
                {
                    continue;
                }
                const auto close = text.find('}', next + 1);
                if (close == std::string_view::npos)
                {
                    return std::nullopt;
                }
                return text.substr(next + 1, close - next - 1);
            }
            return std::nullopt;
        }

        // `duration = n` or `duration = n * m` in the effect's parameters
        constexpr auto durationIn(const std::string_view params) -> std::optional<uint32>
        {
            constexpr std::string_view kDuration = "duration";
            for (std::size_t at = params.find(kDuration); at != std::string_view::npos; at = params.find(kDuration, at + 1))
            {
                if (at > 0 && isWordChar(params[at - 1]))
                {
                    continue;
                }
                std::size_t next = skipSpace(params, at + kDuration.size());
                if (next >= params.size() || params[next] != '=')
                {
                    continue;
                }
                const auto first = numberAt(params, skipSpace(params, next + 1), false);
                if (!first.has_value())
                {
                    continue;
                }
                uint32 seconds = static_cast<uint32>(first->value);
                next           = skipSpace(params, first->end);
                if (next < params.size() && params[next] == '*')
                {
                    if (const auto times = numberAt(params, skipSpace(params, next + 1), false); times.has_value())
                    {
                        seconds *= static_cast<uint32>(times->value);
                    }
                }
                return seconds;
            }
            return std::nullopt;
        }

        // The body of `.onEffectGain = function (...)`, up to the `end` that
        // starts a line; empty when the script has none
        constexpr auto gainBody(const std::string_view text) -> std::string_view
        {
            constexpr std::string_view kGain = ".onEffectGain";
            const auto                 at    = text.find(kGain);
            if (at == std::string_view::npos)
            {
                return {};
            }
            std::size_t next = skipSpace(text, at + kGain.size());
            if (next >= text.size() || text[next] != '=')
            {
                return {};
            }
            next = skipSpace(text, next + 1);
            if (!wordAt(text, next, "function"))
            {
                return {};
            }
            const auto close = text.find(')', next);
            if (close == std::string_view::npos)
            {
                return {};
            }
            for (std::size_t line = text.find('\n', close); line != std::string_view::npos; line = text.find('\n', line + 1))
            {
                if (wordAt(text, line + 1, "end"))
                {
                    return text.substr(close + 1, line - close);
                }
            }
            return {};
        }

        // One line of the body, its comment cut off: a branch or a loop
        // makes the food conditional, an `effect:addMod(xi.mod.X, n)` adds
        inline void readLine(Facts& f, std::string_view line)
        {
            if (const auto comment = line.find("--"); comment != std::string_view::npos)
            {
                line = line.substr(0, comment);
            }
            for (std::size_t at = 0; at < line.size(); ++at)
            {
                if (wordAt(line, at, "if") || wordAt(line, at, "for") || wordAt(line, at, "while"))
                {
                    f.conditional = true;
                }
            }
            constexpr std::string_view kAdd = "effect:addMod(";
            for (std::size_t at = line.find(kAdd); at != std::string_view::npos; at = line.find(kAdd, at + 1))
            {
                if (at > 0 && isWordChar(line[at - 1]))
                {
                    continue;
                }
                std::size_t next = skipSpace(line, at + kAdd.size());
                constexpr std::string_view kMod = "xi.mod.";
                if (line.substr(next, kMod.size()) != kMod)
                {
                    continue;
                }
                next             = next + kMod.size();
                const auto first = next;
                while (next < line.size() && isWordChar(line[next]))
                {
                    ++next;
                }
                const auto name = line.substr(first, next - first);
                next            = skipSpace(line, next);
                if (next >= line.size() || line[next] != ',')
                {
                    continue;
                }
                const auto value = numberAt(line, skipSpace(line, next + 1), true);
                if (!value.has_value() || skipSpace(line, value->end) >= line.size() || line[skipSpace(line, value->end)] != ')')
                {
                    continue;
                }
                f.add(name, value->value);
            }
        }
    } // namespace detail

    // An item script read as food: nullopt when it puts no food effect on
    // its eater or says nothing of how long the effect lasts
    inline auto parseScript(const std::string_view text) -> std::optional<Facts>
    {
        const auto params = detail::effectParams(text);
        if (!params.has_value())
        {
            return std::nullopt;
        }
        const auto duration = detail::durationIn(*params);
        if (!duration.has_value())
        {
            return std::nullopt;
        }
        Facts f;
        f.duration = *duration;
        if (text.find("xi.foodType.RAW_FISH") != std::string_view::npos)
        {
            f.kind = Kind::RawFish;
        }
        else if (text.find("xi.foodType.RAW_MEAT") != std::string_view::npos)
        {
            f.kind = Kind::RawMeat;
        }
        auto body = detail::gainBody(text);
        while (!body.empty())
        {
            const auto end = body.find('\n');
            detail::readLine(f, body.substr(0, end));
            if (end == std::string_view::npos)
            {
                break;
            }
            body.remove_prefix(end + 1);
        }
        if (f.conditional)
        {
            Facts plain;
            plain.duration    = f.duration;
            plain.kind        = f.kind;
            plain.conditional = true;
            return plain;
        }
        return f;
    }

    // The gear scorer's role whose weights rank her food: a seat
    enum class Seat : uint8
    {
        Melee,
        Thief,
        NinjaTank,
        BloodTank,
        BlackMage,
        WhiteMage,
        RedMage,
        Ranged,
    };

    // Her seat in a party role (tools/world/food_rank.py seat_of): a Tank a
    // blood tank, a ninja tank on a Ninja; a Healer a white mage, whatever her
    // job; Damage her job's own fighting seat -- a black mage on a Black Mage,
    // a thief on a Thief, ranged on a Ranger or a Corsair, melee on everyone
    // else; a Puller and no role eat as Damage
    constexpr auto seatFor(const cardian::party::Role role, const xi::Job job) -> Seat
    {
        switch (role)
        {
            case cardian::party::Role::Tank:
                return job == xi::Job::NIN ? Seat::NinjaTank : Seat::BloodTank;
            case cardian::party::Role::Healer:
                return Seat::WhiteMage;
            default:
                switch (job)
                {
                    case xi::Job::BLM:
                        return Seat::BlackMage;
                    case xi::Job::THF:
                        return Seat::Thief;
                    case xi::Job::RNG:
                    case xi::Job::COR:
                        return Seat::Ranged;
                    default:
                        return Seat::Melee;
                }
        }
    }

    // The census's row for a role's food: a Puller and no role eat as Damage
    constexpr auto planRole(const cardian::party::Role role) -> cardian::party::Role
    {
        return role == cardian::party::Role::Tank || role == cardian::party::Role::Healer ? role : cardian::party::Role::Damage;
    }

    // A percentage of a stat, as the game's integer arithmetic takes it
    constexpr auto percent(const int32 base, const int32 pct) -> int32
    {
        return base * pct / 100;
    }

    // Her own attack, defence, accuracy and evasion, the ones an owned
    // cardian's food is judged at: what her level, her base stats, her skills
    // and her gear give, by the game's formulas (battle_entity.cpp ATT, DEF,
    // ACC, EVA, RATT, RACC) with no effect on her -- no buff, no food -- so
    // her pick, and her Food line, hold while Berserk or a meal comes and
    // goes. Her HP and MP are her job's base and her gear's. The census
    // estimates the same numbers for a body of the world (census.py
    // own_estimate)
    struct Own
    {
        uint8 level          = 1;
        int32 skill          = 0;     // her main weapon's skill (hand-to-hand bare-handed)
        int32 str            = 0;     // her base STR and her gear's
        float strMultiplier  = 0.75f; // STR to attack for her main weapon (main.lua's multipliers)
        int32 gearAtt        = 0;
        int32 vit            = 0;     // her base VIT and her gear's
        float vitFactor      = 1.5f;  // VIT to defence (main.lua's PLAYER_ALLIES_VIT_DEF_MULTIPLIER)
        int32 gearDef        = 0;
        int32 dex            = 0;     // her base DEX and her gear's
        float dexMultiplier  = 0.75f; // DEX to accuracy (main.lua's multipliers)
        int32 gearAcc        = 0;
        int32 evasionSkill   = 0;
        int32 agi            = 0;     // her base AGI and her gear's
        int32 gearEva        = 0;
        int32 hp             = 0;     // her base HP and her gear's
        int32 mp             = 0;     // her base MP and her gear's
        int32 rangedSkill    = 0;     // her ranged weapon's skill (or her ammunition's)
        float rStrMultiplier = 1.0f;  // STR to ranged attack (main.lua's RANGED_STR_ATTACK_MULTIPLIER)
        int32 gearRatt       = 0;
        float rAgiMultiplier = 0.75f; // AGI to ranged accuracy (main.lua's RANGED_AGI_ACCURACY_MULTIPLIER)
        int32 gearRacc       = 0;
    };

    // The part of a player's defence his level gives (battle_entity.cpp DEF)
    constexpr auto levelDefence(const uint8 level) -> int32
    {
        if (level < 51)
        {
            return level;
        }
        if (level < 61)
        {
            return 2 * level - 42;
        }
        if (level < 91)
        {
            return level + 18;
        }
        return level + 18 + (level - 89) / 2;
    }

    // Accuracy from a combat skill (battle_entity.cpp GetAccFromSkill)
    constexpr auto accuracyFromSkill(const int32 skill) -> int32
    {
        if (skill > 600)
        {
            return static_cast<int32>((skill - 600) * 0.9f) + 540;
        }
        if (skill > 400)
        {
            return static_cast<int32>((skill - 400) * 0.8f) + 380;
        }
        if (skill > 200)
        {
            return static_cast<int32>((skill - 200) * 0.9f) + 200;
        }
        return skill;
    }

    constexpr auto ownAttack(const Own& o) -> int32
    {
        return std::max(1, 8 + o.skill + static_cast<int32>(static_cast<float>(o.str) * o.strMultiplier) + o.gearAtt);
    }

    constexpr auto ownDefence(const Own& o) -> int32
    {
        return std::max(1, 8 + static_cast<int32>(static_cast<float>(o.vit) * o.vitFactor) + levelDefence(o.level) + o.gearDef);
    }

    constexpr auto ownAccuracy(const Own& o) -> int32
    {
        return accuracyFromSkill(o.skill) + static_cast<int32>(static_cast<float>(o.dex) * o.dexMultiplier) + o.gearAcc;
    }

    constexpr auto ownRangedAttack(const Own& o) -> int32
    {
        return std::max(1, 8 + o.rangedSkill + static_cast<int32>(static_cast<float>(o.str) * o.rStrMultiplier) + o.gearRatt);
    }

    constexpr auto ownRangedAccuracy(const Own& o) -> int32
    {
        return accuracyFromSkill(o.rangedSkill) + static_cast<int32>(static_cast<float>(o.agi) * o.rAgiMultiplier) + o.gearRacc;
    }

    constexpr auto ownEvasion(const Own& o) -> int32
    {
        const int32 skill = o.evasionSkill <= 200 ? o.evasionSkill : 200 + static_cast<int32>((o.evasionSkill - 200) * 0.9f);
        return skill + o.agi / 2 + o.gearEva;
    }

    // The numbers a food's percentages read, hers (statsOf) or the census's
    // estimate; magic accuracy and magic evasion three a level, as the
    // census has them (tiebreakers for every seat)
    struct Stats
    {
        int32 attack    = 0;
        int32 defence   = 0;
        int32 accuracy  = 0;
        int32 evasion   = 0;
        int32 hp        = 0;
        int32 mp        = 0;
        int32 macc      = 0;
        int32 meva      = 0;
        int32 rattack   = 0;
        int32 raccuracy = 0;
    };

    constexpr auto statsOf(const Own& o) -> Stats
    {
        return { ownAttack(o), ownDefence(o), ownAccuracy(o), ownEvasion(o), o.hp, o.mp, 3 * o.level, 3 * o.level, ownRangedAttack(o), ownRangedAccuracy(o) };
    }

    namespace detail
    {
        // A food-only percentage, its cap, the gear stat it raises and which
        // of her numbers it reads (food_rank.py TWINS)
        struct Twin
        {
            std::string_view percent;
            std::string_view cap;
            std::string_view stat;
            int32 Stats::*   reads;
        };
        inline constexpr std::array<Twin, 10> kTwins{ {
            { "FOOD_ATTP", "FOOD_ATT_CAP", "ATT", &Stats::attack },
            { "FOOD_DEFP", "FOOD_DEF_CAP", "DEF", &Stats::defence },
            { "FOOD_ACCP", "FOOD_ACC_CAP", "ACC", &Stats::accuracy },
            { "FOOD_EVAP", "FOOD_EVA_CAP", "EVA", &Stats::evasion },
            { "FOOD_MACCP", "FOOD_MACC_CAP", "MACC", &Stats::macc },
            { "FOOD_MEVAP", "FOOD_MEVA_CAP", "MEVA", &Stats::meva },
            { "FOOD_HPP", "FOOD_HP_CAP", "HP", &Stats::hp },
            { "FOOD_MPP", "FOOD_MP_CAP", "MP", &Stats::mp },
            { "FOOD_RATTP", "FOOD_RATT_CAP", "RATT", &Stats::rattack },
            { "FOOD_RACCP", "FOOD_RACC_CAP", "RACC", &Stats::raccuracy },
        } };
        // FOOD_HP and FOOD_MP are HP and MP; ATTP and DEFP a plain percentage
        inline constexpr std::array<std::pair<std::string_view, std::string_view>, 2> kFlatTwins{ { { "FOOD_HP", "HP" }, { "FOOD_MP", "MP" } } };
        struct Plain
        {
            std::string_view percent;
            std::string_view stat;
            int32 Stats::*   reads;
        };
        inline constexpr std::array<Plain, 2> kPlainPercents{ { { "ATTP", "ATT", &Stats::attack }, { "DEFP", "DEF", &Stats::defence } } };

        inline auto foodOnly(const std::string_view name) -> bool
        {
            for (const auto& t : kTwins)
            {
                if (name == t.percent || name == t.cap)
                {
                    return true;
                }
            }
            for (const auto& [from, to] : kFlatTwins)
            {
                if (name == from)
                {
                    return true;
                }
            }
            for (const auto& p : kPlainPercents)
            {
                if (name == p.percent)
                {
                    return true;
                }
            }
            return false;
        }

        template <std::size_t N>
        auto weightIn(const std::array<weights::Weight, N>& table, const std::string_view name) -> double
        {
            for (const auto& w : table)
            {
                if (w.mod == name)
                {
                    return w.value;
                }
            }
            return 0.0;
        }
    } // namespace detail

    // What a food gives her now, as the stats gear carries, by xi.mod name
    // and sorted by it (food_rank.py gains): its own gear stats as they are,
    // FOOD_HP and FOOD_MP as HP and MP, a percentage as the points it adds at
    // her number with the food's flat part in it -- capped where it has a
    // cap, nothing where the cap is 0
    inline auto gains(const Facts& f, const Stats& her) -> std::vector<Mod>
    {
        Facts out;
        for (const auto& m : f.mods)
        {
            if (!detail::foodOnly(m.name))
            {
                out.add(m.name, m.value);
            }
        }
        for (const auto& [from, to] : detail::kFlatTwins)
        {
            if (const auto v = f.mod(from); v != 0)
            {
                out.add(to, v);
            }
        }
        const Facts flat = out;
        for (const auto& p : detail::kPlainPercents)
        {
            if (const auto pct = f.mod(p.percent); pct != 0)
            {
                out.add(p.stat, percent(her.*(p.reads) + flat.mod(p.stat), pct));
            }
        }
        for (const auto& t : detail::kTwins)
        {
            if (const auto pct = f.mod(t.percent); pct != 0)
            {
                out.add(t.stat, std::min(percent(her.*(t.reads) + flat.mod(t.stat), pct), f.mod(t.cap)));
            }
        }
        std::vector<Mod> given;
        for (auto& m : out.mods)
        {
            if (m.value != 0)
            {
                given.push_back(std::move(m));
            }
        }
        std::sort(given.begin(), given.end(), [](const Mod& a, const Mod& b)
        {
            return a.name < b.name;
        });
        return given;
    }

    // A seat's weight for a stat (food_weights.h)
    inline auto weightOf(const Seat seat, const std::string_view name) -> double
    {
        switch (seat)
        {
            case Seat::Melee:
                return detail::weightIn(weights::kMelee, name);
            case Seat::Thief:
                return detail::weightIn(weights::kThief, name);
            case Seat::NinjaTank:
                return detail::weightIn(weights::kNinjaTank, name);
            case Seat::BloodTank:
                return detail::weightIn(weights::kBloodTank, name);
            case Seat::BlackMage:
                return detail::weightIn(weights::kBlackMage, name);
            case Seat::WhiteMage:
                return detail::weightIn(weights::kWhiteMage, name);
            case Seat::RedMage:
                return detail::weightIn(weights::kRedMage, name);
            default:
                return detail::weightIn(weights::kRanged, name);
        }
    }

    // A food's score for her seat: every stat it gives her now, weighed as
    // the gear scorer weighs it, negatives in full where the seat weighs the
    // stat; rounded to the census's nine places, so equal foods tie exactly
    inline auto score(const Facts& f, const Seat seat, const Stats& her) -> double
    {
        double sum = 0.0;
        for (const auto& m : gains(f, her))
        {
            sum += weightOf(seat, m.name) * m.value;
        }
        return std::round(sum * 1e9) / 1e9;
    }

    // A score worth a named stat's step at least (gear_roles.py NAMED_STEP):
    // a food of only tiebreakers feeds nobody's seat
    constexpr double kNamedStep = 0.005;

    constexpr auto givesSomething(const double value) -> bool
    {
        return value >= kNamedStep;
    }

    constexpr auto edible(const Facts& f, const bool eatsRawFish, const bool eatsRawMeat) -> bool
    {
        switch (f.kind)
        {
            case Kind::RawFish:
                return eatsRawFish;
            case Kind::RawMeat:
                return eatsRawMeat;
            default:
                return true;
        }
    }

    // A Healer's short MP food, eaten as she kneels rather than with the player
    constexpr auto isCookie(const Facts& f) -> bool
    {
        return f.duration < kLongSeconds;
    }

    // What she eats for a role, and whether it is a cookie
    struct Pick
    {
        uint16 itemId = 0;
        bool   cookie = false;

        constexpr auto operator==(const Pick&) const -> bool = default;
    };

    // A food in her bags: its item, how many she carries in all, and its facts
    struct Carried
    {
        uint16 itemId = 0;
        uint32 count  = 0;
        Facts  facts;
    };

    // An owned cardian's food for a role, from her own bags (the user,
    // 2026-10-05): the best score for her seat among the foods she carries
    // (score, at her own numbers) -- foods lasting kLongSeconds at least that
    // feed her seat; for a Healer the best such food giving kHealerFloor of
    // MP while resting or more, else the best such cookie. A tie goes to the
    // stack she carries more of, then the lower item. Nothing suitable:
    // nothing. Conditional foods and raw food not hers to eat are never picked
    inline auto pickOwned(const std::span<const Carried> bag, const Seat seat, const bool healer, const Stats& her, const bool eatsRawFish,
                          const bool eatsRawMeat) -> std::optional<Pick>
    {
        const auto best = [&](const bool cookies) -> std::optional<Pick>
        {
            const Carried* chosen = nullptr;
            double         top    = 0.0;
            for (const auto& c : bag)
            {
                if (c.count == 0 || c.itemId == 0 || c.facts.conditional || !edible(c.facts, eatsRawFish, eatsRawMeat))
                {
                    continue;
                }
                if (isCookie(c.facts) != cookies || (healer && c.facts.mod("MPHEAL") < kHealerFloor))
                {
                    continue;
                }
                const double value = score(c.facts, seat, her);
                if (!givesSomething(value))
                {
                    continue;
                }
                if (chosen == nullptr || value > top || (value == top && (c.count > chosen->count || (c.count == chosen->count && c.itemId < chosen->itemId))))
                {
                    chosen = &c;
                    top    = value;
                }
            }
            if (chosen == nullptr)
            {
                return std::nullopt;
            }
            return Pick{ chosen->itemId, cookies };
        };
        if (const auto lasting = best(false); lasting.has_value())
        {
            return lasting;
        }
        return healer ? best(true) : std::nullopt;
    }

    // The 30-second check: once her look has found her role's food due --
    // one she carries that is no cookie -- she eats it with the player while
    // her row is on, the player has a food effect on and she has none, no
    // fight is on, and she is free: standing still and up (done standing
    // from a kneel), not acting, no order of his waiting, no rest order on
    struct WithPlayer
    {
        bool rowOn         = false;
        bool playerFed     = false;
        bool selfFed       = false;
        bool betweenFights = false;
        bool free          = false;
    };

    constexpr auto eatsWithPlayer(const WithPlayer& m) -> bool
    {
        return m.rowOn && m.playerFed && !m.selfFed && m.betweenFights && m.free;
    }

    // The Healer's cookie: eaten as she is about to kneel short of MP, when
    // her row is on, her food is a cookie she carries, the player's food has
    // armed her and she has none of her own on
    struct BeforeKneel
    {
        bool rowOn     = false;
        bool hasCookie = false;
        bool playerFed = false;
        bool selfFed   = false;
        bool kneeling  = false; // about to kneel, from standing
        bool shortOfMp = false;
    };

    constexpr auto eatsBeforeKneel(const BeforeKneel& k) -> bool
    {
        return k.rowOn && k.hasCookie && k.playerFed && !k.selfFed && k.kneeling && k.shortOfMp;
    }

    // A body of the world's top-up of one food: back to her stock, or a
    // stack where a stack holds less (the census lays the same)
    constexpr auto topUpBy(const uint32 have, const uint32 stackSize) -> uint32
    {
        const uint32 target = std::clamp<uint32>(kStock, 1, std::max<uint32>(stackSize, 1));
        return have >= target ? 0 : target - have;
    }
} // namespace cardian::food
