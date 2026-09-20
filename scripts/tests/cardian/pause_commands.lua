-- The combat pause's input gate (pause P3, src/map/pause/input_gate.h): a command
-- the player's own client sends while the simulation is held starts nothing, waits
-- as his one queued command, and goes to the game's own handler at the release.
--
-- The packets here go through the real dispatcher and meet the real gate: xi_test
-- links the pause's module (src/test/CMakeLists.txt). xi.cardian.pause is bound for
-- tests by src/test/tests/cardian_pawn_stubs.cpp.

---@diagnostic disable: inject-field
local ffi = require('ffi')

ffi.cdef [[
    typedef struct {
        uint16_t id : 9;
        uint16_t size : 7;
        uint16_t sync;
        uint32_t Mode;
    } CARDIAN_TEST_CAMP;
]]

describe('Combat pause: commands', function()
    ---@type CClientEntityPair
    local player

    ---@type CTestEntity
    local mob

    local pause = xi.cardian.pause

    local ACTION   = 0x01A
    local ITEM_USE = 0x037
    local CAMP     = 0x0E8

    local ATTACK     = 0x02
    local CAST_MAGIC = 0x03

    local function tenMinutesGoBy()
        for _ = 1, 9 do
            xi.test.world:skipTime(60)
        end

        for _ = 1, 60 do
            xi.test.world:skipTime(1)
        end
    end

    local function heal()
        local p = ffi.new('CARDIAN_TEST_CAMP')
        p.Mode = 0 -- toggle
        player.packets:send(CAMP, p, ffi.sizeof(p))
    end

    local function assertQueued(packetId, actionId, why)
        local gotPacket, gotAction = pause.queued(player:getID())
        assert(gotPacket == packetId and (actionId == nil or gotAction == actionId),
            string.format('%s: queued packet %s action %s', why, tostring(gotPacket), tostring(gotAction)))
    end

    before_each(function()
        player = xi.test.world:spawnPlayer({ zone = xi.zone.WEST_RONFAURE })
        mob    = player.entities:moveTo('Forest_Funguar')

        mob:respawn()
        mob.assert:isAlive()

        player:changeJob(xi.job.WHM)
        player:setLevel(10)
        player:addSpell(xi.magic.spell.CURE)
        player:setHP(player:getMaxHP())
        player:setMP(player:getMaxMP())
        player:setUnkillable(true)
        mob:setUnkillable(true)
        stub('xi.combat.physicalHitRate.getPhysicalHitRate', 1)
    end)

    after_each(function()
        pause.release()
        mob:setUnkillable(false)
    end)

    it('starts no spell while held, and casts it at the release', function()
        player:setHP(10)
        local mp = player:getMP()

        local idle = player:getCurrentAction()
        assert(pause.hold())
        player.actions:useSpell(player, xi.magic.spell.CURE)
        assert(player:getCurrentAction() == idle, 'a cast began while held: action ' .. tostring(player:getCurrentAction()))
        assertQueued(ACTION, CAST_MAGIC, 'the cast did not queue')

        tenMinutesGoBy()
        assert(player:getHP() == 10, 'the spell landed while held')
        assert(player:getMP() == mp, 'the spell was paid for while held')

        assert(pause.release())
        assert(pause.queued(player:getID()) == nil, 'the command is still queued after the release')
        xi.test.world:skipTime(3)
        assert(player:getHP() > 10, 'the queued spell was never cast after the release')
    end)

    it('draws no weapon while held, and engages at the release', function()
        -- She is on him already, so she is still beside him after the release.
        mob:updateEnmity(player)

        assert(pause.hold())
        player.actions:engage(mob)
        assert(not player:isEngaged(), 'he engaged while held')
        assertQueued(ACTION, ATTACK, 'the attack did not queue')

        tenMinutesGoBy()
        assert(not player:isEngaged(), 'he engaged while held')
        assert(player:getTP() == 0, 'he fought while held')

        assert(pause.release())
        assert(player:isEngaged(), 'the queued attack did not engage him at the release')
        for _ = 1, 8 do
            xi.test.world:skipTime(1)
        end

        assert(player:getTP() > 0, 'the fight never started after the release')
    end)

    it('keeps one command, the newest', function()
        player:setHP(10)
        local mp = player:getMP()

        assert(pause.hold())
        player.actions:useSpell(player, xi.magic.spell.CURE)
        player.actions:engage(mob)
        assertQueued(ACTION, ATTACK, 'the newer command did not replace the older')

        assert(pause.release())
        assert(player:isEngaged(), 'the newest command, the attack, did not go ahead')
        xi.test.world:skipTime(3)
        assert(player:getMP() == mp, 'the replaced command, the spell, was cast too')
    end)

    it('holds an item for the release', function()
        player:addItem(xi.item.MEAT_MITHKABOB)
        local kabob = player:findItem(xi.item.MEAT_MITHKABOB)
        assert(kabob)

        local idle = player:getCurrentAction()
        assert(pause.hold())
        player.actions:useItem(player, kabob:getSlotID(), xi.inventoryLocation.INVENTORY)
        assert(player:getCurrentAction() == idle, 'the item was begun while held: action ' .. tostring(player:getCurrentAction()))
        assertQueued(ITEM_USE, nil, 'the item did not queue')

        tenMinutesGoBy()
        player.assert.no:hasEffect(xi.effect.FOOD)

        assert(pause.release())
        xi.test.world:skipTime(5)
        player.assert:hasEffect(xi.effect.FOOD)
    end)

    it('drops a queued item whose slot holds something else by the release', function()
        -- The packet names a slot, and a bag can be tidied while held.
        player:addItem(xi.item.MEAT_MITHKABOB)
        local kabob = player:findItem(xi.item.MEAT_MITHKABOB)
        assert(kabob)
        local slot = kabob:getSlotID()

        assert(pause.hold())
        player.actions:useItem(player, slot, xi.inventoryLocation.INVENTORY)
        assertQueued(ITEM_USE, nil, 'the item did not queue')

        player:delItem(xi.item.MEAT_MITHKABOB, 1)
        player:addItem(xi.item.APPLE_PIE)
        local pie = player:findItem(xi.item.APPLE_PIE)
        assert(pie and pie:getSlotID() == slot, 'precondition: the pie took the kabob\'s slot')

        assert(pause.release())
        xi.test.world:skipTime(5)
        player.assert.no:hasEffect(xi.effect.FOOD)
    end)

    it('holds /heal for the release', function()
        assert(pause.hold())
        heal()
        player.assert.no:hasEffect(xi.effect.HEALING)
        assertQueued(CAMP, nil, '/heal did not queue')

        tenMinutesGoBy()
        player.assert.no:hasEffect(xi.effect.HEALING)

        assert(pause.release())
        player.assert:hasEffect(xi.effect.HEALING)
    end)

    it('lets through what is not a command', function()
        -- Shopping works through a pause on purpose: talking to an NPC is not queued.
        local npc = player.entities:moveTo('Stone_Monument')

        assert(pause.hold())
        player.actions:trigger(npc, { eventId = 900 })
        assert(pause.queued(player:getID()) == nil, 'talking to an NPC was queued')
    end)

    it('queues nothing when nothing is held', function()
        player:setHP(10)
        player.actions:useSpell(player, xi.magic.spell.CURE)
        assert(pause.queued(player:getID()) == nil, 'a command was queued with no hold')

        xi.test.world:skipTime(3)
        assert(player:getHP() > 10, 'the spell was not cast')
    end)
end)
