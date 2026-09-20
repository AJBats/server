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

#include "common/logging.h"
#include "input_gate.h"
#include "pawn/players.h"

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

} // namespace

auto hold(const uint32 holderCharId, const std::string_view holderName) -> Result
{
    if (timer::is_held())
    {
        return Result::AlreadyHeld;
    }

    timer::hold();

    book.holder     = holderCharId;
    book.holderName = holderName;
    book.heldSince  = realtime::now();
    ++book.holds;

    ShowInfoFmt("pause: held by {} ({})", book.holderName, book.holder);
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

    ShowInfoFmt("pause: released after {:.1f}s, {} (held by {})", std::chrono::duration<double>(heldFor).count(), why, book.holderName);

    book.holder = 0;
    book.holderName.clear();

    // Last, with the clock running and nothing held: each goes to the game's own
    // handler as if it had just arrived.
    input::replay();
    return Result::Ok;
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
