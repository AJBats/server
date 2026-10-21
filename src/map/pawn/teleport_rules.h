/*
===========================================================================

  Cardian: the teleport shout's rules (teleport.h, ROADMAP "The teleport
  shout"): the crags a shout can ask for, who answers one, who goes when
  the spell lands, and when the White Mage casts.

  Pure: no engine types. xi_test pins them
  (src/test/tests/cardian_teleport_tests.cpp).

===========================================================================
*/

#pragma once

#include "common/cbasetypes.h"

#include <array>
#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace pawn::teleport
{
    // A crag a teleport shout can ask for, by the finder goal's log: the
    // White Mage's Teleport to it, the gate crystal each passenger must hold
    // (the spell's own rule, scripts/globals/spells/enhancing_teleport.lua),
    // the zone it lands in, and the power of the TELEPORT effect the spell
    // puts on each passenger (xi.teleport.id)
    struct Crag
    {
        uint8            log = 0;
        std::string_view name;
        uint16           spell    = 0; // SpellID
        uint16           keyItem  = 0; // xi::KeyItem
        uint16           zone     = 0; // xi::ZoneId
        uint16           teleport = 0; // xi.teleport.id
    };

    // Holla, Dem and Mea (the user, 2026-10-18), then Zilart's three,
    // Altep, Yhoat and Vahzl (2026-10-21), each a log of its own for good
    constexpr std::array<Crag, 6> kCrags{ {
        { 0, "Holla", 122, 352, 102, 2 },
        { 1, "Dem", 123, 353, 108, 1 },
        { 2, "Mea", 124, 354, 117, 5 },
        { 3, "Altep", 121, 357, 114, 6 },
        { 4, "Yhoat", 120, 356, 124, 3 },
        { 5, "Vahzl", 139, 355, 112, 4 },
    } };

    // The crag a goal's log names; nullptr past the last
    constexpr auto cragOf(const uint8 log) -> const Crag*
    {
        return log < kCrags.size() ? &kCrags[log] : nullptr;
    }

    namespace rules
    {
        // ---- who answers ----------------------------------------------------

        // Her jobs cast the spell at her levels, as the game judges it
        // (spell::CanUseSpell): her main job at its level, or her support job
        // at its own. `mainNeeds` and `subNeeds` are the spell's level for
        // each of her jobs, 255 for a job that never casts it
        constexpr auto castsAt(const uint8 mainNeeds, const uint8 mlvl, const uint8 subNeeds, const uint8 slvl) -> bool
        {
            return (mainNeeds != 255 && mlvl >= mainNeeds) || (subNeeds != 255 && slvl >= subNeeds);
        }

        // She answers a teleport shout: she knows the spell, her jobs cast
        // it, and her MP covers it as she stands
        constexpr auto answers(const bool knows, const bool casts, const uint32 mp, const uint32 cost) -> bool
        {
            return knows && casts && mp >= cost;
        }

        // The White Mages who answer a shout: the nearest few, each with her
        // price, every one a yes -- a reverse auction, the cheapest his to
        // take (the user, 2026-10-20)
        constexpr std::size_t kAnswersAtMost = 3;

        // ---- her price --------------------------------------------------------

        // 500 to 1,500 gil, in hundreds
        constexpr uint32 kPriceLeast = 500;
        constexpr uint32 kPriceMost  = 1500;
        constexpr uint32 kPriceStep  = 100;
        constexpr uint32 kPriceSteps = (kPriceMost - kPriceLeast) / kPriceStep;

        // Her price, from a roll of 0 to kPriceSteps
        constexpr auto priceOf(const uint32 roll) -> uint32
        {
            return kPriceLeast + (roll % (kPriceSteps + 1)) * kPriceStep;
        }

        // Her own roll for a crag: the same every time she answers it, so a
        // shout again never hears her name another price (the user,
        // 2026-10-21), nor other words
        constexpr auto rollOf(const uint32 charid, const uint8 log) -> uint32
        {
            uint32 h = charid * 2654435761u + log * 40503u;
            h ^= h >> 15;
            h *= 2246822519u;
            h ^= h >> 13;
            return h;
        }

        constexpr auto priceFor(const uint32 charid, const uint8 log) -> uint32
        {
            return priceOf(rollOf(charid, log));
        }

        // Gil as people write it: 1,000
        inline auto gilWords(const uint32 gil) -> std::string
        {
            const auto plain = std::to_string(gil);
            std::string out;
            for (std::size_t i = 0; i < plain.size(); ++i)
            {
                if (i > 0 && (plain.size() - i) % 3 == 0)
                {
                    out += ',';
                }
                out += plain[i];
            }
            return out;
        }

        // Her answer to the shout, in one of five ways: the crag, and no
        // price -- the finder shows her price beside her words
        inline auto quoteLine(const uint32 seed, const std::string_view crag) -> std::string
        {
            const std::string name(crag);
            switch (seed % 5)
            {
                case 0:
                    return name + "? I can take you there.";
                case 1:
                    return "Sure, I'll port you to " + name + ".";
                case 2:
                    return "Teleport-" + name + ", coming right up.";
                case 3:
                    return "I can get you to " + name + ".";
                default:
                    return name + " it is. Ready when you are.";
            }
        }

        // Her price said again as she reaches him, in one of five ways
        inline auto priceLine(const uint32 seed, const uint32 price) -> std::string
        {
            const auto gil = gilWords(price);
            switch (seed % 5)
            {
                case 0:
                    return "That'll be " + gil + "g!";
                case 1:
                    return gil + " gil and we're off!";
                case 2:
                    return "Teleport's " + gil + "g. Trade me when you're ready.";
                case 3:
                    return gil + " gil, please, then gather round.";
                default:
                    return "That's " + gil + "g for the teleport!";
            }
        }

        // The trade she takes: his gil, her price exactly, and nothing beside it
        constexpr auto paysExactly(const uint32 gil, const bool anythingElse, const uint32 price) -> bool
        {
            return !anythingElse && gil == price;
        }

        // Her word on any other trade
        inline auto wrongTradeLine(const uint32 seed, const uint32 price) -> std::string
        {
            const auto gil = gilWords(price);
            return seed % 2 == 0 ? "Just the " + gil + " gil, please." : "That's not it. " + gil + "g, exactly.";
        }

        // ---- leaving town -------------------------------------------------------

        // Hired in town and not yet paid and gathered, she keeps to town: he
        // leaves it -- for a zone that is no city, settled there -- and she
        // leaves his party and goes about her business (the user, 2026-10-20)
        constexpr auto keepsToTown(const bool hiredInTown, const bool castUnderWay, const bool inTownNow) -> bool
        {
            return !hiredInTown || castUnderWay || inTownNow;
        }

        // ---- who goes -------------------------------------------------------

        // The spell's reach: the party members within its radius of her
        // (spell_list.radius, 10 yalms), under the game's vertical cap on an
        // area spell (CTargetFind::validEntity, 8 yalms)
        constexpr float kRadius = 10.0f;
        constexpr float kRise   = 8.0f;

        // Why a party member stays behind when the spell lands, in the order
        // it is told: a reason that lasts before one a step would mend
        enum class Stay : uint8
        {
            Goes,
            Elsewhere,  // in another zone
            KnockedOut,
            NoCrystal,  // no gate crystal for the crag
            Mounted,
            OutOfRange,
        };

        // One party member as the spell sees her, measured from the caster
        struct Member
        {
            bool  sameZone   = true;
            bool  alive      = true;
            bool  hasCrystal = true;
            bool  mounted    = false;
            float distance   = 0.0f;
            float rise       = 0.0f; // the height between them
        };

        constexpr auto stayOf(const Member& m, const float radius = kRadius) -> Stay
        {
            if (!m.sameZone)
            {
                return Stay::Elsewhere;
            }
            if (!m.alive)
            {
                return Stay::KnockedOut;
            }
            if (!m.hasCrystal)
            {
                return Stay::NoCrystal;
            }
            if (m.mounted)
            {
                return Stay::Mounted;
            }
            if (m.distance > radius || m.rise >= kRise)
            {
                return Stay::OutOfRange;
            }
            return Stay::Goes;
        }

        // Why she stays, in plain words for party chat: "<name> stays
        // behind. <this>"
        inline auto reasonOf(const Stay stay, const std::string_view crag) -> std::string
        {
            switch (stay)
            {
                case Stay::Elsewhere:
                    return "In another area.";
                case Stay::KnockedOut:
                    return "Knocked out.";
                case Stay::NoCrystal:
                    return "No gate crystal for " + std::string(crag) + ".";
                case Stay::Mounted:
                    return "Riding a chocobo.";
                case Stay::OutOfRange:
                    return "Too far away.";
                default:
                    return "";
            }
        }

        // ---- when she casts -------------------------------------------------

        // Gathered: the player, and every member who could go, stand
        // kGatherMargin inside the spell's edge -- it measures where they
        // stand when it lands, twenty seconds on. The player comes first:
        // without him there is no point, and mounted he is asked to get
        // down. Once he is gathered the others get kPatience seconds; then
        // she casts, and whoever is still out of reach stays behind
        constexpr float  kGatherMargin = 2.0f;
        constexpr double kPatience     = 20.0;

        enum class Ready : uint8
        {
            WaitForPlayer,
            PlayerMounted,
            WaitForOthers,
            Cast,
        };

        // He is gathered: on foot, well inside her reach
        constexpr auto gathered(const Member& m) -> bool
        {
            return stayOf(m, kRadius - kGatherMargin) == Stay::Goes;
        }

        // `playerGatheredFor`: the seconds he has stood gathered
        inline auto ready(const Member& player, const std::vector<Member>& others, const double playerGatheredFor) -> Ready
        {
            if (!gathered(player))
            {
                const auto stay = stayOf(player, kRadius - kGatherMargin);
                return stay == Stay::Mounted ? Ready::PlayerMounted : Ready::WaitForPlayer;
            }
            if (playerGatheredFor >= kPatience)
            {
                return Ready::Cast;
            }
            for (const auto& m : others)
            {
                // one who could go, were she near: alive, here, on foot, with the crystal
                const bool couldGo = stayOf(Member{ m.sameZone, m.alive, m.hasCrystal, m.mounted, 0.0f, 0.0f }) == Stay::Goes;
                if (couldGo && !gathered(m))
                {
                    return Ready::WaitForOthers;
                }
            }
            return Ready::Cast;
        }

        // ---- how long a hire lasts --------------------------------------------

        // Her way to him (the trek: a hire runs no town errands), her wait
        // beside him to be paid and for the party to gather, and how many
        // casts she tries that come to nothing (interrupted, or never
        // started) before she gives up
        constexpr double kComeWithin   = 15.0 * 60.0;
        constexpr double kWaitAtMost   = 10.0 * 60.0;
        constexpr int    kTriesAtMost  = 3;
        constexpr double kStartWithin  = 15.0; // a cast ordered and not begun in this long came to nothing
        constexpr float  kBesideYalms  = 15.0f; // she has reached him
        constexpr double kLeaveAfter   = 2.0;   // her last line said, she leaves the party this long after
        constexpr double kSettleAfter  = 6.0;   // the spell's effect wears in 4 s: past this everyone carried has gone
    } // namespace rules
} // namespace pawn::teleport
