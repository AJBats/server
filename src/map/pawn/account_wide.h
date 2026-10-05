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

class CCharEntity;

// A player's major progression is his account's (account_wide.cpp; ROADMAP N)
namespace pawn::accountwide
{
    // The addon's Shared maps (the Link's PREFS): on, the maps any character
    // of his account holds are shown to him too; off, only his own. Kept for
    // the map's life, on until his addon says otherwise; the key item tables
    // are shown to him again when it changes
    void setSharedMaps(CCharEntity* PChar, bool on);
} // namespace pawn::accountwide
