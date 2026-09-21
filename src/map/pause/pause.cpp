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

#include "pause.h"

#include "calendar_store.h"
#include "common/logging.h"
#include "common/settings.h"
#include "entities/char_entity.h"
#include "input_gate.h"
#include "packets/char_status.h"
#include "packets/s2c/0x063_miscdata_status_icons.h"
#include "packets/s2c/0x119_abil_recast.h"
#include "pawn/cardian_link.h"
#include "pawn/players.h"
#include "status_effect_container.h"
#include "utils/zoneutils.h"
#include "zone.h"

#include <fmt/format.h>

namespace cardian::pause
{
namespace
{

// Main thread only, like hold() and release(). Whether the simulation is
// held is not kept here: the clock is the one place that says so.
struct Book
{
    uint32               holder = 0;
    std::string          holderName;
    realtime::time_point heldSince{};
    uint32               holds = 0;
    realtime::duration   heldTotal{};
} book;

template <typename F>
void forEachRealPlayer(F&& func)
{
    zoneutils::ForEachZone(
        [&func](CZone* PZone)
        {
            PZone->ForEachChar(
                [&func](CCharEntity* PChar)
                {
                    if (PChar->PSession != nullptr)
                    {
                        func(PChar);
                    }
                });
        });
}

// The movement lock rides his status packet (packets/char_status.cpp), so a hold
// taken or let go sends it at once rather than at his next change.
void resendStatus()
{
    forEachRealPlayer(
        [](CCharEntity* PChar)
        {
            PChar->pushPacket<CCharStatusPacket>(PChar);
        });
}

// His client counts ability recasts and buff timers down on its own clock, which ran
// on through the hold, so at the release it is told again what is left of each.
void resendTimers()
{
    forEachRealPlayer(
        [](CCharEntity* PChar)
        {
            PChar->pushPacket<GP_SERV_COMMAND_ABIL_RECAST>(PChar);
            PChar->pushPacket<GP_SERV_COMMAND_MISCDATA::STATUS_ICONS>(PChar);
        });
}

} // namespace

auto hold(const uint32 holderCharId, const std::string_view holderName) -> Result
{
    if (timer::is_held())
    {
        return Result::AlreadyHeld;
    }

    timer::hold();
    earth_time::hold_calendar();
    calendar::save(); // whoever follows the game clock by the row sees it stand still

    book.holder     = holderCharId;
    book.holderName = holderName;
    book.heldSince  = realtime::now();
    ++book.holds;

    ShowInfoFmt("pause: held by {} ({})", book.holderName, book.holder);

    resendStatus();
    cardian::link::sendToAll(fmt::format("cd paused {} {}", book.holderName, earth_time::vanadiel_timestamp()));
    return Result::Ok;
}

auto release(const std::string_view why) -> Result
{
    if (!timer::is_held())
    {
        return Result::NotHeld;
    }

    const auto heldFor = realtime::now() - book.heldSince;
    book.heldTotal += heldFor;

    timer::release();
    earth_time::release_calendar();
    calendar::save(); // the drift just grew by this hold, and the row says running again

    ShowInfoFmt("pause: released after {:.1f}s, {} (held by {})", std::chrono::duration<double>(heldFor).count(), why, book.holderName);

    book.holder = 0;
    book.holderName.clear();

    resendStatus();
    resendTimers();
    cardian::link::sendToAll(fmt::format("cd resumed {}", earth_time::vanadiel_timestamp()));

    // Last, with the clock running and nothing held: each goes to the game's own
    // handler as if it had just arrived.
    input::replay();
    return Result::Ok;
}

auto toggle(CCharEntity* PChar) -> std::string
{
    if (!settings::get<bool>("cardian.PAUSE_ENABLED"))
    {
        return "the pause is switched off on this server";
    }

    const uint32 charId = PChar->id;

    if (!timer::is_held())
    {
        // Nobody logs out of a held game (input_gate.h), so nobody holds one on his way out
        if (PChar->StatusEffectContainer->HasStatusEffect(xi::StatusEffect::Leavegame))
        {
            return "you are logging out";
        }

        hold(charId, PChar->getName());
        return "";
    }

    if (book.holder != charId)
    {
        return fmt::format("{} has the game paused", book.holderName);
    }

    release("its holder resumed");
    return "";
}

auto isHeld() -> bool
{
    return timer::is_held();
}

auto status() -> Status
{
    Status status;
    status.held      = timer::is_held();
    status.holds     = book.holds;
    status.heldTotal = book.heldTotal;
    if (status.held)
    {
        status.holder     = book.holder;
        status.holderName = book.holderName;
        status.heldFor    = realtime::now() - book.heldSince;
    }
    return status;
}

auto letGoIfHolderLeft() -> bool
{
    if (!timer::is_held() || book.holder == 0 || pawn::players::online(book.holder))
    {
        return false;
    }

    return release("its holder went offline") == Result::Ok;
}

} // namespace cardian::pause
