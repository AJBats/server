-- The combat pause, against a live scene (pause P2: the held clock and the AI tick
-- that stands down). "Ten minutes go by" is sim:skipTime, which moves real time and
-- ticks the world; a held simulation must come through it unchanged, and carry on
-- from where it stopped once released.
--
-- xi.cardian.pause here is the real manager (it is core), bound for tests by
-- src/test/tests/cardian_pawn_stubs.cpp.

describe('Combat pause', function()
    ---@type CClientEntityPair
    local player

    ---@type CTestEntity
    local mob

    local pause = xi.cardian.pause

    -- Ten minutes, as coarse skips and then fine ones: what matters to a held
    -- simulation is both that much time and that many ticks.
    local function tenMinutesGoBy()
        for _ = 1, 9 do
            xi.test.world:skipTime(60)
        end

        for _ = 1, 60 do
            xi.test.world:skipTime(1)
        end
    end

    local function snapshot()
        local s = {}
        for label, entity in pairs({ player = player, mob = mob }) do
            s[label .. '.x']   = entity:getXPos()
            s[label .. '.y']   = entity:getYPos()
            s[label .. '.z']   = entity:getZPos()
            s[label .. '.rot'] = entity:getRotPos()
            s[label .. '.hp']  = entity:getHP()
            s[label .. '.mp']  = entity:getMP()
            s[label .. '.tp']  = entity:getTP()
        end

        s['mob.ce']     = mob:getCE(player)
        s['mob.ve']     = mob:getVE(player)
        s['protect.ms'] = player:getStatusEffect(xi.effect.PROTECT):getTimeRemaining()
        return s
    end

    local function differences(before, after)
        local changed = {}
        for key, value in pairs(before) do
            if after[key] ~= value then
                table.insert(changed, string.format('%s: %s -> %s', key, tostring(value), tostring(after[key])))
            end
        end

        table.sort(changed)
        return changed
    end

    before_each(function()
        player = xi.test.world:spawnPlayer({ zone = xi.zone.WEST_RONFAURE })
        mob    = player.entities:moveTo('Forest_Funguar')

        -- The zone's mobs outlive a test: a fresh life, so no fight carries over.
        mob:respawn()
        mob.assert:isAlive()

        player:setLevel(10)
        player:setHP(player:getMaxHP())
        player:setUnkillable(true)
        mob:setUnkillable(true)
        stub('xi.combat.physicalHitRate.getPhysicalHitRate', 1)
    end)

    -- Both sides in it: she has enmity on him, he has his weapon out.
    local function startFight()
        mob:updateEnmity(player)
        player.actions:engage(mob)
        for _ = 1, 8 do
            xi.test.world:skipTime(1)
        end

        assert(player:getTP() > 0, 'precondition: the fight is on and he has landed a blow')
    end

    -- The zone's mobs outlive a test, and so would her flag.
    after_each(function()
        pause.release()
        pause.forgetDrift() -- a hold lets real milliseconds by: leave Lua's clock level for the suites after this
        mob:setUnkillable(false)
    end)

    it('is taken and let go', function()
        assert(not pause.isHeld(), 'precondition: nothing held')
        assert(pause.hold(), 'the hold was refused')
        assert(pause.isHeld())
        assert(not pause.hold(), 'a second hold was accepted')
        assert(pause.release(), 'the release was refused')
        assert(not pause.isHeld())
    end)

    it('holds a fight still and lets it carry on', function()
        startFight()
        local tpRunning = player:getTP()

        assert(pause.hold())
        tenMinutesGoBy()
        assert(player:getTP() == tpRunning, string.format('he gained TP while held: %d -> %d', tpRunning, player:getTP()))

        assert(pause.release())
        for _ = 1, 8 do
            xi.test.world:skipTime(1)
        end

        assert(player:getTP() > tpRunning, 'the fight did not carry on after the release')
    end)

    it('is amber: nothing in the scene changes while held', function()
        player:addStatusEffect(xi.effect.PROTECT, { power = 10, duration = 1800, origin = player })
        startFight()

        assert(pause.hold())
        local before = snapshot()
        tenMinutesGoBy()
        local changed = differences(before, snapshot())

        assert(#changed == 0, 'the scene changed while held:\n  ' .. table.concat(changed, '\n  '))
    end)

    it('makes a regen tick that was already due wait for the release', function()
        -- A hold is taken whenever the player asks, not on a tick boundary: a regen
        -- tick can be due and not yet served. It waits like everything else. The
        -- spawn tick moves the clock thirty seconds without a zone tick, which is
        -- how one is left due here.
        player:setHP(10)
        player:addStatusEffect(xi.effect.REGEN, { power = 5, duration = 1800, origin = player })
        xi.test.world:tick(xi.tick.ZONE)
        local hpBefore = player:getHP()
        xi.test.world:tick(xi.tick.SPAWN)
        assert(player:getHP() == hpBefore, 'precondition: no regen was served while the clock moved')

        assert(pause.hold())
        for _ = 1, 5 do
            xi.test.world:tick(xi.tick.ZONE)
        end

        assert(player:getHP() == hpBefore, string.format('regen was served while held, hp %d -> %d', hpBefore, player:getHP()))

        assert(pause.release())
        xi.test.world:tick(xi.tick.ZONE)
        assert(player:getHP() > hpBefore, 'the regen tick that was due never came after the release')
    end)

    it('lands a held cast after the time it had left', function()
        player:changeJob(xi.job.WHM)
        player:setLevel(10)
        player:addSpell(xi.magic.spell.CURE)
        player:setMP(player:getMaxMP())
        player:setHP(10)

        -- Cure takes two seconds: one of them goes by, then the hold.
        player.actions:useSpell(player, xi.magic.spell.CURE)
        xi.test.world:skipTime(1)
        assert(player:getHP() == 10, string.format('precondition: the cast is still in flight (hp %d, action %s)', player:getHP(), tostring(player:getCurrentAction())))

        assert(pause.hold())
        tenMinutesGoBy()
        assert(player:getHP() == 10, 'the cast landed while held')

        assert(pause.release())
        assert(player:getHP() == 10, 'the cast landed at the release, before its time')

        xi.test.world:skipTime(2)
        assert(player:getHP() > 10, 'the cast never landed after the release')
    end)

    it('stops a chase mid-stride and lets it finish', function()
        mob:updateEnmity(player)
        xi.test.world:skipTime(1)

        -- He steps well away; she comes after him, a step a tick.
        player:setPos(player:getXPos() + 20, player:getYPos(), player:getZPos())
        local startX, startZ = mob:getXPos(), mob:getZPos()
        for _ = 1, 3 do
            xi.test.world:skipTime(1)
        end

        local movedX, movedZ = mob:getXPos(), mob:getZPos()
        assert(movedX ~= startX or movedZ ~= startZ, 'precondition: she is on her way to him')

        assert(pause.hold())
        tenMinutesGoBy()
        assert(mob:getXPos() == movedX and mob:getZPos() == movedZ, 'she kept walking while held')

        assert(pause.release())
        for _ = 1, 3 do
            xi.test.world:skipTime(1)
        end

        assert(mob:getXPos() ~= movedX or mob:getZPos() ~= movedZ, 'she did not set off again after the release')
    end)

    it('holds a respawn timer', function()
        mob:setUnkillable(false)
        player:claimAndKillMob(mob)

        -- Five minutes to respawn; ten go by held.
        assert(pause.hold())
        tenMinutesGoBy()
        xi.test.world:tick(xi.tick.SPAWN)
        mob.assert.no:isSpawned()

        assert(pause.release())
        xi.test.world:skipTime(305)
        xi.test.world:tick(xi.tick.SPAWN)
        mob.assert:isSpawned()
    end)
end)
