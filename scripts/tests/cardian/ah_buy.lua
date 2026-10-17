-- A cardian's bid on the Auction House screen with the player's purse behind hers
-- (pawn::auction::purchaseShared, a copy of the game's own purchase,
-- auctionutils::PurchasingItems): the cheapest listing at or under the bid is hers
-- at the bid, her gil paid first and the rest from his purse, and the seller paid
-- into his delivery box by the auction house's own trigger -- all of it or none.
-- The purchase is one move of item_moves.h (#481): refused or failed by the
-- database -- before its COMMIT, at it, or after it with its answer lost -- it
-- leaves both purses and her bag as their rows are, and nothing made or lost:
-- rolled back, it did not happen; committed, it did.
--
-- xi.cardian.ahbuy is bound for xi_test alone (cardian_pawn_stubs.cpp): it buys as
-- the Link's bid does once its gates have passed (pawn::auction::bid judges those),
-- on players of the test's. The listings go up through the Sell page's own moves
-- (xi.cardian.ahsell), and the faults are item_moves.h's, armed there.

describe('Auction house: a bid with the purse behind hers', function()
    local ah  = xi.cardian.ahsell
    local buy = xi.cardian.ahbuy
    local S   = ah.status
    local INV = xi.inventoryLocation.INVENTORY
    local LOG = xi.item.ELM_LOG -- stacks of twelve; on the auction house's shelves

    ---@type CClientEntityPair
    local seller
    ---@type CClientEntityPair
    local buyer
    ---@type CClientEntityPair
    local purse

    local function said(status, want, what)
        assert(status == want, string.format('%s: status %s, wanted %s', what, tostring(status), tostring(want)))
    end

    -- A listing of the seller's up at price: one log, or a stack of twelve
    local function listed(price, stack)
        seller:addItem(LOG, stack and 12 or 1)
        local slot = nil
        for s = 1, 80 do
            local item = seller:getStorageItem(INV, s, 255)
            if item ~= nil and item:getID() == LOG and item:getQuantity() == (stack and 12 or 1) then
                slot = s
                break
            end
        end
        assert(slot ~= nil, 'the seller holds the logs to list')
        local status, _, listing = ah.list(seller, seller, INV, slot, LOG, stack, price)
        said(status, S.OK, string.format('a listing at %d', price))
        return listing
    end

    -- Every slot of a player's inventory, in memory and in his rows, alike: the
    -- gil in slot 0, and each stack's item and quantity
    local function agree(player, what)
        local rows = xi.cardian.lsbank.rows(player:getID(), INV)
        assert(rows[0] ~= nil and rows[0].qty == player:getGil(),
            string.format('%s: %d gil in memory, %s in the row', what, player:getGil(), rows[0] and tostring(rows[0].qty) or 'none'))
        for slot = 1, 80 do
            local item = player:getStorageItem(INV, slot, 255) -- 255: a storage slot, not an equipment one
            local row  = rows[slot]
            local mine = item ~= nil and item:getID() ~= 0 and { item = item:getID(), qty = item:getQuantity() } or nil
            if mine == nil and row ~= nil then
                error(string.format('%s: slot %d holds nothing in memory, %d x%d in the row', what, slot, row.item, row.qty), 2)
            elseif mine ~= nil and row == nil then
                error(string.format('%s: slot %d holds %d x%d in memory, nothing in the row', what, slot, mine.item, mine.qty), 2)
            elseif mine ~= nil and (mine.item ~= row.item or mine.qty ~= row.qty) then
                error(string.format('%s: slot %d holds %d x%d in memory, %d x%d in the row', what, slot, mine.item, mine.qty, row.item, row.qty), 2)
            end
        end
    end

    -- What the three hold: her logs, both purses, and the seller's listings on
    -- sale and sold with their gil waiting in his delivery box
    local function snapshot()
        local mine = ah.listings(seller:getID())
        local sold = 0
        for _, row in ipairs(mine.rows) do
            if row.sold then
                sold = sold + 1
            end
        end
        return {
            herLogs = buyer:getItemCount(LOG),
            herGil  = buyer:getGil(),
            hisGil  = purse:getGil(),
            open    = mine.open,
            sold    = sold,
        }
    end

    local function same(got, want, what)
        for k, v in pairs(want) do
            assert(got[k] == v, string.format('%s: %s is %s, not %s', what, k, tostring(got[k]), tostring(v)))
        end
        agree(buyer, what .. ', her inventory')
        agree(purse, what .. ', his')
    end

    before_each(function()
        ah.prepare()
        ah.failNext(0)
        buy.refuseGives(false)
        seller = xi.test.world:spawnPlayer({ zone = xi.zone.SOUTHERN_SAN_DORIA })
        buyer  = xi.test.world:spawnPlayer({ zone = xi.zone.SOUTHERN_SAN_DORIA })
        purse  = xi.test.world:spawnPlayer({ zone = xi.zone.SOUTHERN_SAN_DORIA })
        seller:addGil(10000)
        buyer:addGil(500)
        purse:addGil(5000)
    end)

    it('buys the cheapest listing at or under her bid, at her bid, from her own purse', function()
        local dearer  = listed(150, false)
        local cheaper = listed(100, false)
        local before  = snapshot()
        assert(buy.purchase(buyer, purse, LOG, false, 200, 0), 'a bid of 200 wins')
        same(snapshot(), { herLogs = before.herLogs + 1, herGil = before.herGil - 200, hisGil = before.hisGil, open = before.open - 1, sold = before.sold + 1 },
            'a bid of 200 from her purse alone')
        local id, name, sale = buy.soldTo(cheaper)
        assert(id == buyer:getID() and name == buyer:getName() and sale == 200, 'the cheaper listing is hers, at her bid')
        id = buy.soldTo(dearer)
        assert(id == 0, 'the dearer one is still on sale')
    end)

    it('splits the price: all her gil first, the rest from his purse', function()
        local listing = listed(400, false)
        local before  = snapshot()
        local bid     = before.herGil + 300
        assert(buy.purchase(buyer, purse, LOG, false, bid, 300), 'her purse and 300 of his win')
        same(snapshot(), { herLogs = before.herLogs + 1, herGil = 0, hisGil = before.hisGil - 300, open = before.open - 1, sold = before.sold + 1 },
            'her whole purse and 300 of his')
        local id, _, sale = buy.soldTo(listing)
        assert(id == buyer:getID() and sale == bid, 'the listing is hers, at the bid')
    end)

    it('buys nothing and pays nothing with nothing listed at or under her bid', function()
        local listing = listed(500, false)
        local before  = snapshot()
        assert(not buy.purchase(buyer, purse, LOG, false, 400, 100), 'a bid under every listing wins nothing')
        same(snapshot(), before, 'a bid that won nothing')
        assert(buy.soldTo(listing) == 0, 'the listing is still on sale')
    end)

    it('keeps both purses and her bag, in memory and in their rows, when her piece is refused', function()
        local listing = listed(100, false)
        local before  = snapshot()
        assert(buy.refuseGives(true), 'the test database refuses the piece')
        local bought = buy.purchase(buyer, purse, LOG, false, before.herGil + 200, 200)
        buy.refuseGives(false)
        assert(not bought, 'the purchase was refused')
        same(snapshot(), before, 'after the refusal')
        assert(buy.soldTo(listing) == 0, 'the listing is still on sale')
        assert(buy.purchase(buyer, purse, LOG, false, before.herGil + 200, 200), 'the same bid made again wins')
        same(snapshot(), { herLogs = before.herLogs + 1, herGil = 0, hisGil = before.hisGil - 200, open = before.open - 1, sold = before.sold + 1 },
            'the same bid made again')
    end)

    describe('when the database fails the purchase', function()
        -- The faults, by item_moves.h's Fault number
        local ROLLED_BACK         = 1 -- the COMMIT rolled back, and told so
        local COMMITTED           = 2 -- the COMMIT went through, and its answer was lost
        local LOST_BEFORE         = 3 -- the connection lost before the COMMIT
        local LOST_AT             = 4 -- the connection lost as the COMMIT was sent
        local COMMITTED_THEN_LOST = 5 -- the COMMIT went through, then the connection was lost
        local LOST_TWICE          = 6 -- lost before the COMMIT, and again while they are put back

        -- The two kinds of purchase: what each starts from, the bid, and what
        -- it leaves when it happens, from what was there before it
        local kinds =
        {
            {
                what  = 'a piece bought from her purse alone',
                setup = function() listed(100, false) end,
                move  = function() return buy.purchase(buyer, purse, LOG, false, 200, 0) end,
                after = function(b) return { herLogs = b.herLogs + 1, herGil = b.herGil - 200, hisGil = b.hisGil, open = b.open - 1, sold = b.sold + 1 } end,
            },
            {
                what  = 'a stack bought with all her gil and the rest from his purse',
                setup = function() listed(1200, true) end,
                move  = function() return buy.purchase(buyer, purse, LOG, true, 1500, 1500 - buyer:getGil()) end,
                after = function(b) return { herLogs = b.herLogs + 12, herGil = 0, hisGil = b.hisGil - (1500 - b.herGil), open = b.open - 1, sold = b.sold + 1 } end,
            },
        }

        -- A purchase failed on purpose: the fault armed, the bid made, and the fault met
        local function failed(fault, kind)
            ah.failNext(fault)
            local bought = kind.move()
            assert(ah.pending() == 0, kind.what .. ': the fault armed for it was never met')
            return bought
        end

        -- Each kind failed one way that leaves the database as before it: the
        -- purchase did not happen, both are as their rows are, the listing is
        -- still on sale, and the same bid made again wins, nothing of theirs left
        -- claimed
        local function leftUndone(fault, how)
            for _, kind in ipairs(kinds) do
                it('leaves ' .. kind.what .. ' undone', function()
                    kind.setup()
                    local before = snapshot()
                    assert(not failed(fault, kind), kind.what .. ', ' .. how .. ': said it went through')
                    same(snapshot(), before, kind.what .. ', ' .. how)
                    assert(kind.move(), kind.what .. ' made again')
                    same(snapshot(), kind.after(before), kind.what .. ', made again')
                end)
            end
        end

        -- Each kind failed one way that commits it: the purchase happened, said
        -- so, and both are as their rows are
        local function keptAnyway(fault, how)
            for _, kind in ipairs(kinds) do
                it('keeps ' .. kind.what, function()
                    kind.setup()
                    local before = snapshot()
                    assert(failed(fault, kind), kind.what .. ', ' .. how .. ': said it did not go through')
                    same(snapshot(), kind.after(before), kind.what .. ', ' .. how)
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

        describe('its connection lost before the COMMIT, and again while they are put back', function()
            leftUndone(LOST_TWICE, 'the connection lost twice')
        end)

        describe('committed, its answer lost', function()
            keptAnyway(COMMITTED, 'committed, its answer lost')
        end)

        describe('committed, then its connection lost', function()
            keptAnyway(COMMITTED_THEN_LOST, 'committed, then the connection lost')
        end)
    end)
end)
