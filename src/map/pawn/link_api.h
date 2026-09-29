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

// The cardian API on the Cardian Link: the companion addon's messages about
// cardians, answered from the pawn module. The transport (cardian_link.h)
// carries them; this file turns each into the game's own calls and the game's
// state back into messages. Two of them are answered from Lua, where the
// game's own tables are: the party finder's goals and the conquest exchange
// (modules/cardian/lua/finder_goals.lua and conquest_exchange.lua).

namespace pawn::linkapi
{
    // Hands every handler of this file to the transport; the pawn module's init
    void registerHandlers();

    // Loads the Lua libraries the handlers ask; the pawn module's init
    void loadLibraries();
} // namespace pawn::linkapi
