// Cardian: the auto rescue's judgement (pawn.AUTO_RESCUE): whether a cardian
// trying to reach the spot her walk sends her to is caught on the zone's
// geometry. Pure arithmetic on seconds and positions, so the rule is tested
// without a map: the controller (CPawnController::RescueTick) supplies the
// clock, where she stands, whether she is trying, and the settings, and sets
// her down at her spot when the answer is yes.
//
// Trying means her walk sent her toward a spot she is farther from than its
// tolerance, within the last second, the danger map not holding her back,
// and she is free to move. Caught means she has tried for `seconds` without
// ever leaving a circle of `radius` round where she stood when she began:
// walking, even round a long detour or after a player who outruns her, she
// leaves it within a second or two; caught on a wall, or stepping back and
// forth beside it, she never does. Her clock stops whenever she stops trying,
// and starts again from where she stands. A rescue she needs again before she
// next walks out of her circle on her own waits twice as long, up to
// 2^maxDoublings times, and one long past (`forgetAfter`) no longer counts:
// a spot she can never stay at is not a teleport every few seconds.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace cardian::rescue
{
    struct Rules
    {
        float   radius       = 4.0f;  // yalms: the circle she must leave to be walking
        double  seconds      = 10.0;  // the first wait
        double  forgetAfter  = 120.0; // seconds: a rescue this long past no longer doubles the wait
        uint8_t maxDoublings = 3;     // the wait at most 2^3 = 8 times the first
    };

    struct Point
    {
        float x = 0.0f;
        float y = 0.0f;
        float z = 0.0f;
    };

    inline auto between(const Point a, const Point b) -> float
    {
        const float dx = a.x - b.x;
        const float dy = a.y - b.y;
        const float dz = a.z - b.z;
        return std::sqrt(dx * dx + dy * dy + dz * dz);
    }

    struct Watch
    {
        bool    running = false; // her clock is running
        double  since   = 0.0;   // seconds: when it began
        Point   from{};          // where she stood then
        uint8_t again   = 0;     // rescues since she last walked out of her circle on her own
        double  lastAt  = 0.0;   // the last rescue
    };

    // The wait before a rescue, with the doublings she has earned
    inline auto waitOf(const Watch& w, const Rules& r) -> double
    {
        return r.seconds * static_cast<double>(1u << std::min(w.again, r.maxDoublings));
    }

    // One look, each tick. True: she is caught, and the caller sets her down
    // at her spot (then calls `rescued`), or, when that spot is where she
    // stands, starts her clock again (`restart`)
    inline auto caught(Watch& w, const double now, const Point at, const bool trying, const Rules& r) -> bool
    {
        if (!trying)
        {
            w.running = false;
            return false;
        }
        if (!w.running || between(at, w.from) > r.radius)
        {
            if (w.running)
            {
                w.again = 0; // out of her circle on her own: she walks
            }
            w.running = true;
            w.since   = now;
            w.from    = at;
            return false;
        }
        if (w.again > 0 && now - w.lastAt > r.forgetAfter)
        {
            w.again = 0;
        }
        return now - w.since >= waitOf(w, r);
    }

    // She was set down at her spot: her clock stops, and the next rescue
    // waits longer until she walks on her own
    inline void rescued(Watch& w, const double now)
    {
        w.running = false;
        w.again   = static_cast<uint8_t>(std::min<int>(w.again + 1, 255));
        w.lastAt  = now;
    }

    // Her spot is where she stands, or nearly: setting her down there helps
    // nothing, so her clock starts again from here
    inline void restart(Watch& w, const double now, const Point at)
    {
        w.running = true;
        w.since   = now;
        w.from    = at;
    }
} // namespace cardian::rescue
