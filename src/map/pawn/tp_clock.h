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

// When a mob's next TP move comes (RESEARCH §12.15, "The TP clock"), for a
// kneeling mage inside its area move: she gets up only when it comes soon,
// and before the party kills it (the user, 2026-10-07: players have a sixth
// sense for the even-match mob the party cleans up anyway). Read off the
// server's own rule, CMobController: a move fires once the mob's TP reaches
// a threshold rolled between 1000 and 3000 at its engage and after every
// move; at 3000 always; at 1000 once the mob is below 25% HP. Which move it
// picks is random, so the one she keeps out of is assumed. Pure, so
// cardian_rest_tests.cpp holds it to account.

#include "common/cbasetypes.h"

#include <algorithm>
#include <limits>
#include <optional>

namespace cardian::tpclock
{
    constexpr double kMinWatch    = 3.0;   // seconds of a climb before its rate is trusted
    constexpr double kAssumedRate = 100.0; // TP a second before then: a fast climb, so an unknown mob is never late
    constexpr double kWindow      = 5.0;   // seconds she needs to stand and walk out of its reach
    constexpr double kGap         = 1.5;   // seconds unwatched after which the climb is started afresh

    // A mob's TP as watched: the climb since its last move (TP falls when a
    // move fires), since it was first seen, or since a gap in the watching --
    // a camp's mob back with the same id, or a fight she left a while -- so
    // a rate never spans what it did not see
    struct Watch
    {
        uint32 mob    = 0;
        double since  = 0.0;
        double lastAt = 0.0;
        uint16 fromTp = 0;
        uint16 lastTp = 0;

        void see(const uint32 id, const uint16 tp, const double now)
        {
            if (id != mob || tp < lastTp || now - lastAt > kGap)
            {
                mob    = id;
                since  = now;
                fromTp = tp;
            }
            lastTp = tp;
            lastAt = now;
        }

        // TP a second over the climb; nothing until it has run kMinWatch
        auto rate(const double now) const -> std::optional<double>
        {
            const double watched = now - since;
            if (watched < kMinWatch)
            {
                return std::nullopt;
            }
            return static_cast<double>(lastTp - fromTp) / watched;
        }
    };

    // The TP its next move fires at
    inline auto firesAt(const uint16 threshold, const uint8 hpp) -> uint16
    {
        return hpp < 25 ? uint16{ 1000 } : std::clamp<uint16>(threshold, 1000, 3000);
    }

    // Seconds until it gets there: none needed once it has, never when it
    // gains no TP, kAssumedRate before its rate is known
    inline auto secondsToMove(const uint16 tp, const uint16 at, const std::optional<double> rate) -> double
    {
        if (tp >= at)
        {
            return 0.0;
        }
        const double perSecond = rate.value_or(kAssumedRate);
        return perSecond > 0.0 ? (at - tp) / perSecond : std::numeric_limits<double>::infinity();
    }

    // She stands for it: it comes within the window, and before the mob dies
    // (lifeLeft negative: unknown, so the move is taken to come first)
    inline auto standsFor(const double toMove, const double lifeLeft, const double window = kWindow) -> bool
    {
        return toMove <= window && (lifeLeft < 0.0 || toMove < lifeLeft);
    }
} // namespace cardian::tpclock
