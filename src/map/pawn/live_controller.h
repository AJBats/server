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

#include "gambit_host.h"
#include "pawn_gambits.h"
#include "rest_math.h"

#include "ai/controllers/player_controller.h"
#include "common/timer.h"
#include "common/types/position.h"

#include <memory>

// The controller of a character his own client drives, with his gambits
// (ROADMAP, the road to subjob, item 7). The pawn module installs it at
// every zone-in of a played character, and pawn::release when a client
// takes a cardian over; it is upstream's player controller plus one thing:
// his gambit engine, run on his tick while his switch is on. His own
// commands are upstream's, untouched -- nothing here overrides them, and
// the engine starts his actions through the doors his client's packets
// reach (CAIContainer::Cast, Ability, WeaponSkill, RangedAttack, Engage).
//
// His rows use his hands and never his feet: his client owns where he
// stands. So a row whose target is out of reach waits for him to walk in;
// a spell or a ranged attack waits until he has stood still a moment,
// since moving would interrupt it; nothing starts while he acts, rests,
// rides, is in an event, is down, is asleep or stunned, or in the wait
// the game keeps after his last cast. An Attack row engages a foe within
// engage reach while he is out of a fight, and never switches him to
// another while he is in one: his target is his; nor does it take again a
// mob he disengaged from himself, while it lives. A refusal the game says
// to him is followed by a quiet spell, so a row the game keeps refusing
// is not said to him at every think. His set is his own
// (pawn::kOwnClientSet), off and empty until he builds it, and a held
// pause stands it down with every other tick.
class CLiveController : public CPlayerController
{
public:
    explicit CLiveController(CCharEntity* PChar);
    ~CLiveController() override;

    auto Tick(timer::time_point tick) -> Task<void> override;

    auto Gambits() -> pawn::CGambits&;

    // Whether his rows may start anything now; and the same, his rest and
    // his state's readiness aside -- in the world, not in an event, a
    // mount, the Mog House, a zoning or his logout, and past the quiet after
    // a refusal -- which first aid weighs with his rest
    auto Ready() -> bool;
    auto RowsMayAct() const -> bool;
    // He has stood where he is for long enough that a cast would not be
    // interrupted by his next step (kStillFor)
    auto StandingStill() const -> bool;
    // The game refused an action the engine started: nothing more from his
    // rows for a short while (kQuietFor)
    void Refused(std::string_view what);
    // His client is about to disengage him (his own "attack off", or a talk
    // to an NPC, which ends a fight): the mob he leaves is his to leave, and
    // his rows do not take it again while it lives. Called by the pawn
    // module's packet hook ahead of upstream's handler
    void LeavingByHand();

    // His kneel on the rest lifecycle a cardian's runs on (rest_math.h
    // State): an action waits until he has risen, and first aid weighs him
    // kneeling as it weighs her
    auto RestAllowsAction() const -> bool;
    auto RestReadyIn(double now) const -> double;

    // The mob he left by his own hand, while it lives (LeavingByHand): his
    // door passes it by
    auto LeftByHand(const CBattleEntity* PFoe) const -> bool;
    // The party's fight he attends as a mage out of it, as of his last tick
    // (CGambits::AttendsFight): what his rows call "the mob"
    auto AttendedFight() const -> CBattleEntity*;

    // Puts this controller on a played character still on upstream's own
    // controller: at every zone-in, and after upstream swapped it back
    // mid-session (a charm ending, a jail)
    static void InstallOn(CCharEntity* PChar);

private:
    // His Attack rows' door: a fight for him while he is out of one
    void EngageDoor();

    std::unique_ptr<pawn::GambitHost> m_Host;
    std::unique_ptr<pawn::CGambits>   m_Gambits;

    // The mob he left by his own hand, forgotten once it dies or is gone
    void WatchLeftAlive();
    // First aid picked him while he kneels: he stands for it, as a cardian
    // does
    void WakeForFirstAid();

    position_t        m_LastPos{};
    timer::time_point m_StillSince{};
    timer::time_point m_QuietUntil{};
    timer::time_point m_NextDoor{};
    EntityId          m_LeftAlive{};
    EntityId          m_Attended{};
    cardian::rest::State m_Rest;
};
