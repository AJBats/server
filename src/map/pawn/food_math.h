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

#include "party_roles.h"

#include "common/cbasetypes.h"
#include "data/enums/job.h"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <optional>
#include <span>
#include <string_view>

// Food (RESEARCH §19): the cardians eat with the player. Her party role
// says what she eats -- the Tank defence food, the Healer food for MP while
// resting, Damage attack food (a Black Mage INT food), a Puller and no role
// as Damage -- and her "Self -> Eat with the player" row eats it: every 30
// seconds between fights, when the player has a food effect on and she has
// none. A Healer whose food is a cookie (a short MP food) eats it instead as
// she kneels for MP, armed by the player's food. A body of the world eats the
// census's pick for her (cardian_food, tools/world/census.py) and is kept
// topped up while she is in the player's party; an owned cardian eats the
// best food for her role that her own bags hold, by the same value rule the
// census uses.
//
// Pure, so xi_test pins it (cardian_food_tests.cpp): a food's facts as its
// item script states them, read by the rule tools/economy/gamedata.py
// parse_food reads them by -- the two must agree -- the stat a food gives,
// the owned cardian's pick, and the moments she eats.
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

    // A food as its script states it: how long its effect lasts, who may eat
    // it, and the mods its onEffectGain adds that a role's food is judged
    // by. A script whose onEffectGain branches or loops (by race, by party
    // size) is conditional: its mods are left at zero and no rule picks it
    struct Facts
    {
        uint32 duration    = 0; // seconds
        Kind   kind        = Kind::Basic;
        bool   conditional = false;
        int32  att         = 0;
        int32  attp        = 0;
        int32  foodAttp    = 0;
        int32  foodAttCap  = 0;
        int32  def         = 0;
        int32  defp        = 0;
        int32  foodDefp    = 0;
        int32  foodDefCap  = 0;
        int32  intel       = 0;
        int32  mpheal      = 0;
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

        constexpr void addMod(Facts& f, const std::string_view name, const int32 value)
        {
            if (name == "ATT")
            {
                f.att += value;
            }
            else if (name == "ATTP")
            {
                f.attp += value;
            }
            else if (name == "FOOD_ATTP")
            {
                f.foodAttp += value;
            }
            else if (name == "FOOD_ATT_CAP")
            {
                f.foodAttCap += value;
            }
            else if (name == "DEF")
            {
                f.def += value;
            }
            else if (name == "DEFP")
            {
                f.defp += value;
            }
            else if (name == "FOOD_DEFP")
            {
                f.foodDefp += value;
            }
            else if (name == "FOOD_DEF_CAP")
            {
                f.foodDefCap += value;
            }
            else if (name == "INT")
            {
                f.intel += value;
            }
            else if (name == "MPHEAL")
            {
                f.mpheal += value;
            }
        }

        // One line of the body, its comment cut off: a branch or a loop
        // makes the food conditional, an `effect:addMod(xi.mod.X, n)` adds
        constexpr void readLine(Facts& f, std::string_view line)
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
                addMod(f, name, value->value);
            }
        }
    } // namespace detail

    // An item script read as food: nullopt when it puts no food effect on
    // its eater or says nothing of how long the effect lasts
    constexpr auto parseScript(const std::string_view text) -> std::optional<Facts>
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

    // What a role eats (RESEARCH §19.2 item 1)
    enum class Class : uint8
    {
        Attack,
        Int,
        Defence,
        Mp,
    };

    constexpr auto classFor(const cardian::party::Role role, const xi::Job job) -> Class
    {
        switch (role)
        {
            case cardian::party::Role::Tank:
                return Class::Defence;
            case cardian::party::Role::Healer:
                return Class::Mp;
            default:
                return job == xi::Job::BLM ? Class::Int : Class::Attack;
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

    // The stat a food gives her at her attack and defence: the game's own
    // formula for those two (battle_entity.cpp ATT, DEF: a flat part, then a
    // percentage of the stat it raised, capped -- no cap, nothing), INT and
    // MP while healing as they are
    constexpr auto gain(const Facts& f, const Class cls, const int32 attack, const int32 defence) -> int32
    {
        switch (cls)
        {
            case Class::Attack:
            {
                const int32 base = attack + f.att;
                return f.att + percent(base, f.attp) + std::min(percent(base, f.foodAttp), f.foodAttCap);
            }
            case Class::Defence:
            {
                const int32 base = defence + f.def;
                return f.def + percent(base, f.defp) + std::min(percent(base, f.foodDefp), f.foodDefCap);
            }
            case Class::Int:
                return f.intel;
            default:
                return f.mpheal;
        }
    }

    // Her own attack and defence, the ones an owned cardian's food is judged
    // at: what her level, her base stats, her weapon skill and her gear give,
    // by the game's formula (battle_entity.cpp ATT, DEF) with no effect on
    // her -- no buff, no food -- so her pick, and her Food line, hold while
    // Berserk or a meal comes and goes. The census estimates the same two
    // numbers for a body of the world (census.py Pantry.estimates)
    struct Own
    {
        uint8 level         = 1;
        int32 skill         = 0;     // her main weapon's skill (hand-to-hand bare-handed)
        int32 str           = 0;     // her base STR and her gear's
        float strMultiplier = 0.75f; // STR to attack for her main weapon (main.lua's multipliers)
        int32 gearAtt       = 0;
        int32 vit           = 0;     // her base VIT and her gear's
        float vitFactor     = 1.5f;  // VIT to defence (main.lua's PLAYER_ALLIES_VIT_DEF_MULTIPLIER)
        int32 gearDef       = 0;
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

    constexpr auto ownAttack(const Own& o) -> int32
    {
        return std::max(1, 8 + o.skill + static_cast<int32>(static_cast<float>(o.str) * o.strMultiplier) + o.gearAtt);
    }

    constexpr auto ownDefence(const Own& o) -> int32
    {
        return std::max(1, 8 + static_cast<int32>(static_cast<float>(o.vit) * o.vitFactor) + levelDefence(o.level) + o.gearDef);
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
    // 2026-10-05): the strongest food of the role's class, at her attack and
    // defence -- for attack, INT and defence a food lasting kLongSeconds at
    // least that gives the stat; for MP the strongest long food giving
    // kHealerFloor at least, else the strongest such cookie. A tie goes to
    // the stack she carries more of, then the lower item. Nothing suitable:
    // nothing. Conditional foods and raw food not hers to eat are never picked
    constexpr auto pickOwned(const std::span<const Carried> bag, const Class cls, const int32 attack, const int32 defence, const bool eatsRawFish,
                             const bool eatsRawMeat) -> std::optional<Pick>
    {
        const auto best = [&](const bool cookies) -> std::optional<Pick>
        {
            const Carried* chosen = nullptr;
            int32          top    = 0;
            for (const auto& c : bag)
            {
                if (c.count == 0 || c.itemId == 0 || c.facts.conditional || !edible(c.facts, eatsRawFish, eatsRawMeat))
                {
                    continue;
                }
                if (isCookie(c.facts) != cookies)
                {
                    continue;
                }
                const int32 value = gain(c.facts, cls, attack, defence);
                if (value < (cls == Class::Mp ? kHealerFloor : 1))
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
        return cls == Class::Mp ? best(true) : std::nullopt;
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
