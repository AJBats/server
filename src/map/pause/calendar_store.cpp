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

#include <algorithm>
#include <string>

namespace cardian::pause::calendar
{
namespace
{

constexpr auto kSaveEvery = std::chrono::minutes(1);

bool                 armed = false;
realtime::time_point lastSave{};

auto toMs(const earth_time::time_point tp) -> int64
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(tp.time_since_epoch()).count();
}

} // namespace

auto snapshot() -> Saved
{
    return Saved{ toMs(earth_time::now()), toMs(earth_time::game_now()) };
}

auto driftAtBoot(const Saved& saved, const earth_time::time_point realNow, const bool runsOffline) -> earth_time::duration
{
    const auto atWrite = std::chrono::milliseconds(saved.realMs - saved.gameMs);
    const auto off     = std::max<int64>(0, toMs(realNow) - saved.realMs);
    return atWrite + std::chrono::milliseconds(runsOffline ? 0 : off);
}

void load()
{
    const auto rset = db::preparedStmt("SELECT real_ms, game_ms FROM cardian_clock WHERE id = 1");
    if (!rset)
    {
        ShowWarning("calendar: no cardian_clock table (apply modules/cardian/sql/cardian_clock.sql): the game clock starts level with real time and is not saved");
        return;
    }

    if (rset->next())
    {
        const Saved saved{ rset->get<int64>("real_ms"), rset->get<int64>("game_ms") };
        const bool  runsOffline = settings::get<bool>("cardian.CLOCK_RUNS_OFFLINE");
        const auto  drift       = driftAtBoot(saved, earth_time::now(), runsOffline);

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

    // As text: the statement binder carries no 64-bit integer, and the server converts exactly
    const auto now = snapshot();
    db::preparedStmt("REPLACE INTO cardian_clock (id, real_ms, game_ms) VALUES (1, ?, ?)", std::to_string(now.realMs), std::to_string(now.gameMs));
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
