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

#include "lifts.h"

#include "pawn_travel.h"
#include "porter_mesh.h"

#include "common/earth_time.h"
#include "common/logging.h"

#include "data/datasets/zones/npcs/dataset.h"
#include "data/loader.h"
#include "entities/npc_entity.h"
#include "navmesh/navmesh.h"
#include "utils/zoneutils.h"
#include "zone.h"

#include <cmath>
#include <mutex>
#include <numbers>
#include <unordered_map>

namespace
{
    // A landing: this far beyond the door, away from the platform's centre
    constexpr float kLandingOut = 1.5f;

    // A door's floor is looked for this far above and below the door: the
    // data sets some doors mid-shaft (Palborough Mines' `_3z4`, 15 yalms
    // above its floor)
    constexpr float kFloorReach = 20.0f;

    // The mesh point nearest `at`: on the navmesh by the client's rule where
    // her walks keep to it (a town's), the zone's elsewhere -- and where that
    // one is not built
    auto snap(CZone* PZone, const position_t& at) -> std::optional<position_t>
    {
        if (pawn::porter::mesh::walkedOnClient(PZone))
        {
            if (const auto onTown = pawn::travel::snapToTown(PZone->GetID(), at); onTown.has_value())
            {
                return onTown;
            }
        }
        if (auto* navMesh = PZone->navMesh(); navMesh != nullptr)
        {
            return navMesh->findClosestValidPoint(at);
        }
        return std::nullopt;
    }

    // An end of the shaft: its door, the floor beside it and the landing
    // beyond it; nullopt when the mesh has no ground there
    auto endAt(CZone* PZone, const CNpcEntity* PDoor, const float cx, const float cz) -> std::optional<pawn::lifts::End>
    {
        const float dx  = PDoor->loc.p.x - cx;
        const float dz  = PDoor->loc.p.z - cz;
        const float len = std::hypot(dx, dz);
        if (len < 0.1f)
        {
            return std::nullopt;
        }
        position_t out = PDoor->loc.p;
        out.x          = cx + dx / len * (len + kLandingOut);
        out.z          = cz + dz / len * (len + kLandingOut);

        // The ground there nearest the door's own height, within kFloorReach
        // of it: the mesh is asked at heights a step apart
        std::optional<position_t> landing;
        for (float dy = -kFloorReach; dy <= kFloorReach; dy += 4.0f)
        {
            position_t at = out;
            at.y          = PDoor->loc.p.y + dy;
            const auto onMesh = snap(PZone, at);
            if (!onMesh.has_value() || std::hypot(onMesh->x - out.x, onMesh->z - out.z) > 2.0f || std::abs(onMesh->y - PDoor->loc.p.y) > kFloorReach)
            {
                continue;
            }
            if (!landing.has_value() || std::abs(onMesh->y - PDoor->loc.p.y) < std::abs(landing->y - PDoor->loc.p.y))
            {
                landing = onMesh;
            }
        }
        if (!landing.has_value())
        {
            return std::nullopt;
        }
        pawn::lifts::End end;
        end.door    = PDoor->loc.p;
        end.landing = *landing;
        end.floor   = landing->y;
        return end;
    }

    auto readZone(CZone* PZone) -> std::vector<pawn::lifts::Lift>
    {
        std::vector<pawn::lifts::Lift> lifts;
        const auto npcs = xi::data::loadZoneFile<xi::data::datasets::zones::npcs::Dataset>(PZone->GetID());
        if (!npcs)
        {
            return lifts;
        }
        for (const auto& npc : *npcs)
        {
            if (!npc.Elevator.has_value())
            {
                continue;
            }
            auto* PPlatform = dynamic_cast<CNpcEntity*>(zoneutils::GetEntity(npc.Id, TYPE_NPC));
            auto* PLower    = zoneutils::GetNpcByName(PZone, npc.Elevator->LowerDoor);
            auto* PUpper    = zoneutils::GetNpcByName(PZone, npc.Elevator->UpperDoor);
            if (PPlatform == nullptr || PLower == nullptr || PUpper == nullptr)
            {
                continue;
            }
            pawn::lifts::Lift lift;
            lift.platformId  = PPlatform->id;
            lift.name        = PPlatform->getName();
            lift.cx          = PPlatform->loc.p.x;
            lift.cz          = PPlatform->loc.p.z;
            lift.periodMs    = npc.Elevator->Period;
            lift.doorDelayMs = npc.Elevator->DoorDelay;
            lift.reversed    = npc.Elevator->Reversed;
            lift.timed       = npc.Elevator->Lever == xi::Elevator::TimedAutomatic && npc.Elevator->Period > 0;
            const auto lower = endAt(PZone, PLower, lift.cx, lift.cz);
            const auto upper = endAt(PZone, PUpper, lift.cx, lift.cz);
            if (!lower.has_value() || !upper.has_value() || std::abs(lower->floor - upper->floor) < 2.0f)
            {
                ShowWarningFmt("lift: {} in {}: no landing on the mesh at both its doors; no cardian rides it", lift.name, PZone->getName());
                continue;
            }
            lift.ends[0] = *lower;
            lift.ends[1] = *upper;
            ShowInfoFmt("lift: {} in {}: centre ({:.1f}, {:.1f}); {}'s end at {:.1f}, landing ({:.1f}, {:.1f}); {}'s end at {:.1f}, landing ({:.1f}, {:.1f}); "
                        "{}",
                        lift.name, PZone->getName(), lift.cx, lift.cz, npc.Elevator->LowerDoor, lift.ends[0].floor, lift.ends[0].landing.x, lift.ends[0].landing.z,
                        npc.Elevator->UpperDoor, lift.ends[1].floor, lift.ends[1].landing.x, lift.ends[1].landing.z,
                        lift.timed ? fmt::format("timed, every {} ms", lift.periodMs) : std::string("a lever's: no cardian rides it on her own"));
            lifts.push_back(std::move(lift));
        }
        return lifts;
    }

    std::mutex                                               liftsMutex;
    std::unordered_map<uint16, std::vector<pawn::lifts::Lift>> liftsByZone;
} // namespace

namespace pawn::lifts
{
    auto in(CZone* PZone) -> const std::vector<Lift>&
    {
        static const std::vector<Lift> none;
        if (PZone == nullptr)
        {
            return none;
        }
        const auto           key = static_cast<uint16>(PZone->GetID());
        std::lock_guard<std::mutex> lock(liftsMutex);
        if (const auto it = liftsByZone.find(key); it != liftsByZone.end())
        {
            return it->second;
        }
        return liftsByZone.emplace(key, readZone(PZone)).first->second;
    }

    auto nowMs() -> int64_t
    {
        return static_cast<int64_t>(earth_time::vanadiel_timestamp_ms());
    }

    auto platformOf(const Lift& lift) -> cardian::lift::Platform
    {
        cardian::lift::Platform p;
        p.reversed = lift.reversed;
        p.periodMs = lift.timed ? lift.periodMs : 0;
        if (const auto* PPlatform = dynamic_cast<const CNpcEntity*>(zoneutils::GetEntity(lift.platformId, TYPE_NPC)); PPlatform != nullptr)
        {
            p.animUp     = PPlatform->animation == xi::Animation::ElevatorUp;
            p.timestampS = PPlatform->GetLocalVar("TransportTimestamp");
            p.travelS    = PPlatform->GetLocalVar("TransportTravel");
        }
        return p;
    }

    auto stateOf(const Lift& lift) -> cardian::lift::State
    {
        return cardian::lift::stateAt(platformOf(lift), nowMs());
    }

    auto heightOf(const Lift& lift) -> float
    {
        return cardian::lift::heightAt(stateOf(lift), lift.ends[0].floor, lift.ends[1].floor);
    }

    auto underFoot(CZone* PZone, const position_t& at) -> const Lift*
    {
        for (const auto& lift : in(PZone))
        {
            if (cardian::lift::onPlatform(lift.cx, lift.cz, kRadius, at.x, at.z) && std::abs(at.y - heightOf(lift)) <= 1.5f)
            {
                return &lift;
            }
        }
        return nullptr;
    }

    auto endFor(const Lift& lift, const position_t& at) -> int
    {
        for (int end = 0; end < 2; ++end)
        {
            if (std::abs(at.y - lift.ends[end].floor) <= 1.5f)
            {
                return end;
            }
        }
        return -1;
    }

    auto spotOf(const Lift& lift, const int end, const int i) -> position_t
    {
        // Six spots on a ring round the centre, the first toward the end's door
        const auto& e     = lift.ends[end];
        const float base  = std::atan2(e.door.z - lift.cz, e.door.x - lift.cx);
        const float angle = base + static_cast<float>(i % 6) * std::numbers::pi_v<float> / 3.0f;
        position_t  spot  = e.landing;
        spot.x            = lift.cx + std::cos(angle) * 1.0f;
        spot.z            = lift.cz + std::sin(angle) * 1.0f;
        spot.y            = e.floor;
        return spot;
    }

    auto byPlatform(CZone* PZone, const uint32 platformId) -> const Lift*
    {
        for (const auto& lift : in(PZone))
        {
            if (lift.platformId == platformId)
            {
                return &lift;
            }
        }
        return nullptr;
    }
} // namespace pawn::lifts
