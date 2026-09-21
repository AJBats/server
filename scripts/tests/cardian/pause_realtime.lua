-- What keeps working through a combat pause (pause P3, the clock sweep). A hold stops
-- the simulation clock and nothing else: a guard or a throttle that measures real time
-- must go on measuring it, or it never reopens until the release. Each test here lets
-- real time go by while held (skipTime moves both clocks; the held one stands still).
--
-- xi.cardian.pause is bound for tests by src/test/tests/cardian_pawn_stubs.cpp.

describe('Combat pause: real time runs on', function()
    ---@type CClientEntityPair
    local player

    ---@type CClientEntityPair
    local other

    local pause = xi.cardian.pause

    local CHAR_UPDATE   = 0x00D
    local TRADE_REQUEST = 0x021
    local CHAR_STATUS   = 0x037

    local function received(who, packetType)
        local count = 0
        for _, packet in pairs(who.packets:getIncoming()) do
            if packet.type == packetType then
                count = count + 1
            end
        end

        return count
    end

    before_each(function()
        player = xi.test.world:spawnPlayer({ zone = xi.zone.GM_HOME })
        other  = xi.test.world:spawnPlayer({ zone = xi.zone.GM_HOME })
        other:setPos(player:getXPos(), player:getYPos(), player:getZPos())
        xi.test.world:skipTime(1)
    end)

    after_each(function()
        pause.release()
        pause.forgetDrift() -- a hold lets real milliseconds by: leave Lua's clock level for the suites after this
    end)

    it('lets him sort his bag as often as he likes', function()
        -- The sort guard allows one sort a second, and logs out whoever keeps beating it.
        assert(pause.hold())

        for round = 1, 6 do
            player:addItem(xi.item.FIRE_ARROW, 10)
            player:addItem(xi.item.FIRE_ARROW, 10)
            local slotsBefore = player:getFreeSlotsCount()

            player.actions:sortContainer(xi.inv.INVENTORY)
            assert(player:getFreeSlotsCount() > slotsBefore, string.format('sort %d while held merged nothing', round))

            xi.test.world:skipTime(2)
        end
    end)

    -- A client is shown the players around it after its position packet asks, at the
    -- zone's next player sync.
    local function looksAround(who)
        who.actions:move(who:getXPos(), who:getYPos(), who:getZPos(), 0)
    end

    it('goes on sending his updates as his gear changes', function()
        -- His updates leave at most every quarter second, held or not. His own status
        -- packet leaves in that same throttled step as what others are told of him.
        player:addItem(xi.item.BRONZE_DAGGER)
        local dagger = player:findItem(xi.item.BRONZE_DAGGER)
        assert(dagger)

        assert(pause.hold())
        xi.test.world:skipTime(1)

        for round = 1, 3 do
            player.packets:clear()
            if round % 2 == 1 then
                player.actions:equipSet({ { index = dagger:getSlotID(), kind = xi.slot.MAIN, container = 0 } })
            else
                player.actions:equipSet({ { index = 0, kind = xi.slot.MAIN, container = 0 } })
            end

            xi.test.world:skipTime(1)
            assert(received(player, CHAR_STATUS) > 0, string.format('gear change %d while held sent no update', round))
        end
    end)

    it('shows a client that looks around the players there', function()
        -- The player sync is paced, and one pass has gone by held before he asks.
        assert(pause.hold())
        xi.test.world:skipTime(2)

        other.packets:clear()
        looksAround(other)
        xi.test.world:skipTime(2)
        assert(received(other, CHAR_UPDATE) > 0, 'held, he was never shown the player beside him')
    end)

    it('lets an unanswered trade invite lapse', function()
        -- An invite nobody answers blocks its target for sixty seconds.
        local third = xi.test.world:spawnPlayer({ zone = xi.zone.GM_HOME })
        third:setPos(player:getXPos(), player:getYPos(), player:getZPos())

        assert(pause.hold())
        player.actions:tradeRequest(other)
        assert(received(other, TRADE_REQUEST) == 1, 'precondition: the first invite arrived')

        xi.test.world:skipTime(61)
        other.packets:clear()
        third.actions:tradeRequest(other)
        assert(received(other, TRADE_REQUEST) == 1, 'a minute on, held, the first invite still blocks the next')
    end)

    it('counts held minutes as playtime', function()
        local before = player:getPlaytime(true)

        assert(pause.hold())
        xi.test.world:skipTime(600)

        local gained = player:getPlaytime(true) - before
        assert(gained >= 600, string.format('ten held minutes counted as %d s of playtime', gained))
    end)
end)
