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

#include <cstdint>

// What a venture's cast cost her kit, and whether she fishes on
// (pawn/angler.h), judged from her counts of the trip's rod and bait as the
// cast began and after its release. A lure is a bait the game keeps through
// a catch (it does not stack); the game takes it on a snapped line, and a
// broken rod takes it too (fishingutils' RodBreak). The player's Line snap
// choice says whether a lost lure is replaced or ends the venture; a broken
// rod is replaced by a spare of hers, or ends the venture.
namespace cardian::kitloss
{
    // Her rods and bait of the trip's kinds, as the cast began and after it
    struct Counts
    {
        uint32_t rodsBefore = 0;
        uint32_t rodsAfter  = 0;
        uint32_t baitBefore = 0;
        uint32_t baitAfter  = 0;
    };

    enum class Outcome : uint8_t
    {
        FishOn,          // nothing lost that ends it: she casts again
        LureSnapped,     // her lure lost to a snapped line, and the player chose that this end the venture
        RodAndLure,      // her rod broke and took her lure: the venture ends (the player chose so, or she holds no spare rod)
        RodBroke,        // her rod broke and she holds no spare: the venture ends
        SpareRod,        // her rod broke, and she goes on with a spare
        SpareRodAndLure, // her rod broke and took her lure, and she goes on with a spare rod and another lure
    };

    // `lure`: her bait is a lure; `endOnSnap`: a lost lure ends the venture,
    // the player's choice
    constexpr auto judge(const Counts& c, const bool lure, const bool endOnSnap) -> Outcome
    {
        const bool rodBroke = c.rodsAfter < c.rodsBefore;
        const bool lureLost = lure && c.baitAfter < c.baitBefore;
        const bool spareRod = c.rodsAfter > 0;
        if (rodBroke)
        {
            if (lureLost)
            {
                return endOnSnap || !spareRod ? Outcome::RodAndLure : Outcome::SpareRodAndLure;
            }
            return spareRod ? Outcome::SpareRod : Outcome::RodBroke;
        }
        return lureLost && endOnSnap ? Outcome::LureSnapped : Outcome::FishOn;
    }
} // namespace cardian::kitloss
