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

#include "data/datasets/zones/settings/dataset.h"
#include "data/loader.h"
#include "map/navmesh/detour_navmesh.h"
#include "map/pawn/subzone_graph.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace
{
    using MeshPtr = std::unique_ptr<dtNavMesh, decltype(&dtFreeNavMesh)>;
    namespace sz  = cardian::subzones;

    auto yalms(const position_t& a, const position_t& b) -> float
    {
        return std::sqrt((a.x - b.x) * (a.x - b.x) + (a.y - b.y) * (a.y - b.y) + (a.z - b.z) * (a.z - b.z));
    }

    // A zone's navmesh file as the map loads it (DetourNavMesh::load's MSET
    // set of tiles), into a dtNavMesh of the test's own; none without the file
    auto loadMesh(const std::string& zone) -> MeshPtr
    {
        struct SetHeader
        {
            int             magic;
            int             version;
            int             numTiles;
            dtNavMeshParams params;
        };
        struct TileHeader
        {
            dtTileRef tileRef;
            int       dataSize;
        };
        MeshPtr       mesh(nullptr, &dtFreeNavMesh);
        std::ifstream in(fmt::format("navmeshes/{}.nav", zone), std::ios_base::binary);
        SetHeader     header{};
        if (!in.good() || !in.read(reinterpret_cast<char*>(&header), sizeof(header)))
        {
            return mesh;
        }
        mesh.reset(dtAllocNavMesh());
        if (!mesh || dtStatusFailed(mesh->init(&header.params)))
        {
            mesh.reset();
            return mesh;
        }
        for (int i = 0; i < header.numTiles; ++i)
        {
            TileHeader tile{};
            if (!in.read(reinterpret_cast<char*>(&tile), sizeof(tile)) || !tile.tileRef || !tile.dataSize)
            {
                break;
            }
            auto* data = static_cast<unsigned char*>(dtAlloc(tile.dataSize, DT_ALLOC_PERM));
            std::memset(data, 0, tile.dataSize);
            in.read(reinterpret_cast<char*>(data), tile.dataSize);
            mesh->addTile(data, tile.dataSize, DT_TILE_FREE_DATA, tile.tileRef, nullptr);
        }
        return mesh;
    }

    // Where the start's piece and the goal's piece come closest, for a route
    // that does not join them: the nearest few spots, by how far across and
    // how far up
    auto closestSpots(const dtNavMesh& mesh, const sz::Pieces& pieces, const sz::Piece a, const sz::Piece b) -> std::string
    {
        std::vector<position_t> mine;
        std::vector<position_t> theirs;
        for (int t = 0; t < mesh.getMaxTiles(); ++t)
        {
            const dtMeshTile* tile = mesh.getTile(t);
            if (tile == nullptr || tile->header == nullptr)
            {
                continue;
            }
            for (int p = 0; p < tile->header->polyCount; ++p)
            {
                const sz::Piece piece = pieces.pieceOf[pieces.tileBase[t] + p];
                if (piece != a && piece != b)
                {
                    continue;
                }
                for (int v = 0; v < tile->polys[p].vertCount; ++v)
                {
                    (piece == a ? mine : theirs).push_back(cardian::route::fromDetour(&tile->verts[tile->polys[p].verts[v] * 3]));
                }
            }
        }
        std::map<float, std::string> spots;
        for (const auto& point : mine)
        {
            for (const auto& other : theirs)
            {
                const float across = std::hypot(point.x - other.x, point.z - other.z);
                if (across <= 10.0f)
                {
                    spots.emplace(across + std::fabs(point.y - other.y), fmt::format("({:.1f}, {:.1f}, {:.1f}) and ({:.1f}, {:.1f}, {:.1f}), {:.1f}y across and {:.1f}y up",
                                                                                     point.x, point.y, point.z, other.x, other.y, other.z, across,
                                                                                     std::fabs(point.y - other.y)));
                }
            }
        }
        std::string out = fmt::format("{} pieces; the start's {} polygons, the goal's {}; nearest:", pieces.polys.size() - 1, pieces.polys[a], pieces.polys[b]);
        int         shown = 0;
        for (const auto& [_, text] : spots)
        {
            out += "\n  " + text;
            if (++shown == 3)
            {
                break;
            }
        }
        return out;
    }

    // A walk across one zone, by the whole-zone search
    void walks(const char* zone, const char* what, const position_t& from, const position_t& to)
    {
        const auto mesh = loadMesh(zone);
        if (!mesh)
        {
            SKIP("no navmesh for " << zone);
        }
        const auto route = cardian::route::search(*mesh, from, to, sz::kBodyExtents, sz::kLineExtents);
        REQUIRE(route.has_value());
        if (!route->complete)
        {
            const auto pieces = sz::piecesOf(*mesh);
            UNSCOPED_INFO(zone << ", " << what << ": no walk; "
                               << closestSpots(*mesh, pieces, sz::pieceAt(*mesh, pieces, from, sz::kLineExtents), sz::pieceAt(*mesh, pieces, to, sz::kLineExtents)));
        }
        CHECK(route->complete);
    }

    struct Region
    {
        std::map<uint16, MeshPtr>    meshes;
        std::map<uint16, sz::Pieces> pieces;
        sz::World                    world;

        auto piece(const uint16 zone, const position_t& at) const -> sz::Piece
        {
            const auto& mesh = *meshes.at(zone);
            const auto  p    = sz::pieceAt(mesh, pieces.at(zone), at, sz::kBodyExtents);
            return p != 0 ? p : sz::pieceAt(mesh, pieces.at(zone), at, sz::kLineExtents);
        }
    };

    // These zones' meshes, pieces and the lines among them, as the map joins
    // the world's (pawn_travel.cpp)
    auto regionOf(const std::vector<std::pair<xi::ZoneId, const char*>>& zones) -> std::optional<Region>
    {
        Region region;
        for (const auto& [id, name] : zones)
        {
            auto mesh = loadMesh(name);
            if (!mesh)
            {
                return std::nullopt;
            }
            region.pieces[static_cast<uint16>(id)] = sz::piecesOf(*mesh);
            region.meshes.emplace(static_cast<uint16>(id), std::move(mesh));
        }
        for (const auto& [id, _] : zones)
        {
            const auto zone     = static_cast<uint16>(id);
            const auto settings = xi::data::loadZoneFile<xi::data::datasets::zones::settings::Dataset>(id);
            REQUIRE(settings);
            for (const auto& line : settings->ZoneLines)
            {
                const auto to = static_cast<uint16>(line.DestinationZone);
                if (to == zone || !region.meshes.contains(to))
                {
                    continue;
                }
                region.world.lines.push_back({ .zone    = zone,
                                               .to      = to,
                                               .origin  = line.Origin,
                                               .arrival = line.Destination,
                                               .leaves  = sz::lineEnd(*region.meshes.at(zone), region.pieces.at(zone), line.Origin),
                                               .lands   = sz::lineEnd(*region.meshes.at(to), region.pieces.at(to), line.Destination) });
            }
            if (!region.pieces.at(zone).drops.empty())
            {
                region.world.drops[zone] = region.pieces.at(zone).drops;
            }
        }
        sz::join(region.world);
        return region;
    }

    // A zone's main ground: its biggest piece
    auto mainPiece(const sz::Pieces& pieces) -> sz::Piece
    {
        sz::Piece main = 0;
        for (std::size_t p = 1; p < pieces.polys.size(); ++p)
        {
            if (main == 0 || pieces.polys[p] > pieces.polys[main])
            {
                main = static_cast<sz::Piece>(p);
            }
        }
        return main;
    }

    // The pieces a body reaches from `start` by its zone's drops (`down`),
    // or the pieces from which a body reaches `start` by them
    auto reachOf(const sz::Pieces& pieces, const sz::Piece start, const bool down) -> std::vector<bool>
    {
        std::vector<bool>      seen(pieces.polys.size(), false);
        std::vector<sz::Piece> todo{ start };
        seen[start] = true;
        while (!todo.empty())
        {
            const auto at = todo.back();
            todo.pop_back();
            for (const auto& drop : pieces.drops)
            {
                const auto from = down ? drop.from : drop.to;
                const auto to   = down ? drop.to : drop.from;
                if (from == at && !seen[to])
                {
                    seen[to] = true;
                    todo.push_back(to);
                }
            }
        }
        return seen;
    }

    // Every zone's mesh, pieces and lines, the lines placed and the world
    // joined as the map does it (pawn_travel.cpp buildWorld); a zone without
    // a mesh takes piece 0
    struct WholeWorld
    {
        std::map<uint16, MeshPtr>     meshes;
        std::map<uint16, sz::Pieces>  pieces;
        std::map<uint16, std::string> names;
        sz::World                     world;

        // A body's piece, as pawn_travel.cpp's bodyPiece finds it
        auto piece(const uint16 zone, const position_t& at) const -> sz::Piece
        {
            if (!meshes.contains(zone))
            {
                return 0;
            }
            const auto& mesh = *meshes.at(zone);
            const auto  p    = sz::pieceAt(mesh, pieces.at(zone), at, sz::kBodyExtents);
            return p != 0 ? p : sz::pieceAt(mesh, pieces.at(zone), at, sz::kLineExtents);
        }
    };

    auto wholeWorld() -> std::unique_ptr<WholeWorld>
    {
        using Settings = xi::data::datasets::zones::settings::Dataset;
        struct Crossing
        {
            uint16     to{};
            position_t origin{};
            position_t arrival{};
        };
        auto                                    out = std::make_unique<WholeWorld>();
        std::map<uint16, std::vector<Crossing>> crossings;
        for (int z = 1; z < 300; ++z)
        {
            const auto id       = static_cast<xi::ZoneId>(z);
            const auto settings = xi::data::loadZoneFile<Settings>(id);
            if (!settings)
            {
                continue;
            }
            const auto zone  = static_cast<uint16>(z);
            out->names[zone] = std::filesystem::path(xi::data::zoneFilePath(id, "zone")).parent_path().filename().string();
            for (const auto& line : settings->ZoneLines)
            {
                if (static_cast<uint16>(line.DestinationZone) != zone)
                {
                    crossings[zone].push_back({ static_cast<uint16>(line.DestinationZone), line.Origin, line.Destination });
                }
            }
            if (auto mesh = loadMesh(out->names[zone]); mesh)
            {
                out->pieces[zone] = sz::piecesOf(*mesh);
                out->meshes.emplace(zone, std::move(mesh));
            }
        }
        const auto place = [&](const uint16 zone, const position_t& at) -> sz::LineEnd
        {
            return out->meshes.contains(zone) ? sz::lineEnd(*out->meshes.at(zone), out->pieces.at(zone), at) : sz::LineEnd{ 0, at };
        };
        for (const auto& [zone, lines] : crossings)
        {
            for (const auto& line : lines)
            {
                out->world.lines.push_back(
                    { .zone = zone, .to = line.to, .origin = line.origin, .arrival = line.arrival, .leaves = place(zone, line.origin), .lands = place(line.to, line.arrival) });
            }
        }
        for (const auto& [zone, pieces] : out->pieces)
        {
            if (!pieces.drops.empty())
            {
                out->world.drops[zone] = pieces.drops;
            }
        }
        sz::join(out->world);
        return out;
    }

    // A zone line's two ends, looked at one at a time
    enum class End
    {
        Origin,  // its trigger, in the zone it leaves
        Arrival, // where it sets her down, in the zone it lands in
    };

    // Whether a line's end stands on ground its zone's main ground joins: an
    // origin on a piece the main ground reaches by walking and drops, an
    // arrival on a piece that reaches the main ground. Piece 0 joins every
    // piece. Each zone's reach is worked out once, into `cache`
    auto joinsMain(const WholeWorld& whole, const uint16 zone, const sz::LineEnd& end, const End which, std::map<std::pair<uint16, End>, std::vector<bool>>& cache)
        -> bool
    {
        const auto& pieces = whole.pieces.at(zone);
        const auto  key    = std::make_pair(zone, which);
        if (!cache.contains(key))
        {
            cache[key] = reachOf(pieces, mainPiece(pieces), which == End::Origin);
        }
        return end.piece == 0 || cache.at(key)[end.piece];
    }
} // namespace

TEST_CASE("Route search: a walk the mesh joins reaches its zone line in one search", "[cardian][route]")
{
    // OPEN_ISSUES #373: recruits circled 95 yalms short of the gate to Southern San d'Oria; the mesh joins them
    walks("Northern_San_dOria", "from where the recruits circled to the gate to Southern San d'Oria", { -51.0f, 0.0f, 44.0f, 0, 0 },
          { -0.107f, -7.068f, -36.133f, 0, 0 });
    walks("Northern_San_dOria", "from the arrival from Port San d'Oria to the gate to Southern San d'Oria", { -125.375f, 10.371f, 268.640f, 0, 0 },
          { -0.107f, -7.068f, -36.133f, 0, 0 });
    // the way round to North Gustaberg's high ground
    walks("South_Gustaberg", "from the arrival from North Gustaberg's lowlands to the west line", { 0.840f, -5.027f, 76.838f, 0, 0 },
          { -440.465f, 33.268f, -118.436f, 0, 0 });
    walks("North_Gustaberg", "from the arrival from South Gustaberg (west) to the Konschtat Highlands line", { -440.792f, 35.531f, -272.901f, 0, 0 },
          { -518.828f, 33.034f, 592.074f, 0, 0 });
}

TEST_CASE("Sub-zones: North Gustaberg's lowlands reach Konschtat Highlands only by South Gustaberg", "[cardian][route]")
{
    // OPEN_ISSUES #126: no walk in North Gustaberg joins its lowlands to the high ground with the Konschtat line
    const auto region = regionOf({ { xi::ZoneId::PortBastok, "Port_Bastok" },
                                   { xi::ZoneId::NorthGustaberg, "North_Gustaberg" },
                                   { xi::ZoneId::SouthGustaberg, "South_Gustaberg" },
                                   { xi::ZoneId::KonschtatHighlands, "Konschtat_Highlands" } });
    if (!region.has_value())
    {
        SKIP("a navmesh of the Gustaberg region is missing");
    }
    const auto north     = static_cast<uint16>(xi::ZoneId::NorthGustaberg);
    const auto konschtat = static_cast<uint16>(xi::ZoneId::KonschtatHighlands);

    const position_t fromBastok{ 660.004f, -2.478f, 306.236f, 0, 0 };   // the arrival from Port Bastok
    const position_t fromWest{ -440.792f, 35.531f, -272.901f, 0, 0 };   // the arrival from South Gustaberg's west line
    const auto       lowlands  = region->piece(north, fromBastok);
    const auto       highlands = region->piece(north, fromWest);
    REQUIRE(lowlands != 0);
    REQUIRE(highlands != 0);
    CHECK(lowlands != highlands);

    // from the lowlands: out to South Gustaberg, back in by its west line, and on to Konschtat
    const auto low = sz::plan(region->world, north, lowlands, fromBastok, konschtat, std::nullopt, std::nullopt);
    REQUIRE(low.reachable);
    REQUIRE(low.firstLine.has_value());
    CHECK(region->world.lines[*low.firstLine].to == static_cast<uint16>(xi::ZoneId::SouthGustaberg));
    CHECK(low.lines == 3);

    // from the high ground: straight to the line
    const auto high = sz::plan(region->world, north, highlands, fromWest, konschtat, std::nullopt, std::nullopt);
    REQUIRE(high.reachable);
    REQUIRE(high.firstLine.has_value());
    CHECK(region->world.lines[*high.firstLine].to == konschtat);
    CHECK(high.lines == 1);

    // back from Konschtat to Port Bastok: in by the high ground, and the same way round
    const position_t fromKonschtat{ -518.867f, 35.538f, 588.640f, 0, 0 }; // the arrival from Konschtat Highlands
    const auto       back = sz::plan(region->world, north, region->piece(north, fromKonschtat), fromKonschtat, static_cast<uint16>(xi::ZoneId::PortBastok),
                                     std::nullopt, std::nullopt);
    REQUIRE(back.reachable);
    REQUIRE(back.firstLine.has_value());
    CHECK(region->world.lines[*back.firstLine].to == static_cast<uint16>(xi::ZoneId::SouthGustaberg));
}

namespace
{
    // The nearest a route's straights come to a point, setting out from `from`
    auto closestApproach(const position_t& from, const std::vector<position_t>& points, const position_t& c) -> float
    {
        float      best = std::numeric_limits<float>::max();
        position_t a    = from;
        for (const auto& b : points)
        {
            const auto closest = cardian::route::detail::nearestOn(cardian::route::toDetour(a), cardian::route::toDetour(b), cardian::route::toDetour(c));
            best               = std::min(best, yalms(cardian::route::fromDetour(closest.data()), c));
            a                  = b;
        }
        return best;
    }

    // The point a fraction of the way along a route, by its length
    auto along(const position_t& from, const std::vector<position_t>& points, const float fraction) -> position_t
    {
        float total = 0.0f;
        auto  a     = from;
        for (const auto& b : points)
        {
            total += yalms(a, b);
            a = b;
        }
        float left = total * fraction;
        a          = from;
        for (const auto& b : points)
        {
            const float leg = yalms(a, b);
            if (leg >= left && leg > 0.0f)
            {
                const float t = left / leg;
                return { a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t, 0, 0 };
            }
            left -= leg;
            a = b;
        }
        return a;
    }
} // namespace

TEST_CASE("Route kept out of danger: a mob on the way is walked round, and a choke is known before she sets out", "[cardian][route]")
{
    const auto mesh = loadMesh("South_Gustaberg");
    if (!mesh)
    {
        SKIP("no navmesh for South_Gustaberg");
    }
    using cardian::route::Hazard;
    const position_t from{ 0.840f, -5.027f, 76.838f, 0, 0 };      // the arrival from North Gustaberg's lowlands
    const position_t to{ -440.465f, 33.268f, -118.436f, 0, 0 };   // the west line
    const auto       plain = cardian::route::search(*mesh, from, to, sz::kBodyExtents, sz::kLineExtents);
    REQUIRE(plain.has_value());
    REQUIRE(plain->complete);

    // With no mob about, as short as the game's own route, near enough
    const auto clear = cardian::route::searchAvoiding(*mesh, from, to, sz::kBodyExtents, sz::kLineExtents, {}, {});
    REQUIRE(clear.has_value());
    REQUIRE(clear->complete);
    CHECK(clear->firstDanger == -1);
    CHECK(clear->length < plain->length * 1.1f + 5.0f);

    SECTION("a mob on the way: round it, out of its reach")
    {
        for (const float fraction : { 0.25f, 0.5f, 0.75f })
        {
            const auto   mob   = along(from, plain->points, fraction);
            const Hazard h     = { .at = mob, .radius = 12.0f };
            const auto   round = cardian::route::searchAvoiding(*mesh, from, to, sz::kBodyExtents, sz::kLineExtents, { h }, {});
            REQUIRE(round.has_value());
            REQUIRE(round->complete);
            CHECK(round->firstDanger == -1);
            CHECK(closestApproach(from, round->points, mob) >= h.radius - 0.01f);
        }
    }

    SECTION("a mob that cannot see the way is no danger, but for its unseen reach")
    {
        const auto   mob     = along(from, plain->points, 0.5f);
        const auto   blind   = [](size_t, const position_t&) { return false; };
        const auto   ignored = cardian::route::searchAvoiding(*mesh, from, to, sz::kBodyExtents, sz::kLineExtents, { Hazard{ .at = mob, .radius = 12.0f } }, blind);
        REQUIRE(ignored.has_value());
        CHECK(ignored->firstDanger == -1);
        CHECK(closestApproach(from, ignored->points, mob) < 12.0f); // straight past it
        const auto ambush = cardian::route::searchAvoiding(*mesh, from, to, sz::kBodyExtents, sz::kLineExtents, { Hazard{ .at = mob, .radius = 12.0f, .unseen = 5.0f } }, blind);
        REQUIRE(ambush.has_value());
        CHECK(ambush->firstDanger == -1);
        CHECK(closestApproach(from, ambush->points, mob) >= 5.0f - 0.01f);
    }

    SECTION("the line held: she walks up to the choke and stops out of its reach")
    {
        const Hazard h    = { .at = to, .radius = 25.0f };
        const auto   held = cardian::route::searchAvoiding(*mesh, from, to, sz::kBodyExtents, sz::kLineExtents, { h }, {});
        REQUIRE(held.has_value());
        REQUIRE(held->complete);
        REQUIRE(held->firstDanger >= 0);
        const auto& stand = held->firstDanger == 0 ? from : held->points[held->firstDanger - 1];
        CHECK(yalms(stand, to) >= h.radius - 0.01f);
        CHECK(closestApproach(from, std::vector<position_t>(held->points.begin(), held->points.begin() + held->firstDanger), to) >= h.radius - 0.01f);
    }

    SECTION("a stretch planned again keeps to its ground: round a mob within it, held where it is too narrow to go round")
    {
        const auto  end   = along(from, plain->points, 150.0f / plain->length);
        const auto  mob   = position_t((from.x + end.x) / 2.0f, (from.y + end.y) / 2.0f, (from.z + end.z) / 2.0f, 0, 0);
        const auto  crow  = yalms(from, end);
        const float reach = std::max(150.0f * 1.5f, crow) + 60.0f;
        const auto  wide  = cardian::route::searchAvoiding(*mesh, from, end, sz::kBodyExtents, sz::kBodyExtents, { Hazard{ .at = mob, .radius = 12.0f } }, {}, reach);
        REQUIRE(wide.has_value());
        REQUIRE(wide->complete);
        CHECK(wide->firstDanger == -1);
        CHECK(closestApproach(from, wide->points, mob) >= 12.0f - 0.01f);
        for (const auto& p : wide->points)
        {
            CHECK(yalms(p, from) + yalms(p, end) <= reach + 6.0f); // the ends are found on the mesh, a little off the points asked
        }

        // ground no wider than the mob's reach across the middle: every way
        // over it passes the mob
        const auto tight = cardian::route::searchAvoiding(*mesh, from, end, sz::kBodyExtents, sz::kBodyExtents, { Hazard{ .at = mob, .radius = 12.0f } }, {}, crow + 2.0f);
        REQUIRE(tight.has_value());
        CHECK((!tight->complete || tight->firstDanger >= 0));
    }

    SECTION("the nearest ground out of every reach: just outside a mob's, and none where one holds all the ground near")
    {
        const Hazard h    = { .at = from, .radius = 10.0f };
        const auto   safe = cardian::route::searchSafeGround(*mesh, from, sz::kBodyExtents, { h }, {}, 150.0f);
        REQUIRE(safe.has_value());
        REQUIRE(safe->complete);
        REQUIRE_FALSE(safe->points.empty());
        CHECK(yalms(safe->points.back(), from) >= h.radius - 0.01f);
        CHECK(safe->length < h.radius + 10.0f); // the nearest, not any

        const auto here = cardian::route::searchSafeGround(*mesh, to, sz::kLineExtents, { h }, {}, 150.0f);
        REQUIRE(here.has_value());
        CHECK(here->complete);
        CHECK(here->points.empty()); // out of every reach where she stands

        const auto none = cardian::route::searchSafeGround(*mesh, from, sz::kBodyExtents, { Hazard{ .at = from, .radius = 400.0f } }, {}, 150.0f);
        REQUIRE(none.has_value());
        CHECK_FALSE(none->complete);
    }

    SECTION("setting out inside a mob's reach, she walks out of it")
    {
        const Hazard h   = { .at = from, .radius = 10.0f };
        const auto   out = cardian::route::searchAvoiding(*mesh, from, to, sz::kBodyExtents, sz::kLineExtents, { h }, {});
        REQUIRE(out.has_value());
        REQUIRE(out->complete);
        CHECK(out->firstDanger == -1);

        // every step outward until she is out of its reach, and never back in
        position_t a      = from;
        bool       inside = true;
        for (const auto& b : out->points)
        {
            const auto closest = cardian::route::detail::nearestOn(cardian::route::toDetour(a), cardian::route::toDetour(b), cardian::route::toDetour(h.at));
            if (inside)
            {
                CHECK(yalms(b, h.at) >= yalms(a, h.at) - 0.01f);
            }
            else
            {
                CHECK(yalms(cardian::route::fromDetour(closest.data()), h.at) >= h.radius - 0.01f);
            }
            inside = inside && yalms(b, h.at) < h.radius;
            a      = b;
        }
        CHECK_FALSE(inside);
    }

    SECTION("setting out inside the edge of a mob's reach beside her way, she leaves it by the shortest way, then goes on")
    {
        // The mob stands off to the side of her way, on one side and then on
        // the other, she 3 yalms inside its reach: going on along her way
        // would keep her inside it for 10 yalms, stepping straight away from
        // the mob for 3
        const auto  ahead = along(from, plain->points, 20.0f / plain->length);
        const float dx    = ahead.x - from.x;
        const float dz    = ahead.z - from.z;
        const float len   = std::sqrt(dx * dx + dz * dz);
        const float depth = 3.0f;
        const float r     = 18.5f;
        for (const float side : { 1.0f, -1.0f })
        {
            const Hazard h   = { .at = position_t(from.x - side * dz / len * (r - depth), from.y, from.z + side * dx / len * (r - depth), 0, 0), .radius = r };
            const auto   out = cardian::route::searchAvoiding(*mesh, from, to, sz::kBodyExtents, sz::kLineExtents, { h }, {});
            REQUIRE(out.has_value());
            REQUIRE(out->complete);
            CHECK(out->firstDanger == -1);
            CHECK(out->length < clear->length + 15.0f);

            float      inside = 0.0f;
            position_t a      = from;
            for (const auto& b : out->points)
            {
                inside += cardian::route::detail::insideLength(cardian::route::toDetour(a), cardian::route::toDetour(b), cardian::route::toDetour(h.at), h.radius);
                a = b;
            }
            INFO("the mob on side " << side << "; yalms walked inside the reach: " << inside);
            CHECK(inside < depth * 1.5f + 1.5f);
        }
    }
}

TEST_CASE("Route kept out of danger: La Theine Plateau end to end, as short as the game's own route", "[cardian][route]")
{
    // La Theine Plateau's mesh holds some 13,000 off-mesh links, half of them
    // disabled and the rest ledges dropped down only: end to end, from the
    // West Ronfaure line to the Valkurm Dunes line
    const auto mesh = loadMesh("La_Theine_Plateau");
    if (!mesh)
    {
        SKIP("no navmesh for La_Theine_Plateau");
    }
    using cardian::route::Hazard;
    const position_t from{ -558.569f, -7.049f, 688.049f, 0, 0 }; // the line to West Ronfaure
    const position_t to{ 160.169f, 24.326f, -764.007f, 0, 0 };   // the line to Valkurm Dunes
    const auto       plain = cardian::route::search(*mesh, from, to, sz::kLineExtents, sz::kLineExtents);
    REQUIRE(plain.has_value());
    REQUIRE(plain->complete);

    const auto clear = cardian::route::searchAvoiding(*mesh, from, to, sz::kLineExtents, sz::kLineExtents, {}, {});
    REQUIRE(clear.has_value());
    REQUIRE(clear->complete);
    CHECK(clear->firstDanger == -1);
    CHECK(clear->length < plain->length * 1.1f + 5.0f);

    const Hazard h    = { .at = to, .radius = 25.0f };
    const auto   held = cardian::route::searchAvoiding(*mesh, from, to, sz::kLineExtents, sz::kLineExtents, { h }, {});
    REQUIRE(held.has_value());
    REQUIRE(held->complete);
    CHECK(held->firstDanger >= 0);
}

TEST_CASE("Route kept out of danger: timings (opt-in, CARDIAN_BENCH=1)", "[cardian][route]")
{
    if (std::getenv("CARDIAN_BENCH") == nullptr)
    {
        SKIP("the timings run with CARDIAN_BENCH=1");
    }
    using cardian::route::Hazard;
    struct Case
    {
        const char* zone;
        position_t  from;
        position_t  to;
    };
    const Case cases[] = {
        { "South_Gustaberg", { 0.840f, -5.027f, 76.838f, 0, 0 }, { -440.465f, 33.268f, -118.436f, 0, 0 } },
        { "La_Theine_Plateau", { -558.569f, -7.049f, 688.049f, 0, 0 }, { 160.169f, 24.326f, -764.007f, 0, 0 } },
    };
    for (const auto& c : cases)
    {
        const auto mesh = loadMesh(c.zone);
        if (!mesh)
        {
            continue;
        }
        const auto facesBegan = std::chrono::steady_clock::now();
        const auto faces      = cardian::route::facesOf(*mesh);
        const auto facesUs    = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - facesBegan).count();
        WARN(c.zone << " faces: " << faces.faces.size() << " worked out in " << facesUs / 1000.0 << " ms");
        const auto plain = cardian::route::search(*mesh, c.from, c.to, sz::kLineExtents, sz::kLineExtents);
        REQUIRE(plain.has_value());

        // mobs as play has them: one every 40 yalms along the way, 15 yalms
        // to one side or the other, every circle 12 yalms
        std::vector<Hazard> scattered;
        float               total = plain->length;
        for (int i = 1; i * 40.0f < total; ++i)
        {
            auto p = along(c.from, plain->points, i * 40.0f / total);
            p.x += (i % 2 == 0 ? 15.0f : -15.0f);
            scattered.push_back({ .at = p, .radius = 12.0f });
        }
        const std::vector<Hazard> choke = { { .at = c.to, .radius = 25.0f } };

        // every mob sees everything, and the sight tests a real one's eyes
        // would cost are counted
        int        sightTests = 0;
        const auto sight      = [&](size_t, const position_t&)
        {
            ++sightTests;
            return true;
        };
        const auto time = [&](const char* what, const std::vector<Hazard>& hazards)
        {
            sightTests       = 0;
            const auto began = std::chrono::steady_clock::now();
            const auto route = cardian::route::searchAvoiding(*mesh, c.from, c.to, sz::kLineExtents, sz::kLineExtents, hazards, sight, 0.0f, &faces);
            const auto us    = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - began).count();
            REQUIRE(route.has_value());
            WARN(c.zone << " " << what << ": " << us / 1000.0 << " ms, " << route->searched << " points searched, " << sightTests << " sight tests, "
                        << route->length << "y (Detour's " << plain->length << "y), danger at " << route->firstDanger);
        };
        time("clear", {});
        time("scattered", scattered);
        time("choke", choke);

        // a stretch planned again: 150 yalms of the way, a mob on its middle
        const auto stretchEnd = along(c.from, plain->points, 150.0f / total);
        const auto stretchMob = along(c.from, plain->points, 75.0f / total);
        const auto began      = std::chrono::steady_clock::now();
        const auto stretch    = cardian::route::searchAvoiding(*mesh, c.from, stretchEnd, sz::kLineExtents, sz::kBodyExtents, { { .at = stretchMob, .radius = 12.0f } },
                                                               sight, 150.0f * 1.5f + 60.0f, &faces);
        const auto us         = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - began).count();
        REQUIRE(stretch.has_value());
        WARN(c.zone << " stretch: " << us / 1000.0 << " ms, " << stretch->searched << " points searched, " << stretch->length << "y, complete "
                    << stretch->complete << ", danger at " << stretch->firstDanger);
    }
}

// A line's choice (pawn::travel::nextHop): the shortest walk across the
// world's sub-zones from a body to a zone, timed over the whole world
TEST_CASE("Sub-zones: a line's choice, timed (opt-in, CARDIAN_BENCH=1)", "[cardian][route]")
{
    if (std::getenv("CARDIAN_BENCH") == nullptr)
    {
        SKIP("the timings run with CARDIAN_BENCH=1");
    }
    const auto whole = wholeWorld();
    WARN("the world: " << whole->meshes.size() << " meshes, " << whole->world.lines.size() << " lines");
    struct Case
    {
        const char* what;
        xi::ZoneId  from;
        position_t  at;
        xi::ZoneId  to;
    };
    const Case cases[] = {
        { "Jugner Forest's Davoi gate to Davoi", xi::ZoneId::JugnerForest, { -241.5f, -8.1f, -593.6f, 0, 0 }, xi::ZoneId::Davoi },
        { "North Gustaberg's lowlands to Konschtat Highlands", xi::ZoneId::NorthGustaberg, { 660.004f, -2.478f, 306.236f, 0, 0 }, xi::ZoneId::KonschtatHighlands },
        { "Northern San d'Oria to Jugner Forest", xi::ZoneId::NorthernSanDoria, { -125.375f, 10.371f, 268.640f, 0, 0 }, xi::ZoneId::JugnerForest },
        { "Northern San d'Oria to Windurst Waters", xi::ZoneId::NorthernSanDoria, { -125.375f, 10.371f, 268.640f, 0, 0 }, xi::ZoneId::WindurstWaters },
        { "North Gustaberg's lowlands to a zone no line reaches: every walk searched", xi::ZoneId::NorthGustaberg, { 660.004f, -2.478f, 306.236f, 0, 0 },
          static_cast<xi::ZoneId>(0) },
    };
    for (const auto& c : cases)
    {
        const auto    zone  = static_cast<uint16>(c.from);
        const auto    piece = whole->piece(zone, c.at);
        double        total = 0.0;
        double        most  = 0.0;
        sz::Plan      plan;
        constexpr int kRuns = 20;
        for (int i = 0; i < kRuns; ++i)
        {
            const auto began = std::chrono::steady_clock::now();
            plan             = sz::plan(whole->world, zone, piece, c.at, static_cast<uint16>(c.to), std::nullopt, std::nullopt);
            const double ms  = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - began).count();
            total += ms;
            most = std::max(most, ms);
        }
        WARN(c.what << ": " << (plan.reachable ? "a walk" : "NO walk") << ", " << plan.lines << " lines, " << plan.length << "y; " << total / kRuns
                    << " ms a choice on average, " << most << " ms at most");
    }
}

TEST_CASE("Route kept out of danger: a ledge taller than the mesh's climb is dropped off, never climbed", "[cardian][route]")
{
    const auto mesh = loadMesh("Valkurm_Dunes");
    if (!mesh)
    {
        SKIP("no navmesh for Valkurm_Dunes");
    }

    // an enabled two-way link of the mesh rising 3 to 5 yalms -- the meshes
    // mark their ledges so, and the game's own paths climb them -- its foot
    // and its top
    std::optional<std::pair<position_t, position_t>> ledge;
    float                                            climb = 0.0f;
    for (int t = 0; t < mesh->getMaxTiles() && !ledge.has_value(); ++t)
    {
        const dtMeshTile* tile = static_cast<const dtNavMesh&>(*mesh).getTile(t);
        if (tile == nullptr || tile->header == nullptr)
        {
            continue;
        }
        climb = tile->header->walkableClimb;
        for (int c = 0; c < tile->header->offMeshConCount && !ledge.has_value(); ++c)
        {
            // its ends as the search reads them: the link's own vertices,
            // snapped to the ground
            const auto&  con     = tile->offMeshCons[c];
            const auto&  poly    = tile->polys[con.poly];
            const float* atStart = &tile->verts[poly.verts[0] * 3];
            const float* atEnd   = &tile->verts[poly.verts[1] * 3];
            const float  rise    = atEnd[1] - atStart[1]; // Detour's y is up

            // one walked at all (the meshes disable most of their links) and
            // joined to the ground at both ends
            if (poly.flags & cardian::route::kPolyDisabled)
            {
                continue;
            }
            bool landsAtStart = false;
            bool landsAtEnd   = false;
            for (unsigned int l = poly.firstLink; l != DT_NULL_LINK; l = tile->links[l].next)
            {
                landsAtStart = landsAtStart || tile->links[l].edge == 0;
                landsAtEnd   = landsAtEnd || tile->links[l].edge == 1;
            }
            if ((con.flags & DT_OFFMESH_CON_BIDIR) && landsAtStart && landsAtEnd && std::fabs(rise) > 3.0f && std::fabs(rise) <= 5.0f)
            {
                const auto a = cardian::route::fromDetour(atStart);
                const auto b = cardian::route::fromDetour(atEnd);
                ledge        = rise > 0.0f ? std::pair{ a, b } : std::pair{ b, a };
            }
        }
    }
    REQUIRE(ledge.has_value());
    const auto [foot, top] = *ledge;
    REQUIRE(climb < 3.0f);

    const auto near_ = [](const position_t& p, const position_t& q) { return yalms(p, q) < 0.5f; };

    // up: never straight from the foot to the top
    const auto up = cardian::route::searchAvoiding(*mesh, foot, top, sz::kBodyExtents, sz::kBodyExtents, {}, {});
    REQUIRE(up.has_value());
    position_t a = foot;
    for (const auto& b : up->points)
    {
        CHECK_FALSE((near_(a, foot) && near_(b, top)));
        a = b;
    }

    // down: straight off the ledge
    const auto down = cardian::route::searchAvoiding(*mesh, top, foot, sz::kBodyExtents, sz::kBodyExtents, {}, {});
    REQUIRE(down.has_value());
    REQUIRE(down->complete);
    CHECK(down->length < yalms(top, foot) + 2.0f);
}

TEST_CASE("Route slopes: steep faces climbed for kClimbRun on end are refused, a bump and the way down or across walked", "[cardian][route]")
{
    using cardian::route::Face;
    using cardian::route::detail::Vec;

    // A ramp rising 0.7 a yalm along x (35 degrees) from x0 to x1, ten
    // yalms across: two triangles, split on its diagonal
    const auto ramp = [](const float x0, const float x1)
    {
        return std::vector<Face>{ { x0, -5.0f, x1, -5.0f, x1, 5.0f, 0.7f, 0.0f }, { x0, -5.0f, x1, 5.0f, x0, 5.0f, 0.7f, 0.0f } };
    };
    const auto climbs = [](const std::vector<Face>& faces, const Vec& a, const Vec& b)
    {
        return cardian::route::detail::climbsOver([&](const auto& visit)
        {
            for (const auto& f : faces)
            {
                visit(f);
            }
        }, a, b);
    };
    const Vec west{ -2.0f, 0.0f, 0.0f };
    const Vec east{ 6.0f, 0.0f, 0.0f };

    CHECK_FALSE(climbs(ramp(0.0f, 1.4f), west, east)); // a bump: 1.4 yalms of it
    CHECK(climbs(ramp(0.0f, 1.6f), west, east));       // past kClimbRun
    CHECK_FALSE(climbs(ramp(0.0f, 4.0f), east, west)); // down it
    CHECK_FALSE(climbs(ramp(0.0f, 4.0f), Vec{ 2.0f, 0.0f, -4.0f }, Vec{ 2.0f, 0.0f, 4.0f })); // across it

    // two ramps a hair apart are one climb; a yalm apart, two bumps
    auto joined = ramp(0.0f, 1.0f);
    for (const auto& f : ramp(1.05f, 2.05f))
    {
        joined.push_back(f);
    }
    CHECK(climbs(joined, west, east));
    auto split = ramp(0.0f, 1.0f);
    for (const auto& f : ramp(2.0f, 3.0f))
    {
        split.push_back(f);
    }
    CHECK_FALSE(climbs(split, west, east));

    // a walk shorter than kClimbRun holds no such climb
    CHECK_FALSE(climbs(ramp(0.0f, 4.0f), Vec{ 0.5f, 0.0f, 0.0f }, Vec{ 1.9f, 0.0f, 0.0f }));
}

TEST_CASE("Route kept out of danger: ground steeper than a player climbs is walked down, never up", "[cardian][route]")
{
    const auto mesh = loadMesh("Valkurm_Dunes");
    if (!mesh)
    {
        SKIP("no navmesh for Valkurm_Dunes");
    }

    const auto faces = cardian::route::facesOf(*mesh);

    // The hill by Valkurm Dunes' goblins (2026-10-12): a player walks up to
    // its shoulder and no further, where it steepens past 30 degrees; the
    // mesh, built for monsters, walks on up
    const position_t foot(691.7f, -2.9f, -201.3f, 0, 0);
    const position_t shoulder(672.1f, -4.98f, -217.08f, 0, 0);
    const position_t top(652.9f, -11.1f, -218.4f, 0, 0);

    const auto up = cardian::route::searchAvoiding(*mesh, foot, top, sz::kBodyExtents, sz::kBodyExtents, {}, {}, 0.0f, &faces);
    REQUIRE(up.has_value());
    CHECK_FALSE((up->complete && !up->points.empty()));

    const auto toShoulder = cardian::route::searchAvoiding(*mesh, foot, shoulder, sz::kBodyExtents, sz::kBodyExtents, {}, {}, 0.0f, &faces);
    REQUIRE(toShoulder.has_value());
    CHECK(toShoulder->complete);

    const auto down = cardian::route::searchAvoiding(*mesh, top, foot, sz::kBodyExtents, sz::kBodyExtents, {}, {}, 0.0f, &faces);
    REQUIRE(down.has_value());
    CHECK(down->complete);
    CHECK(down->length < yalms(top, foot) + 15.0f);

    // A cliff near the La Theine line, a band of four yalms climbing 38 to 44
    // degrees that one polygon averages to 29: read by its steep faces, not climbed
    const position_t below(606.98f, -16.82f, 287.74f, 0, 0);
    const position_t onTop(620.9f, -22.79f, 292.8f, 0, 0);
    const auto       cliff = cardian::route::searchAvoiding(*mesh, below, onTop, sz::kBodyExtents, sz::kBodyExtents, {}, {}, 0.0f, &faces);
    REQUIRE(cliff.has_value());
    if (cliff->complete)
    {
        CHECK(cliff->length > yalms(below, onTop) + 20.0f); // round by some gentler way, not up the face
    }
}

// Every zone's lines a walk joins, walked under the slope rule: a pair the
// sub-zones join (the mesh walks them, ledges as the rule takes them) that
// the search now finds no way between is a way the slope rule has closed --
// stairs, a ramp to a line -- and listed. Opt-in: it loads every mesh
TEST_CASE("Route slopes: the world's lines a walk joins, kept to a player's slope (opt-in, CARDIAN_SLOPES=1)", "[cardian][route]")
{
    if (std::getenv("CARDIAN_SLOPES") == nullptr)
    {
        SKIP("the slope survey runs with CARDIAN_SLOPES=1");
    }
    using Settings = xi::data::datasets::zones::settings::Dataset;
    int         zones   = 0;
    int         pairs   = 0;
    int         refused = 0;
    std::string closed;
    for (int z = 1; z < 300; ++z)
    {
        const auto id       = static_cast<xi::ZoneId>(z);
        const auto settings = xi::data::loadZoneFile<Settings>(id);
        if (!settings || settings->ZoneLines.size() < 2)
        {
            continue;
        }
        const std::string name = std::filesystem::path(xi::data::zoneFilePath(id, "zone")).parent_path().filename().string();
        const auto        mesh = loadMesh(name);
        if (!mesh)
        {
            continue;
        }
        ++zones;
        const auto  faces  = cardian::route::facesOf(*mesh);
        const auto  pieces = sz::piecesOf(*mesh);
        const auto& lines  = settings->ZoneLines;
        for (size_t i = 0; i < lines.size(); ++i)
        {
            for (size_t j = i + 1; j < lines.size(); ++j)
            {
                const auto& a  = lines[i].Origin;
                const auto& b  = lines[j].Origin;
                const auto  pa = sz::pieceAt(*mesh, pieces, a, sz::kLineExtents);
                if (pa == 0 || pa != sz::pieceAt(*mesh, pieces, b, sz::kLineExtents) || yalms(a, b) < 10.0f)
                {
                    continue;
                }
                ++pairs;
                const auto route = cardian::route::searchAvoiding(*mesh, a, b, sz::kLineExtents, sz::kLineExtents, {}, {}, 0.0f, &faces);
                if (!route.has_value() || !route->complete)
                {
                    ++refused;
                    closed += fmt::format(" {} ({:.0f},{:.0f},{:.0f})->({:.0f},{:.0f},{:.0f});", name, a.x, a.y, a.z, b.x, b.y, b.z);
                }
            }
        }
    }
    WARN(fmt::format("{} zones, {} walks between lines the sub-zones join, {} closed by the slope rule:{}", zones, pairs, refused, closed));
}

// How steep each yalm of a few walks in Valkurm Dunes climbs, read off the
// mesh's detail: the measurements behind kClimbSlope, run again after any
// change to it
TEST_CASE("Route slopes (opt-in, CARDIAN_SLOPES=1)", "[cardian][route]")
{
    if (std::getenv("CARDIAN_SLOPES") == nullptr)
    {
        SKIP("the slope survey runs with CARDIAN_SLOPES=1");
    }
    const auto mesh = loadMesh("Valkurm_Dunes");
    REQUIRE(mesh);
    const auto faces = cardian::route::facesOf(*mesh);
    dtNavMeshQuery query;
    REQUIRE(dtStatusSucceed(query.init(mesh.get(), 2048)));
    dtQueryFilter filter;
    filter.setIncludeFlags(0xFFFF);
    filter.setExcludeFlags(cardian::route::kPolyDisabled);

    // The ground's height under (x, z), near `nearY` (the zone's y, down)
    const auto ground = [&](const float x, const float z, const float nearY) -> std::optional<float>
    {
        const float  at[3]{ x, -nearY, -z };
        const float  ext[3]{ 1.0f, 6.0f, 1.0f };
        dtPolyRef    ref{};
        float        on[3]{};
        if (dtStatusFailed(query.findNearestPoly(at, ext, &filter, &ref, on)) || !ref)
        {
            return std::nullopt;
        }
        float h = 0.0f;
        if (dtStatusFailed(query.getPolyHeight(ref, on, &h)))
        {
            return std::nullopt;
        }
        return -h;
    };

    // A walk's route, sampled a yalm at a time: the steepest climbs up
    const auto survey = [&](const std::string& what, const position_t& from, const position_t& to)
    {
        const auto route = cardian::route::searchAvoiding(*mesh, from, to, sz::kBodyExtents, sz::kBodyExtents, {}, {}, 0.0f, &faces);
        REQUIRE(route.has_value());
        std::vector<float> ups;
        position_t         a = from;
        std::optional<float> lastY = ground(a.x, a.z, a.y);
        float lastX = a.x;
        float lastZ = a.z;
        std::string profile;
        for (const auto& b : route->points)
        {
            const float len   = std::sqrt((b.x - a.x) * (b.x - a.x) + (b.z - a.z) * (b.z - a.z));
            const int   steps = std::max(1, static_cast<int>(std::ceil(len)));
            for (int k = 1; k <= steps; ++k)
            {
                const float f = static_cast<float>(k) / static_cast<float>(steps);
                const float x = a.x + (b.x - a.x) * f;
                const float z = a.z + (b.z - a.z) * f;
                const auto  y = ground(x, z, a.y + (b.y - a.y) * f);
                if (y.has_value() && lastY.has_value())
                {
                    const float run  = std::sqrt((x - lastX) * (x - lastX) + (z - lastZ) * (z - lastZ));
                    const float rise = *lastY - *y; // the zone's y is down: a climb lowers it
                    if (run > 0.01f)
                    {
                        const float deg = std::atan2(rise, run) * 180.0f / 3.14159265f;
                        ups.push_back(deg);
                        if (deg > 30.0f)
                        {
                            profile += fmt::format(" ({:.0f},{:.0f}):{:.0f}", x, z, deg);
                        }
                    }
                }
                lastY = y;
                lastX = x;
                lastZ = z;
            }
            a = b;
        }
        std::sort(ups.begin(), ups.end());
        const auto pct = [&](const float p) { return ups.empty() ? 0.0f : ups[std::min(ups.size() - 1, static_cast<size_t>(p * ups.size()))]; };
        std::string points;
        for (const auto& p : route->points)
        {
            points += fmt::format(" ({:.1f},{:.1f},{:.1f})", p.x, p.y, p.z);
        }
        WARN(fmt::format("{}: {}{:.0f}y, {} links, {} yalms sampled; climbs: median {:.0f}, 90% {:.0f}, 99% {:.0f}, steepest {:.0f} degrees; over 30:{}; points:{}", what,
                         route->complete ? "" : "NOT COMPLETE ", route->length, route->links, ups.size(), pct(0.5f), pct(0.9f), pct(0.99f), ups.empty() ? 0.0f : ups.back(),
                         profile, route->points.size() <= 40 ? points : std::string(" (many)")));
    };

    // Jevyak's climb on 2026-10-12, which a player cannot make
    survey("the goblins' hill", position_t(691.7f, -2.9f, -201.3f, 0, 0), position_t(652.9f, -11.1f, -218.4f, 0, 0));
    // the player's climb up it, as far as he could go, and the rest of it
    survey("up to the player's highest", position_t(691.7f, -2.9f, -201.3f, 0, 0), position_t(672.1f, -4.98f, -217.08f, 0, 0));
    survey("from the player's highest to the top", position_t(672.1f, -4.98f, -217.08f, 0, 0), position_t(652.9f, -11.1f, -218.4f, 0, 0));
    // the cliff she climbed on 2026-10-12, near the La Theine line: from the player below to her, and on from her
    survey("the player below the cliff to her on it", position_t(606.98f, -16.82f, 287.74f, 0, 0), position_t(620.9f, -22.79f, 292.8f, 0, 0));
    survey("her on the cliff to the La Theine line", position_t(620.9f, -22.79f, 292.8f, 0, 0), position_t(760.4f, -12.0f, 405.0f, 0, 0));
    survey("the player below the cliff to the La Theine line", position_t(606.98f, -16.82f, 287.74f, 0, 0), position_t(760.4f, -12.0f, 405.0f, 0, 0));
    // ways players walk: the Konschtat line to the bat cave, the cave to the La Theine line
    survey("Konschtat line to the cave", position_t(919.0f, -4.6f, -426.0f, 0, 0), position_t(780.0f, -8.0f, -230.0f, 0, 0));
    survey("the goblins to the La Theine line", position_t(691.7f, -2.9f, -201.3f, 0, 0), position_t(760.4f, -12.0f, 405.0f, 0, 0));
}

TEST_CASE("Route kept out of danger: a slope the mesh joins only by a switched-off drop is dropped down, never climbed", "[cardian][route]")
{
    // Valkurm Dunes: a slope a player slides down, whose foot and top the
    // mesh's builder joins by drop links it switched off, a walk already
    // joining them -- three minutes round. Down, she takes the drop; up, the
    // drop is never climbed and she walks round
    const auto mesh = loadMesh("Valkurm_Dunes");
    if (!mesh)
    {
        SKIP("no navmesh for Valkurm_Dunes");
    }
    const auto       faces = cardian::route::facesOf(*mesh);
    const position_t top(354.4f, -15.2f, 192.9f, 0, 0);
    const position_t side(349.5f, -13.8f, 195.5f, 0, 0);
    const position_t bottom(345.0f, -9.7f, 189.1f, 0, 0);

    for (const auto& from : { top, side })
    {
        const auto down = cardian::route::searchAvoiding(*mesh, from, bottom, sz::kBodyExtents, sz::kLineExtents, {}, {}, 0.0f, &faces);
        REQUIRE(down.has_value());
        REQUIRE(down->complete);
        CHECK(down->length < 30.0f);
    }

    const auto up = cardian::route::searchAvoiding(*mesh, bottom, top, sz::kBodyExtents, sz::kLineExtents, {}, {}, 0.0f, &faces);
    if (up.has_value() && up->complete)
    {
        CHECK(up->length > 60.0f);
    }
}

namespace
{
    // The nearest ground of one piece to a point, within `extents` of it
    auto nearestOn(const dtNavMesh& mesh, const sz::Pieces& pieces, const sz::Piece piece, const position_t& point, const float* extents) -> std::optional<position_t>
    {
        dtNavMeshQuery query;
        if (dtStatusFailed(query.init(&mesh, 64)))
        {
            return std::nullopt;
        }
        dtQueryFilter filter;
        filter.setExcludeFlags(cardian::route::kPolyDisabled);
        const auto at = cardian::route::toDetour(point);
        dtPolyRef  refs[2048];
        int        count = 0;
        query.queryPolygons(at.data(), extents, &filter, refs, &count, 2048);
        std::optional<position_t> best;
        for (int i = 0; i < count; ++i)
        {
            float closest[3]{};
            if (pieces.of(mesh, refs[i]) != piece || dtStatusFailed(query.closestPointOnPoly(refs[i], at.data(), closest, nullptr)))
            {
                continue;
            }
            const auto onIt = cardian::route::fromDetour(closest);
            if (!best.has_value() || yalms(onIt, point) < yalms(*best, point))
            {
                best = onIt;
            }
        }
        return best;
    }

    // From the main ground beside a zone line -- `body`, when given -- the
    // walk to the line's zone takes that very line: she walks to a spot the
    // walker finds on her own ground, and the line sets her down on the
    // piece its arrival is placed on
    void takesLine(const char* what, const std::pair<xi::ZoneId, const char*> from, const std::pair<xi::ZoneId, const char*> to, const position_t& trigger,
                   const std::optional<position_t> body = std::nullopt)
    {
        const auto region = regionOf({ from, to });
        if (!region.has_value())
        {
            SKIP("a navmesh is missing for " << what);
        }
        INFO(what);
        const auto  zone     = static_cast<uint16>(from.first);
        const auto  next     = static_cast<uint16>(to.first);
        const auto& mesh     = *region->meshes.at(zone);
        const auto& pieces   = region->pieces.at(zone);
        const auto  main     = mainPiece(pieces);
        const float beside[] = { 20.0f, 15.0f, 20.0f };
        const auto  at       = body.has_value() ? body : nearestOn(mesh, pieces, main, trigger, beside);
        REQUIRE(at.has_value());
        const auto here = region->piece(zone, *at);
        CHECK(here == main);

        const auto plan = sz::plan(region->world, zone, here, *at, next, std::nullopt, std::nullopt);
        REQUIRE(plan.reachable);
        REQUIRE(plan.firstLine.has_value());
        CHECK(plan.lines == 1);
        const auto& line = region->world.lines[*plan.firstLine];
        CHECK(yalms(line.origin, trigger) < 0.5f);

        // the walker's goal is the nearest ground to the spot (pawn_travel.cpp
        // routeAvoiding), and it is hers
        CHECK(line.leaves.piece == here);
        CHECK(sz::pieceAt(mesh, pieces, line.leaves.at, sz::kLineExtents) == here);

        // set down at the spot, she stands on the arrival's piece
        CHECK(line.lands.piece != 0);
        CHECK(region->piece(next, line.lands.at) == line.lands.piece);
    }
} // namespace

namespace
{
    // The pieces with ground within `extents` of a point, biggest first, by
    // their polygon count
    auto piecesNear(const dtNavMesh& mesh, const sz::Pieces& pieces, const position_t& point, const float* extents) -> std::vector<std::pair<int, sz::Piece>>
    {
        dtNavMeshQuery                         query;
        std::vector<std::pair<int, sz::Piece>> found;
        if (dtStatusFailed(query.init(&mesh, 64)))
        {
            return found;
        }
        dtQueryFilter filter;
        filter.setExcludeFlags(cardian::route::kPolyDisabled);
        const auto at = cardian::route::toDetour(point);
        dtPolyRef  refs[1024];
        int        count = 0;
        query.queryPolygons(at.data(), extents, &filter, refs, &count, 1024);
        std::map<sz::Piece, int> seen;
        for (int i = 0; i < count; ++i)
        {
            const auto piece = pieces.of(mesh, refs[i]);
            if (piece != 0)
            {
                seen[piece] = pieces.polys[piece];
            }
        }
        for (const auto& [piece, polys] : seen)
        {
            found.emplace_back(polys, piece);
        }
        std::sort(found.rbegin(), found.rend());
        return found;
    }

    // An end the survey calls odd: off the mesh, or on a piece with one ten
    // times its size within 12 yalms across and 30 up of its point
    constexpr float kOddNear[] = { 12.0f, 30.0f, 12.0f };

    auto oddEnd(const sz::Pieces& pieces, const sz::LineEnd& end, const std::vector<std::pair<int, sz::Piece>>& nearby) -> bool
    {
        return end.piece == 0 || (!nearby.empty() && end.piece != nearby[0].second && pieces.polys[end.piece] * 10 < nearby[0].first);
    }

    // An end the travel map uses at a spot other than its point: the ground
    // under it (subzones.h lineEnd)
    auto movedEnd(const position_t& point, const sz::LineEnd& end) -> bool
    {
        return yalms(point, end.at) > 0.01f;
    }

    // One end of a line, by its zone's folder name, which end, and the zone
    // at the line's other end, as the guard and the survey name it
    auto endName(const WholeWorld& whole, const sz::Line& line, const End which) -> std::string
    {
        const uint16 zone  = which == End::Origin ? line.zone : line.to;
        const uint16 other = which == End::Origin ? line.to : line.zone;
        return fmt::format("{} {} zone {}", whole.names.at(zone), which == End::Origin ? "origin to" : "arrival from", other);
    }
} // namespace

TEST_CASE("Zone lines: a trigger over the path is taken from the ground under it, not a scrap of mesh beside it", "[cardian][route]")
{
    // ROADMAP T, "A line's ends": Jugner Forest's line to Davoi floats 9
    // yalms over the path, a one-polygon scrap of mesh nearest it; the party
    // stood at the gate with no walk into Davoi
    takesLine("Jugner Forest to Davoi", { xi::ZoneId::JugnerForest, "Jugner_Forest" }, { xi::ZoneId::Davoi, "Davoi" }, { -240.440f, -16.683f, -597.493f, 0, 0 },
              position_t{ -241.5f, -8.1f, -593.6f, 0, 0 });
    takesLine("East Ronfaure to King Ranperre's Tomb", { xi::ZoneId::EastRonfaure, "East_Ronfaure" }, { xi::ZoneId::KingRanperresTomb, "King_Ranperres_Tomb" },
              { 199.982f, -8.500f, -544.574f, 0, 0 });
    takesLine("Beaucedine Glacier to Xarcabard", { xi::ZoneId::BeaucedineGlacier, "Beaucedine_Glacier" }, { xi::ZoneId::Xarcabard, "Xarcabard" },
              { -251.957f, -86.893f, 260.655f, 0, 0 });
}

TEST_CASE("Zone lines: West Ronfaure reaches Northern San d'Oria from its main ground", "[cardian][route]")
{
    // OPEN_ISSUES #493: one of West Ronfaure's lines to Northern San d'Oria
    // stands on ground the mesh leaves apart from the main ground; from the
    // main ground the walk takes the other
    takesLine("West Ronfaure to Northern San d'Oria", { xi::ZoneId::WestRonfaure, "West_Ronfaure" }, { xi::ZoneId::NorthernSanDoria, "Northern_San_dOria" },
              { -157.666f, -66.124f, 379.960f, 0, 0 });
}

// Every zone line of the world as the travel map places it, so a change to
// the placement, the meshes or the lines shows: the ends it uses at the
// ground under their point named (the six triggers that float over the path
// beside a scrap of mesh, ROADMAP T, "A line's ends"), the ends the survey
// calls odd counted, and the ends on ground their zone's main ground does not
// join counted. Most of those last stand on a region of their own, as they
// should (an arena, a reach of a zone only the lines lead to); the rest are
// the meshes' own faults (OPEN_ISSUES #493). The opt-in surveys below name
// every one, in log/lines_survey.txt and log/lines_islands.txt
TEST_CASE("Zone lines: the ends moved under their point named, and the odd ends and the ends apart from their zone's main ground counted", "[cardian][route]")
{
    // Of the 1,394 ends in zones with a mesh -- 697 origins and 697
    // arrivals -- measured 2026-10-15
    const std::vector<std::string> kMoved = {
        "alzadaal_undersea_ruins origin to zone 79", "beaucedine_glacier origin to zone 112", "dangruf_wadi origin to zone 106",
        "east_ronfaure origin to zone 190",          "jugner_forest origin to zone 149",     "velugannon_palace origin to zone 130",
    };
    constexpr int kOdd           = 75;
    constexpr int kOriginsApart  = 265;
    constexpr int kArrivalsApart = 224;

    const auto whole = wholeWorld();
    if (whole->meshes.empty())
    {
        SKIP("no navmeshes");
    }
    std::vector<std::string>                            moved;
    int                                                 ends          = 0;
    int                                                 odd           = 0;
    int                                                 originsApart  = 0;
    int                                                 arrivalsApart = 0;
    std::map<std::pair<uint16, End>, std::vector<bool>> cache;
    const auto                                          look = [&](const sz::Line& line, const End which)
    {
        const uint16 zone  = which == End::Origin ? line.zone : line.to;
        const auto&  point = which == End::Origin ? line.origin : line.arrival;
        const auto&  end   = which == End::Origin ? line.leaves : line.lands;
        if (!whole->meshes.contains(zone) || mainPiece(whole->pieces.at(zone)) == 0)
        {
            return;
        }
        ++ends;
        const auto& pieces = whole->pieces.at(zone);
        if (movedEnd(point, end))
        {
            moved.push_back(endName(*whole, line, which));
        }
        if (oddEnd(pieces, end, piecesNear(*whole->meshes.at(zone), pieces, point, kOddNear)))
        {
            ++odd;
        }
        if (!joinsMain(*whole, zone, end, which, cache))
        {
            ++(which == End::Origin ? originsApart : arrivalsApart);
        }
    };
    for (const auto& line : whole->world.lines)
    {
        look(line, End::Origin);
        look(line, End::Arrival);
    }
    std::sort(moved.begin(), moved.end());
    std::string movedText;
    for (const auto& name : moved)
    {
        movedText += "\n  " + name;
    }
    INFO(ends << " line ends; the opt-in surveys (CARDIAN_LINES=1) name every end in log/lines_survey.txt and log/lines_islands.txt");
    INFO("used at the ground under their point:" << movedText);
    CHECK(moved == kMoved);
    INFO("odd: " << odd);
    CHECK(odd == kOdd);
    INFO("apart from their zone's main ground: " << originsApart << " origins, " << arrivalsApart << " arrivals");
    CHECK(originsApart == kOriginsApart);
    CHECK(arrivalsApart == kArrivalsApart);
}

// Where the travel map places every zone line's two ends (subzones.h
// lineEnd), beside the ground straight under each and the pieces near it.
// An end is marked MOVED when the map uses it at the ground under its point,
// and ODD as the guard above counts it. Written to log/lines_survey.txt
// (OPEN_ISSUES #493)
TEST_CASE("Zone lines: where each end is placed (opt-in, CARDIAN_LINES=1)", "[cardian][route]")
{
    if (std::getenv("CARDIAN_LINES") == nullptr)
    {
        SKIP("the line survey runs with CARDIAN_LINES=1");
    }
    std::ofstream out("log/lines_survey.txt");
    const auto    whole = wholeWorld();

    // The ground straight under a point: its piece, and where
    const float kColumn[] = { 1.5f, 60.0f, 1.5f };
    struct Hit
    {
        sz::Piece  piece = 0;
        position_t at{};
        int        polys = 0;
    };
    const auto column = [&](const uint16 zone, const position_t& p) -> Hit
    {
        const auto&    mesh   = *whole->meshes.at(zone);
        const auto&    pieces = whole->pieces.at(zone);
        dtNavMeshQuery query;
        Hit            hit;
        if (dtStatusFailed(query.init(&mesh, 64)))
        {
            return hit;
        }
        dtQueryFilter filter;
        filter.setExcludeFlags(cardian::route::kPolyDisabled);
        const auto at  = cardian::route::toDetour(p);
        dtPolyRef  ref = 0;
        float      nearest[3]{};
        if (dtStatusFailed(query.findNearestPoly(at.data(), kColumn, &filter, &ref, nearest)) || !ref)
        {
            return hit;
        }
        hit.piece = pieces.of(mesh, ref);
        hit.at    = cardian::route::fromDetour(nearest);
        hit.polys = pieces.polys[hit.piece];
        return hit;
    };

    int        ends  = 0;
    int        odd   = 0;
    int        moved = 0;
    const auto survey = [&](const sz::Line& line, const End which)
    {
        const uint16 zone  = which == End::Origin ? line.zone : line.to;
        const uint16 other = which == End::Origin ? line.to : line.zone;
        const auto&  p     = which == End::Origin ? line.origin : line.arrival;
        const auto&  end   = which == End::Origin ? line.leaves : line.lands;
        if (!whole->meshes.contains(zone))
        {
            return;
        }
        ++ends;
        const auto& pieces = whole->pieces.at(zone);
        const auto  under  = column(zone, p);
        const auto  nearby = piecesNear(*whole->meshes.at(zone), pieces, p, kOddNear);
        std::string text   = fmt::format("({:.1f}, {:.1f}, {:.1f}) placed on piece {} ({} polys) at ({:.1f}, {:.1f}, {:.1f}), {:.1f}y off; under it piece {} ({} polys) at y {:.1f}; nearby:",
                                         p.x, p.y, p.z, end.piece, pieces.polys[end.piece], end.at.x, end.at.y, end.at.z, yalms(p, end.at), under.piece, under.polys,
                                         under.at.y);
        for (std::size_t i = 0; i < nearby.size() && i < 4; ++i)
        {
            text += fmt::format(" {}({})", nearby[i].second, nearby[i].first);
        }
        const bool isMoved = movedEnd(p, end);
        const bool isOdd   = oddEnd(pieces, end, nearby);
        moved += isMoved ? 1 : 0;
        odd += isOdd ? 1 : 0;
        out << (isMoved ? "MOVED " : isOdd ? "ODD   " : "ok    ") << whole->names.at(zone) << " (" << zone << ") " << (which == End::Origin ? "-> " : "<- ") << other
            << (which == End::Origin ? " origin  " : " arrival ") << text << (isMoved && isOdd ? " (odd)" : "") << "\n";
    };
    for (const auto& line : whole->world.lines)
    {
        survey(line, End::Origin);
        survey(line, End::Arrival);
    }
    out << "\n" << ends << " line ends, " << moved << " used at the ground under their point, " << odd << " odd\n";
    WARN("line survey written to log/lines_survey.txt: " << ends << " ends, " << moved << " moved, " << odd << " odd");
}

// Which zone line ends stand on ground their zone's main ground does not
// join, each placed as the travel map places it: an origin on a piece the
// main ground does not reach by walking and drops, an arrival on a piece
// that does not reach it, with how near its piece and the main ground come.
// Written to log/lines_islands.txt (OPEN_ISSUES #493)
TEST_CASE("Zone lines: islands (opt-in, CARDIAN_LINES=1)", "[cardian][route]")
{
    if (std::getenv("CARDIAN_LINES") == nullptr)
    {
        SKIP("the line survey runs with CARDIAN_LINES=1");
    }
    std::ofstream out("log/lines_islands.txt");
    const auto    whole = wholeWorld();

    int                                                 origins = 0, originIslands = 0, arrivals = 0, arrivalIslands = 0;
    std::map<std::pair<uint16, End>, std::vector<bool>> cache;
    const auto                                          survey = [&](const sz::Line& line, const End which)
    {
        const uint16 zone  = which == End::Origin ? line.zone : line.to;
        const uint16 other = which == End::Origin ? line.to : line.zone;
        const auto&  p     = which == End::Origin ? line.origin : line.arrival;
        const auto&  end   = which == End::Origin ? line.leaves : line.lands;
        if (!whole->meshes.contains(zone))
        {
            return;
        }
        const auto& pieces = whole->pieces.at(zone);
        const auto  main   = mainPiece(pieces);
        if (main == 0)
        {
            return;
        }
        ++(which == End::Origin ? origins : arrivals);
        if (joinsMain(*whole, zone, end, which, cache))
        {
            return;
        }
        ++(which == End::Origin ? originIslands : arrivalIslands);
        out << (which == End::Origin ? "ORIGIN  " : "ARRIVAL ") << whole->names.at(zone) << " (" << zone << ") " << (which == End::Origin ? "-> " : "<- ") << other
            << fmt::format(" at ({:.1f}, {:.1f}, {:.1f}): piece {} ({} polys), main {} ({} polys): ", p.x, p.y, p.z, end.piece, pieces.polys[end.piece], main,
                           pieces.polys[main])
            << (pieces.polys[end.piece] < 600 ? closestSpots(*whole->meshes.at(zone), pieces, end.piece, main) : std::string("a large piece of its own")) << "\n";
    };
    for (const auto& line : whole->world.lines)
    {
        survey(line, End::Origin);
        survey(line, End::Arrival);
    }
    out << "\n"
        << origins << " origins, " << originIslands << " on ground the zone's main ground does not reach; " << arrivals << " arrivals, " << arrivalIslands
        << " on ground that does not reach it\n";
    WARN("islands written to log/lines_islands.txt: " << originIslands << " of " << origins << " origins, " << arrivalIslands << " of " << arrivals << " arrivals");
}
