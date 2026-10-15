-- The shell's bank taking a holder's haul at once (#472's Deposit Haul,
-- pawn/ls_bank_moves.h depositEach): each stack a rule names in one bag moves
-- as a deposit does, the rest stays with her, a full bank takes what it still
-- can, and a look says what a press would move without moving it. The map
-- hands depositEach the junk rule; xi_test hands it a list of items.

describe('Linkshell bank: depositing each stack a rule names', function()
    local bank      = xi.cardian.lsbank
    local kShell    = 0xC4B1 -- a shell of the test's own
    local INV       = xi.inventoryLocation.INVENTORY
    local LOG       = xi.item.ELM_LOG
    local ASH       = xi.item.ASH_LOG
    local FLINT     = xi.item.FLINT_STONE
    local NO_ITEM     = 0x0103 -- CL_S_NO_ITEM
    local REFUSED     = 0x010B -- CL_S_REFUSED
    local BANK_FULL   = 0x0201 -- CL_S_BANK_FULL
    local ROLLED_BACK = 1      -- moves::MoveFault: the next COMMIT rolled back, and told so

    ---@type CClientEntityPair
    local player

    -- Every slot of his inventory, in memory and in his rows, alike
    local function agree(what)
        local rows = bank.rows(player:getID(), INV)
        for slot = 1, 80 do
            local item = player:getStorageItem(INV, slot, 255) -- 255: a storage slot, not an equipment one
            local row  = rows[slot]
            local mine = item ~= nil and item:getID() ~= 0 and { item = item:getID(), qty = item:getQuantity() } or nil
            if (mine == nil) ~= (row == nil) or (mine ~= nil and (mine.item ~= row.item or mine.qty ~= row.qty)) then
                error(string.format('%s: slot %d differs in memory and in his row', what, slot), 2)
            end
        end
    end

    local function inBank(itemId)
        local n = 0
        for _, stack in ipairs(bank.stacks(kShell)) do
            if stack.item == itemId then
                n = n + stack.qty
            end
        end
        return n
    end

    -- What the bank and he hold of each item
    local function holds(want, what)
        for itemId, counts in pairs(want) do
            local inIt, his = inBank(itemId), player:getItemCount(itemId)
            assert(inIt == counts[1] and his == counts[2],
                string.format('%s: item %d, %d in the bank and %d with him, not %d and %d', what, itemId, inIt, his, counts[1], counts[2]))
        end
        agree(what)
    end

    before_each(function()
        player = xi.test.world:spawnPlayer({ zone = xi.zone.SOUTHERN_SAN_DORIA })
        bank.prepare(kShell)
        player:addItem(LOG, 12)
        player:addItem(ASH, 12)
        player:addItem(FLINT, 12)
        holds({ [LOG] = { 0, 12 }, [ASH] = { 0, 12 }, [FLINT] = { 0, 12 } }, 'before any move')
    end)

    it('moves every stack named and leaves the rest', function()
        local status, count, full = bank.depositEach(player, kShell, INV, { LOG, ASH }, false)
        holds({ [LOG] = { 12, 0 }, [ASH] = { 12, 0 }, [FLINT] = { 0, 12 } }, 'the logs deposited')
        assert(status == 0 and count == 2 and full == 0, string.format('outcome %d, %d moved, full %d', status, count, full))
    end)

    it('says so when nothing there is named', function()
        local status, count = bank.depositEach(player, kShell, INV, { xi.item.BRONZE_SWORD }, false)
        holds({ [LOG] = { 0, 12 }, [ASH] = { 0, 12 }, [FLINT] = { 0, 12 } }, 'nothing named')
        assert(status == NO_ITEM and count == 0, string.format('outcome %d, %d moved', status, count))
    end)

    it('takes what a full bank still has room for', function()
        bank.fill(kShell, 199)
        local status, count, full = bank.depositEach(player, kShell, INV, { LOG, ASH }, false)
        holds({ [LOG] = { 12, 0 }, [ASH] = { 0, 12 } }, 'one slot left')
        assert(status == 0 and count == 1 and full == 1, string.format('outcome %d, %d moved, full %d', status, count, full))
    end)

    it('moves nothing into a bank with no room', function()
        bank.fill(kShell, 200)
        local status, count, full = bank.depositEach(player, kShell, INV, { LOG, ASH }, false)
        holds({ [LOG] = { 0, 12 }, [ASH] = { 0, 12 } }, 'no room')
        assert(status == BANK_FULL and count == 0 and full == 1, string.format('outcome %d, %d moved, full %d', status, count, full))
    end)

    it('goes on past a stack with no room to one that merges', function()
        bank.fill(kShell, 199)
        assert(bank.deposit(player, kShell, INV, player:findItem(ASH):getSlotID(), 6) == 0, 'half the ash logs banked first, the bank full with them')
        local status, count, full = bank.depositEach(player, kShell, INV, { LOG, ASH }, false)
        holds({ [LOG] = { 0, 12 }, [ASH] = { 12, 0 } }, 'the elm logs had no room, the ash logs merged')
        assert(status == 0 and count == 1 and full == 1, string.format('outcome %d, %d moved, full %d', status, count, full))
    end)

    it('stops at a refusal, nothing more tried', function()
        bank.failNext(ROLLED_BACK)
        local status, count = bank.depositEach(player, kShell, INV, { LOG, ASH }, false)
        holds({ [LOG] = { 0, 12 }, [ASH] = { 0, 12 } }, 'the first refused, the second not tried')
        assert(status == REFUSED and count == 0, string.format('outcome %d, %d moved', status, count))
        bank.failNext(0)
    end)

    it('says what a press would move, and moves nothing', function()
        bank.fill(kShell, 198)
        local status, count, full, would = bank.depositEach(player, kShell, INV, { LOG, ASH, FLINT }, true)
        holds({ [LOG] = { 0, 12 }, [ASH] = { 0, 12 }, [FLINT] = { 0, 12 } }, 'a look')
        assert(status == 0 and count == 2 and full == 1, string.format('outcome %d, %d would move, full %d', status, count, full))
        local moved, movedCount, movedFull, went = bank.depositEach(player, kShell, INV, { LOG, ASH, FLINT }, false)
        assert(moved == status and movedCount == count and movedFull == full, 'the press moved other than the look said')
        for i, stack in ipairs(would) do
            local w = went[i]
            assert(w ~= nil and w.slot == stack.slot and w.item == stack.item and w.qty == stack.qty,
                string.format('stack %d: the look said %d x%d from slot %d', i, stack.item, stack.qty, stack.slot))
        end
    end)
end)
