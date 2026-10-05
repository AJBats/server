-----------------------------------
-- Cardian: the party warps with the player
-- (modules/cardian/lua/party_warp.lua)
--
-- Under xi_test nobody is a cardian and no addon is bound to ask through:
-- src/test/tests/cardian_pawn_stubs.cpp answers isCardian with no and
-- cardianOfferPartyWarp with no. A test that wants a cardian beside the
-- player mocks one on CBaseEntity.isCardian, and one that wants the question
-- put mocks the offer and keeps the purchase it was handed, to answer it as
-- the server would (xi.cardian.partyWarp.resolve). The warps bought here are
-- West Ronfaure's outpost vendor's home warp and a town teleporter's warp to
-- Gustaberg, each finished by upstream's own handler.
-----------------------------------
describe('Cardian party warp', function()
    -- The module's table; an empty one if it did not load, so a test says so
    local partyWarp = xi.cardian and xi.cardian.partyWarp or {}

    ---@type CClientEntityPair
    local player
    ---@type CClientEntityPair
    local partner

    local purse    = 100000
    local homeWarp = 2   -- the vendor's option: home, paid in gil
    local townNpc  = 716 -- Jeanvirgaud's teleporter event (Northern San d'Oria)

    before_each(function()
        player  = xi.test.world:spawnPlayer()
        partner = xi.test.world:spawnPlayer()
        player:setNation(xi.nation.BASTOK)
        partner:setNation(xi.nation.SANDORIA)
        player:setLevel(30)
        player:gotoZone(xi.zone.WEST_RONFAURE)
        partner:gotoZone(xi.zone.WEST_RONFAURE)
        player.actions:inviteToParty(partner)
        partner.actions:acceptPartyInvite()
        player:setGil(purse)
        -- the Ronfaure and Gustaberg outposts' warps are his, so they are sold to him at their fee
        player:addTeleport(xi.nation.BASTOK, xi.region.RONFAURE + 5)
        player:addTeleport(xi.nation.BASTOK, xi.region.GUSTABERG + 5)
    end)

    local function partnerIsCardian()
        stub('CBaseEntity.isCardian', function(entity)
            return entity:getID() == partner:getID()
        end)
    end

    -- The question put, as the server would be handed it: kept to answer
    local function askedKept()
        local asked = {}
        stub('CBaseEntity.cardianOfferPartyWarp', function(entity, purchase, cardians, seconds)
            asked.purchase, asked.cardians, asked.seconds = purchase, cardians, seconds
            return true
        end)
        return asked
    end

    -- Every move of anyone, kept by entity
    local function movesKept()
        local moved = {}
        stub('CBaseEntity.setPos', function(entity, ...)
            moved[entity:getID()] = { ... }
        end)
        return moved
    end

    local function buyHomeWarp()
        xi.conquest.vendorOnEventFinish(player, homeWarp, xi.region.RONFAURE)
    end

    local homeFee = function()
        return xi.conquest.outpostFee(player, xi.region.RONFAURE)
    end

    it('sells a player with nobody beside him his warp at once, as ever', function()
        assert(homeFee() > 0, 'the warp costs nothing: the test proves no charge')
        buyHomeWarp()
        player.assert:hasEffect(xi.effect.TELEPORT)
        assert(player:getGil() == purse - homeFee(), string.format('paid %d, not the fee', purse - player:getGil()))
    end)

    it('sells it at once when no addon is there to ask', function()
        partnerIsCardian()
        buyHomeWarp()
        player.assert:hasEffect(xi.effect.TELEPORT)
        assert(player:getGil() == purse - homeFee(), 'not paid at once')
    end)

    it('leaves a party member who is not a cardian out of the question', function()
        local asked = askedKept()
        buyHomeWarp()
        assert(asked.purchase == nil, 'asked with no cardian beside him')
        player.assert:hasEffect(xi.effect.TELEPORT)
    end)

    it('charges nothing and moves nobody while he is asked, naming the cardian beside him', function()
        partnerIsCardian()
        local asked = askedKept()

        buyHomeWarp()
        player.assert.no:hasEffect(xi.effect.TELEPORT)
        assert(player:getGil() == purse, 'charged before the answer')
        assert(asked.purchase ~= nil, 'the question was not put')
        assert(asked.cardians ~= nil and #asked.cardians == 1 and asked.cardians[1] == partner:getID(), 'the cardian beside him was not named')
        assert(asked.seconds == 30, string.format('the question stands %s s', tostring(asked.seconds)))
    end)

    it('calls the whole warp off on a no: nothing charged, nobody moves', function()
        assert(partyWarp.resolve ~= nil, 'xi.cardian.partyWarp did not load')
        partnerIsCardian()
        local asked = askedKept()
        buyHomeWarp()

        local moved = movesKept()
        local went  = partyWarp.resolve(player, false, asked.purchase, asked.cardians)
        assert(went == 0, 'someone went on a no')
        player.assert.no:hasEffect(xi.effect.TELEPORT)
        assert(player:getGil() == purse, 'charged on a no')
        assert(next(moved) == nil, 'someone moved on a no')
    end)

    it('buys the warp once on a yes and sends the cardian beside him to his nation, not hers', function()
        partnerIsCardian()
        local asked = askedKept()
        buyHomeWarp()

        local moved = movesKept()
        local went  = partyWarp.resolve(player, true, asked.purchase, asked.cardians)
        assert(went == 1, string.format('%s went with him', tostring(went)))
        player.assert:hasEffect(xi.effect.TELEPORT)
        assert(player:getStatusEffect(xi.effect.TELEPORT):getPower() == xi.teleport.id.HOME_NATION, 'his warp goes elsewhere')
        assert(player:getGil() == purse - homeFee(), string.format('paid %d on the yes, not the fee once', purse - player:getGil()))
        local to = moved[partner:getID()]
        assert(to ~= nil, 'the cardian stayed')
        assert(to[5] == xi.zone.BASTOK_MINES, string.format('the cardian went to zone %s, not Bastok Mines', tostring(to[5])))
    end)

    it('buys nothing and moves nobody on a yes he can no longer pay for', function()
        partnerIsCardian()
        local asked = askedKept()
        buyHomeWarp()
        player:setGil(0)

        local moved = movesKept()
        local went  = partyWarp.resolve(player, true, asked.purchase, asked.cardians)
        assert(went == 0, 'someone went on an unpaid warp')
        player.assert.no:hasEffect(xi.effect.TELEPORT)
        assert(next(moved) == nil, 'someone moved on an unpaid warp')
    end)

    it('holds a teleporter\'s warp to an outpost the same way, and sends the cardian to that outpost', function()
        partnerIsCardian()
        local asked  = askedKept()
        local option = 5 + xi.region.GUSTABERG -- the gil option for Gustaberg
        local fee    = xi.conquest.outpostFee(player, xi.region.GUSTABERG)

        xi.conquest.teleporterOnEventFinish(player, townNpc, option, townNpc)
        player.assert.no:hasEffect(xi.effect.TELEPORT)
        assert(player:getGil() == purse, 'charged before the answer')

        local moved = movesKept()
        partyWarp.resolve(player, true, asked.purchase, asked.cardians)
        player.assert:hasEffect(xi.effect.TELEPORT)
        assert(player:getGil() == purse - fee, string.format('paid %d, not the fee once', purse - player:getGil()))
        local to = moved[partner:getID()]
        assert(to ~= nil and to[5] == xi.zone.NORTH_GUSTABERG, string.format('the cardian went to zone %s', tostring(to and to[5])))
    end)

    it('refuses a yes while a warp is already under way on him: nothing charged, nobody moves', function()
        partnerIsCardian()
        local asked = askedKept()
        buyHomeWarp()
        player:addStatusEffect(xi.effect.TELEPORT, { power = xi.teleport.id.WARP, duration = 60, origin = player })

        local moved = movesKept()
        local went  = partyWarp.resolve(player, true, asked.purchase, asked.cardians)
        assert(went == 0, 'someone went beside a warp already under way')
        assert(player:getGil() == purse, string.format('charged %d beside a warp already under way', purse - player:getGil()))
        assert(next(moved) == nil, 'someone moved beside a warp already under way')
    end)

    it('warps him dead on a yes, charged once, and his party with him', function()
        partnerIsCardian()
        local asked = askedKept()
        buyHomeWarp()
        player:setHP(0)
        xi.test.world:tickEntity(player)
        assert(player:isDead(), 'precondition: he is dead')

        local moved = movesKept()
        local went  = partyWarp.resolve(player, true, asked.purchase, asked.cardians)
        assert(went == 1, string.format('%s went with him', tostring(went)))
        assert(player:getGil() == purse - homeFee(), string.format('paid %d, not the fee once', purse - player:getGil()))
        local his = moved[player:getID()]
        assert(his ~= nil and his[5] == xi.zone.BASTOK_MINES, string.format('he went to zone %s, not Bastok Mines', tostring(his and his[5])))
        assert(moved[partner:getID()] ~= nil, 'the cardian stayed')
        player.assert.no:hasEffect(xi.effect.TELEPORT)
    end)

    it('leaves out a cardian who is no longer beside him on a yes', function()
        partnerIsCardian()
        local asked = askedKept()
        buyHomeWarp()
        partner:gotoZone(xi.zone.EAST_RONFAURE)

        local moved = movesKept()
        local went  = partyWarp.resolve(player, true, asked.purchase, asked.cardians)
        assert(went == 0 and moved[partner:getID()] == nil, 'a cardian in another zone went with him')
        player.assert:hasEffect(xi.effect.TELEPORT)
    end)
end)
