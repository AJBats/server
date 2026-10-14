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

// Cardian: the linkshell bank's rules (pawn/ls_bank_math.h): the order it
// keeps, how stacks of one item merge and what keeps two apart, where a
// deposit lands, a full bank, what a withdrawal takes and what it is refused,
// and the Rare check as the game makes it.
#include "map/pawn/ls_bank_math.h"

#include <catch2/catch_test_macros.hpp>

#include <vector>

using namespace cardian::lsbank;

namespace
{
    auto stackOf(const uint16 item, const uint32 qty, const uint32 stack, const uint8 mark = 0) -> Stack
    {
        Stack s;
        s.item     = item;
        s.qty      = qty;
        s.stack    = stack;
        s.extra[0] = mark;
        return s;
    }

    auto itemsOf(const std::vector<Stack>& bank) -> std::vector<uint16>
    {
        std::vector<uint16> items;
        for (const auto& s : bank)
        {
            items.push_back(s.item);
        }
        return items;
    }

    auto qtysOf(const std::vector<Stack>& bank) -> std::vector<uint32>
    {
        std::vector<uint32> qtys;
        for (const auto& s : bank)
        {
            qtys.push_back(s.qty);
        }
        return qtys;
    }
} // namespace

TEST_CASE("Linkshell bank: the bank keeps its order, by item and the fuller stack first", "[cardian][lsbank]")
{
    const auto bank = settled({ stackOf(4509, 3, 12), stackOf(17298, 1, 1), stackOf(4096, 12, 12), stackOf(4509, 12, 12, 1) });
    CHECK(itemsOf(bank) == std::vector<uint16>{ 4096, 4509, 4509, 17298 });
    CHECK(qtysOf(bank) == std::vector<uint32>{ 12, 12, 3, 1 }); // the two of 4509 differ in their extra data: both kept, the fuller first
    // the same stacks in any order settle the same way
    CHECK(itemsOf(settled({ stackOf(4096, 12, 12), stackOf(4509, 12, 12, 1), stackOf(17298, 1, 1), stackOf(4509, 3, 12) })) == itemsOf(bank));
}

TEST_CASE("Linkshell bank: stacks of one item with the same extra data merge up to the stack size", "[cardian][lsbank]")
{
    const auto bank = settled({ stackOf(4509, 5, 12), stackOf(4509, 10, 12), stackOf(4509, 7, 12) });
    CHECK(qtysOf(bank) == std::vector<uint32>{ 12, 10 }); // 22: one full stack, the rest after it
    // an item that does not stack keeps a slot each
    CHECK(settled({ stackOf(17298, 1, 1), stackOf(17298, 1, 1) }).size() == 2);
    // different extra data never merges: a signature, charges, a pearl's shell
    CHECK(settled({ stackOf(4509, 5, 12, 1), stackOf(4509, 5, 12, 2) }).size() == 2);
    // nothing of nothing
    CHECK(settled({ stackOf(4509, 0, 12) }).empty());
}

TEST_CASE("Linkshell bank: a deposit merges into its kind and lands in order", "[cardian][lsbank]")
{
    const auto bank = settled({ stackOf(4096, 12, 12), stackOf(4509, 8, 12), stackOf(17298, 1, 1) });

    const auto topUp = deposit(bank, stackOf(4509, 3, 12));
    REQUIRE(topUp.has_value());
    CHECK(qtysOf(topUp->bank) == std::vector<uint32>{ 12, 11, 1 }); // merged: no new slot
    CHECK(topUp->landed == 1);

    const auto spill = deposit(bank, stackOf(4509, 6, 12));
    REQUIRE(spill.has_value());
    CHECK(itemsOf(spill->bank) == std::vector<uint16>{ 4096, 4509, 4509, 17298 });
    CHECK(qtysOf(spill->bank) == std::vector<uint32>{ 12, 12, 2, 1 }); // 14: a full stack and the rest
    CHECK(spill->landed == 1);

    const auto first = deposit(bank, stackOf(1, 1, 1));
    REQUIRE(first.has_value());
    CHECK(first->landed == 0); // the lowest item id goes to the top
    CHECK(first->bank.front().item == 1);

    CHECK_FALSE(deposit(bank, stackOf(4509, 0, 12)).has_value());
}

TEST_CASE("Linkshell bank: a full bank takes a stack only where it merges", "[cardian][lsbank]")
{
    std::vector<Stack> full;
    for (uint16 i = 0; i < kSize - 1; ++i)
    {
        full.push_back(stackOf(static_cast<uint16>(10000 + i), 1, 1));
    }
    full.push_back(stackOf(4509, 5, 12));
    full = settled(full);
    REQUIRE(full.size() == kSize);

    CHECK_FALSE(deposit(full, stackOf(17298, 1, 1)).has_value()); // a slot of its own: no room
    CHECK(deposit(full, stackOf(4509, 7, 12)).has_value());        // merges to 12: no new slot
    CHECK_FALSE(deposit(full, stackOf(4509, 8, 12)).has_value());  // 13 needs a second slot: whole or not at all
}

TEST_CASE("Linkshell bank: a withdrawal takes from the slot the page saw", "[cardian][lsbank]")
{
    const auto bank = settled({ stackOf(4096, 12, 12), stackOf(4509, 8, 12), stackOf(17298, 1, 1) });

    CHECK(judgeWithdraw(bank, 2, 4509, 8) == CL_S_OK);
    CHECK(judgeWithdraw(bank, 2, 4509, 3) == CL_S_OK);
    CHECK(judgeWithdraw(bank, 0, 4509, 1) == CL_S_NO_ITEM);
    CHECK(judgeWithdraw(bank, 4, 4509, 1) == CL_S_NO_ITEM);
    CHECK(judgeWithdraw(bank, 2, 4096, 1) == CL_S_ITEM_SLIPPED_AWAY); // the bank changed under the page
    CHECK(judgeWithdraw(bank, 2, 4509, 9) == CL_S_BAD_QUANTITY);
    CHECK(judgeWithdraw(bank, 2, 4509, 0) == CL_S_BAD_QUANTITY);

    const auto whole = withdraw(bank, 2, 8);
    REQUIRE(whole.has_value());
    CHECK(itemsOf(*whole) == std::vector<uint16>{ 4096, 17298 });

    const auto part = withdraw(bank, 2, 3);
    REQUIRE(part.has_value());
    CHECK(qtysOf(*part) == std::vector<uint32>{ 12, 5, 1 });

    CHECK_FALSE(withdraw(bank, 2, 9).has_value());
    CHECK_FALSE(withdraw(bank, 9, 1).has_value());
}

TEST_CASE("Linkshell bank: a deposit and its withdrawal leave the bank as it was", "[cardian][lsbank]")
{
    const auto bank = settled({ stackOf(4096, 12, 12), stackOf(17298, 1, 1) });
    auto       in   = stackOf(13566, 1, 1, 7);
    in.extra[23]    = 0x5A; // the last byte of its extra data, a signature's
    const auto taken = deposit(bank, in);
    REQUIRE(taken.has_value());
    const auto& landed = taken->bank[taken->landed];
    CHECK(landed.item == 13566);
    CHECK(landed.extra == in.extra); // everything it carried, kept
    const auto back = withdraw(taken->bank, taken->landed + 1, 1);
    REQUIRE(back.has_value());
    CHECK(itemsOf(*back) == itemsOf(bank));
    CHECK(qtysOf(*back) == qtysOf(bank));
}

TEST_CASE("Linkshell bank: a Rare item is refused to one who holds one, as the game refuses it", "[cardian][lsbank]")
{
    CHECK(judgeReceive(true, true, 10) == CL_S_RARE_OWNED);
    CHECK(judgeReceive(true, true, 0) == CL_S_RARE_OWNED); // the Rare check comes first, as the game's
    CHECK(judgeReceive(true, false, 10) == CL_S_OK);
    CHECK(judgeReceive(false, true, 10) == CL_S_OK); // an ordinary item: one held already is no matter
    CHECK(judgeReceive(false, false, 0) == CL_S_NO_SPACE);
}
