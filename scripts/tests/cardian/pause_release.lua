-- The combat pause's release and its logout rule (pause P5). A client counts ability
-- recasts and buff timers down on its own clock, which runs on through a hold, so the
-- release tells it again what is left of each. The treasure pool's five minutes hold
-- with the rest, but an item everybody has lotted on is given out at once, held or
-- not. Nobody logs out of a held game, and nobody on his way out holds one.
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
        uint16_t Mode;
        uint16_t Kind;
    } CARDIAN_TEST_REQLOGOUT;

    typedef struct {
        uint16_t id : 9;
        uint16_t size : 7;
        uint16_t sync;
        uint8_t  TrophyItemIndex;
        uint8_t  PropertyItemIndex;
        uint8_t  padding00[2];
    } CARDIAN_TEST_TROPHY_ENTRY;
]]

describe('Combat pause: the release and logging out', function()
    ---@type CClientEntityPair
    local player

    local pause = xi.cardian.pause

    local ABIL_RECAST  = 0x119
    local MISCDATA     = 0x063
    local REQLOGOUT    = 0x0E7
    local TROPHY_ENTRY = 0x041

    local STATUS_ICONS = 9 -- 0x063's type

    local LOGOUT_TOGGLE = 0
    local LOGOUT_OFF    = 2
    local KIND_LOGOUT   = 1

    local PROVOKE_RECAST = 5
    local PROTECT_ICON   = 40

    local function tenMinutesGoBy()
        for _ = 1, 9 do
            xi.test.world:skipTime(60)
        end

        for _ = 1, 60 do
            xi.test.world:skipTime(1)
        end
    end

    local function u16(packet, offset)
        return packet.data[offset] + packet.data[offset + 1] * 256
    end

    -- The seconds his client was last told are left on a recast; nil if it was told nothing.
    local function recastTold(recastId)
        local seconds = nil
        for _, packet in pairs(player.packets:getIncoming()) do
            if packet.type == ABIL_RECAST then
                seconds = 0
                for slot = 0, 30 do
                    local at = 0x04 + slot * 8
                    if packet.data[at + 3] == recastId then
                        seconds = u16(packet, at)
                    end
                end
            end
        end

        return seconds
    end

    -- When his client was last told a buff ends, in the packet's own units (1/60 s of
    -- Vana'diel time); nil if it was told no buff icons with this one among them.
    local function expiryTold(icon)
        local expiry = nil
        for _, packet in pairs(player.packets:getIncoming()) do
            if packet.type == MISCDATA and u16(packet, 0x04) == STATUS_ICONS then
                expiry = nil
                for slot = 0, 31 do
                    if u16(packet, 0x08 + slot * 2) == icon then
                        local at = 0x48 + slot * 4
                        expiry = u16(packet, at) + u16(packet, at + 2) * 65536
                    end
                end
            end
        end

        return expiry
    end

    local function requestLogout(mode)
        local p = ffi.new('CARDIAN_TEST_REQLOGOUT')
        p.Mode = mode
        p.Kind = KIND_LOGOUT
        player.packets:send(REQLOGOUT, p, ffi.sizeof(p))
    end

    before_each(function()
        player = xi.test.world:spawnPlayer({ zone = xi.zone.GM_HOME })
        xi.test.world:skipTime(1)
    end)

    after_each(function()
        pause.release()
        pause.forgetDrift() -- a hold lets real milliseconds by: leave Lua's clock level for the suites after this
    end)

    it('tells his client what is left of a recast at the release', function()
        player:addRecast(xi.recast.ABILITY, PROVOKE_RECAST, 30)
        xi.test.world:skipTime(10)

        pause.hold()
        tenMinutesGoBy()
        player.packets:clear()
        pause.release()

        local seconds = recastTold(PROVOKE_RECAST)
        assert(seconds ~= nil, 'the release sent no recast packet')
        assert(seconds >= 18 and seconds <= 20, string.format('told %d seconds, about 20 are left', seconds))
    end)

    -- The test's real clock hardly moves, so a buff with the same time left is told the
    -- same end, before the hold and at the release.
    it('tells his client his buff timers at the release', function()
        player:addStatusEffect(xi.effect.PROTECT, { power = 10, duration = 300, origin = player })
        xi.test.world:skipTime(1)
        local before = expiryTold(PROTECT_ICON)
        assert(before ~= nil, 'his client was never told of Protect')

        pause.hold()
        tenMinutesGoBy()
        player.packets:clear()
        pause.release()

        local after = expiryTold(PROTECT_ICON)
        assert(after ~= nil, 'the release sent no buff icons with Protect among them')

        local drift = ((after - before + 2 ^ 31) % 2 ^ 32 - 2 ^ 31) / 60
        assert(math.abs(drift) <= 3, string.format('Protect\'s end moved %.0f seconds over the hold', drift))
    end)

    describe('the treasure pool', function()
        ---@type CClientEntityPair
        local other

        local function lot(who)
            local p = ffi.new('CARDIAN_TEST_TROPHY_ENTRY')
            p.TrophyItemIndex = 0
            who.packets:send(TROPHY_ENTRY, p, ffi.sizeof(p))
        end

        local function eitherHas(item)
            return player:hasItem(item) or other:hasItem(item)
        end

        before_each(function()
            other = xi.test.world:spawnPlayer({ zone = xi.zone.GM_HOME })
            player.actions:inviteToParty(other)
            other.actions:acceptPartyInvite()
            xi.test.world:skipTime(1)

            player:addTreasure(xi.item.FIRE_CRYSTAL)
        end)

        it('keeps an item nobody lots on through a hold', function()
            pause.hold()
            tenMinutesGoBy()
            assert(not eitherHas(xi.item.FIRE_CRYSTAL), 'the pool gave the item out in a held game')

            pause.release()
            tenMinutesGoBy()
            assert(eitherHas(xi.item.FIRE_CRYSTAL), 'the pool never gave the item out after the release')
        end)

        it('gives an item out while held once everybody has lotted', function()
            pause.hold()
            lot(player)
            assert(not eitherHas(xi.item.FIRE_CRYSTAL), 'the item went before everybody had lotted')

            lot(other)
            assert(eitherHas(xi.item.FIRE_CRYSTAL), 'everybody lotted and the item stayed in the pool')
        end)
    end)

    it('starts no logout while held, and says why', function()
        pause.hold()
        player.packets:clear()
        requestLogout(LOGOUT_TOGGLE)
        xi.test.world:skipTime(1)

        assert(not player:hasStatusEffect(xi.effect.LEAVEGAME), 'a logout began in a held game')

        local told = false
        for _, packet in pairs(player.packets:getIncoming()) do
            if packet.type == 0x017 then
                told = true
            end
        end

        assert(told, 'he was not told why')

        pause.release()
        tenMinutesGoBy()
        assert(not player:hasStatusEffect(xi.effect.LEAVEGAME), 'the refused logout was kept for the release')
    end)

    it('lets him stop a logout already counting down', function()
        requestLogout(LOGOUT_TOGGLE)
        assert(player:hasStatusEffect(xi.effect.LEAVEGAME), 'the logout did not begin')

        pause.hold()
        requestLogout(LOGOUT_OFF)
        assert(not player:hasStatusEffect(xi.effect.LEAVEGAME), 'he could not stop his logout while held')
    end)

    it('gives no hold to a player on his way out', function()
        requestLogout(LOGOUT_TOGGLE)
        assert(player:hasStatusEffect(xi.effect.LEAVEGAME), 'the logout did not begin')

        local refusal = player:cardianPause()
        assert(refusal ~= '', 'the button answered nothing')
        assert(not pause.isHeld(), 'a player logging out took the hold')
    end)
end)
