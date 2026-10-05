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

#include "mog_house.h"

#include "cardian_link_messages.h"
#include "pawn.h"
#include "pawn_controller.h"

#include "ai/ai_container.h"
#include "common/logging.h"
#include "common/utils.h"
#include "entities/char_entity.h"
#include "packets/c2s/0x100_myroom_job.h"
#include "utils/petutils.h"
#include "zone.h"

#include <magic_enum/magic_enum.hpp>

namespace pawn::moghouse
{
    namespace
    {
        constexpr float kMoogleReach = 8.0f;  // yalms, the player to a Nomad Moogle
        constexpr float kHerReach    = 20.0f; // yalms, the cardian to that moogle

        // A moogle the zone shows: the content gates and the cutscene-only
        // moogles leave theirs hidden
        auto inSight(const CBaseEntity* PNpc) -> bool
        {
            return PNpc->status == xi::Status::Normal || PNpc->status == xi::Status::Update;
        }
    } // namespace

    auto placeOf(const CCharEntity* PPlayer) -> Place
    {
        Place place;
        if (PPlayer == nullptr || PPlayer->loc.zone == nullptr)
        {
            return place;
        }
        // His own, as the game's job change asks (0x100's validate): not a
        // Mog House he visits
        if (PPlayer->m_moghouseID == PPlayer->id)
        {
            place.mogHouse = true;
            return place;
        }
        // The nearest, because a town stands its moogles together
        float nearest = kMoogleReach;
        for (const auto* PNpc : PPlayer->loc.zone->queryEntitiesByName("Nomad_Moogle"))
        {
            if (PNpc == nullptr || !inSight(PNpc))
            {
                continue;
            }
            if (const float away = distance(PPlayer->loc.p, PNpc->loc.p); away <= nearest)
            {
                place.moogle = PNpc;
                nearest      = away;
            }
        }
        return place;
    }

    auto reaches(const Place& place, const CCharEntity* PPlayer, const CCharEntity* PPawn) -> uint16
    {
        if ((!place.mogHouse && place.moogle == nullptr) || PPlayer == nullptr || PPlayer->loc.zone == nullptr || PPawn == nullptr)
        {
            return CL_S_NO_MOG_HOUSE;
        }
        if (PPawn->loc.zone == nullptr)
        {
            return place.mogHouse ? CL_S_NOT_IN_CITY : CL_S_OTHER_ZONE;
        }
        // A Mog House stands in the city zone it opens from, so he is still
        // in that zone while inside it
        const bool sameZone = PPawn->loc.zone == PPlayer->loc.zone;
        if (place.mogHouse)
        {
            return sameZone || pawn::sameCity(PPawn->loc.zone, PPlayer->loc.zone) ? CL_S_OK : CL_S_NOT_IN_CITY;
        }
        if (!sameZone)
        {
            return CL_S_OTHER_ZONE;
        }
        return distance(PPawn->loc.p, place.moogle->loc.p) <= kHerReach ? CL_S_OK : CL_S_FAR_FROM_MOOGLE;
    }

    auto changeJobs(CCharEntity* PPawn, uint8 mainJob, uint8 subJob) -> uint16
    {
        // Between zones her gear and her party have nowhere to be told
        if (PPawn->loc.zone == nullptr)
        {
            return CL_S_CANNOT_NOW;
        }
        if (PPawn->isDead())
        {
            return CL_S_KNOCKED_OUT;
        }
        if (PPawn->isInEvent())
        {
            return CL_S_IN_EVENT;
        }
        // In a fight with her weapon drawn, or without it: attending one from
        // its edge, or walking in on the party's mob
        auto* PController = dynamic_cast<CPawnController*>(PPawn->PAI->GetController());
        if (PPawn->PAI->IsEngaged() || (PController != nullptr && PController->PartyFightTarget() != nullptr))
        {
            return CL_S_IN_A_FIGHT;
        }

        const auto unlocked = [&](const uint8 job)
        {
            return (PPawn->jobs.unlocked & (1u << job)) != 0;
        };
        // The job she has already is no change: the game's job change would
        // still fill her HP and MP
        if (mainJob == static_cast<uint8>(PPawn->GetMJob()))
        {
            mainJob = 0;
        }
        if (subJob == static_cast<uint8>(PPawn->GetSJob()))
        {
            subJob = 0;
        }
        if (mainJob >= MAX_JOBTYPE || subJob >= MAX_JOBTYPE)
        {
            return CL_S_MALFORMED;
        }
        if (mainJob != 0 && !unlocked(mainJob))
        {
            return CL_S_JOB_LOCKED;
        }
        if (subJob != 0)
        {
            if (!unlocked(0))
            {
                return CL_S_NO_SUPPORT_JOBS;
            }
            if (!unlocked(subJob))
            {
                return CL_S_JOB_LOCKED;
            }
            if (subJob == (mainJob != 0 ? mainJob : static_cast<uint8>(PPawn->GetMJob())))
            {
                return CL_S_SAME_JOB;
            }
        }
        if (mainJob == 0 && subJob == 0)
        {
            return CL_S_OK;
        }

        // A pet does not come with her into a Mog House, and an order waiting
        // for her (one given in a pause) was given to the job she leaves
        if (PPawn->PPet != nullptr)
        {
            petutils::DespawnPet(PPawn);
        }
        if (PController != nullptr)
        {
            PController->ClearQueuedOrders("her job changed");
        }

        const auto before = fmt::format("{} {}/{} {}", magic_enum::enum_name(PPawn->GetMJob()), PPawn->GetMLevel(), magic_enum::enum_name(PPawn->GetSJob()), PPawn->GetSLevel());

        // The player's own job change, as his client's packet asks it. Its
        // validate is his Mog House's, so the checks above stand in for it
        GP_CLI_COMMAND_MYROOM_JOB change{};
        change.MainJobIndex    = mainJob;
        change.SupportJobIndex = subJob;
        change.process(nullptr, PPawn);

        ShowInfoFmt("pawn: {} changes jobs at the Mog House: {} -> {} {}/{} {}", PPawn->getName(), before, magic_enum::enum_name(PPawn->GetMJob()), PPawn->GetMLevel(),
                    magic_enum::enum_name(PPawn->GetSJob()), PPawn->GetSLevel());
        return CL_S_OK;
    }
} // namespace pawn::moghouse
