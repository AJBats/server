// Cardian: the chocobo porter (pawn/porter_math.h), and the trek's approach to
// its zone line (pawn/line_approach.h): the places he is offered from where
// he stands, nearest first, and each ride's time against what is left on his
// rental; when a ride is over; what the next zone line asks of a rider; the
// route his addon steers him along -- sent a stretch at a time, followed,
// strayed from -- and a snag on it: planned round first, set down past
// second, the ride given up after a few; and when a trek takes a line from
// short of it.
#include "pawn/line_approach.h"
#include "pawn/porter_math.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <chrono>
#include <limits>
#include <unordered_map>
#include <vector>

using namespace cardian::porter;
using Catch::Matchers::WithinAbs;

namespace
{
    // A city (zone 230) beside a field (zone 100) that allows a mount, a
    // dungeon (zone 150) that allows none, and a far field (zone 103)
    auto world() -> std::vector<Place>
    {
        return {
            Place{ .number = 0, .kind = Kind::City, .zone = 230 },
            Place{ .number = 1, .kind = Kind::Outpost, .zone = 103, .key = 1, .x = 148.0f, .y = -8.0f, .z = 93.0f },
            Place{ .number = 2, .kind = Kind::Crag, .zone = 100, .key = 2, .x = 420.0f, .y = 19.0f, .z = 20.0f },
            Place{ .number = 3, .kind = Kind::Outpost, .zone = 150, .key = 9 },
            Place{ .number = 4, .kind = Kind::Crag, .zone = 199, .key = 5 }, // no line reaches it
        };
    }

    auto allowsMount(const uint16_t zone) -> bool
    {
        return zone == 100 || zone == 103;
    }

    // A route along the ground, x then z, its first point where he set out
    struct OnGround
    {
        float x = 0.0f;
        float z = 0.0f;
    };
    auto route(std::initializer_list<OnGround> points) -> std::vector<Point>
    {
        std::vector<Point> out;
        for (const auto& p : points)
        {
            out.push_back(Point{ p.x, 0.0f, p.z });
        }
        return out;
    }
} // namespace

TEST_CASE("Porter: the places offered, nearest first, none he cannot ride to", "[cardian][porter]")
{
    const std::unordered_map<uint16_t, uint32_t> hops{ { 100, 0 }, { 230, 1 }, { 150, 1 }, { 103, 2 } };
    const auto                                    offers = offered(world(), 100, hops, allowsMount);

    REQUIRE(offers.size() == 3);
    // the crag in his own zone, then the city through its gate, then the far outpost
    CHECK(offers[0].place.number == 2);
    CHECK(offers[0].hops == 0);
    CHECK(offers[1].place.number == 0);
    CHECK(offers[1].hops == 1);
    CHECK(offers[2].place.number == 1);
    CHECK(offers[2].hops == 2);
    // the outpost in a dungeon, which allows no mount, and the crag no line reaches, are left out
    for (const auto& offer : offers)
    {
        CHECK(offer.place.number != 3);
        CHECK(offer.place.number != 4);
    }

    // standing in the city, it is no ride
    const auto fromCity = offered(world(), 230, { { 230, 0 }, { 100, 1 } }, allowsMount);
    for (const auto& offer : fromCity)
    {
        CHECK(offer.place.kind != Kind::City);
    }
}

TEST_CASE("Porter: a ride's time, and whether his rental covers it with a tenth to spare", "[cardian][porter]")
{
    // the planner's yalms ridden at 8 yalms a second, rounded up
    CHECK(rideSeconds(800.0f, 8.0f) == 100);
    CHECK(rideSeconds(801.0f, 8.0f) == 101);
    CHECK(rideSeconds(0.0f, 8.0f) == 0);
    CHECK(rideSeconds(800.0f, 0.0f) == kNoTime);         // no pace: too long to count
    CHECK(rideSeconds(1.0e7f, 8.0f) == kNoTime);         // beyond what a second's count holds
    CHECK(rideSeconds(65534.0f * 8.0f, 8.0f) == 65534); // the longest it counts

    // a 100 s ride wants 110 s left on the rental
    CHECK(rentalCovers(100, 110.5f));
    CHECK_FALSE(rentalCovers(100, 109.5f));
    CHECK_FALSE(rentalCovers(100, 100.0f)); // the bare time is not enough: it would end a few yalms short
    CHECK(rentalCovers(0, 0.0f));

    // a rental of 30 minutes, 25 of them left: a 20-minute ride is covered, 23 minutes is not
    CHECK(rentalCovers(20 * 60, 25.0f * 60.0f));
    CHECK_FALSE(rentalCovers(23 * 60, 25.0f * 60.0f));

    // a mount with no end covers any ride; one too long to count is never covered by a rental
    CHECK(rentalCovers(kNoTime, std::nullopt));
    CHECK_FALSE(rentalCovers(kNoTime, 1.0e9f));
}

TEST_CASE("Porter: a city is reached in its zone, any other place at its spot", "[cardian][porter]")
{
    const auto places = world();
    CHECK(arrived(places[0], 230, 500.0f));
    CHECK_FALSE(arrived(places[0], 100, 0.0f));
    CHECK(arrived(places[2], 100, kArriveYalms));
    CHECK_FALSE(arrived(places[2], 100, kArriveYalms + 0.5f));
    CHECK_FALSE(arrived(places[2], 103, 0.0f));
}

TEST_CASE("Porter: a zone line is ridden through, got down at for the city's gate, or no way on", "[cardian][porter]")
{
    CHECK(atLine(true, false) == AtLine::RideOn);
    CHECK(atLine(true, true) == AtLine::RideOn); // a city that allows a mount is ridden into
    CHECK(atLine(false, true) == AtLine::GetDown);
    CHECK(atLine(false, false) == AtLine::NoWayOn); // a dungeon on the way: the porter takes him through none
}

TEST_CASE("Porter: why a ride ends", "[cardian][porter]")
{
    CHECK_FALSE(ends(State{}).has_value());

    CHECK(ends(State{ .arrived = true }) == End::Arrived);
    CHECK(ends(State{ .arrived = true, .mounted = false }) == End::Arrived); // arrived on foot through a city's gate

    // off his chocobo -- its rental run out -- but for the city's gate, where the porter got him down
    CHECK(ends(State{ .mounted = false }) == End::OnFoot);
    CHECK_FALSE(ends(State{ .mounted = false, .atCityGate = true }).has_value());
    CHECK(ends(State{ .dead = true }) == End::OnFoot);

    CHECK(ends(State{ .mobOn = true }) == End::Attacked);
    CHECK(ends(State{ .mounted = false, .mobOn = true }) == End::OnFoot);
}

TEST_CASE("Porter: a route is sent a stretch at a time, the next as he nears its end", "[cardian][porter]")
{
    // a short route goes whole, its last point what the route's is
    auto s = segment(10, 1, Ends::Line);
    CHECK(s.from == 1);
    CHECK(s.to == 10);
    CHECK(s.ends == Ends::Line);

    // a long one goes kWindow at a time, more to follow
    s = segment(100, 1, Ends::Stop);
    CHECK(s.from == 1);
    CHECK(s.to == 1 + kWindow);
    CHECK(s.ends == Ends::More);

    // the last stretch ends as the route does
    s = segment(100, 80, Ends::Stop);
    CHECK(s.to == 100);
    CHECK(s.ends == Ends::Stop);

    // past its end, nothing more is sent
    s = segment(10, 12, Ends::Line);
    CHECK(s.from == 10);
    CHECK(s.to == 10);

    // the next stretch is due once he rides to a waypoint within kRefillWithin of the last sent
    CHECK_FALSE(refill(100, 1, 1 + kWindow));
    CHECK_FALSE(refill(100, 1 + kWindow - kRefillWithin - 1, 1 + kWindow));
    CHECK(refill(100, 1 + kWindow - kRefillWithin, 1 + kWindow));
    CHECK_FALSE(refill(33, 30, 33)); // the whole route is sent
}

TEST_CASE("Porter: his progress along the route, and straying from it", "[cardian][porter]")
{
    // east 20 yalms, then north 20
    const auto way = route({ { 0, 0 }, { 20, 0 }, { 20, 20 } });

    // riding the first leg, he rides to the corner
    CHECK(progress(way, 1, Point{ 10, 0, 0.5f }) == 1);
    CHECK_THAT(offRoute(way, 1, Point{ 10, 0, 0.5f }), WithinAbs(0.5f, 0.001f));
    CHECK_FALSE(strayed(way, 1, Point{ 10, 0, 0.5f }));

    // within reach of the corner, he rides on to the end
    CHECK(progress(way, 1, Point{ 19, 0, 0.5f }) == 2);
    // round it and up the second leg: the nearer leg is his
    CHECK(progress(way, 1, Point{ 20.5f, 0, 10 }) == 2);
    // at the end, past every waypoint
    CHECK(progress(way, 2, Point{ 20, 0, 19 }) == 3);
    CHECK(offRoute(way, 3, Point{ 50, 0, 50 }) == 0.0f); // past the end nothing is strayed from

    // the height aside: his client stands a yalm above the mesh
    CHECK_THAT(offRoute(way, 1, Point{ 10, 1.5f, 0 }), WithinAbs(0.0f, 0.001f));

    // more than kStrayYalms off the leg he rides, he has strayed
    CHECK_FALSE(strayed(way, 1, Point{ 10, 0, kStrayYalms - 0.1f }));
    CHECK(strayed(way, 1, Point{ 10, 0, kStrayYalms + 0.1f }));

    // at the end: past its last waypoint, or run past it within kEndReach --
    // where his addon stops -- but not short of it
    CHECK(atEnd(way, 3, Point{ 20, 0, 18.5f }));
    CHECK_FALSE(atEnd(way, 2, Point{ 20, 0, 17 }));            // 3 yalms short of it, still riding to it
    CHECK(atEnd(way, 2, Point{ 20.5f, 0, 23 }));               // run 3 yalms past it
    CHECK_FALSE(atEnd(way, 2, Point{ 20, 0, 20 + kEndReach + 0.5f })); // run on far past it
    CHECK(atEnd({}, 0, Point{}));

    // run on through a zone line: beyond its end, however far within the bound
    CHECK(beyondEnd(way, Point{ 20, 0, 50 }, 40.0f));
    CHECK_FALSE(beyondEnd(way, Point{ 20, 0, 70 }, 40.0f));
    CHECK_FALSE(beyondEnd(way, Point{ 20, 0, 15 }, 40.0f)); // short of it
}

TEST_CASE("Porter: a leg on a cliff's top above him is not his, however near in plan", "[cardian][porter]")
{
    // north along the cliff's foot, up round its end, and back south along its
    // top, 4 yalms above the foot and 10 yalms west of it
    const std::vector<Point> way{ { 0, 7, 0 }, { 0, 7, 20 }, { -10, 3, 20 }, { -10, 3, 0 } };

    // at the foot, 2 yalms from the top's leg in plan and 8 from his own: the
    // top's is refused for its height, and his own, past kStrayYalms, kept
    CHECK(progress(way, 1, Point{ -8, 7, 2 }) == 1);
    // nor is a waypoint on the top reached from under it
    CHECK(progress(way, 3, Point{ -10, 7, 0.5f }) == 3);
    // once up there, the top's leg is his
    CHECK(progress(way, 1, Point{ -9.5f, 3, 10 }) == 3);
}

TEST_CASE("Porter: a long leg over a rise is his by the ground's height, not its ends'", "[cardian][porter]")
{
    // a short leg, then 40 yalms north over a rise 5 yalms above both its
    // ends, then east
    const std::vector<Point> way{ { 0, 0, 0 }, { 0, 0, 10 }, { 0, 0, 50 }, { 10, 0, 50 } };
    Ground                   ground(way.size());
    ground[2] = { 0, -2.5f, -5, -2.5f, 0 };

    // on top of the rise, halfway along: the straight line between its ends
    // passes 5 yalms under him; the ground under it does not
    CHECK(progress(way, 1, Point{ 0, -5, 30 }) == 1);
    CHECK(progress(way, 1, Point{ 0, -5, 30 }, ground) == 2);
    CHECK_THAT(groundAt(way, ground, 2, 0.5f), WithinAbs(-5.0f, 0.001f));
    CHECK_THAT(groundAt(way, ground, 2, 0.375f), WithinAbs(-3.75f, 0.001f));
    // a leg with no heights keeps the straight line
    CHECK_THAT(groundAt(way, ground, 3, 0.5f), WithinAbs(0.0f, 0.001f));
}

TEST_CASE("Porter: a far leg on his level never takes him past the leg he rides", "[cardian][porter]")
{
    // a long leg whose ground is not known, over a rise; the legs after it
    // run 120 yalms on, one of them at his height
    const std::vector<Point> way{ { 0, 0, 0 }, { 0, -10, 100 }, { 0, -6, 140 }, { 0, -6, 150 } };

    // 20 yalms along the first, 4 above its straight line: no leg near him
    // is on his level, and the far one at his height is not his
    CHECK(progress(way, 1, Point{ 0, -6, 20 }) == 1);
    CHECK_FALSE(strayed(way, 1, Point{ 0, -6, 20 }));
}

TEST_CASE("Porter: a few yalms on along the route, for a set-down", "[cardian][porter]")
{
    const auto way = route({ { 0, 0 }, { 20, 0 }, { 20, 20 } });

    // on the first leg, 5 on
    auto on = along(way, 1, Point{ 4, 0, 1 }, 5.0f);
    CHECK_FALSE(on.pastEnd);
    CHECK_THAT(on.at.x, WithinAbs(9.0f, 0.001f));
    CHECK_THAT(on.at.z, WithinAbs(0.0f, 0.001f));

    // round the corner
    on = along(way, 1, Point{ 18, 0, 0 }, 5.0f);
    CHECK_FALSE(on.pastEnd);
    CHECK_THAT(on.at.x, WithinAbs(20.0f, 0.001f));
    CHECK_THAT(on.at.z, WithinAbs(3.0f, 0.001f));

    // the route runs out first: its end
    on = along(way, 2, Point{ 20, 0, 18 }, 5.0f);
    CHECK(on.pastEnd);
    CHECK_THAT(on.at.z, WithinAbs(20.0f, 0.001f));
}

TEST_CASE("Porter: a snag is planned round first, set down past second, and the ride given up after a few", "[cardian][porter]")
{
    Snags s;
    const Point spot{ 100, 0, 100 };

    // the first snag at a spot: planned round from there
    CHECK(snagged(s, spot) == SnagAnswer::Replan);
    // the second at the same spot (within kSnagSameSpot): set down past it
    CHECK(snagged(s, Point{ 101, 0, 101 }) == SnagAnswer::SetDown);
    setDown(s, Point{ 105, 0, 100 });
    CHECK(s.setDowns == 1);

    // where the set-down left him, a snag is a first one again
    CHECK(snagged(s, Point{ 105, 0, 100 }) == SnagAnswer::Replan);
    CHECK(snagged(s, Point{ 105, 0, 100 }) == SnagAnswer::SetDown);
    setDown(s, Point{ 110, 0, 100 });

    // a snag at another spot is a first one
    CHECK(snagged(s, Point{ 110, 0, 108 }) == SnagAnswer::Replan);
    CHECK(snagged(s, Point{ 110, 0, 108 }) == SnagAnswer::SetDown);
    setDown(s, Point{ 115, 0, 108 });
    CHECK(s.setDowns == kSetDownsMax);

    // with kSetDownsMax set-downs and no getting on, the next set-down due ends the ride
    CHECK(snagged(s, Point{ 115, 0, 108 }) == SnagAnswer::Replan);
    CHECK(snagged(s, Point{ 115, 0, 108 }) == SnagAnswer::GiveUp);

    // getting on -- kSnagProgress from where the last left him -- forgets them all
    Snags t;
    CHECK(snagged(t, spot) == SnagAnswer::Replan);
    CHECK(snagged(t, spot) == SnagAnswer::SetDown);
    setDown(t, Point{ 105, 0, 100 });
    rode(t, Point{ 105 + kSnagProgress - 1.0f, 0, 100 });
    CHECK(t.setDowns == 1); // not yet
    rode(t, Point{ 105 + kSnagProgress, 0, 100 });
    CHECK_FALSE(t.any);
    CHECK(t.setDowns == 0);
    CHECK(snagged(t, spot) == SnagAnswer::Replan);

    // snags at spot after spot, never twice at one, end the ride past kSnagsMax
    Snags u;
    for (uint8_t i = 0; i < kSnagsMax; ++i)
    {
        CHECK(snagged(u, Point{ 100.0f + 10.0f * (i % 2), 0, 100 }) == SnagAnswer::Replan);
    }
    CHECK(snagged(u, Point{ 100, 0, 100 }) == SnagAnswer::GiveUp);
}

TEST_CASE("Trek: a line is taken at its spot, or from short of it once the walk stalls within the slack", "[cardian][porter][route]")
{
    using namespace cardian::trek;
    using namespace std::chrono_literals;

    CHECK(reached(kTransferDistance - 0.1f));
    CHECK_FALSE(reached(kTransferDistance));

    position_t line{};
    line.x = 100.0f;
    const timer::time_point t0{};

    LineApproach approach;
    approach.watch(101, line, 30.0f, t0);
    CHECK_FALSE(approach.stalled(30.0f, t0 + 2s));
    CHECK(approach.stalled(30.0f, t0 + 4s)); // no headway for 3 s, within the slack

    // headway puts the stall off
    approach.watch(101, line, 29.0f, t0 + 4s);
    CHECK_FALSE(approach.stalled(29.0f, t0 + 5s));
    // a tenth of a yalm is no headway
    approach.watch(101, line, 28.9f, t0 + 6s);
    CHECK(approach.stalled(28.9f, t0 + 7500ms));

    // held at a choke or sneaking by: no stall
    approach.held(t0 + 7s);
    CHECK_FALSE(approach.stalled(28.9f, t0 + 9s));

    // beyond the slack nothing is taken from short of it
    LineApproach distant;
    distant.watch(101, line, kCrossingSlack + 5.0f, t0);
    CHECK_FALSE(distant.stalled(kCrossingSlack + 5.0f, t0 + 10s));

    // another line starts the watch afresh
    position_t other{};
    other.x = 500.0f;
    approach.watch(102, other, 20.0f, t0 + 20s);
    CHECK_FALSE(approach.stalled(20.0f, t0 + 21s));
    CHECK_THAT(approach.best, WithinAbs(20.0f, 0.001f));
}

TEST_CASE("Porter: strays that leave his way no shorter are a snag, though he never stood still", "[cardian][porter]")
{
    using namespace cardian::porter;

    // planned again after straying, the way left each time less than 3 yalms shorter than the best
    int n = 0;
    n     = stalls(n, 1151.0f, 1149.0f);
    CHECK(n == 1);
    n = stalls(n, 1148.0f, 1149.0f);
    CHECK(n == 2);
    n = stalls(n, 1148.0f, 1151.0f);
    CHECK(n == kStallsToSnag);

    // a replan that gains the margin starts the count again
    CHECK(stalls(2, 1148.0f, 1140.0f) == 0);
    CHECK(stalls(2, 1148.0f, 1145.0f + 0.01f) == 3);
    // the first plan in a zone has no best to gain on
    CHECK(stalls(0, std::numeric_limits<float>::max(), 900.0f) == 0);
}

TEST_CASE("Porter: each set-down on one ride goes further on", "[cardian][porter]")
{
    using namespace cardian::porter;
    CHECK_THAT(setDownAhead(1), WithinAbs(5.0f, 0.001f));
    CHECK_THAT(setDownAhead(2), WithinAbs(15.0f, 0.001f));
    CHECK_THAT(setDownAhead(3), WithinAbs(25.0f, 0.001f));
    CHECK_THAT(setDownAhead(0), WithinAbs(kSetDownAhead, 0.001f));
}

TEST_CASE("Porter: learned spots near each other are one stretch kept clear of, a lone one a small circle", "[cardian][porter]")
{
    using namespace cardian::porter;

    // La Theine's cliffside, learned on one ride: one far spot and four in a row
    const std::vector<Point> spots{ { -407.0f, 0.3f, 106.1f }, { -306.1f, -5.2f, 61.0f }, { -289.0f, -3.1f, 58.1f },
                                    { -264.7f, -3.8f, 53.2f }, { -251.9f, -4.9f, 54.4f } };
    const auto               areas = avoidAreas(spots);

    // the far spot alone, at its own small radius
    const auto lone = std::ranges::count_if(areas, [](const Circle& c) { return c.radius == kSnagAvoid; });
    CHECK(lone == 1);

    // the row is one chain: every point along it within a wide circle, no gap a route could thread
    for (float x = -306.0f; x <= -252.0f; x += 1.0f)
    {
        const Point probe{ x, -4.0f, 61.0f + (54.4f - 61.0f) * (x + 306.0f) / 54.0f };
        const bool  covered = std::ranges::any_of(areas, [&](const Circle& c) { return c.radius == kAreaRadius && flat(c.at, probe) <= kAreaRadius; });
        CHECK(covered);
    }

    // nothing joins the far spot to the row
    const Point between{ -356.0f, -2.0f, 83.0f };
    CHECK(std::ranges::none_of(areas, [&](const Circle& c) { return flat(c.at, between) <= c.radius; }));

    // none, nothing to keep clear of
    CHECK(avoidAreas({}).empty());
}
