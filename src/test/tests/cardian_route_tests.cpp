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
                region.world.lines.push_back({ .zone         = zone,
                                               .to           = to,
                                               .origin       = line.Origin,
                                               .arrival      = line.Destination,
                                               .originPiece  = sz::pieceAt(*region.meshes.at(zone), region.pieces.at(zone), line.Origin, sz::kLineExtents),
                                               .arrivalPiece = sz::pieceAt(*region.meshes.at(to), region.pieces.at(to), line.Destination, sz::kLineExtents) });
            }
            if (!region.pieces.at(zone).drops.empty())
            {
                region.world.drops[zone] = region.pieces.at(zone).drops;
            }
        }
        return region;
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
            const auto route = cardian::route::searchAvoiding(*mesh, c.from, c.to, sz::kLineExtents, sz::kLineExtents, hazards, sight);
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
                                                               sight, 150.0f * 1.5f + 60.0f);
        const auto us         = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - began).count();
        REQUIRE(stretch.has_value());
        WARN(c.zone << " stretch: " << us / 1000.0 << " ms, " << stretch->searched << " points searched, " << stretch->length << "y, complete "
                    << stretch->complete << ", danger at " << stretch->firstDanger);
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
