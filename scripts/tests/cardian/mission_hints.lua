-----------------------------------
-- Cardian: mission hints
-- (modules/cardian/lua/mission_hints.lua, hints/sandoria_rank2.lua)
--
-- A cardian in the player's party says the step's lines in party chat
-- when he reaches it. The lines are caught by stubbing printToPlayer.
-- Under xi_test nobody is a cardian (cardian_pawn_stubs.cpp answers
-- isCardian with no), so the partner is mocked as one.
-----------------------------------
describe('Cardian mission hints', function()
    local SANDORIA = xi.mission.log_id.SANDORIA
    local mission  = xi.mission.id.sandoria

    ---@type CClientEntityPair
    local player
    ---@type CClientEntityPair
    local partner
    local said

    -- The content settings as the tests found them (the warp test sets the era's)
    local restrictContent = xi.settings.main.RESTRICT_CONTENT
    local enableSoa       = xi.settings.main.ENABLE_SOA

    after_each(function()
        xi.settings.main.RESTRICT_CONTENT = restrictContent
        xi.settings.main.ENABLE_SOA       = enableSoa
    end)

    local function step(id)
        for _, s in ipairs(xi.cardian.hints.steps) do
            if s.id == id then
                return s
            end
        end
        error('no hint step ' .. id)
    end

    -- What the partner said, from `from` on, as one list of lines
    local function linesSince(from)
        local out = {}
        for i = from, #said do
            assert(said[i].name == partner:getName(), string.format('a hint said by %s, not the cardian', tostring(said[i].name)))
            out[#out + 1] = said[i].line
        end
        return out
    end

    local function assertSays(from, id, field)
        local want = xi.cardian.hints.lines(step(id)[field or 'say'], player)
        local got  = linesSince(from)
        assert(#got == #want, string.format('%s: expected %d line(s), got %d', id, #want, #got))
        for i = 1, #want do
            assert(got[i] == want[i], string.format('%s line %d: expected %q, got %q', id, i, want[i], got[i]))
        end
    end

    local function cardianPartner()
        stub('CBaseEntity.isCardian', function(entity)
            return entity:getID() == partner:getID()
        end)
    end

    local function together(zone)
        player:gotoZone(zone)
        partner:gotoZone(zone)
        player.actions:inviteToParty(partner)
        partner.actions:acceptPartyInvite()
    end

    before_each(function()
        player  = xi.test.world:spawnPlayer()
        partner = xi.test.world:spawnPlayer()

        -- A San d'Orian at rank 2 with 2-1 done, as the mission tests set theirs up
        player:setNation(xi.nation.SANDORIA)
        player:addMission(xi.mission.log_id.SOA, xi.mission.id.soa.RUMORS_FROM_THE_WEST)
        local soa = player:getMissionStatus(xi.mission.log_id.SOA)
        soa = utils.mask.setBit(soa, 0, true)
        soa = utils.mask.setBit(soa, 1, true)
        player:setMissionStatus(xi.mission.log_id.SOA, soa)
        player:completeMission(xi.mission.log_id.SOA, xi.mission.id.soa.RUMORS_FROM_THE_WEST)
        player:setRank(2)
        player:setRankPoints(4000)
        for id = mission.SMASH_THE_ORCISH_SCOUTS, mission.THE_RESCUE_DRILL do
            player:addMission(SANDORIA, id)
            player:completeMission(SANDORIA, id)
        end

        said = {}
        stub('CBaseEntity.printToPlayer', function(entity, message, channel, name)
            if channel == xi.msg.channel.PARTY then
                said[#said + 1] = { name = name, line = message }
            end
        end)
    end)

    it('has a cardian say the step the player reaches', function()
        cardianPartner()
        together(xi.zone.SOUTHERN_SAN_DORIA)

        -- Talking to the gate guard between missions: the step is taking 2-2
        player.entities:gotoAndTrigger('Ambrotien', { eventId = 2009, finishOption = 104 })
        player.assert:hasMission(SANDORIA, mission.THE_DAVOI_REPORT)

        -- Between missions she says nothing (the gate guards are the player's
        -- to find); the accepted mission's event says its first step
        assertSays(1, 'sandoria.davoi.go-green')
    end)

    it('says a step once, however often the player looks', function()
        cardianPartner()
        player:addMission(SANDORIA, mission.THE_DAVOI_REPORT)
        together(xi.zone.DAVOI)

        player.entities:gotoAndTrigger('Zantaviat', { eventId = 100 })
        local after = #said
        assertSays(after - #step('sandoria.davoi.find-document').say + 1, 'sandoria.davoi.find-document')

        -- Zantaviat again, with the document still lost: nothing new
        player.entities:gotoAndTrigger('Zantaviat')
        assert(#said == after, 'a step said twice')
    end)

    it('says nothing, and marks nothing, with no cardian in the party', function()
        player:addMission(SANDORIA, mission.THE_DAVOI_REPORT)
        together(xi.zone.DAVOI)
        player.entities:gotoAndTrigger('Zantaviat', { eventId = 100 })
        assert(#said == 0, 'a hint without a cardian')

        -- A cardian joining later hears the step at the next look
        cardianPartner()
        xi.cardian.hints.check(player, false)
        assertSays(1, 'sandoria.davoi.find-document')
    end)

    it('answers !cardian hint from where he stands, and gives the specifics asked again', function()
        cardianPartner()
        player:addMission(SANDORIA, mission.THE_DAVOI_REPORT)
        player:setMissionStatus(SANDORIA, 1)
        together(xi.zone.DAVOI)
        xi.cardian.hints.check(player, false)
        assertSays(1, 'sandoria.davoi.find-document')

        -- In the goal's zone: what we're looking for, and which way
        local from = #said + 1
        assert(xi.cardian.hints.remind(player), 'no reminder')
        local got = linesSince(from)
        assert(#got == 1 and got[1]:find("^We're looking for the water he mentioned%."), string.format('asked: %q', tostring(got[1])))

        -- Asked again, not having moved: the specifics
        from = #said + 1
        assert(xi.cardian.hints.remind(player), 'no reminder')
        assertSays(from, 'sandoria.davoi.find-document', 'more')
    end)

    it('names the next zone on the road when asked on the way', function()
        cardianPartner()
        player:addMission(SANDORIA, mission.THE_DAVOI_REPORT)
        player:setLevel(16)
        together(xi.zone.LA_THEINE_PLATEAU)
        assert(xi.cardian.hints.stepOf(player).id == 'sandoria.davoi.go')
        local from = #said + 1
        assert(xi.cardian.hints.remind(player), 'no reminder')
        local got = linesSince(from)
        assert(#got == 1 and got[1] == 'On to Jugner Forest from here, on the road to Davoi.', string.format('asked: %q', tostring(got[1])))
    end)

    it('volunteers nothing when off, and the specifics when full', function()
        cardianPartner()
        player:addMission(SANDORIA, mission.THE_DAVOI_REPORT)
        player:setMissionStatus(SANDORIA, 1)
        together(xi.zone.DAVOI)
        assert(xi.cardian.hints.setLevel(player, 'off'))
        said = {}
        xi.cardian.hints.check(player, false)
        assert(#said == 0, 'hints off, yet she volunteered')

        assert(xi.cardian.hints.setLevel(player, 'full'))
        player:setMissionStatus(SANDORIA, 2)
        player:addKeyItem(xi.ki.LOST_DOCUMENT)
        said = {}
        xi.cardian.hints.check(player, false)
        assertSays(1, 'sandoria.davoi.return-document', 'more')
        assert(not xi.cardian.hints.setLevel(player, 'loud'), 'an unknown level was taken')
    end)

    it('follows the party level: green below 16, ready from 16', function()
        player:addMission(SANDORIA, mission.THE_DAVOI_REPORT)
        player:setLevel(11)
        assert(xi.cardian.hints.stepOf(player).id == 'sandoria.davoi.go-green')
        player:setLevel(16)
        assert(xi.cardian.hints.stepOf(player).id == 'sandoria.davoi.go')
    end)

    it('follows the refiner: gravel in, the top lever, the bottom lever', function()
        cardianPartner()
        player:addMission(SANDORIA, mission.JOURNEY_TO_BASTOK)
        player:setMissionStatus(SANDORIA, 5)
        player:addItem(xi.item.PICKAXE, 1)
        assert(xi.cardian.hints.stepOf(player).id == 'sandoria.bastok1.mine')
        player:setCharVar('refiner_input', 1)
        assert(xi.cardian.hints.stepOf(player).id == 'sandoria.bastok1.top-lever')
        player:setCharVar('refiner_input', 0)
        player:setCharVar('refiner_output', 1)
        assert(xi.cardian.hints.stepOf(player).id == 'sandoria.bastok1.bottom-lever')
        player:setCharVar('refiner_output', 0)
        player:addItem(xi.item.ONZ_OF_MYTHRIL_SAND, 1)
        assert(xi.cardian.hints.stepOf(player).id == 'sandoria.bastok1.hand-in')
    end)

    it('drops the map seller once we own the map', function()
        cardianPartner()
        player:addMission(SANDORIA, mission.JOURNEY_TO_BASTOK)
        player:setMissionStatus(SANDORIA, 5)
        player:addItem(xi.item.PICKAXE, 1)
        player:addKeyItem(xi.ki.MAP_OF_THE_PALBOROUGH_MINES)
        together(xi.zone.PORT_BASTOK)
        assert(xi.cardian.hints.stepOf(player).id == 'sandoria.bastok1.mine')
        xi.cardian.hints.check(player, false)
        -- the step's nudge alone: Port Bastok's note is the map seller, said only without the map
        assertSays(1, 'sandoria.bastok1.mine')
    end)

    it('gives warp tips only where home point crystals warp', function()
        cardianPartner()
        player:addMission(SANDORIA, mission.JOURNEY_ABROAD)
        player:setMissionStatus(SANDORIA, 6)
        together(xi.zone.SOUTHERN_SAN_DORIA)
        assert(xi.cardian.hints.stepOf(player).id == 'sandoria.journey.go-windurst')
        xi.cardian.hints.check(player, false)
        assert(#said == 2, string.format('warps: expected the route and the warp tip, got %d line(s)', #said))

        -- The era's rules (the prod profile): content held to before Seekers
        -- of Adoulin, whose era module leaves crystals setting the home point only
        xi.settings.main.RESTRICT_CONTENT = 1
        xi.settings.main.ENABLE_SOA       = 0
        player:setMissionStatus(SANDORIA, 7)
        local from = #said + 1
        xi.cardian.hints.check(player, false)
        assertSays(from, 'sandoria.journey.go-bastok')
        assert(#said - from + 1 == 1, 'no warps: a warp tip was said')
    end)

    it('says a zone note once, however often the player comes back', function()
        cardianPartner()
        player:addMission(SANDORIA, mission.JOURNEY_ABROAD)
        player:setMissionStatus(SANDORIA, 2)
        player:addKeyItem(xi.ki.LETTER_TO_THE_CONSULS_SANDORIA)
        together(xi.zone.VALKURM_DUNES)
        assert(xi.cardian.hints.stepOf(player).id == 'sandoria.journey.first-consul')
        xi.cardian.hints.check(player, false)
        local heard = #said
        assert(heard > #xi.cardian.hints.lines(step('sandoria.journey.first-consul').say, player), 'no Valkurm note with the step')

        -- Out to La Theine Plateau and back, the cardian with him: nothing new
        player:gotoZone(xi.zone.LA_THEINE_PLATEAU)
        partner:gotoZone(xi.zone.LA_THEINE_PLATEAU)
        player:gotoZone(xi.zone.VALKURM_DUNES)
        partner:gotoZone(xi.zone.VALKURM_DUNES)
        assert(xi.cardian.hints.stepOf(player).id == 'sandoria.journey.first-consul')
        assert(#player:getParty() == 2, 'the cardian left the party on the way')
        xi.cardian.hints.check(player, true)
        assert(#said == heard, string.format('the note was said again: %d line(s) more', #said - heard))
    end)
end)
