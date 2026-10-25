// Cardian: a town walk that reads as a person's, not the navmesh's (ROADMAP
// W, "Natural town walks"; RESEARCH §26), its rules as plain functions. The
// mesh's route is the same for everyone and turns hard at every corner, so a
// crowd walked it in lockstep down one line. Each body walks it her own
// way: on her side of the street at her own distance from its middle, her
// line wandering a little either side of that over the length of a street,
// and every corner taken as a curve; she sets out after a beat of her own
// and keeps a pace of her own. All of it is decided once, when the walk is
// planned: nothing is corrected tick by tick, which the client would draw as
// a kink every 0.4 s.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <numbers>
#include <optional>
#include <vector>

namespace cardian::townwalk
{
    struct Pt
    {
        float x = 0.0f;
        float y = 0.0f;
        float z = 0.0f;
    };

    // How one body walks, fixed for her: from her name (world.cpp
    // walkMannerOf), so she walks the same way every time she is seen
    struct Style
    {
        float right      = 0.0f;  // yalms to the right of the way she walks (negative: to its left)
        float drift      = 0.0f;  // how far her line wanders either side of that
        float wavelength = 16.0f; // yalms along the way for one wander
        float phase      = 0.0f;  // where in the wander she starts, 0 to 1
        float spacing    = 0.0f;  // a long stretch gets a point at least this often to wander through (0: none)
        float round      = 0.0f;  // a corner's curve starts at most this far before it (0: corners kept)
        float minTurn    = 0.35f; // radians: a turn this gentle is left as it is
    };

    inline auto flat(const Pt& a, const Pt& b) -> float
    {
        return std::hypot(b.x - a.x, b.z - a.z);
    }

    inline auto lerp(const Pt& a, const Pt& b, const float t) -> Pt
    {
        return Pt{ a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t };
    }

    // The angle the walk turns through at `b`, coming from `a` and going on
    // to `c`, 0 to pi; 0 where either leg has no length
    inline auto turnAt(const Pt& a, const Pt& b, const Pt& c) -> float
    {
        const float ax = b.x - a.x, az = b.z - a.z, bx = c.x - b.x, bz = c.z - b.z;
        const float la = std::hypot(ax, az), lb = std::hypot(bx, bz);
        if (la < 1e-4f || lb < 1e-4f)
        {
            return 0.0f;
        }
        const float cosine = std::clamp((ax * bx + az * bz) / (la * lb), -1.0f, 1.0f);
        return std::acos(cosine);
    }

    // A long stretch given points every `spacing` yalms, so her line has
    // somewhere to wander on it; heights between two points are the line's,
    // put on the ground by the caller
    inline auto densify(const std::vector<Pt>& route, const float spacing) -> std::vector<Pt>
    {
        if (spacing <= 0.0f || route.size() < 2)
        {
            return route;
        }
        std::vector<Pt> out;
        out.push_back(route.front());
        for (size_t i = 1; i < route.size(); ++i)
        {
            const float len    = flat(route[i - 1], route[i]);
            const auto  pieces = static_cast<int>(std::ceil(len / spacing));
            for (int k = 1; k < pieces; ++k)
            {
                out.push_back(lerp(route[i - 1], route[i], static_cast<float>(k) / static_cast<float>(pieces)));
            }
            out.push_back(route[i]);
        }
        return out;
    }

    // Her sideways offset at `along` yalms into the walk: her side's
    // distance and her wander about it
    inline auto offsetAt(const Style& style, const float along) -> float
    {
        if (style.drift <= 0.0f || style.wavelength <= 0.0f)
        {
            return style.right;
        }
        return style.right + style.drift * std::sin(2.0f * std::numbers::pi_v<float> * (along / style.wavelength + style.phase));
    }

    // Her walk along the route: its inner points slid to her side and her
    // wander, its long stretches given points to wander through, and each
    // corner rounded into a curve of points. `onGround(p)` puts a point on the
    // ground she walks, or nullopt off it; `straight(a, b)` says whether she
    // walks straight from a to b on it. A point or a curve that fails either
    // is left as the route had it, and the first and last points never move:
    // where she stands, and where she is going
    template <class OnGround, class Straight>
    auto shape(const std::vector<Pt>& route, const Style& style, OnGround onGround, Straight straight) -> std::vector<Pt>
    {
        if (route.size() < 3 && style.spacing <= 0.0f)
        {
            return route;
        }
        constexpr float kSnapSlack = 0.6f; // a point the ground moves further than this is no longer hers

        // Her side and her wander, measured along the route as it was; a walk
        // with neither keeps the route's points as they are, unasked
        const std::vector<Pt> dense  = densify(route, style.spacing);
        const bool            slides = style.right != 0.0f || style.drift > 0.0f;
        std::vector<Pt>       slid;
        slid.reserve(dense.size());
        slid.push_back(dense.front());
        float along = 0.0f;
        for (size_t i = 1; i + 1 < dense.size(); ++i)
        {
            if (!slides)
            {
                slid.push_back(dense[i]);
                continue;
            }
            along += flat(dense[i - 1], dense[i]);
            const Pt&   prev = dense[i - 1];
            const Pt&   next = dense[i + 1];
            const float dx   = next.x - prev.x;
            const float dz   = next.z - prev.z;
            const float len  = std::hypot(dx, dz);
            Pt          kept = dense[i];
            if (len > 0.01f)
            {
                // the right of a heading (dx, dz), x east and z north, is (dz, -dx)
                const float offset = offsetAt(style, along);
                for (const float share : { 1.0f, 0.5f })
                {
                    Pt want = dense[i];
                    want.x += dz / len * offset * share;
                    want.z += -dx / len * offset * share;
                    if (const auto ground = onGround(want); ground.has_value() && flat(*ground, want) < kSnapSlack && straight(slid.back(), *ground) &&
                                                            straight(*ground, next))
                    {
                        kept = *ground;
                        break;
                    }
                }
            }
            slid.push_back(kept);
        }
        slid.push_back(dense.back());

        if (style.round <= 0.0f)
        {
            return slid;
        }

        // Every corner a curve: a quadratic Bezier from a point before it to
        // a point after it, the corner its control, met already turning
        std::vector<Pt> out;
        out.reserve(slid.size() * 2);
        out.push_back(slid.front());
        for (size_t i = 1; i + 1 < slid.size(); ++i)
        {
            const Pt&   a     = out.back();
            const Pt&   b     = slid[i];
            const Pt&   c     = slid[i + 1];
            const float turn  = turnAt(a, b, c);
            const float ab    = flat(a, b);
            const float bc    = flat(b, c);
            const float reach = std::min({ style.round, 0.45f * ab, 0.45f * bc });
            if (turn < style.minTurn || reach < 0.3f)
            {
                out.push_back(b);
                continue;
            }
            const Pt         p0    = lerp(b, a, reach / ab);
            const Pt         p2    = lerp(b, c, reach / bc);
            const int        steps = turn > 1.2f ? 4 : turn > 0.7f ? 3 : 2;
            std::vector<Pt>  curve;
            bool             fits  = true;
            std::optional<Pt> last = a;
            for (int k = 0; k <= steps && fits; ++k)
            {
                const float t   = static_cast<float>(k) / static_cast<float>(steps);
                const float u   = 1.0f - t;
                const Pt    raw = Pt{ u * u * p0.x + 2.0f * u * t * b.x + t * t * p2.x, u * u * p0.y + 2.0f * u * t * b.y + t * t * p2.y,
                                   u * u * p0.z + 2.0f * u * t * b.z + t * t * p2.z };
                const auto  ground = onGround(raw);
                fits               = ground.has_value() && flat(*ground, raw) < kSnapSlack && straight(*last, *ground);
                if (fits)
                {
                    curve.push_back(*ground);
                    last = *ground;
                }
            }
            if (fits && straight(*last, c))
            {
                out.insert(out.end(), curve.begin(), curve.end());
            }
            else
            {
                out.push_back(b);
            }
        }
        out.push_back(slid.back());
        return out;
    }

    // ---- her timing --------------------------------------------------------------

    // A fraction 0 to 1 from a name's hash and a salt, so each of a body's
    // traits is her own and none follows another
    inline auto fraction(const uint32_t hash, const uint32_t salt) -> float
    {
        uint32_t h = hash ^ (salt * 2654435761u);
        h ^= h >> 15;
        h *= 2246822519u;
        h ^= h >> 13;
        return static_cast<float>(h % 10000u) / 10000.0f;
    }

    // The beat she waits before she sets out on a leg, from a roll 0 to 1:
    // mostly short, now and then long (a long tail, as people's are)
    inline auto startBeat(const float roll, const float minSeconds, const float maxSeconds) -> float
    {
        const float r = std::clamp(roll, 0.0f, 1.0f);
        return minSeconds + (maxSeconds - minSeconds) * r * r * r;
    }
} // namespace cardian::townwalk
