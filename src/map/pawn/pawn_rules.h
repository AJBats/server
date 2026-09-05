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

#include "formation_math.h"

#include <cmath>
#include <cstddef>
#include <optional>
#include <string>
#include <utility>
#include <vector>

// The rules a cardian fights by, as pure functions over plain facts: one
// answer per question, asked the same way from every door into a fight
// (the party's fight, a hunter's pull, the player's order, self-defence)
// and from the fight itself. The controller gathers the facts; nothing
// here reads an entity, so a rule change fails in the tests before it
// fails in the field.
namespace cardian::rules
{
    // The server's own gate: CCharEntity::CanAttack throws a character out
    // of a fight ("you lose sight") whose target is more than this far
    // away, checked at every swing. A cardian drawing at the edge is thrown
    // out by the mob's next step, so she draws a little inside it and
    // walks in the rest.
    constexpr float kServerSightRange = 30.0f;
    constexpr float kDrawRange        = kServerSightRange - 2.0f;

    // The clearance every planned point keeps from a danger circle. The
    // same padding judges a pull, so the pick and the fight never disagree
    // by it: a target just outside a mob's circle was once a clean pick and
    // an unclean fight, and she drew on it and dropped it in a loop.
    constexpr float kClearance = 1.5f;

    // Yes, or no and why -- in the narration's voice, so a refusal reads in
    // the log as its reason
    struct Verdict
    {
        bool        ok = true;
        std::string why;

        explicit operator bool() const
        {
            return ok;
        }
    };

    inline auto allowed() -> Verdict
    {
        return {};
    }

    inline auto blocked(std::string why) -> Verdict
    {
        return { false, std::move(why) };
    }

    // ------------------------------------------------------------------
    // May she fight this?
    // ------------------------------------------------------------------

    // What the controller knows about a would-be target
    struct EngageFacts
    {
        bool  exists      = false;
        bool  alive       = false;
        bool  retreating  = false; // the "on me" switch is up
        bool  underground = false; // untargetable, or a worm below (pawn::isUnderground)
        bool  hostile     = false; // a mob of the Mob allegiance
        bool  claimable   = true;  // unclaimed, or claimed within her alliance (CCharEntity::IsMobOwner)
        bool  cooldown    = false; // the draw cooldown is still running
        float distance    = 0.0f;  // yalms from her
    };

    // In the order the reasons matter to the reader: a dead target is not
    // "too far", a retreat is not "claimed"
    inline auto mayFight(const EngageFacts& f) -> Verdict
    {
        if (!f.exists)
        {
            return blocked("no target");
        }
        if (!f.alive)
        {
            return blocked("dead");
        }
        if (f.retreating)
        {
            return blocked("retreating");
        }
        if (f.underground)
        {
            return blocked("underground");
        }
        if (!f.hostile)
        {
            return blocked("not hostile");
        }
        if (!f.claimable)
        {
            return blocked("claimed by another party");
        }
        if (f.distance > kDrawRange)
        {
            return blocked(std::to_string(static_cast<int>(std::lround(f.distance))) + " y away");
        }
        if (f.cooldown)
        {
            return blocked("draw cooldown");
        }
        return allowed();
    }

    // A refusal the walk in cures: only the distance, or the draw's own
    // wait, stands in the way. She walks in with her weapon away and draws
    // when the rules allow.
    inline auto worthWalkingIn(const EngageFacts& f) -> bool
    {
        if (mayFight(f))
        {
            return false;
        }
        EngageFacts there = f;
        there.distance    = 0.0f;
        there.cooldown    = false;
        return mayFight(there).ok;
    }

    // ------------------------------------------------------------------
    // Is this pull clean?
    // ------------------------------------------------------------------

    using Circle  = cardian::formation::Circle;
    using Circles = std::vector<Circle>;

    constexpr std::size_t npos = static_cast<std::size_t>(-1);

    // The planning circles: every danger grown by the clearance
    inline auto padded(const Circles& circles, const float clearance = kClearance) -> Circles
    {
        Circles out;
        out.reserve(circles.size());
        for (const auto& c : circles)
        {
            out.push_back(Circle{ c.x, c.z, c.radius + clearance });
        }
        return out;
    }

    // Why a pull is unclean: the target stands inside a circle (its
    // guard), or the straight way in from `from` enters one. `ignore` is
    // the target's own circle when it is an aggressive mob itself -- the
    // pull is allowed into that one. nullopt = clean.
    struct PullBlock
    {
        std::size_t circle       = npos;  // index into the circles
        bool        targetInside = false; // else the way in enters it
    };

    inline auto pullBlocked(const Circles& paddedCircles, const float fromX, const float fromZ, const float toX, const float toZ, const std::size_t ignore = npos) -> std::optional<PullBlock>
    {
        using cardian::formation::depthInside;
        using cardian::formation::segmentEnters;

        // The guard first: a target inside a circle is the stronger fact
        for (std::size_t i = 0; i < paddedCircles.size(); ++i)
        {
            if (i != ignore && depthInside(paddedCircles[i], toX, toZ) > 0.0f)
            {
                return PullBlock{ i, true };
            }
        }
        for (std::size_t i = 0; i < paddedCircles.size(); ++i)
        {
            if (i != ignore && segmentEnters(paddedCircles[i], fromX, fromZ, toX, toZ))
            {
                return PullBlock{ i, false };
            }
        }
        return std::nullopt;
    }
} // namespace cardian::rules
