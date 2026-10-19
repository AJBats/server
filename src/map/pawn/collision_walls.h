/*
===========================================================================

  Cardian: the collision walls, built into a zone's navmesh

===========================================================================
*/

#pragma once

// The walls tools/walls cuts from a zone's raw collision by the client's own
// rule (ROADMAP W, "Collision walls"; #535) are the places the navmesh leads a
// route where a running player cannot go. They are built into the zone's
// navmesh as barriers: a thin upright quad stands on each wall's line, from
// under its floor to above a body's height, and Recast rasterizes it as
// unwalkable, so the navmesh parts along the wall and every route goes round
// it. In a walled zone the builder's drop links go one way, down: each spans
// a ledge taller than the navmesh's climb, which a player never climbs. A
// two-way wall's barrier blocks them too; a one-way wall's (a ledge dropped
// off and not climbed back) lets them by, so a drop across it stays where
// the builder links it -- a drop over the navmesh's climb. A one-way wall
// over a lower drop parts the navmesh both ways (#538). The navmesh is the
// zone's, so its mobs keep to the walls and the one-way links too. The
// walker's own crossings of the navmesh's borders, its seams
// (pawn/route_search.h), ask `crosses`, so none of them crosses a wall.
//
// The file is modules/cardian/walls/<zone id>.walls, written by
// tools/walls/walls.py: comment lines start with '#', and every other line is
// one wall, "kind count x y z x y z ...", its points in FFXI's axes (y down).
// A zone with walls in such a file builds its navmesh with them at every
// start and never writes upstream's navmeshes/<zone>.nav, reading it only
// when the build fails. Header-only: the zone and the navmesh builder (map
// core) read it, and xi_test links both.

#include "common/cbasetypes.h"
#include "common/settings.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

namespace cardian::walls
{

constexpr float kBelow = 0.5f; // yalms under a wall's floor its quad reaches, so the floor's span and the quad's are one
constexpr float kAbove = 2.0f; // and over it: a body's height, above the navmesh's climb
constexpr float kCell  = 4.0f; // yalms: the side of the grid a wall's segments are sorted into
constexpr int   kMaxPoints = 100000; // a line claiming more points than this is a broken file's, and skipped

// One barrier triangle: its three corners x, y, z in FFXI's axes, then 1
// for a one-way wall's (0 for a two-way wall's)
using Triangle = std::array<float, 10>;
// One wall's line: its points x, y, z in FFXI's axes
using Line = std::vector<std::array<float, 3>>;

// One wall: its kind (1 two-way, 2 one-way) and its line
struct Wall
{
    int  kind{};
    Line pts;
};

inline auto path(const uint16 zoneId) -> std::string
{
    return "modules/cardian/walls/" + std::to_string(zoneId) + ".walls";
}

// Where a walled navmesh is saved once built, and the walker's seams over it,
// for tools/walls to draw in the development tools; untracked (.cache/), and
// never read back by the server
inline auto builtPath(const uint16 zoneId, const std::string& ext = "nav") -> std::string
{
    std::error_code ec;
    std::filesystem::create_directories(".cache/cardian_navmeshes", ec);
    return ".cache/cardian_navmeshes/" + std::to_string(zoneId) + "." + ext;
}

// Whether the zone's navmesh is built with walls: switched on, and a walls file for it
inline auto exist(const uint16 zoneId) -> bool
{
    std::error_code ec;
    return settings::get<bool>("cardian.COLLISION_WALLS") && std::filesystem::exists(path(zoneId), ec);
}

// The zone's walls; none where the zone has no walls
inline auto lines(const uint16 zoneId) -> std::vector<Wall>
{
    std::vector<Wall> out;
    if (!exist(zoneId))
    {
        return out;
    }

    std::ifstream file(path(zoneId));
    std::string   text;
    while (std::getline(file, text))
    {
        if (text.empty() || text[0] == '#')
        {
            continue;
        }

        std::istringstream in(text);
        int                kind  = 0;
        int                count = 0;
        if (!(in >> kind >> count) || count < 2 || count > kMaxPoints)
        {
            continue;
        }

        Line pts(static_cast<std::size_t>(count));
        bool whole = true;
        for (auto& p : pts)
        {
            whole = whole && static_cast<bool>(in >> p[0] >> p[1] >> p[2]);
        }

        if (whole)
        {
            out.push_back(Wall{ .kind = kind, .pts = std::move(pts) });
        }
    }

    return out;
}

// Whether the zone's navmesh is built with walls: switched on, and its walls
// file holding at least one -- the same test as the walls the config carries
inline auto present(const uint16 zoneId) -> bool
{
    return exist(zoneId) && !lines(zoneId).empty();
}

// The zone's walls as barrier triangles, two for each segment of each wall's
// line; none where the zone has no walls
inline auto load(const uint16 zoneId) -> std::vector<Triangle>
{
    std::vector<Triangle> out;
    for (const auto& [kind, pts] : lines(zoneId))
    {
        const float oneWay = kind == 2 ? 1.0f : 0.0f;
        for (std::size_t i = 0; i + 1 < pts.size(); ++i)
        {
            const auto& a = pts[i];
            const auto& b = pts[i + 1];
            // y is down: the quad's foot is kBelow under each end's floor, its top kAbove over it
            const std::array<float, 3> aLow{ a[0], a[1] + kBelow, a[2] };
            const std::array<float, 3> aTop{ a[0], a[1] - kAbove, a[2] };
            const std::array<float, 3> bLow{ b[0], b[1] + kBelow, b[2] };
            const std::array<float, 3> bTop{ b[0], b[1] - kAbove, b[2] };
            out.push_back({ aLow[0], aLow[1], aLow[2], bLow[0], bLow[1], bLow[2], bTop[0], bTop[1], bTop[2], oneWay });
            out.push_back({ aLow[0], aLow[1], aLow[2], bTop[0], bTop[1], bTop[2], aTop[0], aTop[1], aTop[2], oneWay });
        }
    }

    return out;
}

// The barriers whose box meets [bmin, bmax] (FFXI's axes) added to a tile's
// gathered triangles: a two-way wall's with `blocking`, the builder's
// unwalkable area, which its drop links never cross; a one-way wall's with
// `ledge`, any other: upright, Recast still leaves it unwalkable, but the
// links pass it
inline void append(const std::vector<Triangle>& barriers, const float* bmin, const float* bmax,
                   std::vector<float>& verts, std::vector<int>& indices, std::vector<unsigned char>& areas,
                   const unsigned char blocking, const unsigned char ledge)
{
    for (const auto& t : barriers)
    {
        bool outside = false;
        for (int axis = 0; axis < 3 && !outside; ++axis)
        {
            const float lo = std::min({ t[axis], t[3 + axis], t[6 + axis] });
            const float hi = std::max({ t[axis], t[3 + axis], t[6 + axis] });
            outside        = hi < bmin[axis] || lo > bmax[axis];
        }

        if (outside)
        {
            continue;
        }

        const auto base = static_cast<int>(verts.size() / 3);
        verts.insert(verts.end(), t.begin(), t.begin() + 9);
        indices.push_back(base);
        indices.push_back(base + 1);
        indices.push_back(base + 2);
        areas.push_back(t[9] != 0.0f ? ledge : blocking);
    }
}

// A zone's walls sorted into a grid, to ask whether a straight line crosses
// one: whether it passes through a barrier's quad, between kBelow under the
// wall's floor and kAbove over it
class Index
{
public:
    explicit Index(const std::vector<Wall>& walls)
    {
        for (const auto& [kind, pts] : walls)
        {
            for (std::size_t i = 0; i + 1 < pts.size(); ++i)
            {
                const auto& a = pts[i];
                const auto& b = pts[i + 1];
                const auto  n = static_cast<int>(segments_.size());
                segments_.push_back({ a[0], a[1], a[2], b[0], b[1], b[2] });
                for (int cx = cellOf(std::min(a[0], b[0])); cx <= cellOf(std::max(a[0], b[0])); ++cx)
                {
                    for (int cz = cellOf(std::min(a[2], b[2])); cz <= cellOf(std::max(a[2], b[2])); ++cz)
                    {
                        grid_[key(cx, cz)].push_back(n);
                    }
                }
            }
        }
    }

    auto empty() const -> bool
    {
        return segments_.empty();
    }

    // Whether the line from (ax, ay, az) to (bx, by, bz), FFXI's axes, passes through a wall
    auto crosses(const float ax, const float ay, const float az, const float bx, const float by, const float bz) const -> bool
    {
        for (int cx = cellOf(std::min(ax, bx)); cx <= cellOf(std::max(ax, bx)); ++cx)
        {
            for (int cz = cellOf(std::min(az, bz)); cz <= cellOf(std::max(az, bz)); ++cz)
            {
                const auto it = grid_.find(key(cx, cz));
                if (it == grid_.end())
                {
                    continue;
                }

                for (const int n : it->second)
                {
                    const auto& s = segments_[static_cast<std::size_t>(n)];
                    // where the two meet on the ground: t along the line, u along the wall
                    const float dx = bx - ax;
                    const float dz = bz - az;
                    const float ex = s[3] - s[0];
                    const float ez = s[5] - s[2];
                    const float den = dx * ez - dz * ex;
                    if (std::fabs(den) < 1e-9f)
                    {
                        continue;
                    }

                    const float t = ((s[0] - ax) * ez - (s[2] - az) * ex) / den;
                    const float u = ((s[0] - ax) * dz - (s[2] - az) * dx) / den;
                    if (t < 0.0f || t > 1.0f || u < 0.0f || u > 1.0f)
                    {
                        continue;
                    }

                    const float lineY = ay + (by - ay) * t;
                    const float wallY = s[1] + (s[4] - s[1]) * u;
                    if (lineY >= wallY - kAbove && lineY <= wallY + kBelow) // y is down
                    {
                        return true;
                    }
                }
            }
        }

        return false;
    }

private:
    static auto cellOf(const float v) -> int
    {
        return static_cast<int>(std::floor(v / kCell));
    }

    static auto key(const int cx, const int cz) -> int64
    {
        return (static_cast<int64>(cx) << 32) ^ static_cast<int64>(static_cast<uint32>(cz));
    }

    std::vector<std::array<float, 6>>              segments_;
    std::unordered_map<int64, std::vector<int>>    grid_;
};

// The zone's walls, indexed once and kept; nullptr where the zone has none
inline auto indexFor(const uint16 zoneId) -> const Index*
{
    static std::mutex                                   mutex;
    static std::map<uint16, std::unique_ptr<Index>>     kept;
    const std::lock_guard                               lock(mutex);
    auto&                                               slot = kept[zoneId];
    if (slot == nullptr)
    {
        slot = std::make_unique<Index>(lines(zoneId));
    }

    return slot->empty() ? nullptr : slot.get();
}

} // namespace cardian::walls
