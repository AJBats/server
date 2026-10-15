-- The shell's bank when a move fails (#478; ROADMAP N, "No assumption of one map
-- server"). A move is one database transaction, its character changed in memory
-- through the game's own item transaction. It can fail on the wire at its COMMIT --
-- the database rolls the move back and says so, or it commits and its answer is lost
-- -- and its connection can be lost before the COMMIT, at it, after it, or again while
-- the character is put back, when MariaDB rolls back what the connection held. Every
-- way, the bank's rows, the character's rows and the character in memory must agree,
-- and nothing may be made or lost: rolled back, the move did not happen; committed,
-- it did.
--
-- xi.cardian.lsbank is bound for xi_test alone (cardian_pawn_stubs.cpp): it makes the
-- moves of pawn/ls_bank_moves.h on a player of the test's, the holder of a shell of
-- the test's, fails the next move on purpose, and reads the rows back.

describe('Linkshell bank: a move that fails', function()
    local bank   = xi.cardian.lsbank
    local kShell = 0xC4B0 -- a shell of the test's own
    local INV    = xi.inventoryLocation.INVENTORY
    local LOG    = xi.item.ELM_LOG
    local ASH    = xi.item.ASH_LOG

    -- The faults, by pawn::lsbank::moves::MoveFault's number
    local ROLLED_BACK         = 1 -- the COMMIT rolled back, and told so
    local COMMITTED           = 2 -- the COMMIT went through, and its answer was lost
    local LOST_BEFORE         = 3 -- the connection lost before the COMMIT
    local LOST_AT             = 4 -- the connection lost as the COMMIT was sent
    local COMMITTED_THEN_LOST = 5 -- the COMMIT went through, then the connection was lost
    local LOST_TWICE          = 6 -- lost before the COMMIT, and again while he is put back
    local BANK_READ_FAILS     = 7 -- the move's read of the bank failed

    ---@type CClientEntityPair
    local player

    -- Every slot of his inventory, in memory and in his rows, alike: the gil in
    -- slot 0, and each stack's item and quantity
    local function agree(what)
        local rows = bank.rows(player:getID(), INV)
        assert(rows[0] ~= nil and rows[0].qty == player:getGil(),
            string.format('%s: %d gil in memory, %s in his row', what, player:getGil(), rows[0] and tostring(rows[0].qty) or 'none'))
        for slot = 1, 80 do
            local item = player:getStorageItem(INV, slot, 255) -- 255: a storage slot, not an equipment one
            local row  = rows[slot]
            local mine = item ~= nil and item:getID() ~= 0 and { item = item:getID(), qty = item:getQuantity() } or nil
            if mine == nil and row ~= nil then
                error(string.format('%s: slot %d holds nothing in memory, %d x%d in his row', what, slot, row.item, row.qty), 2)
            elseif mine ~= nil and row == nil then
                error(string.format('%s: slot %d holds %d x%d in memory, nothing in his row', what, slot, mine.item, mine.qty), 2)
            elseif mine ~= nil and (mine.item ~= row.item or mine.qty ~= row.qty) then
                error(string.format('%s: slot %d holds %d x%d in memory, %d x%d in his row', what, slot, mine.item, mine.qty, row.item, row.qty), 2)
            end
        end
    end

    -- How many of an item the bank holds
    local function inBank(itemId)
        local n = 0
        for _, stack in ipairs(bank.stacks(kShell)) do
            if stack.item == itemId then
                n = n + stack.qty
            end
        end
        return n
    end

    local function logSlot()
        return player:findItem(LOG):getSlotID()
    end

    -- How many logs the bank and he hold, and how much gil
    local function logs()
        return inBank(LOG), player:getItemCount(LOG)
    end

    local function gil()
        return bank.gilOf(kShell), player:getGil()
    end

    -- The six kinds of move: what each starts from, the move, and what the bank and
    -- he hold before it and after it
    local kinds =
    {
        {
            what   = 'a whole stack deposited',
            count  = logs,
            before = { 0, 12 },
            after  = { 12, 0 },
            move   = function()
                return bank.deposit(player, kShell, INV, logSlot(), 12)
            end,
        },
        {
            what   = 'part of a stack deposited',
            count  = logs,
            before = { 0, 12 },
            after  = { 5, 7 },
            move   = function()
                return bank.deposit(player, kShell, INV, logSlot(), 5)
            end,
        },
        {
            what   = 'a withdrawal',
            count  = logs,
            before = { 12, 0 },
            after  = { 0, 12 },
            setup  = function()
                assert(bank.deposit(player, kShell, INV, logSlot(), 12) == 0, 'the logs could not be banked first')
            end,
            move   = function()
                return bank.withdraw(player, kShell, 1, 12, INV)
            end,
        },
        {
            what   = 'gil in',
            count  = gil,
            before = { 0, 1000 },
            after  = { 300, 700 },
            move   = function()
                return bank.gil(player, kShell, 300, false)
            end,
        },
        {
            what   = 'all his gil in',
            count  = gil,
            before = { 0, 1000 },
            after  = { 1000, 0 },
            move   = function()
                return bank.gil(player, kShell, 1000, false)
            end,
        },
        {
            what   = 'gil out',
            count  = gil,
            before = { 400, 600 },
            after  = { 0, 1000 },
            setup  = function()
                assert(bank.gil(player, kShell, 400, false) == 0, 'the gil could not be banked first')
            end,
            move   = function()
                return bank.gil(player, kShell, 400, true)
            end,
        },
    }

    -- The bank and he hold what they should, and he is as his rows are
    local function holds(kind, want, what)
        local inIt, his = kind.count()
        assert(inIt == want[1] and his == want[2],
            string.format('%s: %d in the bank and %d with him, not %d and %d', what, inIt, his, want[1], want[2]))
        agree(what)
    end

    -- A move failed on purpose: the fault armed, the move made, and the fault met
    local function failed(fault, move)
        bank.failNext(fault)
        local said = move()
        assert(bank.pending() == 0, 'the fault armed for the move was never met')
        return said
    end

    before_each(function()
        player = xi.test.world:spawnPlayer({ zone = xi.zone.SOUTHERN_SAN_DORIA })
        bank.prepare(kShell)
        player:addItem(LOG, 12)
        player:addGil(1000)
        assert(player:getItemCount(LOG) == 12, 'twelve Elm Logs to bank')
        agree('before any move')
    end)

    after_each(function()
        bank.failNext(0)
    end)

    describe('when nothing fails', function()
        for _, kind in ipairs(kinds) do
            it('makes ' .. kind.what, function()
                if kind.setup then
                    kind.setup()
                end
                holds(kind, kind.before, kind.what .. ', before it')
                local said = kind.move()
                holds(kind, kind.after, kind.what)
                assert(said == 0, kind.what .. ' said it failed')
            end)
        end
    end)

    -- Each kind of move failed one way that leaves the database as before it: the
    -- move did not happen, he is as his rows are, and the same move made again goes
    -- through, nothing of his left claimed and no lock left held
    local function leftUndone(fault, how)
        for _, kind in ipairs(kinds) do
            it('leaves ' .. kind.what .. ' undone', function()
                if kind.setup then
                    kind.setup()
                end
                local said = failed(fault, kind.move)
                holds(kind, kind.before, kind.what .. ', ' .. how)
                assert(said ~= 0, kind.what .. ' said it went through')
                local again = kind.move()
                holds(kind, kind.after, kind.what .. ', made again')
                assert(again == 0, kind.what .. ' made again said it failed')
            end)
        end
    end

    -- Each kind of move failed one way that commits it: the move happened, said so,
    -- and he is as his rows are
    local function keptAnyway(fault, how)
        for _, kind in ipairs(kinds) do
            it('keeps ' .. kind.what, function()
                if kind.setup then
                    kind.setup()
                end
                local said = failed(fault, kind.move)
                holds(kind, kind.after, kind.what .. ', ' .. how)
                assert(said == 0, kind.what .. ' went through and said it failed')
            end)
        end
    end

    describe('rolled back', function()
        leftUndone(ROLLED_BACK, 'rolled back')
    end)

    describe('its connection lost before the COMMIT', function()
        leftUndone(LOST_BEFORE, 'the connection lost before the COMMIT')
    end)

    describe('its connection lost at the COMMIT', function()
        leftUndone(LOST_AT, 'the connection lost at the COMMIT')
    end)

    describe('its connection lost before the COMMIT, and again while he is put back', function()
        leftUndone(LOST_TWICE, 'the connection lost twice')
    end)

    describe('committed, its answer lost', function()
        keptAnyway(COMMITTED, 'committed, its answer lost')
    end)

    describe('committed, then its connection lost', function()
        keptAnyway(COMMITTED_THEN_LOST, 'committed, then the connection lost')
    end)

    it("leaves the bank as it was when a deposit's read of the bank fails", function()
        player:addItem(ASH, 12)
        assert(bank.deposit(player, kShell, INV, player:findItem(ASH):getSlotID(), 12) == 0, 'the ash logs could not be banked first')
        local said = failed(BANK_READ_FAILS, function()
            return bank.deposit(player, kShell, INV, logSlot(), 5)
        end)
        assert(inBank(ASH) == 12, string.format('the bank holds %d ash logs, not 12', inBank(ASH)))
        assert(inBank(LOG) == 0 and player:getItemCount(LOG) == 12,
            string.format('%d logs in the bank and %d with him, not 0 and 12', inBank(LOG), player:getItemCount(LOG)))
        agree("a deposit whose read of the bank failed")
        assert(said ~= 0, 'the deposit said it went through')
    end)
end)
