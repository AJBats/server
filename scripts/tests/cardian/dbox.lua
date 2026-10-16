-- A cardian's delivery box from her Mog House page (pawn/dbox.h; the Link's
-- DBOX): what was sent to her taken into her inventory, thrown away or sent
-- back, and what she sends set in her outgoing box for a character, sent,
-- called back, taken back unsent, or cleared once delivered -- each by the
-- game's own routine for it (utils/dboxutils), her box opened and closed again
-- around every request. What waits for her past the eight slots comes into the
-- first that frees.
--
-- xi.cardian.dbox is bound for xi_test alone (cardian_pawn_stubs.cpp): it does
-- what the Link's handler does on players of the test's, the place they stand
-- left out (the handler judges it).

describe('A cardian\'s delivery box', function()
    local dbox = xi.cardian.dbox
    local S    = dbox.status
    local A    = dbox.action
    local ST   = dbox.state
    local IN, OUT = 1, 2
    local LOG  = xi.item.ELM_LOG    -- stacks of twelve
    local PEARL = xi.item.LINKPEARL -- the game delivers none

    ---@type CClientEntityPair
    local her
    ---@type CClientEntityPair
    local him

    local function slotOf(player, itemId)
        return player:findItem(itemId):getSlotID()
    end

    local function said(status, want, what)
        assert(status == want, string.format('%s: status %s, wanted %s', what, tostring(status), tostring(want)))
    end

    -- One request: the status, with the box's slots read after it
    local function act(player, box, action, slot, invSlot, qty, to)
        local status = dbox.act(player, box, action, slot or 0, invSlot or 0, qty or 0, to or 0)
        assert(status ~= nil, 'the test\'s player has a session to lend its scheduler')
        return status, dbox.slots(player:getID(), box)
    end

    -- She sends `qty` logs from her inventory to him, set in slot 0 and sent
    local function sendLogs(qty)
        local status, box = act(her, OUT, A.SET, 0, slotOf(her, LOG), qty, him:getID())
        said(status, S.OK, 'logs set for him')
        assert(box[0].item == LOG and box[0].qty == qty and box[0].state == ST.WAITING and box[0].name == him:getName(), 'set in slot 0, for him, not sent')
        status, box = act(her, OUT, A.SEND, 0)
        said(status, S.OK, 'sent')
        assert(box[0].state == ST.SENT, 'on its way')
    end

    before_each(function()
        her = xi.test.world:spawnPlayer({ zone = xi.zone.SOUTHERN_SAN_DORIA })
        him = xi.test.world:spawnPlayer({ zone = xi.zone.SOUTHERN_SAN_DORIA })
        her:addItem(LOG, 12)
    end)

    it('sends a stack: off her bag, into his box, taken, then cleared from hers once delivered', function()
        sendLogs(5)
        assert(her:getItemCount(LOG) == 7, 'five logs left her bag as they were set')
        assert(dbox.waiting(him:getID()) == 1, 'it waits for him past his eight slots')

        local status, box = act(him, IN, A.LOOK)
        said(status, S.OK, 'his box looked at')
        assert(box[0].item == LOG and box[0].qty == 5 and box[0].state == ST.WAITING and box[0].name == her:getName(), 'pulled into his first slot, from her')
        assert(dbox.waiting(him:getID()) == 0, 'nothing waits beyond')

        status, box = act(him, IN, A.TAKE, 0)
        said(status, S.OK, 'taken')
        assert(him:getItemCount(LOG) == 5 and box[0].state == ST.EMPTY, 'into his inventory, his slot empty')

        status, box = act(her, OUT, A.LOOK)
        assert(box[0].state == ST.DELIVERED, 'delivered, in her box')
        status, box = act(her, OUT, A.CLEAR, 0)
        said(status, S.OK, 'cleared')
        assert(box[0].state == ST.EMPTY, 'her slot empty')
    end)

    it('takes back what is set and not sent, and calls back what is sent and not received', function()
        local status, box = act(her, OUT, A.SET, 0, slotOf(her, LOG), 4, him:getID())
        said(status, S.OK, 'set')
        status, box = act(her, OUT, A.TAKE, 0)
        said(status, S.OK, 'taken back unsent')
        assert(her:getItemCount(LOG) == 12 and box[0].state == ST.EMPTY, 'all twelve hers again')

        sendLogs(3)
        status = act(her, OUT, A.TAKE, 0)
        said(status, S.ALREADY_SENT, 'taken back once sent')
        status, box = act(her, OUT, A.RECALL, 0)
        said(status, S.OK, 'called back')
        assert(box[0].state == ST.WAITING and dbox.waiting(him:getID()) == 0, 'set again, and gone from what waits for him')
        status = act(her, OUT, A.TAKE, 0)
        said(status, S.OK, 'then taken back')
        assert(her:getItemCount(LOG) == 12, 'all twelve hers again')
    end)

    it('throws away what was sent to her, or sends it back', function()
        sendLogs(2)
        local status, box = act(him, IN, A.DROP, 0)
        said(status, S.OK, 'thrown away')
        assert(box[0].state == ST.EMPTY and him:getItemCount(LOG) == 0, 'gone')

        status = act(her, OUT, A.CLEAR, 0)
        said(status, S.OK, 'her slot cleared: what she sent was received, then thrown away')
        local holds = her:getItemCount(LOG)
        sendLogs(2)
        status, box = act(him, IN, A.RETURN, 0)
        said(status, S.OK, 'sent back')
        assert(box[0].state == ST.EMPTY, 'gone from his box')
        status, box = act(her, IN, A.LOOK)
        assert(box[0].item == LOG and box[0].qty == 2 and box[0].name == him:getName(), 'in her box, from him')
        status = act(her, IN, A.TAKE, 0)
        said(status, S.OK, 'and taken')
        assert(her:getItemCount(LOG) == holds, 'hers again')
    end)

    it("refuses what the game would: an item it delivers nowhere, nobody by that charid, an empty slot", function()
        her:addItem(PEARL, 1)
        local status, box = act(her, OUT, A.SET, 0, slotOf(her, PEARL), 1, him:getID())
        said(status, S.NOT_DELIVERABLE, 'a linkpearl')
        assert(box[0].state == ST.EMPTY and her:getItemCount(PEARL) == 1, 'nothing set, the pearl hers')

        status = act(her, OUT, A.SET, 0, slotOf(her, LOG), 1, 4000000000)
        said(status, S.NO_RECIPIENT, 'nobody by that charid')
        status = act(her, OUT, A.SET, 0, slotOf(her, LOG), 13, him:getID())
        said(status, S.BAD_QUANTITY, 'more than the stack holds')
        status = act(him, IN, A.TAKE, 3)
        said(status, S.NO_ITEM, 'an empty slot')
        status = act(her, OUT, A.SEND, 2)
        said(status, S.NO_ITEM, 'nothing to send')
    end)

    it('tells a set\'s refusal before anything moves: what the set itself would refuse', function()
        local INV = xi.inventoryLocation.INVENTORY
        her:addItem(PEARL, 1)
        said(dbox.setRefusal(her, 0, INV, slotOf(her, PEARL), 1, him:getID()), S.NOT_DELIVERABLE, 'a linkpearl')
        said(dbox.setRefusal(her, 0, INV, slotOf(her, LOG), 1, 4000000000), S.NO_RECIPIENT, 'nobody by that charid')
        said(dbox.setRefusal(her, 0, INV, slotOf(her, LOG), 13, him:getID()), S.BAD_QUANTITY, 'more than the stack holds')
        said(dbox.setRefusal(her, 8, INV, slotOf(her, LOG), 1, him:getID()), S.MALFORMED, 'no ninth slot')
        said(dbox.setRefusal(her, 0, INV, slotOf(her, LOG), 0, him:getID()), S.BAD_QUANTITY, 'none of it')

        local status = act(her, OUT, A.SET, 0, slotOf(her, LOG), 1, him:getID())
        said(status, S.OK, 'a log set in slot 0')
        said(dbox.setRefusal(her, 0, INV, slotOf(her, LOG), 1, him:getID()), S.REFUSED, 'slot 0 holds it')
        said(dbox.setRefusal(her, 1, INV, slotOf(her, LOG), 1, him:getID()), S.OK, 'slot 1 free for another')
    end)

    it('sends gil: her inventory\'s first slot, set and sent as any stack, into his purse', function()
        her:addGil(5000)
        local gil = her:getGil()
        local status, box = act(her, OUT, A.SET, 0, 0, gil + 1, him:getID())
        said(status, S.BAD_QUANTITY, 'more gil than she has')
        status, box = act(her, OUT, A.SET, 0, 0, 1200, him:getID())
        said(status, S.OK, 'gil set for him')
        assert(box[0].item == 65535 and box[0].qty == 1200 and box[0].state == ST.WAITING, 'set in slot 0, as gil')
        status, box = act(her, OUT, A.SEND, 0)
        said(status, S.OK, 'sent')
        assert(her:getGil() == gil - 1200 and box[0].state == ST.SENT, 'out of her purse, on its way')

        local his = him:getGil()
        status, box = act(him, IN, A.LOOK)
        assert(box[0].item == 65535 and box[0].qty == 1200 and box[0].name == her:getName(), 'in his first slot, from her')
        status = act(him, IN, A.TAKE, 0)
        said(status, S.OK, 'taken')
        assert(him:getGil() == his + 1200, 'into his purse')
    end)

    it("takes the auction house's gil into her purse, and has nobody to send it back to", function()
        local ah = xi.cardian.ahsell
        ah.prepare()
        her:addGil(5000)
        local status, _, listing = ah.list(her, her, xi.inventoryLocation.INVENTORY, slotOf(her, LOG), LOG, false, 300)
        assert(status == ah.status.OK, 'a log listed')
        ah.buy(listing, 300)

        local box
        status, box = act(her, IN, A.LOOK)
        assert(box[0].item == 65535 and box[0].qty == 300, 'the sale\'s gil in her first slot')
        status = act(her, IN, A.RETURN, 0)
        said(status, S.NO_SENDER, 'the auction house\'s gil sent back')
        local gil = her:getGil()
        status, box = act(her, IN, A.TAKE, 0)
        said(status, S.OK, 'taken')
        assert(her:getGil() == gil + 300 and box[0].state == ST.EMPTY, 'into her purse')
    end)
end)
