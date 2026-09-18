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

// The stake's arithmetic (RESEARCH §12.16): the tank's tow point, and the
// rule that dissolves a stake the party has left. Pure and entity-free,
// so cardian_stake_tests.cpp can hold it to account; the controller and
// the orders read the entities and call in.

#include "common/cbasetypes.h"
#include "formation_math.h"

#include <cmath>
#include <numbers>
#include <optional>
#include <utility>

namespace cardian::stake
{
    // The flag's 9-to-3 line divides front (positive) from back (negative).
    inline auto forwardOf(const float stakeX, const float stakeZ, const uint8 rotation, const float x, const float z) -> float
    {
        const float radians = rotation * (2.0f * std::numbers::pi_v<float> / 256.0f);
        return (x - stakeX) * std::cos(radians) - (z - stakeZ) * std::sin(radians);
    }

    constexpr float kSettle   = 1.5f;
    constexpr float kMobAhead = 2.0f; // settle allowance remains entirely ahead of the flag

    // A path being empty alone says nothing about arrival: it may have
    // failed. Confirm melee-ready stillness on consecutive mob updates.
    // Tick IDs come from the mob's AI clock; duplicate reads and a missed
    // update cannot supply the second observation. No elapsed-time policy.
    struct Settlement
    {
        std::optional<int64> tick;
        float x = 0.0f;
        float y = 0.0f;
        float z = 0.0f;
        bool meleeReady = false;

        auto observe(const int64 now, const int64 previousTick, const float px, const float py, const float pz,
                     const bool ready) -> bool
        {
            if (tick == now)
            {
                return false;
            }
            constexpr float kStill = 0.1f; // same positional slack as melee step-back
            const bool settled = ready && meleeReady && tick == previousTick &&
                std::hypot(px - x, py - y, pz - z) <= kStill;
            tick = now;
            x = px;
            y = py;
            z = pz;
            meleeReady = ready;
            return settled;
        }
    };

    // The player's pull chooses the fight's spot. A confirmed melee stop
    // ahead of the flag and within 20 yalms is good enough, immediately.
    // Keep that permission through hate changes and local movement; leaving
    // this area revokes it. The controller resets it for a new mob or camp.
    inline auto keepsFightSpot(const bool kept, const bool settled, const float flagDistance, const float forward) -> bool
    {
        return forward >= 0.0f && flagDistance <= 20.0f && (kept || settled);
    }

    struct ReceiveConfig
    {
        float  immediate = 3.0f;
        double secondsPerYalm = 0.5;
        double maxWait = 8.0;
        float  progress = 0.5f;
        double window = 1.0;
    };

    enum class ReceiveAction { Wait, Join, Outside };

    // One initial receive, independent of weapon draw and Provoke's recast.
    // The controller supplies distance to the fixed landing point and owns
    // the monster's identity. Once joined, hate changes never restart it.
    struct Receive
    {
        bool joined = false;
        bool sampled = false;
        float sampleDistance = 0.0f;
        double sampleAt = 0.0;
        std::optional<double> deadline;

        auto update(const double now, const float distance, const bool inCamp,
                    const bool hasHate, const bool fighting, const ReceiveConfig& config) -> ReceiveAction
        {
            if (joined)
            {
                return ReceiveAction::Join;
            }
            if (!inCamp)
            {
                *this = {};
                return ReceiveAction::Outside;
            }
            if (!fighting)
            {
                *this = {}; // drawing on an idle monster is not a pull
                return ReceiveAction::Wait;
            }
            if (hasHate || distance <= config.immediate)
            {
                joined = true;
                return ReceiveAction::Join;
            }
            const auto delay = [&](const float d)
            {
                return std::clamp((d - config.immediate) * config.secondsPerYalm, 0.0, config.maxWait);
            };
            if (!sampled)
            {
                sampled = true;
                sampleAt = now;
                sampleDistance = distance;
                // Observe whether it is approaching before deciding it
                // stopped. The eventual deadline still starts here.
            }
            else if (sampleDistance - distance >= config.progress)
            {
                sampleDistance = distance;
                sampleAt = now;
                deadline.reset(); // genuine resumed approach gets its time
            }
            else if (distance - sampleDistance >= config.progress || now - sampleAt >= config.window)
            {
                if (!deadline.has_value())
                {
                    // Last inward progress is when the grace starts. A
                    // flyby cannot lengthen it by travelling farther away.
                    deadline = sampleAt + delay(sampleDistance);
                }
                sampleDistance = distance;
                sampleAt = now;
            }
            if (deadline.has_value() && now >= *deadline)
            {
                joined = true;
            }
            return joined ? ReceiveAction::Join : ReceiveAction::Wait;
        }
    };

    // The tow point: one mob's reach past the stake, on the line from the
    // mob through the stake. A mob chasing her there stops when it is a
    // reach from her -- on the stake. A mob already on the stake has no
    // line, so the point uses the engine's heading: x = cos, z = -sin.
    inline auto towPoint(const float mobX, const float mobZ, const float stakeX, const float stakeZ, const float reach, const uint8 rotation) -> std::pair<float, float>
    {
        const float dx  = stakeX - mobX;
        const float dz  = stakeZ - mobZ;
        const float len = std::sqrt(dx * dx + dz * dz);
        if (len < 0.01f)
        {
            const float radians = rotation * (2.0f * std::numbers::pi_v<float> / 256.0f);
            return { stakeX + std::cos(radians) * reach, stakeZ - std::sin(radians) * reach };
        }
        return { stakeX + dx / len * reach, stakeZ + dz / len * reach };
    }

    // A new monster has not yet settled. Only a settled monster gets the
    // wider drift band before it needs towing again.
    inline auto towing(const bool newMob, const bool wasTowing, const float distance, const float tolerance) -> bool
    {
        return distance > ((newMob || wasTowing) ? tolerance : 2.0f * tolerance);
    }

    inline auto frontlineTowing(const bool newMob, const bool wasTowing, const float distance, const float forward) -> bool
    {
        return forward < 0.0f || towing(newMob, wasTowing, distance, kSettle);
    }

    // Detour toward a world-space goal, not a flank that turns with the
    // tank. Keep the chosen side until the direct route is clear.
    inline auto routePoint(const float mobX, const float mobZ, const float bodyRadius,
                           const float x, const float z, const float goalX, const float goalZ,
                           float& direction) -> std::pair<float, float>
    {
        // A fighting seat can sit inside the usual padding. Leave room to
        // reach it instead of orbiting an unreachable padded ring.
        const float radius = formation::meleeClearance(std::hypot(goalX - mobX, goalZ - mobZ), bodyRadius);
        const formation::Circle body{ mobX, mobZ, radius };
        if (radius <= 0.1f || !formation::segmentCrosses(body, x, z, goalX, goalZ))
        {
            direction = 0.0f;
            return { goalX, goalZ };
        }
        // A pursuing mob can step onto the tank while she takes her seat.
        // Tangent routing assumes she starts outside the body: from inside,
        // its wrapped angles can ask for almost a full lap. Step outward
        // first, then choose the short way round from that new position.
        const float away = std::hypot(x - mobX, z - mobZ);
        if (away < radius)
        {
            direction = 0.0f;
            const float dx = away > 0.01f ? x - mobX : goalX - mobX;
            const float dz = away > 0.01f ? z - mobZ : goalZ - mobZ;
            const float scale = (radius + 0.2f) / std::hypot(dx, dz);
            return { mobX + dx * scale, mobZ + dz * scale };
        }
        const auto point = formation::detourAround(body, x, z, goalX, goalZ, 0.2f, std::min(1.0f, radius * 0.5f), 0.3f, direction);
        if (direction == 0.0f)
        {
            const float cross = (x - mobX) * (point.second - mobZ) - (z - mobZ) * (point.first - mobX);
            direction = cross >= 0.0f ? 1.0f : -1.0f;
        }
        return point;
    }

    // Remember a side only while routing around substantially the same
    // obstacle. Measure drift from the last reset, so small steps accumulate.
    struct Route
    {
        static constexpr float kMobDrift = 1.0f;
        float direction = 0.0f;
        float mobX = 0.0f;
        float mobZ = 0.0f;
        bool sampled = false;

        void reset()
        {
            *this = {};
        }

        auto observe(const float x, const float z) -> bool
        {
            if (sampled && std::hypot(x - mobX, z - mobZ) < kMobDrift)
            {
                return false;
            }
            const bool changed = sampled;
            direction = 0.0f;
            mobX = x;
            mobZ = z;
            sampled = true;
            return changed;
        }

        auto point(const float x, const float z, const float radius,
                   const float tankX, const float tankZ, const float goalX, const float goalZ) -> std::pair<float, float>
        {
            observe(x, z);
            return routePoint(x, z, radius, tankX, tankZ, goalX, goalZ, direction);
        }
    };

    // The stake dissolves once the whole party has left its zone: nobody
    // of the party in it, and somebody of it seen in another zone. A
    // player loading between zones is seen nowhere, so a zone line alone
    // never dissolves it
    inline auto dissolves(const uint32 inZone, const uint32 elsewhere) -> bool
    {
        return inZone == 0 && elsewhere > 0;
    }

    // Other owned bodies may have waited elsewhere all session. Only the
    // owner's observed arrival elsewhere establishes departure; anybody
    // still at camp keeps it standing. Loading bodies are not observed.
    class Census
    {
    public:
        explicit Census(const uint16 zone)
        : zone_(zone)
        {
        }

        void observe(const uint16 zone, const bool owner)
        {
            inZone_         += zone == zone_;
            ownerElsewhere_ = ownerElsewhere_ || (owner && zone != zone_);
        }

        auto dissolves() const -> bool
        {
            return cardian::stake::dissolves(inZone_, ownerElsewhere_ ? 1 : 0);
        }

    private:
        uint16 zone_;
        uint32 inZone_         = 0;
        bool   ownerElsewhere_ = false;
    };

} // namespace cardian::stake
