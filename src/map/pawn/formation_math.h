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

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <numbers>
#include <optional>
#include <utility>
#include <vector>

// The formation's arithmetic, kept free of entities, settings and clocks so
// xi_test can pin it (src/test/tests/cardian_formation_tests.cpp). The link
// derives motion from the position stream with deriveMotion; the pawn
// controller aims with predictAhead and paces with catchUpSpeed.
namespace cardian::formation
{
    // One streamed position in server axes (x/z are the ground plane)
    struct Sample
    {
        float         x        = 0.0f;
        float         z        = 0.0f;
        unsigned char rotation = 0; // LSB's 0..255 heading
        bool          moving   = false;
    };

    struct Motion
    {
        float vx      = 0.0f; // yalms/s
        float vz      = 0.0f;
        float yawRate = 0.0f; // radians/s, signed
    };

    // Velocity and turn rate from two consecutive samples dtSeconds apart,
    // exponentially smoothed against the previous motion (the addon samples
    // every ~50 ms while moving, so a single pair is noisy). A standing
    // sample, an implausible dt, or a first moving sample after a stop
    // produce the raw or zero answer: a stop must collapse any prediction
    // at once, not fade it.
    inline auto deriveMotion(const Sample& previous, const Motion& previousMotion, const Sample& fresh, const float dtSeconds, const float smoothing = 0.5f) -> Motion
    {
        if (!fresh.moving || dtSeconds < 0.01f || dtSeconds > 1.0f)
        {
            return {};
        }

        const float rawVx = (fresh.x - previous.x) / dtSeconds;
        const float rawVz = (fresh.z - previous.z) / dtSeconds;

        // The heading wraps: take the shortest signed step between the two
        int rotationStep = static_cast<int>(fresh.rotation) - static_cast<int>(previous.rotation);
        if (rotationStep > 127)
        {
            rotationStep -= 256;
        }
        else if (rotationStep < -128)
        {
            rotationStep += 256;
        }
        const float rawYawRate = static_cast<float>(rotationStep) * (2.0f * std::numbers::pi_v<float> / 256.0f) / dtSeconds;

        const bool continuing = previous.moving && (previousMotion.vx != 0.0f || previousMotion.vz != 0.0f);
        if (!continuing)
        {
            return { rawVx, rawVz, rawYawRate };
        }
        return {
            previousMotion.vx + (rawVx - previousMotion.vx) * smoothing,
            previousMotion.vz + (rawVz - previousMotion.vz) * smoothing,
            previousMotion.yawRate + (rawYawRate - previousMotion.yawRate) * smoothing,
        };
    }

    struct Prediction
    {
        float dx    = 0.0f; // add to the observed position
        float dz    = 0.0f;
        float ahead = 0.0f; // yalms applied (the length of dx/dz)
    };

    // Where the player will be horizonSeconds from now: straight-line along
    // the motion, the horizon shortened as the turn rate approaches
    // fullTurnRate (down to a quarter), the displacement capped at
    // capYalms. Nothing for a standing or crawling player.
    inline auto predictAhead(const Motion& motion, const bool moving, const float horizonSeconds, const float capYalms, const float minSpeed = 0.5f, const float fullTurnRate = 3.0f) -> Prediction
    {
        const float speed = std::hypot(motion.vx, motion.vz);
        if (!moving || speed <= minSpeed || horizonSeconds <= 0.0f || capYalms <= 0.0f)
        {
            return {};
        }

        const float horizon = horizonSeconds * std::clamp(1.0f - std::fabs(motion.yawRate) / fullTurnRate, 0.25f, 1.0f);

        Prediction  p;
        p.dx            = motion.vx * horizon;
        p.dz            = motion.vz * horizon;
        const float len = std::hypot(p.dx, p.dz);
        if (len > capYalms)
        {
            p.dx *= capYalms / len;
            p.dz *= capYalms / len;
        }
        p.ahead = std::min(len, capYalms);
        return p;
    }

    // The pace for a pawn gapYalms from its point: normal when the player
    // stands or the gap is within a yalm, catch-up speed at catchUpDistance
    // and beyond, a straight ramp between. A pawn only ever closes a gap to
    // a point the player defines, so it nets out no faster than the player.
    inline auto catchUpSpeed(const float gapYalms, const bool playerMoving, const float normalSpeed, const float catchUp, const float catchUpDistance) -> float
    {
        if (!playerMoving)
        {
            return normalSpeed;
        }
        const float distance = std::max(catchUpDistance, 1.0f);
        const float ramp     = std::clamp((gapYalms - 1.0f) / (distance - 1.0f + 0.001f), 0.0f, 1.0f);
        return normalSpeed + (catchUp - normalSpeed) * ramp;
    }

    // The stand-off ring: while the party holds for the player's strike,
    // no formation point sits within `radius` of the mob (melee reach plus
    // a margin). A point inside the ring is pushed out along its own
    // bearing from the mob; one past the mob from the player's side (the
    // lead's point, aimed ahead of a player standing at the mob) comes
    // round to the player's bearing, so nobody circles behind an unstruck
    // mob. Returns the new x/z; unchanged when already outside, or when
    // there is no bearing to take (the point and the player both on the
    // mob).
    inline auto standOff(const float mobX, const float mobZ, const float playerX, const float playerZ, const float radius, const float x, const float z) -> std::pair<float, float>
    {
        float dx = x - mobX;
        float dz = z - mobZ;
        if (std::hypot(dx, dz) >= radius)
        {
            return { x, z };
        }

        const float px = playerX - mobX;
        const float pz = playerZ - mobZ;
        if (std::hypot(px, pz) > 0.001f && dx * px + dz * pz <= 0.0f)
        {
            dx = px;
            dz = pz;
        }

        const float len = std::hypot(dx, dz);
        if (len < 0.001f)
        {
            return { x, z };
        }
        return { mobX + dx / len * radius, mobZ + dz / len * radius };
    }

    // ------------------------------------------------------------------
    // The ring (M3.9, "re-parent the followers"): every cardian but the
    // lead follows the player themself, in a seat on the ring around them.
    // Follow is the silent default -- a seat picked by job, below -- and
    // Lead is ahead of the player, off the ring. The values are what
    // Formation rows persist, so they are frozen.
    // ------------------------------------------------------------------

    enum class Slot : unsigned short
    {
        Follow     = 0, // auto: a seat by job
        Lead       = 1, // ahead of the player: the hunter's place
        FlankLeft  = 2,
        FlankRight = 3,
        RearLeft   = 4,
        RearRight  = 5,
        Behind     = 6,
    };
    constexpr unsigned short SlotCount = 7;

    inline auto slotName(const Slot slot) -> const char*
    {
        constexpr std::array<const char*, SlotCount> names{ "auto", "lead", "flank left", "flank right", "rear left", "rear right", "behind" };
        const auto                                   i = static_cast<std::size_t>(slot);
        return i < names.size() ? names[i] : "?";
    }

    inline auto isRingSlot(const Slot slot) -> bool
    {
        return slot != Slot::Follow && slot != Slot::Lead;
    }

    // One cardian to seat, in party order
    struct Seat
    {
        bool                melee = false; // a melee job: the flanks first
        std::optional<Slot> claimed;       // a Formation row's ring seat
    };

    // Seat the ring. Rows claim first; then the melee, then everyone else,
    // each in party order. A melee prefers a flank, then a rear quarter,
    // then behind; the others prefer a rear quarter, then behind, then a
    // flank. Within a kind the side with fewer cardians so far wins, ties
    // to the right; behind counts as neither side. A full ring doubles up
    // behind. A Follow or Lead claim is no claim (the caller keeps leads
    // off the list). Returns one ring seat per entry.
    inline auto assignSlots(const std::vector<Seat>& seats) -> std::vector<Slot>
    {
        std::vector<Slot>          result(seats.size(), Slot::Follow);
        std::array<int, SlotCount> taken{};
        int                        left  = 0;
        int                        right = 0;

        const auto occupy = [&](const std::size_t i, const Slot slot)
        {
            result[i] = slot;
            ++taken[static_cast<std::size_t>(slot)];
            if (slot == Slot::FlankLeft || slot == Slot::RearLeft)
            {
                ++left;
            }
            if (slot == Slot::FlankRight || slot == Slot::RearRight)
            {
                ++right;
            }
        };
        const auto isFree = [&](const Slot slot)
        {
            return taken[static_cast<std::size_t>(slot)] == 0;
        };
        const auto sides = [&](const Slot rightSeat, const Slot leftSeat) -> std::optional<Slot>
        {
            if (isFree(rightSeat) && isFree(leftSeat))
            {
                return left < right ? leftSeat : rightSeat;
            }
            if (isFree(rightSeat))
            {
                return rightSeat;
            }
            if (isFree(leftSeat))
            {
                return leftSeat;
            }
            return std::nullopt;
        };
        const auto flank = [&]
        {
            return sides(Slot::FlankRight, Slot::FlankLeft);
        };
        const auto rear = [&]
        {
            return sides(Slot::RearRight, Slot::RearLeft);
        };
        const auto behind = [&]() -> std::optional<Slot>
        {
            return isFree(Slot::Behind) ? std::optional<Slot>{ Slot::Behind } : std::nullopt;
        };

        for (std::size_t i = 0; i < seats.size(); ++i)
        {
            if (seats[i].claimed.has_value() && isRingSlot(*seats[i].claimed))
            {
                occupy(i, *seats[i].claimed);
            }
        }

        for (const bool meleePass : { true, false })
        {
            for (std::size_t i = 0; i < seats.size(); ++i)
            {
                if (result[i] != Slot::Follow || seats[i].melee != meleePass)
                {
                    continue;
                }
                std::optional<Slot> seat = meleePass ? flank() : rear();
                if (!seat.has_value())
                {
                    seat = meleePass ? rear() : behind();
                }
                if (!seat.has_value())
                {
                    seat = meleePass ? behind() : flank();
                }
                occupy(i, seat.value_or(Slot::Behind));
            }
        }
        return result;
    }
    // ------------------------------------------------------------------
    // Aggro avoidance geometry (M3.87). Every detection type is a circle of
    // that type's range plus a buffer; a cardian moves on the server with
    // the mobs, so it may stand boldly just outside and is pushed away as
    // a mob roams toward it.
    // ------------------------------------------------------------------

    struct Circle
    {
        float x      = 0.0f;
        float z      = 0.0f;
        float radius = 0.0f;
    };

    inline auto planarDistance(const float ax, const float az, const float bx, const float bz) -> float
    {
        return std::hypot(ax - bx, az - bz);
    }

    // How deep inside the circle a point is (0 = outside or on the rim)
    inline auto depthInside(const Circle& c, const float x, const float z) -> float
    {
        return std::max(0.0f, c.radius - planarDistance(c.x, c.z, x, z));
    }

    template <typename Circles>
    inline auto insideAny(const Circles& circles, const float x, const float z) -> bool
    {
        for (const auto& c : circles)
        {
            if (depthInside(c, x, z) > 0.0f)
            {
                return true;
            }
        }
        return false;
    }

    // Move the point radially out of every circle it sits in, a few passes
    // so overlapping circles settle. A point exactly at a centre is pushed
    // along +x. Returns the new point; unchanged when already clear.
    template <typename Circles>
    inline auto pushOut(const Circles& circles, float x, float z, const float clearance = 0.1f) -> std::pair<float, float>
    {
        for (int pass = 0; pass < 6; ++pass)
        {
            bool moved = false;
            for (const auto& c : circles)
            {
                const float d = planarDistance(c.x, c.z, x, z);
                if (d >= c.radius)
                {
                    continue;
                }
                const float want = c.radius + clearance;
                if (d < 0.001f)
                {
                    x = c.x + want;
                }
                else
                {
                    x = c.x + (x - c.x) / d * want;
                    z = c.z + (z - c.z) / d * want;
                }
                moved = true;
            }
            if (!moved)
            {
                break;
            }
        }
        return { x, z };
    }

    // The nearest clear angle to `idealAngle` on a ring, sampled every
    // `stepRadians` outward on both sides out to the opposite point.
    // `pointAt(angle)` produces the candidate as {x, z} in whatever angle
    // convention the caller's ring uses (nearPosition's, for formation
    // slots), so this stays convention-free. nullopt when the whole ring is
    // inside danger.
    template <typename Circles, typename PointAt>
    inline auto safestAngleOnRing(const Circles& circles, const float idealAngle, PointAt&& pointAt, const float stepRadians = 0.2617994f) -> std::optional<float>
    {
        const auto clearAt = [&](const float angle) -> bool
        {
            const auto [x, z] = pointAt(angle);
            return !insideAny(circles, x, z);
        };

        if (clearAt(idealAngle))
        {
            return idealAngle;
        }
        const int steps = static_cast<int>(std::ceil(std::numbers::pi_v<float> / stepRadians));
        for (int k = 1; k <= steps; ++k)
        {
            const float delta = static_cast<float>(k) * stepRadians;
            if (clearAt(idealAngle + delta))
            {
                return idealAngle + delta;
            }
            if (clearAt(idealAngle - delta))
            {
                return idealAngle - delta;
            }
        }
        return std::nullopt;
    }

    // Does the segment a->b pass through the circle? (endpoints included)
    inline auto segmentCrosses(const Circle& c, const float ax, const float az, const float bx, const float bz) -> bool
    {
        const float dx  = bx - ax;
        const float dz  = bz - az;
        const float len = dx * dx + dz * dz;
        float       t   = 0.0f;
        if (len > 0.0f)
        {
            t = std::clamp(((c.x - ax) * dx + (c.z - az) * dz) / len, 0.0f, 1.0f);
        }
        const float px = ax + dx * t;
        const float pz = az + dz * t;
        return planarDistance(c.x, c.z, px, pz) < c.radius;
    }

    // The settle rule's meter. `improvement` is how much closer to her ideal
    // spot the best clear spot on offer would put her than the spot she
    // holds; beyond `tolerance` it charges the itch at that rate (yalms per
    // second), below it the itch drains at the shortfall. The caller moves
    // her when the level reaches her patience. Never below zero.
    inline auto itchAfter(const float level, const float improvement, const float tolerance, const float dtSeconds) -> float
    {
        return std::max(0.0f, level + (improvement - tolerance) * dtSeconds);
    }

    // The closest the segment a->b comes to the circle's centre
    inline auto segmentClosest(const Circle& c, const float ax, const float az, const float bx, const float bz) -> float
    {
        const float dx  = bx - ax;
        const float dz  = bz - az;
        const float len = dx * dx + dz * dz;
        float       t   = 0.0f;
        if (len > 0.0f)
        {
            t = std::clamp(((c.x - ax) * dx + (c.z - az) * dz) / len, 0.0f, 1.0f);
        }
        return planarDistance(c.x, c.z, ax + dx * t, az + dz * t);
    }

    // Does the segment a->b go INTO the circle: closer to the centre than
    // both the rim and where it starts, by more than `slack`. A start that
    // is already within the rim and moves away does not count (planning
    // against padded circles puts a cardian there on purpose); a start
    // outside that cuts in does.
    inline auto segmentEnters(const Circle& c, const float ax, const float az, const float bx, const float bz, const float slack = 0.3f) -> bool
    {
        const float start = planarDistance(c.x, c.z, ax, az);
        return segmentClosest(c, ax, az, bx, bz) < std::min(c.radius, start) - slack;
    }

    // The first point along a->b at `clearance` outside the circle: where
    // an approach to something inside it stops. nullopt when `a` is already
    // that close or closer, or the segment never reaches the ring.
    inline auto approachRim(const Circle& c, const float ax, const float az, const float bx, const float bz, const float clearance = 0.0f) -> std::optional<std::pair<float, float>>
    {
        const float r  = c.radius + clearance;
        const float fx = ax - c.x;
        const float fz = az - c.z;
        if (fx * fx + fz * fz <= r * r)
        {
            return std::nullopt;
        }
        const float dx = bx - ax;
        const float dz = bz - az;
        const float A  = dx * dx + dz * dz;
        if (A <= 0.0f)
        {
            return std::nullopt;
        }
        const float B    = 2.0f * (fx * dx + fz * dz);
        const float C    = fx * fx + fz * fz - r * r;
        const float disc = B * B - 4.0f * A * C;
        if (disc < 0.0f)
        {
            return std::nullopt;
        }
        const float t = (-B - std::sqrt(disc)) / (2.0f * A);
        if (t < 0.0f || t > 1.0f)
        {
            return std::nullopt;
        }
        return std::pair{ ax + dx * t, az + dz * t };
    }

    // A waypoint that takes the way a->b round the circle without ever
    // cutting into it, the shorter way round to the point from which b is
    // in the clear (the tangent from b). From outside the ring (the radius
    // plus clearance) it is the tangent point from a, so the walk there
    // touches the ring and no more. From the ring itself (within `band` of
    // it) it is a step of at most `arc` yalms along the ring -- a cardian at
    // the boundary walks along it, never across it (a chord between two rim
    // points cuts inside). At the clear point already: no step. `preferDir`
    // forces the way round (+1 counter-clockwise, -1 clockwise); 0 takes
    // the shorter.
    inline auto detourAround(const Circle& c, const float ax, const float az, const float bx, const float bz, const float clearance = 0.5f, const float arc = 3.0f, const float band = 1.0f, const float preferDir = 0.0f) -> std::pair<float, float>
    {
        constexpr float twoPi = 2.0f * std::numbers::pi_v<float>;

        const float r        = c.radius + clearance;
        const float da       = planarDistance(c.x, c.z, ax, az);
        const float db       = planarDistance(c.x, c.z, bx, bz);
        const float thetaA   = std::atan2(az - c.z, ax - c.x);
        const float thetaB   = std::atan2(bz - c.z, bx - c.x);
        const float exitHalf = db > r ? std::acos(r / db) : 0.0f;

        // How far round, in each direction, to the clear point on that side;
        // the shorter wins
        float dir  = 1.0f;
        float left = twoPi;
        for (const float d : { 1.0f, -1.0f })
        {
            if (preferDir != 0.0f && d != preferDir)
            {
                continue;
            }
            float turn = std::fmod(d * ((thetaB - d * exitHalf) - thetaA), twoPi);
            if (turn < 0.0f)
            {
                turn += twoPi;
            }
            if (turn > twoPi - 0.01f)
            {
                turn = 0.0f;
            }
            if (turn < left)
            {
                left = turn;
                dir  = d;
            }
        }

        const float step  = da > r + band ? std::acos(std::clamp(r / da, -1.0f, 1.0f)) : arc / r;
        const float angle = thetaA + dir * std::min(step, left);
        return { c.x + std::cos(angle) * r, c.z + std::sin(angle) * r };
    }
} // namespace cardian::formation
