-- The combat pause's movement lock and its button (pause P4). Held, a player's client
-- is told speed 0 in his status packet, a position packet moves nobody on the server,
-- and both are as they were after the release: the lock is derived from the hold, so
-- there is nothing to restore. The button is `!cardian pause`, player:cardianPause().
--
-- xi.cardian.pause is bound for tests by src/test/tests/cardian_pawn_stubs.cpp.

describe('Combat pause: movement and the button', function()
    ---@type CClientEntityPair
    local player

    local pause = xi.cardian.pause

    local CHAR_STATUS = 0x037

    -- The speed his client was last told: the low twelve bits at 0x2C of his status packet.
    local function speedTold()
        local speed = nil
        for _, packet in pairs(player.packets:getIncoming()) do
            if packet.type == CHAR_STATUS then
                speed = packet.data[0x2C] + (packet.data[0x2D] % 16) * 256
            end
        end

        return speed
    end

    local function tenMinutesGoBy()
        for _ = 1, 9 do
            xi.test.world:skipTime(60)
        end

        for _ = 1, 60 do
            xi.test.world:skipTime(1)
        end
    end

    before_each(function()
        player = xi.test.world:spawnPlayer({ zone = xi.zone.GM_HOME })
        xi.test.world:skipTime(1)
    end)

    after_each(function()
        pause.release()
    end)

    it('tells his client speed 0 at once, and his own speed at the release', function()
        player.packets:clear()
        assert(pause.hold())
        assert(speedTold() == 0, 'held, his client was told speed ' .. tostring(speedTold()))

        player.packets:clear()
        assert(pause.release())
        local speed = speedTold()
        assert(speed ~= nil and speed > 0, 'released, his client was told speed ' .. tostring(speed))
    end)

    it('holds a mounted player too', function()
        player:addStatusEffect(xi.effect.MOUNTED, { power = 1, duration = 1800, origin = player })
        player.packets:clear()
        assert(pause.hold())
        assert(speedTold() == 0, 'held and mounted, his client was told speed ' .. tostring(speedTold()))
    end)

    it('keeps him where he stands, whatever his client says', function()
        local x, y, z = player:getXPos(), player:getYPos(), player:getZPos()
        local rot     = player:getRotPos()

        assert(pause.hold())
        player.actions:move(x + 5, y, z + 5, (rot + 64) % 256)
        tenMinutesGoBy()
        assert(player:getXPos() == x and player:getZPos() == z, string.format('he moved while held: %.1f, %.1f', player:getXPos() - x, player:getZPos() - z))
        assert(player:getRotPos() == rot, 'he turned while held')

        assert(pause.release())
        player.actions:move(x + 5, y, z + 5, (rot + 64) % 256)
        assert(player:getXPos() == x + 5, 'his client could not move him after the release')
    end)

    it('is the holder\'s to resume', function()
        local other = xi.test.world:spawnPlayer({ zone = xi.zone.GM_HOME })

        assert(player:cardianPause() == '', 'the button did not take the hold')
        assert(pause.isHeld())

        local refusal = other:cardianPause()
        assert(refusal ~= '', 'another player resumed a hold that was not his')
        assert(pause.isHeld(), 'the refused button still let go')

        assert(player:cardianPause() == '', 'the holder could not resume')
        assert(not pause.isHeld())
    end)
end)
