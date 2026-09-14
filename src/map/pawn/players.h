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

class MapSessionContainer;

// The map's real players, as the pawn code asks after them. A character's
// body is destroyed at every zone line and loaded again when the client
// lands, seconds later; the session under his charid lives from login to
// logout (map_networking.cpp finds it by client address at the re-login
// and loads the character into it). "Is he online" is a question about
// the session, so it never blinks with the body. Core rather than the
// pawn module: map_engine hands the container over at startup, and
// xi_test links it without a stub.
namespace pawn::players
{
    void attach(MapSessionContainer& sessions);

    // A real player is online on this map: a session under his charid, or
    // a body in some zone. The body clause is possession, where his body
    // stays behind as a cardian while the session answers to another
    // charid (possess.cpp); a dead client has neither once its session
    // times out
    auto online(uint32 charid) -> bool;
} // namespace pawn::players
