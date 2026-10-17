// Cardian: a cardian's own errands in town (pawn/town_errands.h, the rules in
// pawn/town_errand_math.h): who goes and when she looks at her needs, what
// she needs, the order of her stops, her lines, and her errands going on when
// the player leaves her zone.
#include "map/pawn/town_errand_math.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cctype>
#include <set>
#include <string>
#include <vector>

using namespace cardian::town;

namespace
{
    // A member of his party in a city, free
    auto partyMember() -> WhoFacts
    {
        WhoFacts f;
        f.standing   = true;
        f.inCity     = true;
        f.inHisParty = true;
        return f;
    }

    // One of the world's at her town seat, a real player there to see her
    auto atHangout() -> WhoFacts
    {
        WhoFacts f;
        f.standing  = true;
        f.inCity    = true;
        f.worlds    = true;
        f.atHangout = true;
        f.seen      = true;
        return f;
    }

    auto lower(std::string text) -> std::string
    {
        std::ranges::transform(text, text.begin(), [](const unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return text;
    }

    // A whole word of the line, in lower case
    auto hasWord(const std::string_view line, const std::string& word) -> bool
    {
        std::string current;
        for (const char c : lower(std::string(line)) + " ")
        {
            if (std::isalpha(static_cast<unsigned char>(c)) != 0)
            {
                current += c;
                continue;
            }
            if (current == word)
            {
                return true;
            }
            current.clear();
        }
        return false;
    }
} // namespace

TEST_CASE("Town errands: a member of his party in a city goes, wherever he is", "[cardian][town]")
{
    // with him, or joined in another zone of her city, or crossing it on her
    // way to him: she gets ready where she stands
    CHECK(whoGoes(partyMember()) == Who::Party);

    auto field   = partyMember();
    field.inCity = false;
    CHECK(whoGoes(field) == Who::Nobody);

    auto down     = partyMember();
    down.standing = false;
    CHECK(whoGoes(down) == Who::Nobody);

    auto busy      = partyMember();
    busy.elsewhere = true; // the linkshell's errand, a trek (not hers to him), a walk someone gave her
    CHECK(whoGoes(busy) == Who::Nobody);
}

TEST_CASE("Town errands: one of the world's goes from her own hangout, seen", "[cardian][town]")
{
    CHECK(whoGoes(atHangout()) == Who::Hangout);

    auto unseen = atHangout();
    unseen.seen = false; // out of sight, the census sees to her
    CHECK(whoGoes(unseen) == Who::Nobody);

    auto walking      = atHangout();
    walking.atHangout = false; // on her way in, or out of town
    CHECK(whoGoes(walking) == Who::Nobody);

    auto held   = atHangout();
    held.worlds = false; // held for a player, or not one of the world's
    CHECK(whoGoes(held) == Who::Nobody);

    // In a party, the party's rule decides: one of the world's invited is
    // his party member, wherever he is
    auto invited       = atHangout();
    invited.inHisParty = true;
    CHECK(whoGoes(invited) == Who::Party);
}

TEST_CASE("Town errands: a fight or his order interrupts; an action or a rest only delays", "[cardian][town]")
{
    CHECK_FALSE(interrupted(Busy{}));
    CHECK(setsOutNow(Busy{}, Who::Party));

    CHECK(interrupted(Busy{ .fighting = true }));
    CHECK(interrupted(Busy{ .ordered = true }));
    CHECK_FALSE(setsOutNow(Busy{ .fighting = true }, Who::Party));
    CHECK_FALSE(setsOutNow(Busy{ .ordered = true }, Who::Hangout));

    CHECK_FALSE(interrupted(Busy{ .acting = true }));
    CHECK_FALSE(setsOutNow(Busy{ .acting = true }, Who::Party));
    CHECK_FALSE(interrupted(Busy{ .resting = true }));
    CHECK_FALSE(setsOutNow(Busy{ .resting = true }, Who::Party));
    // a seat's kneeling pose keeps one of the world's from nothing
    CHECK(setsOutNow(Busy{ .resting = true }, Who::Hangout));
}

TEST_CASE("Town errands: she looks as she arrives and as she becomes free, not again", "[cardian][town]")
{
    const uint16_t sandoria = 230;
    const uint16_t port     = 232;
    // arriving free in a zone of a city
    CHECK(looksNow(Watch{ port, true, Who::Party }, sandoria, true, Who::Party));
    CHECK(looksNow(Watch{ 0, false }, sandoria, true, Who::Party));
    // free again: out of a fight
    CHECK(looksNow(Watch{ sandoria, false, Who::Party }, sandoria, true, Who::Party));
    // free all along: done, or nothing here sees to her need
    CHECK_FALSE(looksNow(Watch{ sandoria, true, Who::Party }, sandoria, true, Who::Party));
    CHECK_FALSE(looksNow(Watch{ sandoria, true, Who::Hangout }, sandoria, true, Who::Hangout));
    // one of the world's at her seat invited: his party's now, she looks afresh
    CHECK(looksNow(Watch{ sandoria, true, Who::Hangout }, sandoria, true, Who::Party));
    // not free: no look
    CHECK_FALSE(looksNow(Watch{ port, true, Who::Party }, sandoria, false, Who::Nobody));
    CHECK_FALSE(looksNow(Watch{ sandoria, false }, sandoria, false, Who::Nobody));
}

TEST_CASE("Town errands: what she needs", "[cardian][town]")
{
    // one of his own buys her scrolls, nothing else: her gear and jobs are his
    CHECK(needsOf(NeedFacts{ .worlds = false, .risen = true, .lacksScrolls = true, .careerJobs = true }) == kScrolls);
    CHECK(needsOf(NeedFacts{ .worlds = false, .risen = true, .careerJobs = true }) == 0);

    // one of the world's sees to all three
    CHECK(needsOf(NeedFacts{ .worlds = true, .risen = true }) == kRedress);
    CHECK(needsOf(NeedFacts{ .worlds = true, .lacksScrolls = true }) == kScrolls);
    // her new jobs are dressed for at the counter
    CHECK(needsOf(NeedFacts{ .worlds = true, .careerJobs = true }) == (kJobs | kRedress));
    CHECK(needsOf(NeedFacts{ .worlds = true, .risen = true, .lacksScrolls = true, .careerJobs = true }) == (kJobs | kRedress | kScrolls));
    CHECK(needsOf(NeedFacts{ .worlds = true }) == 0);

    // in his party she changes no job: the role he took her on in stays hers
    CHECK(needsOf(NeedFacts{ .worlds = true, .guest = true, .careerJobs = true }) == 0);
    CHECK(needsOf(NeedFacts{ .worlds = true, .guest = true, .risen = true, .lacksScrolls = true, .careerJobs = true }) == (kRedress | kScrolls));

    // what she saw to in this zone is not owed again here
    CHECK(needsOf(NeedFacts{ .worlds = true, .risen = true, .lacksScrolls = true, .seenTo = kScrolls }) == kRedress);
    CHECK(needsOf(NeedFacts{ .worlds = true, .careerJobs = true, .seenTo = kJobs }) == kRedress);
}

TEST_CASE("Town errands: each stop sees to one need", "[cardian][town]")
{
    CHECK(needOf(Stop::Counter) == kRedress);
    CHECK(needOf(Stop::Guard) == kScrolls);
    CHECK(needOf(Stop::Consulate) == kScrolls);
    CHECK(needOf(Stop::MogHouse) == kJobs);
}

TEST_CASE("Town errands: her stops nearest next", "[cardian][town]")
{
    // she stands at the origin: the guard 10 off, the counter 30
    const std::vector<Spot> two{
        { Stop::Counter, 30.0f, 0.0f, 0.0f },
        { Stop::Guard, 10.0f, 0.0f, 0.0f },
    };
    CHECK(visitOrder(two, 0.0f, 0.0f, 0.0f) == std::vector<std::size_t>{ 1, 0 });
    // from the far side, the counter first
    CHECK(visitOrder(two, 40.0f, 0.0f, 0.0f) == std::vector<std::size_t>{ 0, 1 });

    // nearest NEXT, not nearest to where she started: from the Mog House at
    // 10, the counter at 12 comes before the guard at -11, though the guard
    // stands nearer where she started
    const std::vector<Spot> chain{
        { Stop::Guard, -11.0f, 0.0f, 0.0f },
        { Stop::MogHouse, 10.0f, 0.0f, 0.0f },
        { Stop::Counter, 12.0f, 0.0f, 0.0f },
    };
    CHECK(visitOrder(chain, 0.0f, 0.0f, 0.0f) == std::vector<std::size_t>{ 1, 2, 0 });

    // the counter waits for the Mog House, however near
    const std::vector<Spot> waits{
        { Stop::Guard, 10.0f, 0.0f, 0.0f },
        { Stop::MogHouse, -12.0f, 0.0f, 0.0f },
        { Stop::Counter, 1.0f, 0.0f, 0.0f },
    };
    CHECK(visitOrder(waits, 0.0f, 0.0f, 0.0f) == std::vector<std::size_t>{ 0, 1, 2 });

    const std::vector<Spot> noMog{
        { Stop::Guard, 10.0f, 0.0f, 0.0f },
        { Stop::Counter, -12.0f, 0.0f, 0.0f },
    };
    CHECK(visitOrder(noMog, 0.0f, 0.0f, 0.0f) == std::vector<std::size_t>{ 0, 1 });
    CHECK(visitOrder(noMog, -5.0f, 0.0f, 0.0f) == std::vector<std::size_t>{ 1, 0 });

    // a consulate in another zone goes last, whatever it is put at
    const std::vector<Spot> consulate{
        { Stop::Consulate, 0.0f, 0.0f, 0.0f, true },
        { Stop::Counter, 50.0f, 0.0f, 0.0f },
        { Stop::MogHouse, 20.0f, 0.0f, 0.0f },
    };
    CHECK(visitOrder(consulate, 0.0f, 0.0f, 0.0f) == std::vector<std::size_t>{ 2, 1, 0 });

    // height counts as distance does
    const std::vector<Spot> stairs{
        { Stop::Guard, 3.0f, 20.0f, 0.0f },
        { Stop::Counter, 10.0f, 0.0f, 0.0f },
    };
    CHECK(visitOrder(stairs, 0.0f, 0.0f, 0.0f) == std::vector<std::size_t>{ 1, 0 });

    CHECK(visitOrder({}, 0.0f, 0.0f, 0.0f).empty());
    CHECK(visitOrder({ { Stop::MogHouse, 5.0f, 0.0f, 0.0f } }, 0.0f, 0.0f, 0.0f) == std::vector<std::size_t>{ 0 });
}

TEST_CASE("Town errands: her lines carry no pronoun of hers and no codename", "[cardian][town]")
{
    for (const auto line : { Line::SetsOut, Line::Ready, Line::NotReady })
    {
        const auto lines = linesOf(line);
        CHECK(lines.size() >= 4);
        std::set<std::string_view> distinct(lines.begin(), lines.end());
        CHECK(distinct.size() == lines.size());
        for (const auto text : lines)
        {
            CHECK_FALSE(text.empty());
            CHECK_FALSE(hasWord(text, "she"));
            CHECK_FALSE(hasWord(text, "her"));
            CHECK_FALSE(hasWord(text, "hers"));
            CHECK_FALSE(hasWord(text, "cardian"));
            CHECK_FALSE(hasWord(text, "cardians"));
        }
    }
    CHECK(kSetsOut.size() == 5);
    CHECK(kReady.size() == 5);
    CHECK(kReady[0] == "I'm ready to go!");
}

TEST_CASE("Town errands: a line said a moment ago is passed over", "[cardian][town]")
{
    // no line recent: the roll picks among them all
    for (uint32_t roll = 0; roll < 10; ++roll)
    {
        CHECK(pickLine(Line::Ready, roll, 0) == roll % kReady.size());
    }
    // five members set out together: five different lines
    uint32_t                 recent = 0;
    std::set<std::size_t>    said;
    for (int member = 0; member < 5; ++member)
    {
        const auto index = pickLine(Line::SetsOut, 7, recent);
        CHECK_FALSE(said.contains(index));
        said.insert(index);
        recent |= 1u << index;
    }
    CHECK(said.size() == 5);
    // all said: any again
    CHECK(pickLine(Line::SetsOut, 3, recent) == 3);
    // one left fresh: that one
    CHECK(pickLine(Line::NotReady, 99, 0b1011) == 2);
}

TEST_CASE("Town errands: lines only in his party, the set-out line once a zone", "[cardian][town]")
{
    CHECK(saysSetOut(Who::Party, false));
    CHECK_FALSE(saysSetOut(Who::Party, true)); // taken up again after an interruption, without a word
    CHECK_FALSE(saysSetOut(Who::Hangout, false));
    CHECK_FALSE(saysSetOut(Who::Nobody, false));
}

TEST_CASE("Town errands: he leaving her zone ends nothing", "[cardian][town]")
{
    OnFacts here;
    here.standing   = true;
    here.inHisParty = true;
    here.sameZone   = true;
    // he left: she is in his party still (his body crossing a zone line or
    // in another zone), in the zone she set out in, and keeps going
    CHECK(goesOn(Who::Party, here));

    auto consulate     = here;
    consulate.sameZone = false;
    consulate.onTheWay = true; // her own way to her consulate
    CHECK(goesOn(Who::Party, consulate));

    auto wandered     = here;
    wandered.sameZone = false; // carried off some other way
    CHECK_FALSE(goesOn(Who::Party, wandered));

    auto out       = here;
    out.inHisParty = false; // out of his party: nobody's errands now
    CHECK_FALSE(goesOn(Who::Party, out));

    auto down     = here;
    down.standing = false;
    CHECK_FALSE(goesOn(Who::Party, down));

    // she says so once, when he can hear it
    CHECK(saysNotReady(Who::Party, true, true, false));
    CHECK_FALSE(saysNotReady(Who::Party, true, false, false)); // between zones: said once he lands
    CHECK_FALSE(saysNotReady(Who::Party, true, true, true));   // said already
    CHECK_FALSE(saysNotReady(Who::Party, false, true, false)); // he has not left
    CHECK_FALSE(saysNotReady(Who::Hangout, true, true, false));
}

TEST_CASE("Town errands: one of the world's keeps to her own zone", "[cardian][town]")
{
    OnFacts seat;
    seat.standing = true;
    seat.worlds   = true;
    seat.sameZone = true;
    CHECK(goesOn(Who::Hangout, seat));

    auto invited   = seat;
    invited.worlds = false; // invited into his party, or held for him
    CHECK_FALSE(goesOn(Who::Hangout, invited));

    auto elsewhere     = seat;
    elsewhere.sameZone = false;
    elsewhere.onTheWay = true;
    CHECK_FALSE(goesOn(Who::Hangout, elsewhere));

    CHECK_FALSE(goesOn(Who::Nobody, seat));
}
