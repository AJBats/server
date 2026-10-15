-- Selling on the auction house from the Sell page (pawn/ah_sell.h; the Auction
-- House screen, ROADMAP L). Every listing is the player's -- his fee, his slots,
-- his name on it, its gil to his delivery box -- whoever's bag the item comes
-- from: his own, an alt's or a cardian's he owns. A listing of his comes back into
-- his inventory by its number, and nobody else's does; given a new price, it stays
-- up and he pays the fee at that price again. His sales status lists what he has on
-- sale, and each sale whose gil still waits in his delivery box.
--
-- xi.cardian.ahsell is bound for xi_test alone (cardian_pawn_stubs.cpp): it lists,
-- cancels and reprices as the Link's AH_SELL, AH_CANCEL and AH_REPRICE do, and reads
-- as AH_LISTINGS does, on players of the test's, the places they stand left out (the
-- handlers judge those). A move refused or failed by the database -- before its
-- COMMIT, at it, or after it with its answer lost -- leaves both characters as their
-- rows are, and nothing made or lost: rolled back, it did not happen; committed, it
-- did (item_moves.h, the bank's #478 discipline).

describe('Auction house: the Sell page', function()
    local ah   = xi.cardian.ahsell
    local S    = ah.status
    local INV  = xi.inventoryLocation.INVENTORY
    local LOG  = xi.item.ELM_LOG                -- stacks of twelve; on the auction house's shelves
    local WARP = xi.item.SCROLL_OF_INSTANT_WARP -- the auction house takes none
    local BLADE = xi.item.BRONZE_SWORD

    ---@type CClientEntityPair
    local seller
    ---@type CClientEntityPair
    local holder

    local function slotOf(player, itemId)
        return player:findItem(itemId):getSlotID()
    end

    local function said(status, want, what)
        assert(status == want, string.format('%s: status %s, wanted %s', what, tostring(status), tostring(want)))
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

    before_each(function()
        ah.prepare()
        ah.failNext(0)
        seller = xi.test.world:spawnPlayer({ zone = xi.zone.SOUTHERN_SAN_DORIA, job = xi.job.WAR, level = 10 })
        holder = xi.test.world:spawnPlayer({ zone = xi.zone.SOUTHERN_SAN_DORIA })
        seller:addGil(10000)
        holder:addGil(500)
    end)

    it('lists a piece of his own: the piece off his bag, the fee off his purse', function()
        seller:addItem(LOG, 5)
        local gil = seller:getGil()
        local status, fee, listing = ah.list(seller, seller, INV, slotOf(seller, LOG), LOG, false, 100)
        said(status, S.OK, 'a single Elm Log at 100')
        assert(fee == ah.fee(100, false) and fee > 0, string.format('the fee paid was %d, the fee of 100 is %d', fee, ah.fee(100, false)))
        assert(seller:getGil() == gil - fee, 'his purse paid the fee')
        assert(seller:getItemCount(LOG) == 4, 'one log left his bag')
        local mine = ah.listings(seller:getID())
        assert(mine.open == 1 and #mine.rows == 1, 'his sales status shows one listing')
        local row = mine.rows[1]
        assert(row.listing == listing and row.price == 100 and row.item == LOG and not row.stack and not row.sold, 'the listing as it went up')
        local id, name = ah.sellerOf(listing)
        assert(id == seller:getID() and name == seller:getName(), 'his name is on it')
    end)

    it("lists another's stack as his: her stack, his fee, his name", function()
        holder:addItem(LOG, 12)
        local sellerGil, holderGil = seller:getGil(), holder:getGil()
        local status, fee, listing = ah.list(seller, holder, INV, slotOf(holder, LOG), LOG, true, 1200)
        said(status, S.OK, 'her stack of Elm Logs at 1,200')
        assert(holder:getItemCount(LOG) == 0, 'the whole stack left her bag')
        assert(holder:getGil() == holderGil, 'she paid nothing')
        assert(seller:getGil() == sellerGil - fee and fee == ah.fee(1200, true), 'he paid the stack fee')
        local id = ah.sellerOf(listing)
        assert(id == seller:getID(), 'the listing is his')
        assert(ah.listings(seller:getID()).open == 1, 'it counts against his listings')
        assert(ah.listings(holder:getID()).open == 0, 'and not against hers')
    end)

    it('lists nothing and moves nothing when it refuses', function()
        seller:addItem(LOG, 5)
        seller:addItem(WARP, 1)
        seller:addItem(BLADE, 1)
        seller:equipItem(BLADE, INV, xi.slot.MAIN)
        assert(seller:getEquipID(xi.slot.MAIN) == BLADE, 'he wears the sword')
        local gil = seller:getGil()
        said(ah.list(seller, seller, INV, slotOf(seller, LOG), LOG, true, 500), S.NOT_A_FULL_STACK, 'five logs as a stack')
        said(ah.list(seller, seller, INV, slotOf(seller, WARP), WARP, false, 500), S.NOT_AUCTIONABLE, 'an Instant Warp')
        said(ah.list(seller, seller, INV, slotOf(seller, BLADE), BLADE, false, 500), S.ITEM_EQUIPPED, 'the sword he wears')
        said(ah.list(seller, seller, INV, slotOf(seller, BLADE), BLADE, true, 500), S.NOT_IN_STACKS, 'a sword as a stack')
        said(ah.list(seller, seller, INV, slotOf(seller, LOG), BLADE, false, 500), S.ITEM_SLIPPED_AWAY, 'a slot holding another item than the page saw')
        said(ah.list(seller, seller, xi.inventoryLocation.MOGSAFE, 1, LOG, false, 500), S.NO_SUCH_BAG, "his Mog Safe, which the counter does not reach")
        said(ah.list(holder, holder, INV, slotOf(seller, LOG), LOG, false, 500), S.NO_ITEM, 'a slot that holds nothing')
        said(ah.list(holder, seller, INV, slotOf(seller, LOG), LOG, false, 100000), S.NOT_ENOUGH_GIL, 'a fee more than his purse')
        assert(seller:getItemCount(LOG) == 5 and seller:getItemCount(WARP) == 1, 'every piece is where it was')
        assert(seller:getGil() == gil, 'no fee was paid')
        assert(ah.listings(seller:getID()).open == 0 and ah.listings(holder:getID()).open == 0, 'nothing went up')
    end)

    it('holds him to the listing limit', function()
        xi.test.world:setSetting('map.AH_LIST_LIMIT', 2)
        seller:addItem(LOG, 5)
        said(ah.list(seller, seller, INV, slotOf(seller, LOG), LOG, false, 100), S.OK, 'the first')
        said(ah.list(seller, seller, INV, slotOf(seller, LOG), LOG, false, 100), S.OK, 'the second')
        said(ah.list(seller, seller, INV, slotOf(seller, LOG), LOG, false, 100), S.LISTING_LIMIT, 'a third past a limit of two')
        assert(seller:getItemCount(LOG) == 3, 'the third log stayed')
    end)

    it('takes a listing of his back into his inventory, once', function()
        holder:addItem(LOG, 12)
        local _, _, listing = ah.list(seller, holder, INV, slotOf(holder, LOG), LOG, true, 1200)
        local status, landed = ah.cancel(seller, listing, LOG)
        said(status, S.OK, 'the stack taken back')
        assert(landed > 0 and seller:getItemCount(LOG) == 12, 'the whole stack is in his inventory, not hers')
        assert(holder:getItemCount(LOG) == 0, 'nothing went back to her')
        assert(ah.listings(seller:getID()).open == 0, 'it is off the auction house')
        said(ah.cancel(seller, listing, LOG), S.LISTING_GONE, 'the same listing taken back again')
        assert(seller:getItemCount(LOG) == 12, 'and nothing more came of it')
    end)

    it('gives a listing of his a new price: the fee again, its date fresh, the listing kept', function()
        seller:addItem(LOG, 12)
        local _, _, listing = ah.list(seller, seller, INV, slotOf(seller, LOG), LOG, true, 1200)
        ah.backdate(listing, 86400)
        local wentUp = ah.listings(seller:getID()).rows[1].date
        local gil = seller:getGil()
        local status, fee = ah.reprice(seller, listing, LOG, 900)
        said(status, S.OK, 'the stack down to 900')
        assert(fee == ah.fee(900, true) and seller:getGil() == gil - fee, 'the fee at the new price paid again')
        local mine = ah.listings(seller:getID())
        assert(mine.open == 1 and mine.rows[1].listing == listing and mine.rows[1].price == 900, 'the same listing, at 900')
        assert(mine.rows[1].date >= wentUp + 86400 - 5, string.format('its date fresh: %d, a day after %d', mine.rows[1].date, wentUp))
        gil = seller:getGil()
        status, fee = ah.reprice(seller, listing, LOG, 900)
        said(status, S.OK, 'its own price again')
        assert(fee == 0 and seller:getGil() == gil, 'costs nothing')
        said(ah.reprice(seller, listing, LOG, 0), S.BAD_PRICE, 'a price of nothing')
        said(ah.reprice(holder, listing, LOG, 800), S.LISTING_GONE, "another's listing")
        said(ah.reprice(seller, listing, WARP, 800), S.LISTING_GONE, 'named with another item')
        said(ah.reprice(seller, listing, LOG, 999999999), S.NOT_ENOUGH_GIL, 'a fee more than his purse')
        ah.buy(listing, 900)
        said(ah.reprice(seller, listing, LOG, 800), S.LISTING_GONE, 'once it has sold')
        assert(ah.listings(seller:getID()).rows[1].price == 900, 'what it sold for stands')
    end)

    it('keeps her stack, in memory and in her rows, when the listing is refused', function()
        holder:addItem(LOG, 12)
        local gil = seller:getGil()
        assert(ah.refuseListings(true), 'the test database refuses listings')
        local status = ah.list(seller, holder, INV, slotOf(holder, LOG), LOG, true, 1200)
        ah.refuseListings(false)
        assert(status ~= S.OK, 'the listing was refused')
        assert(holder:getItemCount(LOG) == 12, string.format('her stack is still in her bag: %d logs', holder:getItemCount(LOG)))
        agree(holder, 'her bag after the refusal')
        agree(seller, 'his purse after the refusal')
        assert(seller:getGil() == gil, 'he paid no fee')
        assert(ah.listings(seller:getID()).open == 0, 'nothing went up')
    end)

    describe('when the database fails a move', function()
        -- The faults, by item_moves.h's Fault number
        local ROLLED_BACK         = 1 -- the COMMIT rolled back, and told so
        local COMMITTED           = 2 -- the COMMIT went through, and its answer was lost
        local LOST_BEFORE         = 3 -- the connection lost before the COMMIT
        local LOST_AT             = 4 -- the connection lost as the COMMIT was sent
        local COMMITTED_THEN_LOST = 5 -- the COMMIT went through, then the connection was lost
        local LOST_TWICE          = 6 -- lost before the COMMIT, and again while they are put back

        -- What the two hold, and what he has up: her logs and his, both purses,
        -- his listings on sale and the first one's price
        local function snapshot()
            local mine = ah.listings(seller:getID())
            return {
                herLogs = holder:getItemCount(LOG),
                hisLogs = seller:getItemCount(LOG),
                hisGil  = seller:getGil(),
                herGil  = holder:getGil(),
                open    = mine.open,
                price   = mine.rows[1] ~= nil and mine.rows[1].price or 0,
            }
        end

        local function same(got, want, what)
            for k, v in pairs(want) do
                assert(got[k] == v, string.format('%s: %s is %s, not %s', what, k, tostring(got[k]), tostring(v)))
            end
            agree(seller, what .. ', his inventory')
            agree(holder, what .. ', hers')
        end

        -- The four kinds of move: what each starts from, the move, and what it
        -- leaves when it happens, from what was there before it
        local kinds =
        {
            {
                what  = 'a listing of her whole stack',
                setup = function() holder:addItem(LOG, 12) end,
                move  = function() return (ah.list(seller, holder, INV, slotOf(holder, LOG), LOG, true, 1200)) end,
                after = function(b) return { herLogs = b.herLogs - 12, hisGil = b.hisGil - ah.fee(1200, true), herGil = b.herGil, open = b.open + 1 } end,
            },
            {
                what  = 'a listing of one of his own logs',
                setup = function() seller:addItem(LOG, 5) end,
                move  = function() return (ah.list(seller, seller, INV, slotOf(seller, LOG), LOG, false, 100)) end,
                after = function(b) return { hisLogs = b.hisLogs - 1, hisGil = b.hisGil - ah.fee(100, false), open = b.open + 1 } end,
            },
            {
                what  = 'a take down',
                setup = function(k)
                    seller:addItem(LOG, 5)
                    local _, _, listing = ah.list(seller, seller, INV, slotOf(seller, LOG), LOG, false, 100)
                    k.listing = listing
                end,
                move  = function(k) return (ah.cancel(seller, k.listing, LOG)) end,
                after = function(b) return { hisLogs = b.hisLogs + 1, hisGil = b.hisGil, open = b.open - 1 } end,
            },
            {
                what  = 'a new price',
                setup = function(k)
                    seller:addItem(LOG, 5)
                    local _, _, listing = ah.list(seller, seller, INV, slotOf(seller, LOG), LOG, false, 100)
                    k.listing = listing
                end,
                move  = function(k) return (ah.reprice(seller, k.listing, LOG, 300)) end,
                after = function(b) return { hisLogs = b.hisLogs, hisGil = b.hisGil - ah.fee(300, false), open = b.open, price = 300 } end,
            },
        }

        -- A move failed on purpose: the fault armed, the move made, and the fault met
        local function failed(fault, kind)
            ah.failNext(fault)
            local status = kind.move(kind)
            assert(ah.pending() == 0, kind.what .. ': the fault armed for it was never met')
            return status
        end

        -- Each kind failed one way that leaves the database as before it: the move
        -- did not happen, both are as their rows are, and the same move made again
        -- goes through, nothing of theirs left claimed
        local function leftUndone(fault, how)
            for _, kind in ipairs(kinds) do
                it('leaves ' .. kind.what .. ' undone', function()
                    kind.setup(kind)
                    local before = snapshot()
                    local status = failed(fault, kind)
                    assert(status ~= S.OK, kind.what .. ', ' .. how .. ': said it went through')
                    same(snapshot(), before, kind.what .. ', ' .. how)
                    local again = kind.move(kind)
                    said(again, S.OK, kind.what .. ' made again')
                    same(snapshot(), kind.after(before), kind.what .. ', made again')
                end)
            end
        end

        -- Each kind failed one way that commits it: the move happened, said so,
        -- and both are as their rows are
        local function keptAnyway(fault, how)
            for _, kind in ipairs(kinds) do
                it('keeps ' .. kind.what, function()
                    kind.setup(kind)
                    local before = snapshot()
                    local status = failed(fault, kind)
                    said(status, S.OK, kind.what .. ', ' .. how)
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

    it("never takes back another's listing", function()
        seller:addItem(LOG, 2)
        local _, _, listing = ah.list(seller, seller, INV, slotOf(seller, LOG), LOG, false, 100)
        said(ah.cancel(holder, listing, LOG), S.LISTING_GONE, "his listing taken back by another")
        assert(holder:getItemCount(LOG) == 0 and ah.listings(seller:getID()).open == 1, 'it stays up, his')
        said(ah.cancel(seller, listing, WARP), S.LISTING_GONE, 'his listing named with another item')
    end)

    it('shows a sale while its gil waits in his delivery box', function()
        seller:addItem(LOG, 3)
        local _, _, first = ah.list(seller, seller, INV, slotOf(seller, LOG), LOG, false, 100)
        local _, _, second = ah.list(seller, seller, INV, slotOf(seller, LOG), LOG, false, 120)
        ah.buy(first, 100)
        local mine = ah.listings(seller:getID())
        assert(mine.open == 1, 'one is still on sale')
        assert(#mine.rows == 2 and mine.rows[1].listing == second and not mine.rows[1].sold, 'on sale first')
        assert(mine.rows[2].listing == first and mine.rows[2].sold and mine.rows[2].price == 100, 'then the sale, at what it sold for')
        ah.collect(seller:getID())
        mine = ah.listings(seller:getID())
        assert(#mine.rows == 1 and mine.rows[1].listing == second, 'its gil collected, the sale is off his status')
    end)
end)
