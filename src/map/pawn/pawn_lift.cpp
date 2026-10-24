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

// A cardian and a lift (pawn/lifts.h; OPEN_ISSUES #572, ROADMAP T, "Lifts").
// The game moves nobody on a lift but the client's own player, so the
// server carries her. Three ways she comes to ride one:
//   - aboard its platform as it leaves, it carries her, whatever she was
//     doing (her formation place on it beside him, say);
//   - beside him as he stands on a platform resting at their floor, she
//     boards it with him, to a spot of her own, and steps off again if he
//     does before it leaves;
//   - on another floor of his zone that no walk joins to hers, she walks to
//     a timed lift's landing on her floor, waits for its platform, boards,
//     rides and steps off at his floor (LiftTrek); on a trek whose floor has
//     no walk to a line, she takes a lift to a floor that has (LiftOnTrek,
//     LiftToward from the trek's step).
// Aboard, she stands on her spot and her height follows the platform's;
// arrived, she steps off to the landing, and her follow takes her on.
#include "pawn_controller.h"
#include "lifts.h"
#include "lift_math.h"
#include "pawn.h"
#include "pawn_travel.h"
#include "route_search.h"

#include "ai/ai_container.h"
#include "ai/helpers/pathfind.h"
#include "common/logging.h"
#include "common/settings.h"
#include "entities/char_entity.h"
#include "map_constants.h"
#include "party.h"
#include "zone.h"

#include <chrono>
#include <cmath>
#include <limits>

using namespace std::chrono_literals;

namespace
{
    // The time she needs between starting to board and the platform leaving
    constexpr int64_t kBoardNeedMs = 2500;

    // On her spot, at her landing
    constexpr float kAtSpot    = 0.4f;
    constexpr float kAtLanding = 0.8f;

    // A phase she cannot finish in this long gives the ride up; a trek waits
    // this long before it looks for a lift again
    constexpr auto kGiveUp      = 60s;
    constexpr auto kTrekRetry   = 60s;
    // A search for a lift that found none: the next waits this long (each
    // searches the zone's routes from every lift's two ends)
    constexpr auto kMissedRetry = 5s;
    // How often she looks whether a lift stands between her floor and his
    constexpr auto kLookEvery = 3s;
    // A trek's line reached: its walk ends this near it (the mesh often ends
    // short of a zone's mouth)
    constexpr float kLineSlack = 8.0f;
    // Beside him: within this of the landing at his floor, she boards with him
    constexpr float kBesideHim = 15.0f;
    // Farther than this from the landing, off the platform, something else
    // has moved her: the ride is let go
    constexpr float kOffLanding = 4.0f;

    auto phaseName(const CPawnController::LiftRide::Phase phase) -> const char*
    {
        using Phase = CPawnController::LiftRide::Phase;
        switch (phase)
        {
            case Phase::Landing:
                return "walks to the landing";
            case Phase::Waiting:
                return "waits at the landing";
            case Phase::Boarding:
                return "boards";
            case Phase::Aboard:
                return "is aboard";
            case Phase::Alighting:
                return "steps off";
        }
        return "?";
    }

    // A spot on the lift's platform, seen from above
    auto onPlatform(const pawn::lifts::Lift& lift, const position_t& at) -> bool
    {
        return cardian::lift::onPlatform(lift.cx, lift.cz, pawn::lifts::kRadius, at.x, at.z);
    }

    // He stands on the lift's platform, level with it
    auto aboard(const pawn::lifts::Lift& lift, const CCharEntity* PChar) -> bool
    {
        return PChar != nullptr && cardian::lift::onPlatform(lift.cx, lift.cz, pawn::lifts::kRadius, PChar->loc.p.x, PChar->loc.p.z) &&
               std::abs(PChar->loc.p.y - pawn::lifts::heightOf(lift)) <= 1.5f;
    }
} // namespace

void CPawnController::LetGoOfLift(const std::string_view why)
{
    if (!m_Lift.has_value())
    {
        return;
    }
    const auto* PLift = pawn::lifts::byPlatform(POwner->loc.zone, m_Lift->platform);
    ShowInfoFmt("lift: {} lets go of {} ({})", POwner->getName(), PLift != nullptr ? PLift->name : std::string("?"), why);
    m_Lift.reset();
}

void CPawnController::LiftPhase(const LiftRide::Phase phase, const std::string_view why)
{
    m_Lift->phase = phase;
    m_Lift->since = m_Tick;
    const auto* PLift = pawn::lifts::byPlatform(POwner->loc.zone, m_Lift->platform);
    ShowInfoFmt("lift: {} {} on {} ({})", POwner->getName(), phaseName(phase), PLift != nullptr ? PLift->name : std::string("?"), why);
}

auto CPawnController::LiftSpotIndex() const -> int
{
    // Her place among the party's cardians, so no two take one spot
    int index = 0;
    if (const auto* PParty = static_cast<CCharEntity*>(POwner)->PParty; PParty != nullptr)
    {
        for (const auto* PMember : PParty->members)
        {
            if (PMember == POwner)
            {
                return index;
            }
            if (const auto* PChar = dynamic_cast<const CCharEntity*>(PMember); PChar != nullptr && pawn::isPawn(PChar))
            {
                ++index;
            }
        }
    }
    return static_cast<int>(POwner->id % 6);
}

void CPawnController::LiftStep(const position_t& to, const float floor)
{
    auto*      PPathFind = POwner->PAI->PathFind.get();
    const auto before    = POwner->loc.p;
    PPathFind->Clear();
    PPathFind->StepTo(to, true);
    POwner->loc.p.y = floor; // off the mesh, the platform's floor is the floor: no navmesh height under it
    CountMove(before, kLogicUpdateInterval);
    POwner->updatemask |= UPDATE_POS;
}

auto CPawnController::LiftBesideHim() -> bool
{
    // Following him, free: not held, in no fight, at no camp
    auto* PPlayer = GetLivePlayer();
    if (PPlayer == nullptr || m_Mode != Mode::Follow || m_Waiting || POwner->PAI->IsEngaged() || Staked() || !POwner->PAI->CanFollowPath())
    {
        return false;
    }
    const auto* PLift = pawn::lifts::underFoot(POwner->loc.zone, PPlayer->loc.p);
    if (PLift == nullptr)
    {
        return false;
    }
    const auto state = pawn::lifts::stateOf(*PLift);
    if (state.moving || pawn::lifts::endFor(*PLift, POwner->loc.p) != state.end ||
        distance(POwner->loc.p, PLift->ends[state.end].landing) > kBesideHim ||
        !cardian::lift::boardable(pawn::lifts::platformOf(*PLift), state.end, pawn::lifts::nowMs(), kBoardNeedMs, PLift->doorDelayMs))
    {
        return false;
    }
    m_Lift = LiftRide{ .platform = PLift->platformId, .from = state.end, .alightEnd = 1 - state.end, .spot = LiftSpotIndex(), .withPlayer = true };
    const bool onIt = aboard(*PLift, static_cast<CCharEntity*>(POwner));
    LiftPhase(!onIt && distance(POwner->loc.p, PLift->ends[state.end].landing) > 2.5f ? LiftRide::Phase::Landing : LiftRide::Phase::Boarding,
              fmt::format("{} stands on it", PPlayer->getName()));
    return true;
}

namespace
{
    // Whether the walk from `from` reaches `to` -- the route search the walker
    // plans on (in a town, the town's navmesh by the client's rule) ends
    // within `within` of it
    auto reaches(const xi::ZoneId zone, const position_t& from, const position_t& to, const float within) -> bool
    {
        const auto route = pawn::travel::routeAvoiding(zone, from, to, {}, cardian::route::HazardSight{});
        if (!route.has_value())
        {
            return false;
        }
        if (route->complete)
        {
            return true;
        }
        const position_t& last = route->points.empty() ? from : route->points.back();
        return distance(last, to) <= within;
    }
} // namespace

auto CPawnController::LiftToward(const position_t& goal, const float within, const std::string_view why, const bool trek) -> bool
{
    if (m_Lift.has_value())
    {
        return true;
    }
    if (!settings::get<bool>("pawn.LIFTS") || POwner->loc.zone == nullptr || m_Tick - m_LiftGaveUpAt < kTrekRetry || m_Tick - m_LiftMissedAt < kMissedRetry ||
        pawn::lifts::in(POwner->loc.zone).empty())
    {
        return false;
    }

    // The timed lift whose one end her walk reaches and whose other end
    // reaches the goal, the shortest way round
    const auto               zone = POwner->getZone();
    const pawn::lifts::Lift* best = nullptr;
    int                      from = 0;
    float                    cost = std::numeric_limits<float>::max();
    for (const auto& lift : pawn::lifts::in(POwner->loc.zone))
    {
        if (!lift.timed)
        {
            continue;
        }
        for (int end = 0; end < 2; ++end)
        {
            const auto& mine = lift.ends[end];
            const auto& off  = lift.ends[1 - end];
            if (!reaches(zone, POwner->loc.p, mine.landing, kAtLanding) || !reaches(zone, off.landing, goal, within))
            {
                continue;
            }
            const float c = distance(POwner->loc.p, mine.landing) + distance(off.landing, goal);
            if (c < cost)
            {
                best = &lift;
                from = end;
                cost = c;
            }
        }
    }
    if (best == nullptr)
    {
        m_LiftMissedAt = m_Tick;
        return false;
    }
    m_Lift = LiftRide{ .platform = best->platformId, .from = from, .alightEnd = 1 - from, .spot = LiftSpotIndex(), .trek = trek };
    LiftPhase(LiftRide::Phase::Landing, fmt::format("{}, {:.0f}y round by it", why, cost));
    return true;
}

auto CPawnController::LiftTrek(const CCharEntity* PPlayer) -> bool
{
    if (m_Lift.has_value())
    {
        return true;
    }
    if (!settings::get<bool>("pawn.LIFTS") || PPlayer == nullptr || POwner->loc.zone == nullptr || m_Tick - m_LiftLookedAt < kLookEvery ||
        pawn::lifts::in(POwner->loc.zone).empty())
    {
        return false;
    }
    m_LiftLookedAt = m_Tick;

    // Only on a floor her walk does not reach him from
    if (reaches(POwner->getZone(), POwner->loc.p, PPlayer->loc.p, 3.0f))
    {
        return false;
    }
    return LiftToward(PPlayer->loc.p, 3.0f, fmt::format("{} is on another floor", PPlayer->getName()), false);
}

auto CPawnController::LiftOnTrek(const xi::ZoneId targetZone) -> bool
{
    if (m_Lift.has_value())
    {
        return true;
    }
    if (!settings::get<bool>("pawn.LIFTS") || POwner->loc.zone == nullptr || m_Tick - m_LiftGaveUpAt < kTrekRetry || m_Tick - m_LiftMissedAt < kMissedRetry ||
        pawn::lifts::in(POwner->loc.zone).empty())
    {
        return false;
    }

    // The timed lift whose one end her walk reaches, from whose other end
    // the trek has a line, the shortest way round
    const auto               zone = POwner->getZone();
    const pawn::lifts::Lift* best = nullptr;
    int                      from = 0;
    float                    cost = std::numeric_limits<float>::max();
    for (const auto& lift : pawn::lifts::in(POwner->loc.zone))
    {
        if (!lift.timed)
        {
            continue;
        }
        for (int end = 0; end < 2; ++end)
        {
            const auto& mine = lift.ends[end];
            const auto& off  = lift.ends[1 - end];
            if (!reaches(zone, POwner->loc.p, mine.landing, kAtLanding))
            {
                continue;
            }
            const auto hop = pawn::travel::nextHop(zone, off.landing, targetZone);
            if (!hop.has_value() || !reaches(zone, off.landing, hop->walkTo, kLineSlack))
            {
                continue;
            }
            const float c = distance(POwner->loc.p, mine.landing) + distance(off.landing, hop->walkTo);
            if (c < cost)
            {
                best = &lift;
                from = end;
                cost = c;
            }
        }
    }
    if (best == nullptr)
    {
        m_LiftMissedAt = m_Tick;
        return false;
    }
    m_Lift = LiftRide{ .platform = best->platformId, .from = from, .alightEnd = 1 - from, .spot = LiftSpotIndex(), .trek = true };
    LiftPhase(LiftRide::Phase::Landing, fmt::format("her trek to zone {} goes on from its other floor, {:.0f}y round by it", static_cast<uint16>(targetZone), cost));
    return true;
}

auto CPawnController::LiftTick() -> bool
{
    auto* PZone = POwner->loc.zone;
    if (!settings::get<bool>("pawn.LIFTS") || PZone == nullptr || POwner->isDead() || pawn::lifts::in(PZone).empty())
    {
        m_Lift.reset();
        return false;
    }

    if (!m_Lift.has_value())
    {
        // Aboard a platform as it leaves her floor: it carries her
        const pawn::lifts::Lift* PLeaving = nullptr;
        for (const auto& lift : pawn::lifts::in(PZone))
        {
            const auto state = pawn::lifts::stateOf(lift);
            if (state.moving && state.progress < 0.3f && cardian::lift::onPlatform(lift.cx, lift.cz, pawn::lifts::kRadius, POwner->loc.p.x, POwner->loc.p.z) &&
                std::abs(POwner->loc.p.y - lift.ends[1 - state.end].floor) <= 1.5f)
            {
                PLeaving = &lift;
                m_Lift   = LiftRide{ .platform = lift.platformId, .from = 1 - state.end, .alightEnd = state.end, .spot = LiftSpotIndex() };
                break;
            }
        }
        if (PLeaving != nullptr)
        {
            LiftPhase(LiftRide::Phase::Aboard, "on the platform as it leaves");
        }
        else if (!LiftBesideHim())
        {
            return false;
        }
    }

    const auto* PLift = pawn::lifts::byPlatform(PZone, m_Lift->platform);
    if (PLift == nullptr)
    {
        m_Lift.reset();
        return false;
    }
    const auto  state = pawn::lifts::stateOf(*PLift);
    const auto& from  = PLift->ends[m_Lift->from];
    auto*       PPlayer = GetLivePlayer();
    const auto  giveUp = [&](const std::string_view why)
    {
        ShowInfoFmt("lift: {} gives up on {} ({})", POwner->getName(), PLift->name, why);
        POwner->PAI->PathFind->Clear();
        m_Lift.reset();
        m_LiftGaveUpAt    = m_Tick;
        m_CutOffCheckedAt = {};
    };

    switch (m_Lift->phase)
    {
        case LiftRide::Phase::Landing:
        {
            if (m_Lift->withPlayer && !aboard(*PLift, PPlayer))
            {
                POwner->PAI->PathFind->Clear();
                m_Lift.reset(); // he stepped off before she got there
                return false;
            }
            if (std::hypot(POwner->loc.p.x - from.landing.x, POwner->loc.p.z - from.landing.z) <= kAtLanding)
            {
                POwner->PAI->PathFind->Clear();
                LiftPhase(LiftRide::Phase::Waiting, "at the landing");
                return true;
            }
            if (m_Tick - m_Lift->since > kGiveUp)
            {
                giveUp("no way to its landing");
                return false;
            }
            Intent intent{};
            intent.kind      = Intent::Kind::Path;
            intent.point     = from.landing;
            intent.arrive    = 0.5f;
            intent.tolerance = 0.6f;
            intent.seat      = true; // her own spot: the claim board leaves it
            intent.trek      = m_Lift->trek;
            intent.claimed   = m_Lift->trek;
            Walk(std::move(intent)); // steps her path too
            return true;
        }
        case LiftRide::Phase::Waiting:
        {
            if (m_Lift->withPlayer && !aboard(*PLift, PPlayer))
            {
                m_Lift.reset();
                return false;
            }
            if (std::hypot(POwner->loc.p.x - from.landing.x, POwner->loc.p.z - from.landing.z) > kOffLanding)
            {
                LetGoOfLift("she was taken from the landing");
                return false;
            }
            POwner->PAI->PathFind->Clear();
            POwner->PAI->PathFind->LookAt(position_t(PLift->cx, from.floor, PLift->cz, 0, 0));
            if (cardian::lift::boardable(pawn::lifts::platformOf(*PLift), m_Lift->from, pawn::lifts::nowMs(), kBoardNeedMs, PLift->doorDelayMs))
            {
                LiftPhase(LiftRide::Phase::Boarding, "the platform is here");
            }
            else if (m_Tick - m_Lift->since > kGiveUp)
            {
                giveUp("the platform never came");
                return false;
            }
            return true;
        }
        case LiftRide::Phase::Boarding:
        {
            if (state.moving || state.end != m_Lift->from)
            {
                // It left without her: on it all the same, it carries her
                if (cardian::lift::onPlatform(PLift->cx, PLift->cz, pawn::lifts::kRadius, POwner->loc.p.x, POwner->loc.p.z))
                {
                    LiftPhase(LiftRide::Phase::Aboard, "it left as she boarded");
                    return true;
                }
                if (m_Lift->withPlayer)
                {
                    m_Lift.reset();
                    return false;
                }
                LiftPhase(LiftRide::Phase::Waiting, "it left without her");
                return true;
            }
            if (m_Lift->withPlayer && !aboard(*PLift, PPlayer))
            {
                m_Lift->alightEnd = m_Lift->from;
                LiftPhase(LiftRide::Phase::Alighting, "he stepped off");
                return true;
            }
            if (!onPlatform(*PLift, POwner->loc.p) && std::hypot(POwner->loc.p.x - from.landing.x, POwner->loc.p.z - from.landing.z) > kOffLanding)
            {
                LetGoOfLift("she was taken from the landing");
                return false;
            }
            const auto spot = pawn::lifts::spotOf(*PLift, m_Lift->from, m_Lift->spot);
            if (std::hypot(POwner->loc.p.x - spot.x, POwner->loc.p.z - spot.z) <= kAtSpot)
            {
                LiftPhase(LiftRide::Phase::Aboard, "on her spot");
                return true;
            }
            LiftStep(spot, from.floor);
            return true;
        }
        case LiftRide::Phase::Aboard:
        {
            if (!onPlatform(*PLift, POwner->loc.p))
            {
                LetGoOfLift("she is not on it");
                return false;
            }
            if (state.moving)
            {
                // Carried: her height the platform's
                const auto before = POwner->loc.p;
                POwner->PAI->PathFind->Clear();
                POwner->loc.p.y = pawn::lifts::heightOf(*PLift);
                CountMove(before, kLogicUpdateInterval);
                POwner->updatemask |= UPDATE_POS;
                return true;
            }
            if (state.end != m_Lift->from)
            {
                POwner->loc.p.y   = PLift->ends[state.end].floor;
                m_Lift->alightEnd = state.end;
                LiftPhase(LiftRide::Phase::Alighting, "arrived");
                return true;
            }
            // Resting yet at the floor she boarded at
            if (m_Lift->withPlayer && !aboard(*PLift, PPlayer))
            {
                m_Lift->alightEnd = m_Lift->from;
                LiftPhase(LiftRide::Phase::Alighting, "he stepped off");
                return true;
            }
            if (m_Tick - m_Lift->since > kGiveUp)
            {
                m_Lift->alightEnd = m_Lift->from;
                LiftPhase(LiftRide::Phase::Alighting, "it never left");
                return true;
            }
            POwner->PAI->PathFind->Clear();
            return true;
        }
        case LiftRide::Phase::Alighting:
        {
            const auto& off = PLift->ends[m_Lift->alightEnd];
            if (state.moving && onPlatform(*PLift, POwner->loc.p))
            {
                // Under way again before she was off: aboard still
                LiftPhase(LiftRide::Phase::Aboard, "it left again");
                m_Lift->from = 1 - state.end;
                return true;
            }
            if (!onPlatform(*PLift, POwner->loc.p) && std::hypot(POwner->loc.p.x - off.landing.x, POwner->loc.p.z - off.landing.z) > kOffLanding * 2.0f)
            {
                LetGoOfLift("she was taken from it");
                return false;
            }
            if (std::hypot(POwner->loc.p.x - off.landing.x, POwner->loc.p.z - off.landing.z) <= kAtLanding)
            {
                ShowInfoFmt("lift: {} is off {} at its {} end", POwner->getName(), PLift->name, m_Lift->alightEnd == 0 ? "lower door's" : "upper door's");
                POwner->PAI->PathFind->Clear();
                POwner->loc.p.y = off.landing.y;
                m_Lift.reset();
                m_CutOffCheckedAt = {};    // the floor she stands on now is looked at afresh,
                m_TravelHopKnown  = false; // and her trek's way on from it,
                m_LineApproach.forget();   // its line's approach watched from here, not from before the ride
                return false;
            }
            if (m_Tick - m_Lift->since > kGiveUp)
            {
                giveUp("could not step off");
                return false;
            }
            LiftStep(off.landing, off.floor);
            return true;
        }
    }
    return false;
}
