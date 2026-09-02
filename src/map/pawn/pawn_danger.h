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

#include <vector>

class CBaseEntity;
class CCharEntity;
class CMobEntity;
class CZone;
struct position_t;

// The danger map (M3.87): every idle aggressive mob near a cardian as a
// circle it must stay out of. Each detection type the mob has -- sight,
// sound, magic, low-HP, ambush -- is a circle of that type's range plus
// pawn.AVOID_BUFFER; the largest wins. The cardian's own Sneak and
// Invisible shrink the map the way the game's own detection honours them
// (unless the mob has true sight/sound). Mobs already fighting, owned by
// someone, neutral, dead or flagged no-aggro are not dangers, and neither
// is `exclude` (the hunter's chosen pull).
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
        float       distance = 0.0f; // from the query centre to the mob
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

        static auto of(const CCharEntity* PPawn) -> Profile;
        static auto worstCase() -> Profile
        {
            return {};
        }
    };

    // Dangers whose circle comes within `scan` yalms (planar) of `center`,
    // each circle being the mob's detection sphere sliced at center's height
    // (unordered).
    auto around(CZone* zone, const position_t& center, float scan, const Profile& profile, const CBaseEntity* exclude = nullptr) -> std::vector<Danger>;
} // namespace pawn::danger
