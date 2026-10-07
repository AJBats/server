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

// The perimeter's arithmetic (RESEARCH §12.15): a mob's TP reach, read off
// its own skill list, as two circles -- one round the mob, one round
// whoever it is on -- and the nearest safe spot: outside the ring they
// make, inside cure range of the tank. Pure and entity-free, so
// cardian_perimeter_tests.cpp can hold it to account; the controller reads
// the skill list and walks her there.

#include "common/cbasetypes.h"

#include <algorithm>
#include <cmath>
#include <numbers>
#include <optional>
#include <span>
#include <string_view>
#include <utility>

namespace cardian::perimeter
{
    // How much a move matters to a mage caught in it (move_harm.h): she
    // keeps out of it, avoids it when it is safe to, or ignores it
    enum class Harm : uint8
    {
        Hard,
        Soft,
        None,
    };

    // A TP move as mob_skills describes it. aoe 0 is single target, no
    // area to stand out of; 1 is a round centred on the mob and 2 a round
    // centred on its target, the radius the reach; 4 and 8 are cones from
    // the mob, aimed at its target -- the tank's to turn away from the
    // party, never a mage's to stand out of (the user, 2026-10-07).
    // Hostile: its valid targets include an enemy -- a move that only ever
    // lands on the mob's own side is no danger
    struct Move
    {
        uint8            aoe      = 0;
        float            distance = 0.0f;
        float            radius   = 0.0f;
        bool             hostile  = true;
        std::string_view name{};
        Harm             harm = Harm::Hard;
    };

    // The two circles, as radii, and the move that set each; zero is no
    // circle, and an empty name means the melee floor set it. mob and
    // target are the moves she keeps out of; softMob and softTarget add the
    // ones she avoids when it is safe to, so never less than those
    struct Reach
    {
        float            mob    = 0.0f;
        float            target = 0.0f;
        std::string_view mobBy{};
        std::string_view targetBy{};
        float            softMob    = 0.0f;
        float            softTarget = 0.0f;
    };

    // Round the mob: the largest self-centred round's radius, and the mob's
    // melee reach as the floor. Round its target: the largest target-centred
    // round's radius. Only rounds count: a single-target move lands on the
    // one the mob is on, and a cone is the tank's to aim (the user,
    // 2026-10-07), so a mob with neither lets her tuck in. A move she
    // ignores counts for nothing, one she avoids when safe only in the soft
    // circles. The margin goes on top of each circle there is.
    inline auto reachOf(std::span<const Move> moves, const float meleeReach, const float margin) -> Reach
    {
        Reach r;
        r.mob     = std::max(0.0f, meleeReach);
        r.softMob = r.mob;
        for (const auto& m : moves)
        {
            if (!m.hostile || m.harm == Harm::None || (m.aoe != 1 && m.aoe != 2))
            {
                continue;
            }
            const float radius = m.radius;
            if (m.aoe == 1)
            {
                r.softMob = std::max(r.softMob, radius);
                if (m.harm == Harm::Hard && radius > r.mob)
                {
                    r.mob   = radius;
                    r.mobBy = m.name;
                }
            }
            else
            {
                r.softTarget = std::max(r.softTarget, radius);
                if (m.harm == Harm::Hard && radius > r.target)
                {
                    r.target   = radius;
                    r.targetBy = m.name;
                }
            }
        }
        for (float* circle : { &r.mob, &r.target, &r.softMob, &r.softTarget })
        {
            if (*circle > 0.0f)
            {
                *circle += margin;
            }
        }
        return r;
    }

    // The ring she keeps round the mob, on the tank's side: the mob's own
    // circle, or the tank's circle carried out past the tank, whichever is
    // larger. The tank's circle is carried out from where she holds the mob,
    // `hold` (its melee reach on her), never farther: while she stands off
    // -- walking in, the mob on someone else, a pull still running -- the
    // gap between them is no reach of the mob's, and carried into the ring
    // it pushed the mages out past cure range
    inline auto ringOf(const Reach& reach, const float mobToTank, const float hold) -> float
    {
        return reach.target > 0.0f ? std::max(reach.mob, std::min(mobToTank, hold) + reach.target) : reach.mob;
    }

    // The ring with the moves she avoids when it is safe to: the same
    // carrying out, and never inside the ring she keeps
    inline auto softRingOf(const Reach& reach, const float mobToTank, const float hold) -> float
    {
        const float soft = reach.softTarget > 0.0f ? std::max(reach.softMob, std::min(mobToTank, hold) + reach.softTarget) : reach.softMob;
        return std::max(soft, ringOf(reach, mobToTank, hold));
    }

    // No spot clears the ring and stays in cast range of the tank: the
    // ring point behind the tank is farther from the tank than the range
    inline auto noSafeSpot(const float ring, const float mobToTank, const float castRange) -> bool
    {
        return ring - mobToTank > castRange;
    }

    // The ring she stands outside: the soft one (softRingOf) while the band
    // between it and cure range of the tank leaves `room` to stand in, else
    // the one she keeps out of. A band too thin to stand in would leave the
    // spot finder nothing and her where she stands -- inside the moves she
    // keeps out of, when the plain ring had room to spare
    inline auto ringToKeep(const float ring, const float softRing, const float mobToTank, const float castRange, const float room) -> float
    {
        return mobToTank + castRange - softRing >= room ? softRing : ring;
    }

    // Leave a third of the thin crescent usable instead of collapsing both
    // padded boundaries to a tangent. Below one yalm the walking clearance
    // does not fit; the controller uses its healing-first fallback.
    inline auto crescentInset(const float width) -> float
    {
        return std::clamp(width / 3.0f, 0.5f, 2.5f);
    }

    // Camp positions are compared within the rear half, rather than making
    // every AoE radius impassable. Cure reach matters most; ordinary exposure
    // can be accepted to keep a cramped backline. These costs are in roughly
    // yalm-sized units, with a walking cost and improvement margin below.
    inline auto campCost(const float forward, const float sideways, const float rearDepth, const float toMob, const float ring, const float toTank, const float castRange,
                         const float softRing = 0.0f) -> float
    {
        // the soft ring's band beyond the ring she keeps (softRingOf) weighs
        // a third as much: she steps out of it when that costs little, and
        // never into another mob's aggro or out of cure range for it. A mob
        // with no soft moves has no band, and costs what it always did
        const float alignment = 0.45f * std::abs(sideways) + 0.35f * std::abs(-forward - rearDepth) + 3.0f * std::max(0.0f, forward);
        const float exposure  = 1.5f * std::clamp(ring - toMob, 0.0f, 8.0f) + 0.5f * std::clamp(softRing - std::max(toMob, ring), 0.0f, 8.0f);
        const float healing   = 20.0f * std::max(0.0f, toTank - castRange);
        return alignment + exposure + healing;
    }

    inline auto worthwhileCampMove(const float currentCost, const float destinationCost, const float walk) -> bool
    {
        return destinationCost + 0.25f * walk + 1.5f < currentCost;
    }

    // The safe spots are a crescent: outside the ring round the mob, inside
    // cast range of the tank. The nearest one to where she stands -- a step
    // straight out from the mob when that lands in range, else round the
    // ring just far enough toward the tank's side to be in range, else in
    // toward the tank to the range; nothing when the crescent is empty.
    // She moves only when she has left the crescent, so the tank's small
    // moves never drag her round (the user, 2026-09-17)
    inline auto safeSpot(const float mobX, const float mobZ, const float tankX, const float tankZ, const float ring, const float range, const float x, const float z) -> std::optional<std::pair<float, float>>
    {
        const float tx        = tankX - mobX;
        const float tz        = tankZ - mobZ;
        const float mobToTank = std::sqrt(tx * tx + tz * tz);
        if (noSafeSpot(ring, mobToTank, range))
        {
            return std::nullopt;
        }
        const float dx     = x - mobX;
        const float dz     = z - mobZ;
        const float away   = std::sqrt(dx * dx + dz * dz);
        const float tankAt = mobToTank > 0.01f ? std::atan2(tz, tx) : 0.0f;
        const float herAt  = away > 0.01f ? std::atan2(dz, dx) : tankAt;

        // Outside the ring and out of range: in toward the tank, to the
        // range; inside the ring by then, the ring rule below places her
        if (away >= ring)
        {
            const float ex   = x - tankX;
            const float ez   = z - tankZ;
            const float dist = std::sqrt(ex * ex + ez * ez);
            if (dist <= range)
            {
                return std::pair{ x, z };
            }
            const float nx = tankX + ex / dist * range;
            const float nz = tankZ + ez / dist * range;
            if (std::hypot(nx - mobX, nz - mobZ) >= ring)
            {
                return std::pair{ nx, nz };
            }
        }

        // On the ring: the arc within range of the tank is centred on the
        // tank's bearing, half-width from the cosine rule; her bearing
        // clamped into it is the nearest point of the arc. A tank so far
        // out that its range disc misses the ring altogether has no arc:
        // in toward the tank, to the range, is the nearest safe spot
        float halfWidth = std::numbers::pi_v<float>;
        if (mobToTank > 0.01f)
        {
            const float c = (ring * ring + mobToTank * mobToTank - range * range) / (2.0f * ring * mobToTank);
            if (mobToTank - ring > range)
            {
                const float ex   = x - tankX;
                const float ez   = z - tankZ;
                const float dist = std::sqrt(ex * ex + ez * ez);
                if (dist < 0.01f)
                {
                    return std::pair{ tankX, tankZ };
                }
                return std::pair{ tankX + ex / dist * range, tankZ + ez / dist * range };
            }
            if (c > -1.0f)
            {
                halfWidth = std::acos(std::clamp(c, -1.0f, 1.0f));
            }
        }
        float off = herAt - tankAt;
        while (off > std::numbers::pi_v<float>)
        {
            off -= 2.0f * std::numbers::pi_v<float>;
        }
        while (off < -std::numbers::pi_v<float>)
        {
            off += 2.0f * std::numbers::pi_v<float>;
        }
        const float at = tankAt + std::clamp(off, -halfWidth, halfWidth);
        return std::pair{ mobX + std::cos(at) * ring, mobZ + std::sin(at) * ring };
    }

    // The point on the ray from the mob through `through`, at `range` from
    // the mob. A `through` on the mob has no direction, so the fallback
    // bearing (radians, x = cos, z = sin) serves.
    inline auto atRange(const float mobX, const float mobZ, const float throughX, const float throughZ, const float range, const float fallbackRadians) -> std::pair<float, float>
    {
        const float dx  = throughX - mobX;
        const float dz  = throughZ - mobZ;
        const float len = std::sqrt(dx * dx + dz * dz);
        if (len < 0.01f)
        {
            return { mobX + std::cos(fallbackRadians) * range, mobZ + std::sin(fallbackRadians) * range };
        }
        return { mobX + dx / len * range, mobZ + dz / len * range };
    }

} // namespace cardian::perimeter
