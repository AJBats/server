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

#include "calendar_store.h"

#include "common/database.h"
#include "common/logging.h"
#include "common/settings.h"
#include "common/timer.h"

namespace cardian::pause::calendar
{
namespace
{

constexpr auto kSaveEvery = std::chrono::minutes(1);

bool                 armed = false;
realtime::time_point lastSave{};

} // namespace

void load()
{
    clock_row::ensureTable();

    bool       failed = false;
    const auto saved  = clock_row::read(&failed);
    if (failed)
    {
        ShowError("calendar: cardian_clock could not be made or read: the game clock starts level with real time, any saved drift is forgotten for this run, and nothing is saved");
        return;
    }

    if (saved)
    {
        const bool runsOffline = settings::get<bool>("cardian.CLOCK_RUNS_OFFLINE");
        const auto drift       = driftAtBoot(*saved, earth_time::now(), runsOffline);

        earth_time::set_calendar_drift(drift);
        ShowInfoFmt("calendar: the game clock runs {:.0f}s behind real time ({})",
                    std::chrono::duration<double>(drift).count(),
                    runsOffline ? "its time went by while the server was off" : "it stood still while the server was off");
    }
    else
    {
        ShowInfo("calendar: no saved game clock yet, starting level with real time");
    }

    armed = true;
    save();
}

void save()
{
    if (!armed)
    {
        return;
    }

    clock_row::write(snapshot());
    lastSave = realtime::now();
}

void saveIfDue()
{
    if (armed && realtime::now() - lastSave >= kSaveEvery)
    {
        save();
    }
}

} // namespace cardian::pause::calendar
