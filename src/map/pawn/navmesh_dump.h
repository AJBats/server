/*
===========================================================================

  Cardian: a navmesh build's stages, dumped round one point

===========================================================================
*/

#pragma once

// A development tool for the navmesh built by the client's rule
// (client_navmesh.h): why the build parts ground the client walks (#561).
// With CARDIAN_NAVMESH_DUMP="x z" (FFXI's axes) in the map's environment --
// ./dev-servers.ps1 porter-navmeshes <zone> --dump x z -- each tile whose
// cells hold that point (up to four: the tiles' borders overlap) writes its
// compact heightfield as it stands after each of the build's steps that can
// part the ground, into log/navmesh_dump/<zone>/<step>_<tile>.txt:
//
//   compact   Recast's cells and their joins, as Recast built them: a cell
//             with no floor here was never walkable (its slope, or the
//             filters of the heightfield before it)
//   joins     after the client's walker judged the joins (client_joins.h),
//             with each join it cut and why in cuts_<tile>.txt
//   body      after the body's margin was eroded from every edge
//   berth     after the berth past it was marked tight
//   open      after the open ground too thin or too small to stand as a
//             region went into the tight strip (keepOpenWhole)
//   regions   Recast's regions, from which its polygons are made (regions)
//   contours  the outlines Recast traced round them (contours): a region
//             with none made no polygon, and its ground is a hole
//
// The last two are written by the navmesh builder itself, a marked call
// after each of those steps (navmesh/navmesh_builder.cpp).
//
// A step's file: a line `tile <width> <height> <cs> <ch> <bmin x y z>` (the
// heightfield's, Detour's space), then a line a floor: its cell's column and
// row, its height in FFXI's y, its area (0 none, 63 open ground, 62 the
// tight strip) and its four joins, 1 joined or 0 not, in Recast's order
// (-x, +z, +x, -z in Detour's space). tools/walls/navdump.py reads them.
// Header-only, as the build's other client pieces are.

#include "common/logging.h"

#include <Recast.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <utility>

namespace cardian::client::dump
{

// The point whose tiles are dumped, FFXI's x and z, read once
inline auto point() -> std::optional<std::pair<float, float>>
{
    static const auto at = []() -> std::optional<std::pair<float, float>>
    {
        const char* text = std::getenv("CARDIAN_NAVMESH_DUMP");
        float       x    = 0.0f;
        float       z    = 0.0f;
        if (text == nullptr || std::sscanf(text, "%f %f", &x, &z) != 2)
        {
            return std::nullopt;
        }
        return std::make_pair(x, z);
    }();
    return at;
}

// Whether `chf`'s cells, its border included, hold the point
inline auto holds(const rcCompactHeightfield& chf) -> bool
{
    const auto at = point();
    if (!at.has_value())
    {
        return false;
    }
    const float x = at->first;
    const float z = -at->second; // Detour's z
    return x >= chf.bmin[0] && x < chf.bmin[0] + chf.width * chf.cs && z >= chf.bmin[2] && z < chf.bmin[2] + chf.height * chf.cs;
}

// The tile's name in a file's: its corner, in whole yalms of Detour's space
inline auto tileName(const rcCompactHeightfield& chf) -> std::string
{
    return std::to_string(static_cast<int>(std::lround(chf.bmin[0]))) + "_" + std::to_string(static_cast<int>(std::lround(chf.bmin[2])));
}

// A step's file, opened for writing in the run's folder:
// CARDIAN_NAVMESH_DUMP_DIR (./dev-servers.ps1 gives each zone its own,
// log/navmesh_dump/<zone>), else log/navmesh_dump. One that cannot be
// written is said in the map's log
inline auto openFor(const std::string& step, const rcCompactHeightfield& chf) -> std::ofstream
{
    const char*                 named = std::getenv("CARDIAN_NAVMESH_DUMP_DIR");
    const std::filesystem::path dir   = named != nullptr && *named != '\0' ? named : "log/navmesh_dump";
    std::error_code             ec;
    std::filesystem::create_directories(dir, ec);
    const auto    path = dir / (step + "_" + tileName(chf) + ".txt");
    std::ofstream out(path, std::ios::trunc);
    if (!out)
    {
        ShowWarningFmt("navmesh dump: cannot write {}", path.string());
    }
    return out;
}

// `chf` as it stands after `step`
inline void stage(const std::string& step, const rcCompactHeightfield& chf)
{
    auto out = openFor(step, chf);
    out << "tile " << chf.width << ' ' << chf.height << ' ' << chf.cs << ' ' << chf.ch << ' ' << chf.bmin[0] << ' ' << chf.bmin[1] << ' ' << chf.bmin[2] << '\n';
    for (int z = 0; z < chf.height; ++z)
    {
        for (int x = 0; x < chf.width; ++x)
        {
            const rcCompactCell& cell = chf.cells[x + z * chf.width];
            for (int i = static_cast<int>(cell.index), ni = static_cast<int>(cell.index + cell.count); i < ni; ++i)
            {
                const rcCompactSpan& s = chf.spans[i];
                out << x << ' ' << z << ' ' << -(chf.bmin[1] + s.y * chf.ch) << ' ' << static_cast<int>(chf.areas[i]);
                for (int dir = 0; dir < 4; ++dir)
                {
                    out << ' ' << (rcGetCon(s, dir) != RC_NOT_CONNECTED ? 1 : 0);
                }
                out << '\n';
            }
        }
    }
}

// `chf`'s regions, as Recast's watershed parted it after the berth: a line
// a floor, its cell's column and row, its height in FFXI's y, its area and
// its region (0 none; a tile border's carries 32768)
inline void regions(const rcCompactHeightfield& chf)
{
    if (!holds(chf))
    {
        return;
    }
    auto out = openFor("regions", chf);
    out << "tile " << chf.width << ' ' << chf.height << ' ' << chf.cs << ' ' << chf.ch << ' ' << chf.bmin[0] << ' ' << chf.bmin[1] << ' ' << chf.bmin[2] << '\n';
    for (int z = 0; z < chf.height; ++z)
    {
        for (int x = 0; x < chf.width; ++x)
        {
            const rcCompactCell& cell = chf.cells[x + z * chf.width];
            for (int i = static_cast<int>(cell.index), ni = static_cast<int>(cell.index + cell.count); i < ni; ++i)
            {
                out << x << ' ' << z << ' ' << -(chf.bmin[1] + chf.spans[i].y * chf.ch) << ' ' << static_cast<int>(chf.areas[i]) << ' ' << chf.spans[i].reg << '\n';
            }
        }
    }
}

// The contours Recast traced round `chf`'s regions: per contour a line
// `contour <region> <area>`, then its simplified outline and its raw one,
// each a line of points, x y z in FFXI's axes
inline void contours(const rcContourSet& cset, const rcCompactHeightfield& chf)
{
    if (!holds(chf))
    {
        return;
    }
    auto out = openFor("contours", chf);
    const auto    line = [&](const int* v, const int n)
    {
        for (int k = 0; k < n; ++k)
        {
            out << (k > 0 ? " " : "") << cset.bmin[0] + v[k * 4] * cset.cs << ' ' << -(cset.bmin[1] + v[k * 4 + 1] * cset.ch) << ' '
                << -(cset.bmin[2] + v[k * 4 + 2] * cset.cs);
        }
        out << '\n';
    };
    for (int c = 0; c < cset.nconts; ++c)
    {
        const rcContour& cont = cset.conts[c];
        out << "contour " << cont.reg << ' ' << static_cast<int>(cont.area) << '\n';
        line(cont.verts, cont.nverts);
        line(cont.rverts, cont.nrverts);
    }
}

// The joins the client's walker cut, one line each: the pair's two cells
// (column, row) and floors, the direction from the first to the second, and
// why: `settle` (a walker set down on one of them never came to rest), or
// `walk` (one walker did not reach where the other's rested)
class Cuts
{
public:
    explicit Cuts(const rcCompactHeightfield& chf)
    : out_(openFor("cuts", chf))
    {
    }

    void cut(const int x, const int z, const float y, const int nx, const int nz, const float ny, const int dir, const char* why)
    {
        out_ << x << ' ' << z << ' ' << y << ' ' << nx << ' ' << nz << ' ' << ny << ' ' << dir << ' ' << why << '\n';
    }

private:
    std::ofstream out_;
};

} // namespace cardian::client::dump
