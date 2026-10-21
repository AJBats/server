/*
===========================================================================

  Cardian: the client's own collision for the local player

===========================================================================
*/

#pragma once

// The game client's collision for the player it moves, re-implemented from
// FFXiMain.dll (the 2026-10-15 dump, image base 0x049E0000), line for line
// from tools/walls/clientsim.py, which was checked against La Theine's client
// DAT. A body is a sphere of radius 0.5 centred 0.5 above the feet; a move is
// split into pieces of at most 4 yalms, each marched along in steps of 0.15
// yalm against the triangles near it and settled in up to four passes; a
// triangle whose normal's y is above 0.708 either way is a floor (or a
// ceiling), any other a wall, which counts only with a corner above the
// sphere's centre. A frame walks, then falls.
//
//   moveCollide()  +0x16E7E0, the 4-yalm splitter, into substep(), +0x16E930
//   gather()       +0x16DFD0: the triangles whose box in the move's frame meets the swept box
//   respond()      +0x16F0E0: along the move in 0.15-yalm steps, then settled
//   test()         +0x16F320: one triangle against the sphere
//   Player::frame  CXiCollisionActor slot 0x20, +0xA4ED0: pass 1 walks, pass 2 falls
//
// FFXI's axes: y is DOWN (a higher place has a smaller y). Positions are the
// feet. Each triangle carries its corners in the client's order and the
// normal it stores, the winding's negative. Pure and header-only: the
// navmesh builder judges the porter's navmesh by it (client_joins.h), and
// xi_test pins it.

#include <algorithm>
#include <array>
#include <cmath>
#include <optional>
#include <vector>

namespace cardian::client::collision
{

using Vec = std::array<double, 3>;

constexpr double kR2         = 0.25;       // r^2, r = 0.5
constexpr double kCentre     = 0.5;        // the centre above the feet
constexpr double kRadii[2]   = { 0.5, 0.5 };
constexpr double kMarch      = 0.3;        // the march step, 0.3 * 0.5
constexpr double kFloorNy    = 0.708;      // |n.y| above this: a floor or a ceiling, else a wall
constexpr double kWallAbove  = 0.002;      // a wall counts only with a corner this far above the centre
constexpr double kFloorBelow = 0.3;        // a floor counts only with a corner lower than 0.3 above the centre
constexpr double kWallY      = 0.2;        // a wall's push moves y by a fifth
constexpr double kEpsTri     = 1e-4;
constexpr double kGrav       = 0.0408333;  // the fall's increase a frame, per frame-time unit
constexpr double kFallMax    = 1.0;
constexpr double kFallFirst  = 0.4;
constexpr double kBig        = 9999999.0;
constexpr int    kGatherCap  = 0x3FC;

struct Tri
{
    Vec v[3]; // corners, FFXI axes, the client's order
    Vec n;    // the normal the client stores, unit length
};

inline auto sub(const Vec& a, const Vec& b) -> Vec
{
    return { a[0] - b[0], a[1] - b[1], a[2] - b[2] };
}
inline auto add(const Vec& a, const Vec& b) -> Vec
{
    return { a[0] + b[0], a[1] + b[1], a[2] + b[2] };
}
inline auto mul(const Vec& a, const double s) -> Vec
{
    return { a[0] * s, a[1] * s, a[2] * s };
}
inline auto dot(const Vec& a, const Vec& b) -> double
{
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}
inline auto cross(const Vec& a, const Vec& b) -> Vec
{
    return { a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0] };
}
inline auto norm(const Vec& a) -> Vec
{
    return mul(a, 1.0 / std::sqrt(dot(a, a)));
}

// A triangle from three corners in the client's order, its normal the
// winding's negative, as the client stores it; false for a degenerate one
inline auto makeTri(const Vec& a, const Vec& b, const Vec& c, Tri& out) -> bool
{
    const Vec    n  = mul(cross(sub(b, a), sub(c, a)), -1.0);
    const double ln = std::sqrt(dot(n, n));
    if (!(ln > 0.0))
    {
        return false;
    }
    out = Tri{ { a, b, c }, mul(n, 1.0 / ln) };
    return true;
}

namespace detail
{
    struct Rec
    {
        const Tri* t;
        double     nz;
    };

    struct State
    {
        Vec    res{};
        bool   marching = false;
        bool   blocked  = false; // +0x34125
        bool   yfixed   = false; // +0x34126
        bool   grounded = false; // +0x34128
        double bestW    = kBig;  // +0x34130
        double bestF    = kBig;  // +0x34134
    };

    // +0x16F6C0: (e x w)_i * n_i <= eps, component by component
    inline auto inTri(const Vec& a, const Vec& b, const Vec& c, const Vec& q, const Vec& n) -> bool
    {
        const Vec* corners[3][2] = { { &a, &b }, { &b, &c }, { &c, &a } };
        for (const auto& pair : corners)
        {
            const Vec e = sub(*pair[1], *pair[0]);
            const Vec w = sub(q, *pair[0]);
            if ((w[2] * e[1] - w[1] * e[2]) * n[0] > kEpsTri)
            {
                return false;
            }
            if ((w[0] * e[2] - e[0] * w[2]) * n[1] > kEpsTri)
            {
                return false;
            }
            if ((e[0] * w[1] - w[0] * e[1]) * n[2] > kEpsTri)
            {
                return false;
            }
        }
        return true;
    }

    // +0x16F910
    inline auto edge(const Vec& a, const Vec& b, const Vec& p) -> bool
    {
        const Vec    w  = sub(p, a);
        const Vec    e  = sub(b, a);
        const double le = std::sqrt(dot(e, e));
        if (le == 0.0)
        {
            return false;
        }
        const double proj = dot(w, e) / le;
        if (proj < 0.0 || le < proj)
        {
            return false;
        }
        return kR2 - (dot(w, w) - proj * proj) >= 0.0;
    }

    // +0x16F320
    inline void test(State& st, const Rec& rec, const Vec& p)
    {
        if (st.marching && rec.nz > 0.0)
        {
            return;
        }
        const Vec&   n  = rec.t->n;
        const double nn = dot(n, n);
        if (nn == 0.0)
        {
            return;
        }
        const Vec& v0 = rec.t->v[0];
        const Vec& v1 = rec.t->v[1];
        const Vec& v2 = rec.t->v[2];
        const double t  = dot(sub(v0, p), n) / nn;
        const Vec    q  = add(p, mul(n, t));
        if (!(inTri(v0, v1, v2, q, n) || edge(v0, v1, p) || edge(v1, v2, p) || edge(v2, v0, p)))
        {
            return;
        }
        const Vec    d  = sub(q, p);
        const double d2 = dot(d, d);
        if (!(d2 <= kR2 - 0.001))
        {
            return;
        }
        if (n[1] > kFloorNy || n[1] < -kFloorNy)
        {
            if (!(d2 < st.bestF))
            {
                return;
            }
            const double thr = p[1] - kFloorBelow;
            if (!(v0[1] > thr || v1[1] > thr || v2[1] > thr))
            {
                return;
            }
            if (n[1] < 0.0)
            {
                st.res[1]   = q[1] - kCentre;
                st.grounded = true;
                st.yfixed   = true;
            }
            st.bestF = d2;
        }
        else
        {
            if (!(d2 < st.bestW))
            {
                return;
            }
            const double thr = p[1] - kWallAbove;
            if (!(v0[1] < thr || v1[1] < thr || v2[1] < thr))
            {
                return;
            }
            Vec          u   = sub(p, q);
            const double ul2 = dot(u, u);
            if (ul2 == 0.0)
            {
                return;
            }
            u = mul(u, 1.0 / std::sqrt(ul2));
            if (!(dot(u, n) >= 0.0))
            {
                return;
            }
            const double push = std::sqrt(kR2 - d2);
            st.res[0]         = u[0] * push + p[0];
            st.res[1]         = u[1] * push * kWallY + p[1];
            st.res[2]         = u[2] * push + p[2];
            st.blocked        = true;
            st.bestW          = d2;
        }
    }

    // +0x16DFD0
    inline auto gather(const std::vector<Tri>& tris, const Vec& s, const Vec& a, const Vec& b, const Vec& c, const double L) -> std::vector<Rec>
    {
        std::vector<Rec> out;
        for (const auto& t : tris)
        {
            double mn[3] = { kBig, kBig, kBig };
            double mx[3] = { -kBig, -kBig, -kBig };
            for (const auto& v : t.v)
            {
                const Vec    r     = sub(v, s);
                const double loc[] = { dot(r, a), dot(r, b), dot(r, c) };
                for (int i = 0; i < 3; ++i)
                {
                    mn[i] = std::min(mn[i], loc[i]);
                    mx[i] = std::max(mx[i], loc[i]);
                }
            }
            const double cx = (mx[0] + mn[0]) * 0.5, cy = (mx[1] + mn[1]) * 0.5, cz = (mx[2] + mn[2]) * 0.5;
            const double hx = (mx[0] - mn[0]) * 0.5, hy = (mx[1] - mn[1]) * 0.5, hz = (mx[2] - mn[2]) * 0.5;
            if (cx + hx + kRadii[0] > 0.0 && cy + hy + kRadii[1] > 0.0 && cz + hz + kRadii[0] > 0.0 && hx + kRadii[0] - cx > 0.0 &&
                hy + kRadii[1] - cy > 0.0 && hz + L + kRadii[0] - cz > 0.0)
            {
                out.push_back(Rec{ &t, dot(t.n, c) });
                if (static_cast<int>(out.size()) >= kGatherCap)
                {
                    break;
                }
            }
        }
        return out;
    }

    // +0x16F0E0
    inline auto respond(State& st, const std::vector<Rec>& recs, const Vec& s, const Vec& c, const double L) -> bool
    {
        const double step = kRadii[0] * kMarch;
        double       t    = 0.0;
        st.marching       = true;
        st.blocked        = false;
        st.yfixed         = false;
        while (true)
        {
            const Vec p = add(s, mul(c, t));
            st.bestW    = kBig;
            st.bestF    = kBig;
            st.res[0]   = p[0];
            if (!st.yfixed)
            {
                st.res[1] = p[1];
            }
            st.res[2] = p[2];
            for (const auto& r : recs)
            {
                test(st, r, p);
            }
            if (L <= t || st.blocked)
            {
                break;
            }
            t = std::min(t + step, L);
        }
        st.marching = false;
        int k       = 0;
        while (true)
        {
            st.blocked  = false;
            st.yfixed   = false;
            const Vec p = st.res;
            st.bestW    = kBig;
            st.bestF    = kBig;
            if (recs.empty())
            {
                break;
            }
            for (const auto& r : recs)
            {
                test(st, r, p);
            }
            if (!st.blocked && !st.yfixed)
            {
                break;
            }
            if (k > 2)
            {
                return false;
            }
            ++k;
        }
        return true;
    }
} // namespace detail

struct Moved
{
    bool grounded = false;
    Vec  feet{};
    bool settled = false;
};

// +0x16E930: one piece of a move, from `start` to `end` (feet)
inline auto substep(const std::vector<Tri>& tris, const Vec& start, const Vec& end) -> Moved
{
    const Vec s{ start[0], start[1] - kCentre, start[2] };
    Vec       e{ end[0], end[1] - kCentre, end[2] };
    Vec       up{ 0.0, -1.0, 0.0 };
    if (s[1] == e[1])
    {
        e[1] += 0.001;
    }
    if (s[0] == e[0] && s[2] == e[2])
    {
        up = { 0.0, 0.0, 1.0 };
    }
    const Vec    v = sub(e, s);
    const double L = std::sqrt(dot(v, v));
    const Vec    a = norm(cross(up, v));
    const Vec    c = mul(v, 1.0 / L);
    const Vec    b = cross(c, a);
    detail::State st;
    const auto    recs = detail::gather(tris, s, a, b, c, L);
    const bool    ok   = detail::respond(st, recs, s, c, L);
    return Moved{ st.grounded, ok ? Vec{ st.res[0], st.res[1] + kCentre, st.res[2] } : start, ok };
}

// +0x16E7E0: a move split into pieces of at most 4 yalms across
inline auto moveCollide(const std::vector<Tri>& tris, const Vec& start, const Vec& end) -> Moved
{
    const double dx = end[0] - start[0];
    const double dz = end[2] - start[2];
    const double d  = std::sqrt(dx * dx + dz * dz);
    const int    n  = 1 - static_cast<int>(d * -0.25);
    if (n < 2)
    {
        return substep(tris, start, end);
    }
    const Vec stepv = mul(sub(end, start), 1.0 / n);
    Vec       cur   = start;
    double    acc   = 0.0;
    while (true)
    {
        const Vec nxt = add(cur, stepv);
        acc += 1.0 / n;
        if (acc >= 1.0)
        {
            return substep(tris, cur, end);
        }
        const auto m = substep(tris, cur, nxt);
        if (m.grounded)
        {
            return m;
        }
        cur = nxt;
    }
}

// The local player: +0xA4ED0, pass 1 walks, pass 2 falls
struct Player
{
    Vec    pos{};
    double fall     = 0.0;
    bool   grounded = true;
    bool   first    = false;

    void frame(const std::vector<Tri>& tris, const double mx, const double mz, const double dtf = 1.0)
    {
        if (grounded)
        {
            fall = 0.0;
        }
        const Vec old = pos;
        const Vec desired{ old[0] + mx, old[1], old[2] + mz };
        const auto m1 = moveCollide(tris, old, desired);
        grounded      = m1.grounded;
        const Vec nw  = m1.feet;
        double    g   = dtf * kGrav;
        const double D = std::sqrt((nw[0] - old[0]) * (nw[0] - old[0]) + (nw[2] - old[2]) * (nw[2] - old[2]));
        if (0.01 < D && D < g)
        {
            g = D;
        }
        fall = std::min(fall + g, kFallMax);
        if (!first)
        {
            first = true;
            fall  = std::min(fall, kFallFirst);
        }
        const Vec end2{ nw[0], nw[1] + fall, nw[2] };
        const auto m2 = moveCollide(tris, nw, end2);
        grounded      = m2.grounded;
        pos           = m2.feet;
    }
};

// Where a walker set down at `from` (feet, a little over its floor) comes
// to rest: fallen onto the floor below him and pushed out of any wall he
// overlaps, as the client sets a body down; none when he finds no floor
inline auto settle(const std::vector<Tri>& tris, const Vec& from, const double fps = 30.0) -> std::optional<Vec>
{
    Player p;
    p.pos = from;
    for (int i = 0; i < 12; ++i)
    {
        p.frame(tris, 0.0, 0.0, 60.0 / fps);
        if (p.grounded)
        {
            return p.pos;
        }
    }
    return std::nullopt;
}

// Whether a walker set down at `from` (feet, a little over its floor) and
// pressed toward `to` at walking speed gets there: within `reach` of it on the
// ground and on its floor, within `level` in height (a navmesh cell's floor
// is the top of its column, up to a cell's rise on a slope over the ground at
// its middle) -- the client's own judgement of one short step across a
// navmesh's cells. He is let settle first; one who finds no floor, or stops
// getting anywhere, fails. He is pressed on until within `press` of it
// (`reach` when none is given: a drop's walker is pressed nearer than its
// landing is judged, or he stops on the lip above it), then given a frame to
// land, or up to `landFrames` while he falls (a drop off a ledge)
inline auto walks(const std::vector<Tri>& tris, const Vec& from, const Vec& to, const double reach = 0.1, const double level = 0.6,
                  const double speed = 5.0, const double fps = 30.0, const int landFrames = 1, const double press = -1.0) -> bool
{
    const double pressTo = press < 0.0 ? reach : press;
    const double dtf = 60.0 / fps;
    const double mv  = speed / fps;
    Player       p;
    p.pos = from;
    for (int i = 0; i < 12; ++i)
    {
        p.frame(tris, 0.0, 0.0, dtf);
        if (p.grounded)
        {
            break;
        }
    }
    if (!p.grounded)
    {
        return false;
    }
    const double dx = to[0] - p.pos[0];
    const double dz = to[2] - p.pos[2];
    const double L  = std::sqrt(dx * dx + dz * dz);
    if (L < reach)
    {
        return std::fabs(p.pos[1] - to[1]) <= level;
    }
    const double ux = dx / L;
    const double uz = dz / L;
    double       best = L;
    int          still = 0;
    for (int i = 0; i < static_cast<int>(std::ceil(L / mv)) + 6; ++i)
    {
        const double rx = to[0] - p.pos[0];
        const double rz = to[2] - p.pos[2];
        const double left = std::sqrt(rx * rx + rz * rz);
        if (left <= pressTo)
        {
            break;
        }
        const double step = std::min(mv, left);
        p.frame(tris, ux * step, uz * step, dtf);
        const double now = std::sqrt((to[0] - p.pos[0]) * (to[0] - p.pos[0]) + (to[2] - p.pos[2]) * (to[2] - p.pos[2]));
        still            = now < best - 0.01 ? 0 : still + 1;
        best             = std::min(best, now);
        if (still > 3)
        {
            return false;
        }
    }
    // a frame more to land on the floor there, or more while he falls
    for (int i = 0; i < std::max(1, landFrames); ++i)
    {
        p.frame(tris, 0.0, 0.0, dtf);
        if (p.grounded)
        {
            break;
        }
    }
    const double gx = to[0] - p.pos[0];
    const double gz = to[2] - p.pos[2];
    return std::sqrt(gx * gx + gz * gz) <= reach + 0.05 && std::fabs(p.pos[1] - to[1]) <= level;
}

} // namespace cardian::client::collision
