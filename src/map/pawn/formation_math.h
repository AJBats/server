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
#include <cmath>
#include <numbers>

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
} // namespace cardian::formation
