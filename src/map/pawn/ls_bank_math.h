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

#pragma once

// The linkshell bank's rules (pawn/ls_bank.h): how its stacks merge, the
// order it keeps, where a deposit lands, and what a withdrawal is refused.
// Pure and entity-free, so cardian_ls_bank_tests.cpp can hold it to account;
// ls_bank.cpp feeds it the stacks it reads and writes the ones it returns.

#include "cardian_link_messages.h"

#include "common/cbasetypes.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <optional>
#include <vector>

namespace cardian::lsbank
{
    // The slots the bank has
    constexpr std::size_t kSize = CL_BANK_SIZE;

    // An item's extra data, as char_inventory keeps it: charges, augments, a
    // linkpearl's shell, a signature
    using Extra = std::array<uint8, 24>;

    // One stack in the bank, and how many of its item one slot holds
    struct Stack
    {
        uint16 item  = 0;
        uint32 qty   = 0;
        Extra  extra = {};
        uint32 stack = 1; // the item's stack size
    };

    // Two stacks merge when they are the same item with the same extra data:
    // nothing an item carries is lost to a merge
    inline auto sameKind(const Stack& a, const Stack& b) -> bool
    {
        return a.item == b.item && a.extra == b.extra;
    }

    // The bank's order, which every change keeps: by item id -- the game's
    // own numbering, which groups items by their kind -- then the fuller
    // stack of one item first, then by extra data, so no two stacks tie
    inline auto before(const Stack& a, const Stack& b) -> bool
    {
        if (a.item != b.item)
        {
            return a.item < b.item;
        }
        if (a.qty != b.qty)
        {
            return a.qty > b.qty;
        }
        return a.extra < b.extra;
    }

    // The bank as it always stands: the stacks of each kind pooled and laid
    // out again as full stacks with the rest in one stack after them, and the
    // whole put in order. Empty stacks are dropped
    inline auto settled(const std::vector<Stack>& stacks) -> std::vector<Stack>
    {
        std::vector<Stack> pools;
        for (const auto& s : stacks)
        {
            if (s.qty == 0)
            {
                continue;
            }
            const auto pool = std::find_if(pools.begin(), pools.end(), [&](const Stack& p) { return sameKind(p, s); });
            if (pool == pools.end())
            {
                pools.push_back(s);
            }
            else
            {
                pool->qty += s.qty;
                pool->stack = std::max(pool->stack, s.stack);
            }
        }
        std::vector<Stack> laid;
        for (const auto& pool : pools)
        {
            const uint32 per  = std::max<uint32>(1, pool.stack);
            uint32       left = pool.qty;
            while (left > 0)
            {
                Stack s = pool;
                s.qty   = std::min(left, per);
                left -= s.qty;
                laid.push_back(s);
            }
        }
        std::sort(laid.begin(), laid.end(), before);
        return laid;
    }

    // Where a stack of this kind starts in the bank: its first slot's index
    // (from 0), or none
    inline auto firstOf(const std::vector<Stack>& bank, const Stack& kind) -> std::optional<std::size_t>
    {
        for (std::size_t i = 0; i < bank.size(); ++i)
        {
            if (sameKind(bank[i], kind))
            {
                return i;
            }
        }
        return std::nullopt;
    }

    // A deposit taken: the bank as it then stands, and the index (from 0) of
    // the first stack of the deposited kind
    struct Deposit
    {
        std::vector<Stack> bank;
        std::size_t        landed = 0;
    };

    // The bank with a stack put in: merged with its kind and sorted. None
    // when the result needs more slots than the bank has -- a stack goes in
    // whole or not at all
    inline auto deposit(const std::vector<Stack>& bank, const Stack& in) -> std::optional<Deposit>
    {
        if (in.qty == 0)
        {
            return std::nullopt;
        }
        auto all = bank;
        all.push_back(in);
        Deposit taken{ settled(all), 0 };
        if (taken.bank.size() > kSize)
        {
            return std::nullopt;
        }
        taken.landed = firstOf(taken.bank, in).value_or(0);
        return taken;
    }

    // Why a withdrawal of qty from a bank slot (from 1), which the page saw
    // holding `item`, cannot go, as far as the bank goes: CL_S_OK when it can
    inline auto judgeWithdraw(const std::vector<Stack>& bank, const std::size_t slot, const uint16 item, const uint32 qty) -> uint16
    {
        if (slot == 0 || slot > bank.size())
        {
            return CL_S_NO_ITEM;
        }
        const auto& s = bank[slot - 1];
        if (s.item != item)
        {
            return CL_S_ITEM_SLIPPED_AWAY;
        }
        if (qty == 0 || qty > s.qty)
        {
            return CL_S_BAD_QUANTITY;
        }
        return CL_S_OK;
    }

    // The bank with qty taken off the stack at a slot (from 1), in order
    // again; none when judgeWithdraw would refuse it
    inline auto withdraw(const std::vector<Stack>& bank, const std::size_t slot, const uint32 qty) -> std::optional<std::vector<Stack>>
    {
        if (slot == 0 || slot > bank.size() || qty == 0 || qty > bank[slot - 1].qty)
        {
            return std::nullopt;
        }
        auto after = bank;
        after[slot - 1].qty -= qty;
        return settled(after);
    }

    // Whether the character it goes to can take it, as the game gives an
    // item: a Rare item she holds one of already is refused first, as the
    // game refuses a second (whatever her room), then an inventory with no
    // free slot. CL_S_OK when she can
    inline auto judgeReceive(const bool rare, const bool holdsOne, const std::size_t freeSlots) -> uint16
    {
        if (rare && holdsOne)
        {
            return CL_S_RARE_OWNED;
        }
        if (freeSlots == 0)
        {
            return CL_S_NO_SPACE;
        }
        return CL_S_OK;
    }

    // ---- who may do what, and which bags it reaches ---------------------------

    // What the holder lets a sack holder and a pearl holder do until he says
    // otherwise (the user, 2026-10-14): an officer everything, a pearl only
    // puts items in
    constexpr uint8 kAll          = CL_BANK_PUT | CL_BANK_TAKE | CL_BANK_GIL_IN | CL_BANK_GIL_OUT;
    constexpr uint8 kSackDefault  = kAll;
    constexpr uint8 kPearlDefault = CL_BANK_PUT;

    // What a rank may do (CL_BANK_* bits): the shell's holder always
    // everything, a sack holder and a pearl holder what the holder has set,
    // anyone else nothing
    inline auto canOf(const uint8 rank, const uint8 sackCan, const uint8 pearlCan) -> uint8
    {
        switch (rank)
        {
            case CL_RANK_HOLDER:
                return kAll;
            case CL_RANK_SACK:
                return static_cast<uint8>(sackCan & kAll);
            case CL_RANK_PEARL:
                return static_cast<uint8>(pearlCan & kAll);
            default:
                return 0;
        }
    }

    // The bags of a character by container id (item_container.h's LOC_*):
    // the Mog House's, which open only while he stands in his Mog House
    // (the user, 2026-10-14), and the wardrobes, which take only gear
    inline auto mogHouseBag(const uint8 loc) -> bool
    {
        return loc == 1 || loc == 2 || loc == 4 || loc == 9; // Mog Safe, Storage, Mog Locker, Mog Safe 2
    }

    inline auto wardrobe(const uint8 loc) -> bool
    {
        return loc == 8 || (loc >= 10 && loc <= 16);
    }

    // Whether a bag is one the bank reaches where he stands: the inventory,
    // Mog Satchel, Mog Sack, Mog Case and the wardrobes anywhere it opens,
    // the Mog House's only in his Mog House; never the temporary items or
    // the recycle bin. CL_S_OK, else why not
    inline auto judgeBag(const uint8 loc, const bool inMogHouse) -> uint16
    {
        if (loc == 0 || loc == 5 || loc == 6 || loc == 7 || wardrobe(loc))
        {
            return CL_S_OK;
        }
        if (mogHouseBag(loc))
        {
            return inMogHouse ? static_cast<uint16>(CL_S_OK) : static_cast<uint16>(CL_S_MOG_HOUSE_ONLY);
        }
        return CL_S_NO_SUCH_BAG;
    }
} // namespace cardian::lsbank
