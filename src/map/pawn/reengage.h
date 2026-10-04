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

#include "ai/ai_container.h"
#include "common/settings.h"
#include "common/timer.h"
#include "entities/base_entity.h"

#include <algorithm>
#include <chrono>

// The player's re-engage rule, in one place for his own engage
// (CPlayerController::Engage, a marked touchpoint) and the door of his own
// gambits (CLiveController::EngageDoor), so the door never asks the game
// early. Upstream's retail lockout (CAttackState::EngageLockout) stops the
// disengage trick on the mob he was hitting; a switch to another mob is not
// that trick, and waits at most cardian.REENGAGE_SWITCH_DELAY after his last
// swing. A cardian's own draw has its rule apart (pawn::reengageWait).
namespace cardian::reengage
{
    // cardian.REENGAGE_SWITCH_DELAY: the wait before engaging a different
    // mob, for the player and for a cardian's own draw
    inline auto switchDelay() -> timer::duration
    {
        return std::chrono::milliseconds(static_cast<int64>(settings::get<float>("cardian.REENGAGE_SWITCH_DELAY") * 1000.0f));
    }

    // When he may engage: the lockout for the mob of his last swing; for
    // any other, the lockout or the switch delay after that swing,
    // whichever ends first
    constexpr auto readyAt(const bool sameMob, const timer::time_point lockedUntil, const timer::time_point lastSwing,
                           const timer::duration switchDelay) -> timer::time_point
    {
        return sameMob ? lockedUntil : std::min(lockedUntil, lastSwing + switchDelay);
    }

    // The mob of his last swing, set beside the swing's time
    // (CCharEntity::OnAttack): what he was hitting, whatever engage or
    // target change put him on it
    inline constexpr char kSwungAtVar[] = "cardianSwungAt";

    // When he may engage PTarget. Engaged already, an engage only changes
    // his target and resets no swing, so it waits for nothing
    inline auto playerReadyAt(const CBaseEntity* PChar, const CBaseEntity* PTarget, const timer::time_point lockedUntil,
                              const timer::time_point lastSwing) -> timer::time_point
    {
        if (PChar->PAI->IsEngaged())
        {
            return timer::time_point::min();
        }
        return readyAt(PChar->GetLocalVar(kSwungAtVar) == PTarget->id, lockedUntil, lastSwing, switchDelay());
    }

    inline void markSwing(CBaseEntity* PChar, const CBaseEntity* PTarget)
    {
        PChar->SetLocalVar(kSwungAtVar, PTarget != nullptr ? PTarget->id : 0);
    }
} // namespace cardian::reengage
