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

#include <algorithm>
#include <vector>

class CBaseEntity;
class CBattleEntity;
class CCharEntity;
class CMobEntity;
class CZoneEntities;
struct position_t;

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
        CMobEntity* mob      = nullptr;
        float       distance = 0.0f;  // from the query centre to the mob
        bool        linked   = false; // kin of a mob fighting the one asking
    };

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
