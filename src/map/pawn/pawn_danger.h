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
// as a ball it must stay out of. Whether an idle mob would go for her at
// all is the game's own aggro test, asked (CZoneEntities::tapMobAggro with
// `ask`: everything but detection) -- her level against the mob's (a Too
// Weak mob leaves her be unless she rests), AlwaysAggro, neutral, the
// battlefield, the follow roamers and whatever else the server decides by.
// Each detection type such a mob has -- sight, sound, magic, low-HP,
// ambush -- is a ball of that type's range plus pawn.AVOID_BUFFER (her
// Avoid aggro row); the idle kin of a mob already fighting the cardian is a
// ball of its link range plus the tail the mob keeps behind her
// (pawn.AVOID_TAIL; her Avoid links row) -- she is the one it follows, so
// she is the one who leads it away; the largest ball wins. The
// cardian's own Sneak and Invisible shrink the map the way the game's own
// detection honours them (unless the mob has true sight/sound). Mobs
// already fighting, owned by someone, dead or neutral are not dangers, and
// neither is `exclude` (the hunter's chosen pull).
namespace pawn::danger
{
    // Ranges CMobController::CanDetectTarget hard-codes for the detections
    // without a range modifier: low-HP/ability detection inside 20 y,
    // ambushers at 3 y. MaxDetectionRange bounds the shipped SightRange/
    // SoundRange modifiers (scripts go as high as 60) for the coarse cut.
    constexpr float CloseDetectionRange = 20.0f;
    constexpr float AmbushRange         = 3.0f;
    constexpr float MaxDetectionRange   = 60.0f;

    // What the mobs can detect about the one asking: a cardian's own
    // concealment, health and casting state shrink or grow the map the way
    // CanDetectTarget reads them. worstCase() assumes none of the
    // protections and all of the triggers; party() is that, for judging a
    // pull the whole party will fight beside.
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

        // Whom the game's aggro test is asked about: a mob is a danger when
        // it would go for any of them. None, and only linking makes one.
        std::vector<CCharEntity*> aggroes;

        // Her two avoidance rows, each on its own (ROADMAP K6): Avoid aggro
        // counts the circles of an idle mob that would go for her, Avoid
        // links the circle of an idle kin of a mob fighting her (counts)
        bool aggro = true;
        bool links = true;

        // Yalms more than pawn.AVOID_BUFFER round every circle: a trek's
        // wider berth (pawn.TREK_AVOID_EXTRA, ROADMAP T), with no camp spot
        // to keep her near the edge
        float extraBuffer = 0.0f;

        // A cardian's own profile, with whether she avoids aggro and links
        // (CPawnController::IsAvoidingAggro, IsAvoidingLinks)
        static auto of(CCharEntity* PPawn, bool avoidAggro, bool avoidLinks) -> Profile;
        static auto party(CCharEntity* PPawn) -> Profile; // worst case, asked about every party member in her zone
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

    // Which of an idle mob's circles count, her two avoidance rows each on
    // its own: its detection circles when it would go for her and she
    // avoids aggro; its link circle when its kin fight her and she avoids
    // links. Avoid aggro off and Avoid links on, she walks past idle mobs
    // but keeps clear of her own fight's kin; the other way round, she keeps
    // out of every circle but pays linking kin no mind
    struct Counts
    {
        bool aggressive = false;
        bool links      = false;
    };

    constexpr auto counts(const bool avoidAggro, const bool avoidLinks, const bool wouldAggro, const bool kinFightHer) -> Counts
    {
        return { avoidAggro && wouldAggro, avoidLinks && kinFightHer };
    }

    // Whether she avoids aggro (CPawnController::IsAvoidingAggro). A retreat
    // never does, and a trek always does. In a real player's party his
    // Shortcuts menu's switch decides alone, whatever her own Avoid aggro row
    // says (pawn.h HuntRules::avoidAggro); a body in no real player's party
    // follows her own row
    constexpr auto avoidsAggro(const bool retreating, const bool trekking, const bool inPlayersParty, const bool partySwitch, const bool ownRow) -> bool
    {
        if (retreating)
        {
            return false;
        }
        if (trekking)
        {
            return true;
        }
        return inPlayersParty ? partySwitch : ownRow;
    }

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

    // The same rule split for a mob whose back may be turned (the sneak-by,
    // ROADMAP T slice 3): its sight, which reaches only within its cone, and
    // everything else, which reaches all round. The larger of the two is
    // radiusFor's; zero is no reach of that kind
    inline auto coneFor(const Detection& d, const Profile& p, const float buffer) -> float
    {
        const bool hidesFromSight = (!d.trueDetection && p.invisible) || (p.illusion && !d.seesThroughIllusion);
        return d.sight && !hidesFromSight && d.sightRange > 0.0f ? d.sightRange + buffer : 0.0f;
    }

    inline auto allRoundFor(Detection d, const Profile& p, const float buffer, const float tail) -> float
    {
        d.sight = false;
        return radiusFor(d, p, buffer, tail);
    }

    // A danger as the walker plans round it: its mob, where it stands, and
    // the whole sphere it detects in -- not cut flat at one height, since a
    // route climbs and drops -- with the ambush's sphere that needs no sight
    // (0 when none)
    struct Sphere
    {
        CMobEntity* mob    = nullptr;
        position_t  at{};
        float       radius = 0.0f;
        float       unseen = 0.0f;

        // The radius split as a sneak-by reads it (coneFor, allRoundFor):
        // sight's reach, within the mob's cone, and the reach all round
        float cone     = 0.0f;
        float allRound = 0.0f;
    };

    // Every danger within `scan` (3D, sphere's edge) of `center`, `exclude`
    // none (unordered)
    auto spheres(CZoneEntities* entities, const position_t& center, float scan, const Profile& profile, const CBaseEntity* exclude = nullptr) -> std::vector<Sphere>;

    // Line of sight from a sphere's mob to a point, the mob's own test
    // (CBaseEntity::CanSeeTarget: a ray against the zone's collision mesh,
    // cached). Every way a mob notices a character but the 3 y ambush ends
    // in this test, and so does linking, so a mob behind a wall is no
    // danger to a point it cannot see
    auto sees(const Sphere& sphere, const position_t& point) -> bool;
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
