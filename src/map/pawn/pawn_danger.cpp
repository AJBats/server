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
#include "status_effect_container.h"
#include "zone.h"

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

    auto Profile::of(const CCharEntity* PPawn) -> Profile
    {
        Profile p;
        p.sneak     = PPawn->StatusEffectContainer->HasStatusEffect(xi::StatusEffect::Sneak);
        p.invisible = PPawn->StatusEffectContainer->HasStatusEffectByFlag(xi::StatusEffectFlag::Invisible);
        p.illusion  = PPawn->StatusEffectContainer->HasStatusEffect(xi::StatusEffect::Illusion);
        p.lowHP     = PPawn->GetHPP() < 75;
        // Magic detection fires only while the target casts an MP spell
        // (CanDetectTarget); a permanent circle would make every bat and
        // elemental a wider no-go zone than a sight mob
        p.casting = PPawn->PAI->IsCurrentState<CMagicState>();
        return p;
    }

    auto around(CZone* zone, const position_t& center, const float scan, const Profile& profile, const CBaseEntity* exclude) -> std::vector<Danger>
    {
        std::vector<Danger> out;
        if (zone == nullptr)
        {
            return out;
        }

        const float buffer    = settings::get<float>("pawn.AVOID_BUFFER");
        const bool  lowHP     = profile.lowHP;
        const bool  sneak     = profile.sneak;
        const bool  invisible = profile.invisible;
        const bool  illusion  = profile.illusion;
        const bool  casting   = profile.casting;

        // Cheapest test first: the coarse cut needs no modifier reads and
        // rejects nearly every mob in the zone. It is bounded by the largest
        // detection range in shipped data, never by a per-type guess.
        const float coarse = scan + MaxDetectionRange + buffer;

        zone->ForEachMob([&](CMobEntity* PMob)
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
                                        if ((PMob->getMobMod(xi::MobMod::AlwaysAggro) == 0 && !PMob->m_Aggro) || PMob->getMobMod(xi::MobMod::NoAggro) > 0)
                                        {
                                            return;
                                        }

                                        // The cardian's own concealment, the way CanDetectTarget
                                        // reads it: Sneak/Invisible unless the mob has true
                                        // detection; Illusion counts as both unless the mob sees
                                        // through illusions
                                        const bool hidesFromSight = (!PMob->m_TrueDetection && invisible) || (illusion && PMob->getMobMod(xi::MobMod::SeesThroughIllusion) == 0);
                                        const bool hidesFromSound = (!PMob->m_TrueDetection && sneak) || (illusion && PMob->getMobMod(xi::MobMod::SeesThroughIllusion) == 0);
                                        const auto detects        = static_cast<xi::Detects>(PMob->getMobMod(xi::MobMod::Detection));

                                        float radius = 0.0f;
                                        if (hasFlag(detects, xi::Detects::Sight) && !hidesFromSight)
                                        {
                                            radius = std::max(radius, static_cast<float>(PMob->getMobMod(xi::MobMod::SightRange)));
                                        }
                                        if (hasFlag(detects, xi::Detects::Hearing) && !hidesFromSound)
                                        {
                                            radius = std::max(radius, static_cast<float>(PMob->getMobMod(xi::MobMod::SoundRange)));
                                        }
                                        if (hasFlag(detects, xi::Detects::Magic) && casting)
                                        {
                                            radius = std::max(radius, static_cast<float>(PMob->getMobMod(xi::MobMod::MagicRange)));
                                        }
                                        if (hasFlag(detects, xi::Detects::Lowhp) && lowHP)
                                        {
                                            radius = std::max(radius, CloseDetectionRange);
                                        }
                                        if (hasFlag(PMob->m_Behavior, xi::Behavior::AggroAmbush) && !hidesFromSound)
                                        {
                                            radius = std::max(radius, AmbushRange);
                                        }
                                        if (radius <= 0.0f)
                                        {
                                            return;
                                        }
                                        radius += buffer;

                                        // Detection is a sphere and the map is flat: the circle is
                                        // that sphere sliced at the query height, and a mob too far
                                        // above or below (a bridge, a cliff) is no danger at all
                                        const float dy = center.y - PMob->loc.p.y;
                                        if (std::fabs(dy) >= radius)
                                        {
                                            return;
                                        }
                                        radius = std::sqrt(radius * radius - dy * dy);

                                        const float dist = distance(center, PMob->loc.p, true); // planar, like the circle
                                        if (dist > scan + radius)
                                        {
                                            return;
                                        }

                                        Danger d;
                                        d.x        = PMob->loc.p.x;
                                        d.z        = PMob->loc.p.z;
                                        d.radius   = radius;
                                        d.mob      = PMob;
                                        d.distance = dist;
                                        out.push_back(d);
                                    });

        return out;
    }
} // namespace pawn::danger
