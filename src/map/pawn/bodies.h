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

#include "common/cbasetypes.h"

#include <unordered_map>

class CBaseEntity;

// The characters the map holds in memory in no zone: a fishing venture's
// body (angler.h), a faded member loaded a moment to sell her catch
// (venture_sales.h). Upstream's EntityId finds a character again through the
// zone it stands in, and an item transaction releases what it claimed by
// finding its owner that way; a character in no zone is found here instead,
// by its id (one marked fallback in entities/entity_id.cpp), so the server's
// own transactions work on these as on anyone. A body is held for as long as
// it lives, and let go before it is destroyed. Header-only: xi_test links
// entity_id.cpp without the pawn module, and needs no stub
namespace pawn::bodies
{
    // Leaked on purpose, as the pawn container: nothing reads it at exit
    inline auto held() -> std::unordered_map<uint32, CBaseEntity*>&
    {
        static auto* list = new std::unordered_map<uint32, CBaseEntity*>();
        return *list;
    }

    inline void hold(const uint32 charid, CBaseEntity* PBody)
    {
        held()[charid] = PBody;
    }

    inline void letGo(const uint32 charid)
    {
        held().erase(charid);
    }

    inline auto find(const uint32 charid) -> CBaseEntity*
    {
        const auto it = held().find(charid);
        return it != held().end() ? it->second : nullptr;
    }
} // namespace pawn::bodies
