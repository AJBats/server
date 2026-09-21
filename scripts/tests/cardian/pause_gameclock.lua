-- Lua's clock is the game clock (pause P7): GetSystemTime(), the JST functions and
-- the expiry of variables all read the Vana'diel calendar on the Unix scale, so every
-- deadline a script keeps -- a mob's next two-hour, a lottery pop, "come back after
-- JST midnight", a variable's expiry -- stands still in a hold and keeps the time it
-- had left. The arithmetic is pinned in src/test/tests/cardian_calendar_tests.cpp.
--
-- The harness's skipTime moves the simulation clock alone, so time going by here is
-- both: skipTime and the Earth clock's seconds (pause.realSecondsGoBy). xi.cardian.pause
-- is bound for tests by src/test/tests/cardian_pawn_stubs.cpp.

describe('Combat pause: Lua reads the game clock', function()
    ---@type CClientEntityPair
    local player

    local pause = xi.cardian.pause

    local tenMinutes = 600

    local function goBy(seconds)
        xi.test.world:skipTime(seconds)
        pause.realSecondsGoBy(seconds)
    end

    before_each(function()
        player = xi.test.world:spawnPlayer({ zone = xi.zone.GM_HOME })
        xi.test.world:skipTime(1)
    end)

    after_each(function()
        pause.release()
        pause.forgetDrift()
    end)

    it('keeps the time a script deadline had left through a hold', function()
        -- What a mob script does: mob:setLocalVar('next', GetSystemTime() + 30)
        local deadline = GetSystemTime() + 30

        pause.hold()
        goBy(tenMinutes)
        assert(GetSystemTime() < deadline, string.format('the deadline came due in a held game, %d s ago', GetSystemTime() - deadline))

        pause.release()
        local left = deadline - GetSystemTime()
        assert(left >= 28 and left <= 30, string.format('30 s were left at the hold, %d at the release', left))

        goBy(31)
        assert(GetSystemTime() >= deadline, 'the deadline never came due after the release')
    end)

    it('keeps a deadline taken from getVanaMidnight on the Vana\'diel midnight it names', function()
        xi.test.world:setVanaTime(12, 0)
        local day      = VanadielUniqueDay()
        local midnight = getVanaMidnight()

        pause.hold()
        goBy(tenMinutes)
        pause.release()

        assert(VanadielUniqueDay() == day, 'precondition: the calendar held')
        assert(GetSystemTime() < midnight, 'the wait for midnight ended in the hold, with the day unchanged')

        -- it comes due as the day turns: a Vana'diel minute is 2.4 s
        goBy(midnight - GetSystemTime() - 5)
        assert(VanadielUniqueDay() == day, 'the day turned before the deadline')
        goBy(10)
        assert(GetSystemTime() >= midnight and VanadielUniqueDay() == day + 1, 'the deadline and the day did not turn together')
    end)

    it('keeps the wait for JST midnight as long as it was', function()
        local left = JstMidnight() - GetSystemTime()
        local hour = JstHour()

        pause.hold()
        goBy(2 * 3600)
        pause.release()

        local leftNow = JstMidnight() - GetSystemTime()
        assert(math.abs(leftNow - left) <= 2, string.format('%d s were left to JST midnight at the hold, %d at the release', left, leftNow))
        assert(JstHour() == hour, 'the JST hour moved in a held game')
    end)

    it('expires a variable by the game clock', function()
        player:setCharVar('[TEST]Lockout', 1, GetSystemTime() + 60)

        pause.hold()
        goBy(tenMinutes)
        assert(player:getCharVar('[TEST]Lockout') == 1, 'the variable expired in a held game')

        pause.release()
        assert(player:getCharVar('[TEST]Lockout') == 1, 'the variable expired at the release')

        goBy(61)
        assert(player:getCharVar('[TEST]Lockout') == 0, 'the variable never expired after the release')
    end)

    it('still tells the real age of a character', function()
        -- getTimeCreated is a real instant: a pause does not make a character younger
        local age = GetSystemTime() - player:getTimeCreated()

        pause.hold()
        goBy(tenMinutes)
        pause.release()

        local older = GetSystemTime() - player:getTimeCreated() - age
        assert(older >= 599 and older <= 602, string.format('ten real minutes went by, the character aged %d s', older))
    end)

    it('says when the party last changed on the same clock', function()
        local other = xi.test.world:spawnPlayer({ zone = xi.zone.GM_HOME })
        player.actions:inviteToParty(other)
        other.actions:acceptPartyInvite()
        goBy(10)

        pause.hold()
        goBy(tenMinutes)
        pause.release()

        local ago = GetSystemTime() - player:getPartyLastMemberJoinedTime()
        assert(ago >= 9 and ago <= 12, string.format('she joined 10 s of game time ago, the script is told %d', ago))
    end)
end)
