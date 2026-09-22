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

#include "view.h"

#include "common/scheduler.h"
#include "entities/char_entity.h"
#include "zone.h"

#include <unordered_map>

namespace cardian::view
{
    namespace
    {
        // charid -> the entity he looks through, by id and by target index in
        // his zone. Resolved at every ask through the zone's index lookup, and
        // the id checked, so a body that despawns or zones is simply not found
        // and a reused index is not mistaken for her.
        struct Origin
        {
            uint32 id;
            uint16 targid;
        };
        std::unordered_map<uint32, Origin> origins;
    } // namespace

    void set(const CCharEntity* PChar, const CBaseEntity* PEntity)
    {
        origins[PChar->id] = { PEntity->id, PEntity->targid };
    }

    void clear(const CCharEntity* PChar)
    {
        origins.erase(PChar->id);
    }

    void clearById(const uint32 charid)
    {
        origins.erase(charid);
    }

    auto origin(const CCharEntity* PChar) -> CBaseEntity*
    {
        if (origins.empty())
        {
            return nullptr;
        }
        const auto it = origins.find(PChar->id);
        if (it == origins.end())
        {
            return nullptr;
        }
        if (PChar->loc.zone == nullptr)
        {
            return nullptr;
        }
        CBaseEntity* PEntity = PChar->loc.zone->GetEntity(it->second.targid, TYPE_PC | TYPE_MOB | TYPE_NPC);
        if (PEntity == nullptr || PEntity->id != it->second.id || PEntity == PChar || PEntity->loc.zone != PChar->loc.zone)
        {
            return nullptr;
        }
        return PEntity;
    }

    auto isViewed(const CBaseEntity* PEntity) -> bool
    {
        for (const auto& [charid, through] : origins)
        {
            if (through.id == PEntity->id)
            {
                return true;
            }
        }
        return false;
    }

    namespace
    {
        std::function<void()> steerTick;
        Scheduler::Token*     steerToken = nullptr; // leaked on purpose: a Token out of scope cancels its timer
    } // namespace

    void startTimers(Scheduler& scheduler)
    {
        if (steerToken != nullptr)
        {
            return;
        }
        steerToken = new Scheduler::Token(scheduler.intervalOnMainThread(std::chrono::milliseconds(kSteerPeriodMs), []() -> void
        {
            if (steerTick)
            {
                steerTick();
            }
        }));
    }

    void setSteerTick(std::function<void()> fn)
    {
        steerTick = std::move(fn);
    }

    auto timersArmed() -> bool
    {
        return steerToken != nullptr;
    }

    void forEachViewer(CZone* PZone, const std::function<void(CCharEntity*, const CBaseEntity*)>& fn)
    {
        for (const auto& [charid, through] : origins)
        {
            CCharEntity* PChar = PZone->GetCharByID(charid);
            if (PChar == nullptr)
            {
                continue;
            }
            if (const auto* PEntity = origin(PChar))
            {
                fn(PChar, PEntity);
            }
        }
    }
} // namespace cardian::view
