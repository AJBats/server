// Cardian: the automatic hold a warp sets (pawn/warp_hold.h): parted from the
// player by magic, either way, a cardian holds where she is; reunited, that
// hold lifts by itself; a hold the player ordered is his alone to lift; and a
// zone change is a walk only when his client walked into a zone line for it.
#include "pawn/warp_hold.h"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <optional>

using namespace cardian::hold;
using namespace std::chrono_literals;

TEST_CASE("Warp hold: parted by a warp, a following cardian holds, and the hold is the automatic one", "[cardian][warphold]")
{
    const Hold following{};
    const Hold held = warped(following);
    CHECK(held.on);
    CHECK_FALSE(held.ordered);
    CHECK(held.warp);
}

TEST_CASE("Warp hold: reunited, the automatic hold lifts by itself", "[cardian][warphold]")
{
    CHECK(reunited(warped(Hold{})) == Hold{});
    CHECK(reunited(Hold{}) == Hold{}); // following already: nothing to lift
}

TEST_CASE("Warp hold: a hold the player ordered is never lifted by a reunion", "[cardian][warphold]")
{
    const Hold his = ordered(true);
    CHECK(reunited(his) == his);
}

TEST_CASE("Warp hold: a warp leaves a hold the player ordered as it is", "[cardian][warphold]")
{
    const Hold his = ordered(true);
    CHECK(warped(his) == his);
    CHECK(reunited(warped(his)) == his); // so a reunion after the warp lifts nothing either
}

TEST_CASE("Warp hold: a second warp before the reunion keeps the automatic hold", "[cardian][warphold]")
{
    const Hold held = warped(Hold{});
    CHECK(warped(held) == held);
}

TEST_CASE("Warp hold: the player's follow order lifts any hold, and is no automatic hold", "[cardian][warphold]")
{
    CHECK(ordered(false) == Hold{});
}

TEST_CASE("Warp hold: a zone change is a walk only with a zone line packet just before it", "[cardian][warphold]")
{
    CHECK(walked(std::chrono::milliseconds(0)));
    CHECK(walked(std::chrono::duration_cast<std::chrono::milliseconds>(kZoneLineWindow)));
    CHECK_FALSE(walked(std::nullopt));                                                                       // no zone line: magic
    CHECK_FALSE(walked(std::chrono::duration_cast<std::chrono::milliseconds>(kZoneLineWindow) + 1ms));     // a stale one (a zone line refused, then a warp)
    CHECK_FALSE(walked(-1ms));
}
