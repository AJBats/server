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

#include "map/pawn/junk_math.h"

#include <catch2/catch_test_macros.hpp>

#include <fstream>
#include <iterator>
#include <string>

using cardian::junk::isJunk;
using cardian::junk::Item;
using cardian::junk::Sources;

namespace
{
    constexpr uint16 kOre     = 643;  // a raw material: used, never made
    constexpr uint16 kIngot   = 651;  // made by a recipe, and used by others
    constexpr uint16 kFeather = 847;  // a raw material a quest asks for, junk all the same
    constexpr uint16 kRing    = 13454; // gear a recipe uses

    auto sources() -> Sources
    {
        Sources s;
        s.used = { kOre, kIngot, kFeather, kRing };
        s.made = { kIngot };
        return s;
    }

    auto material(const uint16 id) -> Item
    {
        return { .id = id, .vendorBuys = true };
    }
} // namespace

TEST_CASE("Junk: a raw material a vendor buys", "[cardian][junk]")
{
    const auto s = sources();
    CHECK(isJunk(material(kOre), s));
    CHECK(isJunk(material(kFeather), s));           // a quest's asking for it changes nothing
    CHECK_FALSE(isJunk(material(kIngot), s));       // a recipe makes it
    CHECK_FALSE(isJunk(material(9999), s));         // no recipe uses it
    CHECK_FALSE(isJunk({ .id = kOre, .vendorBuys = false }, s));
}

TEST_CASE("Junk: never gear, an Ex item or a Rare item", "[cardian][junk]")
{
    auto s = sources();
    s.junk = { kRing, kOre };
    CHECK_FALSE(isJunk({ .id = kRing, .equipment = true, .vendorBuys = true }, s));
    CHECK_FALSE(isJunk({ .id = kOre, .ex = true, .vendorBuys = true }, s));
    CHECK_FALSE(isJunk({ .id = kOre, .rare = true, .vendorBuys = true }, s));
}

TEST_CASE("Junk: the hand-kept exceptions say otherwise either way", "[cardian][junk]")
{
    auto s = sources();
    s.keep = { kOre };
    s.junk = { kIngot };
    CHECK_FALSE(isJunk(material(kOre), s));
    CHECK(isJunk(material(kIngot), s));
    CHECK(isJunk({ .id = kIngot, .vendorBuys = false }, s)); // named junk: sold or banked, whichever his setting says
}

TEST_CASE("Junk: the hand-kept file read, comments and all", "[cardian][junk]")
{
    std::string error;
    const auto  some = cardian::junk::parse<cardian::junk::files::ExceptionFile>("# kept\nkeep:\n  - 640  # COPPER\njunk: []\n", error);
    REQUIRE(some.has_value());
    CHECK(some->keep == std::vector<uint16>{ 640 });

    const auto none = cardian::junk::parse<cardian::junk::files::ExceptionFile>("keep: []\njunk: []\n", error);
    REQUIRE(none.has_value());
    CHECK(none->keep.empty());

    CHECK_FALSE(cardian::junk::parse<cardian::junk::files::ExceptionFile>("keeps: [1]\n", error).has_value());
}

TEST_CASE("Junk: the module's own file parses", "[cardian][junk]")
{
    std::ifstream in("./modules/cardian/junk.yaml", std::ios::binary);
    REQUIRE(in);
    const std::string text{ std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>() };
    std::string       error;
    const bool        ok = cardian::junk::parse<cardian::junk::files::ExceptionFile>(text, error).has_value();
    INFO("junk.yaml: " << error);
    CHECK(ok);
}
