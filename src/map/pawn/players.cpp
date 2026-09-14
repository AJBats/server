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

#include "players.h"

#include "map_session_container.h"
#include "utils/zoneutils.h"

namespace pawn::players
{
    namespace
    {
        MapSessionContainer* sessions = nullptr;
    }

    void attach(MapSessionContainer& container)
    {
        sessions = &container;
    }

    auto online(const uint32 charid) -> bool
    {
        if (charid == 0)
        {
            return false;
        }
        if (sessions != nullptr && sessions->getSessionByCharId(charid) != nullptr)
        {
            return true;
        }
        return zoneutils::GetChar(charid) != nullptr;
    }
} // namespace pawn::players
