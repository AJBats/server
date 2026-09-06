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

#include "common/types/fn.h"
#include "common/types/position.h"

#include <algorithm>
#include <vector>

class CBaseEntity;
class CBattleEntity;
class CCharEntity;
class CMobEntity;
class CZoneEntities;

// The danger map (M3.87): every mob near a cardian that could turn on it,
// as a circle it must stay out of. Each detection type an idle aggressive
// mob has -- sight, sound, magic, low-HP, ambush -- is a circle of that
// type's range plus pawn.AVOID_BUFFER; the idle kin of a mob already
// fighting the cardian is a circle of its link range plus the tail the mob
// keeps behind her (pawn.AVOID_TAIL) -- she is the one it follows, so she is
// the one who leads it away; the largest circle wins. The cardian's own
// Sneak and Invisible shrink the map the way the game's own detection
// honours them (unless the mob has true sight/sound). Mobs already
// fighting, owned by someone, neutral, dead or flagged no-aggro are not
// dangers, and neither is `exclude` (the hunter's chosen pull).
namespace pawn::danger
{
    // Ranges CMobController::CanDetectTarget hard-codes for the detections
    // without a range modifier: low-HP/ability detection inside 20 y,
    // ambushers at 3 y. MaxDetectionRange bounds the shipped SightRange/
    // SoundRange modifiers (scripts go as high as 60) for the coarse cut.
    constexpr float CloseDetectionRange = 20.0f;
    constexpr float AmbushRange         = 3.0f;
    constexpr float MaxDetectionRange   = 60.0f;

    // A danger IS its circle (x, z, radius), so the geometry helpers take a
    // vector of these directly
    struct Danger : cardian::formation::Circle
    {
        CMobEntity* mob          = nullptr;
        float       distance     = 0.0f;  // from the query centre to the mob
        bool        linked       = false; // kin of a mob fighting the one asking
        float       unseenRadius = 0.0f;  // the circle that holds with no line of sight: the ambush's (0 when none)
    };

    // Line of sight from the danger's mob to a point, the mob's own test
    // (CBaseEntity::CanSeeTarget: a ray against the zone's collision mesh,
    // cached). Every way a mob notices a character but the 3 y ambush ends
    // in this test, and so does linking, so a mob behind a wall is no
    // danger to a point it cannot see. The circle is the range; the sight
    // is the rest of the rule.
    auto sees(const Danger& danger, const position_t& point) -> bool;

    // The dangers that matter to a walk from `from` to `to`: those whose
    // mobs see the start, the end, or the point of the way nearest to them
    // -- a wall between makes the rest no danger, except that an ambusher
    // keeps its unseen circle, which the server tests without sight. One
    // rule for the vet, the clear-spot test, the escape test (a walk of no
    // length: the mobs that see her where she stands) and the pull rule,
    // so no two of them disagree. `sees` is the caller's line-of-sight
    // test, memoised per tick.
    using Sight = FnRef<bool(const Danger&, const position_t&)>;
    inline auto forWalk(const std::vector<Danger>& all, const position_t& from, const position_t& to, Sight sees) -> std::vector<Danger>
    {
        std::vector<Danger> out;
        out.reserve(all.size());
        for (const auto& d : all)
        {
            // The point of the way nearest the mob: where a circle across
            // the middle of a walk would first see her
            const float dx  = to.x - from.x;
            const float dz  = to.z - from.z;
            const float len = dx * dx + dz * dz;
            const float t   = len > 0.0f ? std::clamp(((d.x - from.x) * dx + (d.z - from.z) * dz) / len, 0.0f, 1.0f) : 0.0f;
            const position_t nearest(from.x + dx * t, from.y + (to.y - from.y) * t, from.z + dz * t, 0, 0);

            if (sees(d, from) || (len > 0.0f && sees(d, to)) || (t > 0.0f && t < 1.0f && sees(d, nearest)))
            {
                out.push_back(d);
            }
            else if (d.unseenRadius > 0.0f)
            {
                Danger unseen = d;
                unseen.radius = d.unseenRadius;
                out.push_back(unseen);
            }
        }
        return out;
    }

    // What the mobs can detect about the one asking: a cardian's own
    // concealment, health and casting state shrink or grow the map the way
    // CanDetectTarget reads them. worstCase() assumes none of the
    // protections and all of the triggers -- the profile for judging a pull
    // the whole party will fight beside.
    struct Profile
    {
        bool sneak     = false;
        bool invisible = false;
        bool illusion  = false;
        bool lowHP     = true;
        bool casting   = true;

        // Whose fights count for linking: the idle kin of every mob targeting
        // this entity are dangers to it. None for a profile with no fight of
        // its own.
        const CBattleEntity* tailed = nullptr;

        static auto of(const CCharEntity* PPawn) -> Profile;
        static auto worstCase() -> Profile
        {
            return {};
        }
    };

    // One mob's ways of noticing the one asking, as the map reads them
    struct Detection
    {
        bool  sight               = false;
        bool  hearing             = false;
        bool  magic               = false;
        bool  lowHP               = false;
        bool  ambush              = false;
        bool  trueDetection       = false;
        bool  seesThroughIllusion = false;
        float sightRange          = 0.0f;
        float soundRange          = 0.0f;
        float magicRange          = 0.0f;
        bool  links               = false; // its kin are fighting the one asking
        float linkRange           = 0.0f;
    };

    // The radius rule: every detection the profile does not hide from is a
    // candidate, linking (link range plus the tail) is one more, the largest
    // wins and the buffer goes on top. Zero means no danger at all.
    inline auto radiusFor(const Detection& d, const Profile& p, const float buffer, const float tail) -> float
    {
        const bool hidesFromSight = (!d.trueDetection && p.invisible) || (p.illusion && !d.seesThroughIllusion);
        const bool hidesFromSound = (!d.trueDetection && p.sneak) || (p.illusion && !d.seesThroughIllusion);

        float radius = 0.0f;
        if (d.sight && !hidesFromSight)
        {
            radius = std::max(radius, d.sightRange);
        }
        if (d.hearing && !hidesFromSound)
        {
            radius = std::max(radius, d.soundRange);
        }
        if (d.magic && p.casting)
        {
            radius = std::max(radius, d.magicRange);
        }
        if (d.lowHP && p.lowHP)
        {
            radius = std::max(radius, CloseDetectionRange);
        }
        if (d.ambush && !hidesFromSound)
        {
            radius = std::max(radius, AmbushRange);
        }
        if (d.links)
        {
            radius = std::max(radius, d.linkRange + tail);
        }
        return radius > 0.0f ? radius + buffer : 0.0f;
    }

    // Dangers whose circle comes within `scan` yalms (planar) of `center`,
    // each circle being the mob's detection sphere sliced at center's height
    // (unordered).
    auto around(CZoneEntities* entities, const position_t& center, float scan, const Profile& profile, const CBaseEntity* exclude = nullptr) -> std::vector<Danger>;
} // namespace pawn::danger

// The zone's proximity grid, queried for mobs: the same set a sweep of the
// mob list yields, narrowed to the cells within `radius` of `center`. The
// grid only narrows -- callers keep their own precise distance and status
// filters, as they did over the sweep.
namespace pawn
{
    // The entity list a mob near this entity is filed in: its instance's
    // when it stands in one, else its zone's
    auto entitiesAround(const CBaseEntity* PEntity) -> CZoneEntities*;

    auto forEachMobNear(CZoneEntities* entities, const position_t& center, float radius, FnRef<void(CMobEntity*)> fn) -> void;
} // namespace pawn
