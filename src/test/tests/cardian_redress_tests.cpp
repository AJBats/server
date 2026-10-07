// Cardian: a wild cardian re-dressed at the auction house (pawn/redress.h):
// when the map asks the census, what the census's skill values raise, and
// which pieces leave her bag.
#include "pawn/redress_math.h"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstdint>
#include <optional>
#include <set>
#include <vector>

using namespace cardian::redress;

TEST_CASE("Redress: a body never dressed at a counter asks for the level she has", "[cardian][redress]")
{
    CHECK(shouldAsk(12, std::nullopt));
    CHECK(shouldAsk(1, std::nullopt));
    CHECK_FALSE(shouldAsk(0, std::nullopt)); // no level read: nothing to ask for
}

TEST_CASE("Redress: a body dressed at a counter asks again only once she has risen past it", "[cardian][redress]")
{
    CHECK_FALSE(shouldAsk(20, Record{ 20, State::Done }));
    CHECK_FALSE(shouldAsk(19, Record{ 20, State::Done })); // a level lost to death: never a step down
    CHECK(shouldAsk(21, Record{ 20, State::Done }));
}

TEST_CASE("Redress: a request on its way is never asked over", "[cardian][redress]")
{
    CHECK_FALSE(shouldAsk(25, Record{ 20, State::Asked }));
    CHECK_FALSE(shouldAsk(25, Record{ 20, State::Ready }));
}

TEST_CASE("Redress: the row's state reads by its three words", "[cardian][redress]")
{
    CHECK(stateOf("asked") == State::Asked);
    CHECK(stateOf("ready") == State::Ready);
    CHECK(stateOf("done") == State::Done);
    CHECK_FALSE(stateOf("").has_value());
    CHECK_FALSE(stateOf("Done").has_value());
}

TEST_CASE("Redress: the census's skill values read as skill and tenths", "[cardian][redress]")
{
    const auto skills = parseSkills("1:520,3:488,25:120");
    REQUIRE(skills.size() == 3);
    CHECK(skills[0] == SkillValue{ 1, 520 });
    CHECK(skills[1] == SkillValue{ 3, 488 });
    CHECK(skills[2] == SkillValue{ 25, 120 });

    CHECK(parseSkills("").empty());
}

TEST_CASE("Redress: a piece of the skill list that does not read is left out", "[cardian][redress]")
{
    const auto skills = parseSkills("1:520,,x:3,4:,7:70000,300:5,9:90,12 :4");
    REQUIRE(skills.size() == 2);
    CHECK(skills[0] == SkillValue{ 1, 520 });
    CHECK(skills[1] == SkillValue{ 9, 90 });
}

TEST_CASE("Redress: skills are raised to the census's values, never lowered", "[cardian][redress]")
{
    std::array<uint16_t, 64> have{};
    have[1]  = 600; // the game raised her sword past the census's point: it stands
    have[3]  = 400;
    have[25] = 120; // exactly there: nothing to write

    const auto wanted = parseSkills("1:520,3:488,25:120,12:310");
    const auto out    = raises(wanted, [&](const uint8_t skill)
    {
        return have[skill];
    });

    REQUIRE(out.size() == 2);
    CHECK(out[0] == SkillValue{ 3, 488 });
    CHECK(out[1] == SkillValue{ 12, 310 });
}

TEST_CASE("Redress: the pieces the census had issued read as item ids", "[cardian][redress]")
{
    CHECK(parseIds("16465,12505,0,abc,12505") == std::set<uint16_t>{ 12505, 16465 });
    CHECK(parseIds("").empty());
}

TEST_CASE("Redress: only a census piece the new plan has no place for leaves her bag", "[cardian][redress]")
{
    const std::set<uint16_t> plan{ 16535, 12576 };   // the new plan: a sword and a body piece
    const std::set<uint16_t> issued{ 16465, 12576 }; // what the census had issued her before

    CHECK(dropsPiece(16465, false, plan, issued));       // her old dagger: issued, out of the plan, unworn
    CHECK_FALSE(dropsPiece(16465, true, plan, issued));  // ... but never one she still wears
    CHECK_FALSE(dropsPiece(12576, false, plan, issued)); // a piece the new plan keeps
    CHECK_FALSE(dropsPiece(13014, false, plan, issued)); // a piece somebody else gave her
}

namespace
{
    constexpr uint8_t  kWAR      = 1;
    constexpr uint8_t  kMNK      = 2;
    constexpr uint8_t  kWHM      = 3;
    constexpr uint8_t  kBLM      = 4;
    constexpr uint8_t  kNIN      = 13;
    constexpr uint32_t kStarting = 126; // a new character's unlocks: the six starting jobs, no support job
} // namespace

TEST_CASE("Redress: a body minted before support jobs takes the planned one, unlocked, at its level", "[cardian][redress]")
{
    // a White Mage 24 with no support job: Black Mage 12, the support job and the Black Mage unlocked
    const auto change = subChange({ kBLM, 12 }, { kWHM, 0, 1, kStarting });
    REQUIRE(change.has_value());
    CHECK(*change == SubChange{ kStarting | 1u | (1u << kBLM), 12, true }); // none before: the game's job change
}

TEST_CASE("Redress: an advanced support job is unlocked with the support job", "[cardian][redress]")
{
    // a Warrior 30 on her Monk support moves to Ninja 15, a job she never had
    const auto change = subChange({ kNIN, 15 }, { kWAR, kMNK, 0, kStarting | 1u });
    REQUIRE(change.has_value());
    CHECK(change->unlocked == (kStarting | 1u | (1u << kNIN)));
    CHECK(change->jobLevel == 15);
    CHECK(change->switchJob);
}

TEST_CASE("Redress: a support job carried is raised to the plan's level, without a job change, and never lowered", "[cardian][redress]")
{
    const uint32_t unlocked = kStarting | 1u;
    const auto     raised   = subChange({ kWAR, 21 }, { kMNK, kWAR, 20, unlocked }); // a Monk 42 on her Warrior 20 since 40
    REQUIRE(raised.has_value());
    CHECK(*raised == SubChange{ unlocked, 21, false }); // the same job: the level change alone, her buffs and recasts kept

    // her Warrior already past the plan (levelled as a main once): it stays, and so does she
    CHECK_FALSE(subChange({ kWAR, 21 }, { kMNK, kWAR, 30, unlocked }).has_value());
    CHECK_FALSE(subChange({ kWAR, 21 }, { kMNK, kWAR, 21, unlocked }).has_value());
}

TEST_CASE("Redress: a job she has levels in, carried but never unlocked, is unlocked", "[cardian][redress]")
{
    const auto change = subChange({ kNIN, 15 }, { kWAR, kNIN, 15, kStarting });
    REQUIRE(change.has_value());
    CHECK(*change == SubChange{ kStarting | 1u | (1u << kNIN), 15, false }); // the unlocks alone
}

TEST_CASE("Redress: no plan, her main, or no real job leaves her support job alone", "[cardian][redress]")
{
    CHECK_FALSE(subChange({ 0, 0 }, { kWAR, kMNK, 9, kStarting | 1u }).has_value());  // under the quest: never taken away
    CHECK_FALSE(subChange({ kMNK, 0 }, { kWAR, 0, 0, kStarting }).has_value());       // a level of none
    CHECK_FALSE(subChange({ kWAR, 15 }, { kWAR, 0, 30, kStarting }).has_value());     // her own main
    CHECK_FALSE(subChange({ 23, 15 }, { kWAR, 0, 0, kStarting }).has_value());        // past the last job
}

TEST_CASE("Redress: an answer is put on her only at the level it was planned for", "[cardian][redress]")
{
    CHECK(answerFits(29, 29));
    // a Warrior asked at 29 on her Monk support; the catch-up dinged her to 31 on
    // Ninja before her next stand: the stale answer would switch her back to Monk
    CHECK_FALSE(answerFits(29, 31));
    CHECK_FALSE(answerFits(31, 30)); // a level lost to death since
    CHECK_FALSE(answerFits(0, 0));
}
