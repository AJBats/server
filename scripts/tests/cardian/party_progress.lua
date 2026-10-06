-----------------------------------
-- Cardian: the party progresses together
-- (modules/cardian/lua/party_progress.lua)
--
-- Under xi_test nobody is a cardian: the pawn module is not linked, and
-- src/test/tests/cardian_pawn_stubs.cpp answers isCardian with no. A test
-- that wants one mocks her with a stub on CBaseEntity.isCardian.
-----------------------------------
describe('Cardian party progress', function()
    local logId     = xi.mission.log_id.BASTOK
    local missionId = xi.mission.id.bastok.THE_ZERUHN_REPORT

    ---@type CClientEntityPair
    local player
    ---@type CClientEntityPair
    local partner

    -- A Bastokan clear of the Seekers of Adoulin opening, as the Bastok
    -- mission tests set up their player
    local function bastokan(p)
        p:setNation(xi.nation.BASTOK)
        p:addMission(xi.mission.log_id.SOA, xi.mission.id.soa.RUMORS_FROM_THE_WEST)
        local missionStatus = p:getMissionStatus(xi.mission.log_id.SOA)
        missionStatus = utils.mask.setBit(missionStatus, 0, true)
        missionStatus = utils.mask.setBit(missionStatus, 1, true)
        p:setMissionStatus(xi.mission.log_id.SOA, missionStatus)
        p:completeMission(xi.mission.log_id.SOA, xi.mission.id.soa.RUMORS_FROM_THE_WEST)
        p:setRank(1)
    end

    before_each(function()
        player  = xi.test.world:spawnPlayer()
        partner = xi.test.world:spawnPlayer()
        bastokan(player)
        bastokan(partner)
    end)

    -- The player takes The Zeruhn Report and carries it to Naji, the partner
    -- joining the party in the Metalworks before the report is handed in
    local function reportWithPartner()
        player:gotoZone(xi.zone.BASTOK_MARKETS)
        player.entities:gotoAndTrigger('Cleades', { eventId = 1000, finishOption = 0 })
        player:gotoZone(xi.zone.ZERUHN_MINES)
        player.entities:gotoAndTrigger('Makarim', { eventId = 121 })

        player:gotoZone(xi.zone.METALWORKS)
        partner:gotoZone(xi.zone.METALWORKS)
        player.actions:inviteToParty(partner)
        partner.actions:acceptPartyInvite()

        player.entities:gotoAndTrigger('Naji', { eventId = 710 })
        player.assert:hasCompletedMission(logId, missionId)
    end

    it('leaves a party member who is not a cardian out', function()
        reportWithPartner()
        partner.assert.no:hasCompletedMission(logId, missionId)
    end)

    it('completes the mission for a cardian in the party and the zone', function()
        stub('CBaseEntity.isCardian', function(entity)
            return entity:getID() == partner:getID()
        end)

        reportWithPartner()
        partner.assert:hasCompletedMission(logId, missionId)
    end)

    -- A grant a script makes by a direct call beside a completion (a job
    -- unlock, a pact, a title, a key item) reaches the cardians the
    -- completion reached; one with no completion beside it is the player's
    it('shares with a cardian a title granted right after the completion', function()
        stub('CBaseEntity.isCardian', function(entity)
            return entity:getID() == partner:getID()
        end)

        reportWithPartner()
        player:addTitle(xi.title.PURVEYOR_IN_TRAINING)
        assert(partner:hasTitle(xi.title.PURVEYOR_IN_TRAINING), 'the cardian present was given the title too')
    end)

    it('keeps from a cardian a title granted with no completion beside it', function()
        -- two who have completed nothing (before_each completed a mission for
        -- the others, which opens the window)
        local loner  = xi.test.world:spawnPlayer()
        local friend = xi.test.world:spawnPlayer()
        stub('CBaseEntity.isCardian', function(entity)
            return entity:getID() == friend:getID()
        end)

        loner:gotoZone(xi.zone.METALWORKS)
        friend:gotoZone(xi.zone.METALWORKS)
        loner.actions:inviteToParty(friend)
        friend.actions:acceptPartyInvite()
        loner:addTitle(xi.title.PURVEYOR_IN_TRAINING)
        assert(loner:hasTitle(xi.title.PURVEYOR_IN_TRAINING), 'the player has his title')
        assert(not friend:hasTitle(xi.title.PURVEYOR_IN_TRAINING), 'a title given outside a completion stays his')
    end)
end)
