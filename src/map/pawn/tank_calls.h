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

#include "common/cbasetypes.h"

#include <optional>
#include <string_view>
#include <vector>

// The tank tactician's Provoke pacing (RESEARCH §17.11, the user
// 2026-10-01). A tank keeps Provoke for the pull and runs it on its clock
// otherwise:
//  1. the pull: she has no fight of her own, and a mob the party fights is
//     on someone else -- Provoke it the moment it is in range;
//  2. upkeep: her fight's mob, Provoke up -- Provoke at once, whoever the
//     mob is on; she does not wait for hate to be ripped;
//  3. the end of the fight: her mob under the hold -- keep Provoke for the
//     next pull, unless an emergency: the mob on the party's Healer or on
//     a member under half HP;
//  4. the add: a second mob on the party while hers is on her is not hers
//     to Provoke; the player directs it.
// Pure, over plain numbers: the tactician (tactics.cpp) reads the party
// and asks; xi_test pins the rules (cardian_tank_calls_tests.cpp).
namespace cardian::tank
{
    struct Mob
    {
        uint32 id       = 0;
        int    hp       = 100; // percent
        uint32 target   = 0;   // whom it is on: a member's id, or 0 for nobody of ours
        float  distance = 0.0f; // from her, yalms
    };

    struct Member
    {
        uint32 id     = 0;
        int    hp     = 100; // percent
        bool   healer = false; // the party's Healer, by the screen's role
    };

    struct View
    {
        uint32                self = 0;
        std::optional<uint32> fight;        // the mob she fights now, alive; none when she has no fight of her own
        bool                  provokeReady = false;
        float                 provokeRange = 0.0f;
        int                   holdHp       = 25; // pawn.TANK_PROVOKE_HOLD_HP: under this much of the mob, Provoke waits for the next pull
        std::vector<Mob>      mobs;              // the mobs the party fights
        std::vector<Member>   members;           // the party, herself included
    };

    enum class Reason : uint8
    {
        Pull,      // off whoever it is on, as it comes in
        Upkeep,    // on its clock
        Emergency, // the end of the fight, but the mob is where it must not be
    };

    struct Call
    {
        uint32 mob    = 0;
        Reason reason = Reason::Pull;

        auto operator==(const Call&) const -> bool = default;
    };

    // Why there is no call, for the map log: the tactician says its mind
    // when it changes, as the bank does
    enum class Held : uint8
    {
        NoProvoke,  // not hers, or on its clock
        Nothing,    // no mob of the party's on anyone else, and no fight of hers
        OutOfRange, // the mob it would Provoke is too far
        OnHer,      // her fight's mob, under the hold, on her: kept for the next pull
        EndOfFight, // her fight's mob, under the hold, on someone who is fine
    };

    struct Decision
    {
        std::optional<Call> call;
        Held                held = Held::Nothing;

        auto operator==(const Decision&) const -> bool = default;
    };

    constexpr auto reasonName(const Reason reason) -> std::string_view
    {
        switch (reason)
        {
            case Reason::Pull:
                return "the pull";
            case Reason::Upkeep:
                return "upkeep";
            case Reason::Emergency:
                return "an emergency";
        }
        return "?";
    }

    constexpr auto heldName(const Held held) -> std::string_view
    {
        switch (held)
        {
            case Held::NoProvoke:
                return "Provoke is not ready";
            case Held::Nothing:
                return "nothing to Provoke";
            case Held::OutOfRange:
                return "out of range";
            case Held::OnHer:
                return "the end of the fight, the mob on her";
            case Held::EndOfFight:
                return "the end of the fight";
        }
        return "?";
    }

    namespace detail
    {
        inline auto memberOf(const View& v, const uint32 id) -> const Member*
        {
            for (const auto& m : v.members)
            {
                if (m.id == id)
                {
                    return &m;
                }
            }
            return nullptr;
        }

        inline auto mobOf(const View& v, const uint32 id) -> const Mob*
        {
            for (const auto& m : v.mobs)
            {
                if (m.id == id)
                {
                    return &m;
                }
            }
            return nullptr;
        }

        // A mob where it must not be: on the party's Healer, or on a member
        // under half HP
        inline auto emergency(const View& v, const Mob& mob) -> bool
        {
            const auto* on = memberOf(v, mob.target);
            return on != nullptr && on->id != v.self && (on->healer || on->hp < 50);
        }

        // The pull first on the Healer, then on whoever is lowest, then the
        // first in: the mob to take off someone else as she stands free
        inline auto better(const View& v, const Mob& a, const Mob& b) -> bool
        {
            const auto* onA = memberOf(v, a.target);
            const auto* onB = memberOf(v, b.target);
            if (onA->healer != onB->healer)
            {
                return onA->healer;
            }
            return onA->hp < onB->hp;
        }
    } // namespace detail

    inline auto decide(const View& v) -> Decision
    {
        if (!v.provokeReady)
        {
            return { std::nullopt, Held::NoProvoke };
        }
        if (v.fight.has_value())
        {
            // Her fight: on its clock, held at the end but for an emergency
            // (rules 2 and 3). A mob of hers the party's picture does not
            // hold is still hers; the add (rule 4) is nobody's here
            const auto* mob = detail::mobOf(v, *v.fight);
            if (mob == nullptr)
            {
                return { std::nullopt, Held::Nothing };
            }
            if (mob->distance > v.provokeRange)
            {
                return { std::nullopt, Held::OutOfRange };
            }
            if (mob->hp >= v.holdHp)
            {
                return { Call{ mob->id, Reason::Upkeep }, Held::Nothing };
            }
            if (detail::emergency(v, *mob))
            {
                return { Call{ mob->id, Reason::Emergency }, Held::Nothing };
            }
            return { std::nullopt, mob->target == v.self ? Held::OnHer : Held::EndOfFight };
        }
        // The pull (rule 1): a mob on someone else, in range
        const Mob* best     = nullptr;
        bool       tooFar   = false; // never `far`: a Windows macro
        for (const auto& mob : v.mobs)
        {
            const auto* on = detail::memberOf(v, mob.target);
            if (on == nullptr || on->id == v.self)
            {
                continue;
            }
            if (mob.distance > v.provokeRange)
            {
                tooFar = true;
                continue;
            }
            if (best == nullptr || detail::better(v, mob, *best))
            {
                best = &mob;
            }
        }
        if (best != nullptr)
        {
            return { Call{ best->id, Reason::Pull }, Held::Nothing };
        }
        return { std::nullopt, tooFar ? Held::OutOfRange : Held::Nothing };
    }

    inline auto call(const View& v) -> std::optional<Call>
    {
        return decide(v).call;
    }
} // namespace cardian::tank
