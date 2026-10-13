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

#include "pawn_danger.h"

#include "common/settings.h"
#include "common/utils.h"

#include "ai/ai_container.h"
#include "ai/states/magic_state.h"
#include "data/enums/detects.h"
#include "entities/char_entity.h"
#include "entities/mob_entity.h"
#include "instance.h"
#include "party.h"
#include "status_effect_container.h"
#include "zone.h"
#include "zone_entities.h"

#include <algorithm>
#include <cmath>

namespace pawn::danger
{
    namespace
    {
        template <typename Flags>
        auto hasFlag(const Flags value, const Flags flag) -> bool
        {
            return (value & flag) != Flags::None;
        }
    } // namespace

    auto Profile::of(CCharEntity* PPawn, const bool avoidAggro, const bool avoidLinks) -> Profile
    {
        Profile p;
        p.aggro     = avoidAggro;
        p.links     = avoidLinks;
        p.sneak     = PPawn->StatusEffectContainer->HasStatusEffect(xi::StatusEffect::Sneak);
        p.invisible = PPawn->StatusEffectContainer->HasStatusEffectByFlag(xi::StatusEffectFlag::Invisible);
        p.illusion  = PPawn->StatusEffectContainer->HasStatusEffect(xi::StatusEffect::Illusion);
        p.lowHP     = PPawn->GetHPP() < 75;
        // Magic detection fires only while the target casts an MP spell
        // (CanDetectTarget); a permanent circle would make every bat and
        // elemental a wider no-go zone than a sight mob
        p.casting = PPawn->PAI->IsCurrentState<CMagicState>();
        p.tailed  = PPawn;
        p.aggroes = { PPawn };
        return p;
    }

    auto Profile::party(CCharEntity* PPawn) -> Profile
    {
        Profile p = worstCase();
        if (PPawn->PParty == nullptr)
        {
            p.aggroes = { PPawn };
            return p;
        }
        for (auto* PMember : PPawn->PParty->members)
        {
            if (PMember->objtype == TYPE_PC && PMember->loc.zone == PPawn->loc.zone && PMember->PInstance == PPawn->PInstance)
            {
                p.aggroes.push_back(static_cast<CCharEntity*>(PMember));
            }
        }
        return p;
    }

    namespace
    {
        // Every idle mob near `center` that is a danger to the profile, each
        // handed to `emit` with the radius of the sphere it detects in (the
        // buffer on), the ambush's that needs no sight (0 when none),
        // whether it links to a fight of hers, and the radius split into its
        // sight's cone and its reach all round: the danger map's one rule
        // (spheres)
        template <typename Emit>
        void collect(CZoneEntities* entities, const position_t& center, const float scan, const Profile& profile, const CBaseEntity* exclude, Emit&& emit)
        {
            if (entities == nullptr)
            {
                return;
            }

            const float buffer = settings::get<float>("pawn.AVOID_BUFFER") + profile.extraBuffer;
            const float tail   = settings::get<float>("pawn.AVOID_TAIL");

            // Cheapest test first: the coarse cut needs no modifier reads and
            // rejects nearly every mob in the zone. It is bounded by the largest
            // detection range in shipped data, never by a per-type guess.
            const float coarse = scan + MaxDetectionRange + buffer;

            // The link parties of every mob fighting the one asking, gathered
            // first so the main pass is one lookup per mob (the game keeps every
            // linking mob of a family in one party per zone, and checks the link
            // distance only when a kin is engaged -- TryLink). A mob on her is
            // at her, so the coarse cut bounds this pass too.
            std::vector<const CParty*> fighting;
            if (profile.tailed != nullptr && profile.links)
            {
                const auto noteFighting = [&](CMobEntity* PMob)
                {
                    if (PMob->PParty != nullptr && PMob->PAI->IsEngaged() && PMob->GetBattleTarget() == profile.tailed &&
                        isWithinDistance(center, PMob->loc.p, coarse))
                    {
                        fighting.push_back(PMob->PParty);
                    }
                };
                forEachMobNear(entities, center, coarse, noteFighting);
            }

            const auto consider = [&](CMobEntity* PMob)
            {
                if (PMob == exclude || !isWithinDistance(center, PMob->loc.p, coarse))
                {
                    return;
                }
                if (PMob->isDead() || PMob->PMaster != nullptr || PMob->m_neutral ||
                    !PMob->PAI->IsSpawned() || PMob->PAI->IsEngaged())
                {
                    return;
                }

                // The game's own answer, detection aside: would it go for anyone
                // the profile stands for (the circles below are detection). Only
                // asked when her Avoid aggro row would count the answer
                const bool wouldAggro = profile.aggro && std::any_of(profile.aggroes.begin(), profile.aggroes.end(), [&](CCharEntity* PChar)
                                                                     { return entities->wouldAggro(PChar, PMob); });

                // Its kin are on her: it links the way CanLink allows -- not
                // flagged no-link, not an underground worm or antlion. The kin are
                // gathered only when her Avoid links row counts them
                const bool underground = (hasFlag(PMob->m_roamFlags, xi::RoamFlag::Worm) || hasFlag(PMob->m_roamFlags, xi::RoamFlag::Ambush)) &&
                                         PMob->IsNameHidden();
                const bool kin = !fighting.empty() && PMob->PParty != nullptr && PMob->getMobMod(xi::MobMod::NoLink) == 0 && !underground &&
                                 std::find(fighting.begin(), fighting.end(), PMob->PParty) != fighting.end();
                const auto [aggressive, links] = counts(profile.aggro, profile.links, wouldAggro, kin);
                if (!aggressive && !links)
                {
                    return;
                }

                const auto detects = static_cast<xi::Detects>(PMob->getMobMod(xi::MobMod::Detection));
                Detection  d;
                d.sight               = aggressive && hasFlag(detects, xi::Detects::Sight);
                d.hearing             = aggressive && hasFlag(detects, xi::Detects::Hearing);
                d.magic               = aggressive && hasFlag(detects, xi::Detects::Magic);
                d.lowHP               = aggressive && hasFlag(detects, xi::Detects::Lowhp);
                d.ambush              = aggressive && hasFlag(PMob->m_Behavior, xi::Behavior::AggroAmbush);
                d.trueDetection       = PMob->m_TrueDetection;
                d.seesThroughIllusion = PMob->getMobMod(xi::MobMod::SeesThroughIllusion) != 0;
                d.sightRange          = static_cast<float>(PMob->getMobMod(xi::MobMod::SightRange));
                d.soundRange          = static_cast<float>(PMob->getMobMod(xi::MobMod::SoundRange));
                d.magicRange          = static_cast<float>(PMob->getMobMod(xi::MobMod::MagicRange));
                d.links               = links;
                d.linkRange           = static_cast<float>(PMob->getMobMod(xi::MobMod::LinkRadius));

                float radius = radiusFor(d, profile, buffer, tail);
                if (radius <= 0.0f)
                {
                    return;
                }

                // The ambush's sphere needs no sight (CanDetectTarget tests it
                // by distance alone), so it holds behind a wall
                const bool hidesFromSound = (!d.trueDetection && profile.sneak) || (profile.illusion && !d.seesThroughIllusion);
                emit(PMob, radius, d.ambush && !hidesFromSound ? AmbushRange + buffer : 0.0f, links, coneFor(d, profile, buffer), allRoundFor(d, profile, buffer, tail));
            };
            forEachMobNear(entities, center, coarse, consider);
        }
    } // namespace

    auto spheres(CZoneEntities* entities, const position_t& center, const float scan, const Profile& profile, const CBaseEntity* exclude) -> std::vector<Sphere>
    {
        std::vector<Sphere> out;
        collect(entities, center, scan, profile, exclude, [&](CMobEntity* PMob, const float radius, const float unseen, bool, const float cone, const float allRound)
        {
            if (distance(center, PMob->loc.p) <= scan + radius)
            {
                out.push_back({ .mob = PMob, .at = PMob->loc.p, .radius = radius, .unseen = unseen, .cone = cone, .allRound = allRound });
            }
        });
        return out;
    }

    auto sees(const Sphere& sphere, const position_t& point) -> bool
    {
        return sphere.mob != nullptr && sphere.mob->CanSeeTarget(point);
    }
} // namespace pawn::danger

namespace pawn
{
    auto entitiesAround(const CBaseEntity* PEntity) -> CZoneEntities*
    {
        if (PEntity->PInstance != nullptr)
        {
            return PEntity->PInstance;
        }
        return PEntity->loc.zone != nullptr ? PEntity->loc.zone->GetZoneEntities() : nullptr;
    }

    auto forEachMobNear(CZoneEntities* entities, const position_t& center, const float radius, FnRef<void(CMobEntity*)> fn) -> void
    {
        if (entities == nullptr)
        {
            return;
        }

        // Allies are mobs too, filed apart; the mob list is what a sweep walks
        const auto& mobs  = entities->GetMobList();
        const auto  visit = [&](CBaseEntity* PEntity)
        {
            if (PEntity->objtype != TYPE_MOB)
            {
                return;
            }
            if (const auto it = mobs.find(PEntity->targid); it != mobs.end() && it->second == PEntity)
            {
                fn(static_cast<CMobEntity*>(PEntity));
            }
        };
        entities->spatialGrid().forEachInRange(center, radius, visit);
    }
} // namespace pawn
