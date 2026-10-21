-----------------------------------
-- Cardian: the party rents with the player
-- (modules/cardian/lua/chocobo_ride.lua)
--
-- Under xi_test nobody rides along: src/test/tests/cardian_pawn_stubs.cpp
-- answers cardianRidersBeside with nobody. A test that wants a rider beside
-- the player mocks it with his partner, riding free (a body of the world) or
-- paying her way (one of his own). The rental is Upper Jeuno's,
-- finished by upstream's own handler, which sets a rider down in Batallia
-- Downs.
-----------------------------------
describe('Cardian chocobo ride', function()
    ---@type CClientEntityPair
    local player
    ---@type CClientEntityPair
    local partner

    local purse = 100000
    local csid  = 10 -- the stable's event; the handler asks only that it is the success it names

    before_each(function()
        player  = xi.test.world:spawnPlayer()
        partner = xi.test.world:spawnPlayer()
        player:setLevel(30)
        partner:setLevel(30)
        player:gotoZone(xi.zone.UPPER_JEUNO)
        partner:gotoZone(xi.zone.UPPER_JEUNO)
        player.actions:inviteToParty(partner)
        partner.actions:acceptPartyInvite()
        player:addKeyItem(xi.keyItem.CHOCOBO_LICENSE)
        player:setGil(purse)
        partner:setGil(0)
    end)

    local function partnerRidesAlong(free)
        stub('CBaseEntity.cardianRidersBeside', function(entity)
            return { { id = partner:getID(), free = free } }
        end)
    end

    -- Every move of anyone, kept by entity
    local function movesKept()
        local moved = {}
        stub('CBaseEntity.setPos', function(entity, ...)
            moved[entity:getID()] = { ... }
        end)
        return moved
    end

    -- His rental, as the stable's event finishes it; his price
    local function rent()
        local price = xi.chocobo.getPrice(player)
        player:setLocalVar('[CHOCOBO]price', price)
        xi.chocobo.renterOnEventFinish(player, csid, 0, csid)
        return price
    end

    it('rents him his chocobo alone with nobody beside him, as ever', function()
        local moved = movesKept()
        local price = rent()
        player.assert:hasEffect(xi.effect.MOUNTED)
        partner.assert.no:hasEffect(xi.effect.MOUNTED)
        assert(player:getGil() == purse - price, string.format('paid %d, not his price', purse - player:getGil()))
        assert(moved[player:getID()] ~= nil and moved[player:getID()][5] == xi.zone.BATALLIA_DOWNS, 'he was not set down in Batallia Downs')
        assert(moved[partner:getID()] == nil, 'the partner moved')
    end)

    it('takes a body of the world beside him along for free', function()
        partnerRidesAlong(true)
        local moved = movesKept()
        local price = rent()
        partner.assert:hasEffect(xi.effect.MOUNTED)
        assert(partner:getGil() == 0, 'she paid')
        assert(player:getGil() == purse - price, string.format('he paid %d, not his own price alone', purse - player:getGil()))
        local to = moved[partner:getID()]
        assert(to ~= nil and to[5] == xi.zone.BATALLIA_DOWNS, 'she was not set down beside him in Batallia Downs')
    end)

    it('has one of his own pay her own way from her gil', function()
        partnerRidesAlong(false)
        partner:setGil(purse)
        local price = rent()
        partner.assert:hasEffect(xi.effect.MOUNTED)
        assert(partner:getGil() < purse, 'she paid nothing')
        assert(player:getGil() == purse - price, string.format('he paid %d, not his own price alone', purse - player:getGil()))
    end)

    it('has his purse pay what hers cannot', function()
        partnerRidesAlong(false)
        local price = rent()
        partner.assert:hasEffect(xi.effect.MOUNTED)
        assert(partner:getGil() == 0, 'her empty purse changed')
        assert(player:getGil() < purse - price, 'he did not pay her way')
    end)

    it('leaves one of his own behind when the two purses cannot pay', function()
        partnerRidesAlong(false)
        local moved = movesKept()
        player:setGil(xi.chocobo.getPrice(player))
        rent()
        player.assert:hasEffect(xi.effect.MOUNTED)
        partner.assert.no:hasEffect(xi.effect.MOUNTED)
        assert(moved[partner:getID()] == nil, 'she was moved without a chocobo')
    end)

    -- The chocobo porter's question, where to: put to him by his rental at a
    -- city's stable, which sets him down in its field, and by nothing else
    local function questionsKept()
        local asked = {}
        stub('CBaseEntity.cardianPorterAsk', function(entity)
            asked[#asked + 1] = entity:getID()
        end)
        return asked
    end

    it("puts the porter's question to him as his rental sets him down", function()
        local asked = questionsKept()
        rent()
        player.assert:hasEffect(xi.effect.MOUNTED)
        assert(#asked == 1 and asked[1] == player:getID(), string.format('asked %d times', #asked))
    end)

    it('puts no question when he rents nothing', function()
        local asked = questionsKept()
        player:setLocalVar('[CHOCOBO]price', xi.chocobo.getPrice(player))
        xi.chocobo.renterOnEventFinish(player, csid, 1, csid) -- he turned the rental down
        player.assert.no:hasEffect(xi.effect.MOUNTED)
        assert(#asked == 0, 'asked with no rental')
    end)

    -- Her own rental on her trek, as pawn/riding.cpp asks it of the stable she
    -- has reached (the partner standing in for a cardian)
    describe('on her trek', function()
        local rentHers = function(payer, free)
            assert(xi.cardian and xi.cardian.chocobo and xi.cardian.chocobo.rent, 'xi.cardian.chocobo.rent did not load')
            return xi.cardian.chocobo.rent(partner, payer, free)
        end

        it('rents a body of the world her chocobo for free and leaves the crossing to her trek', function()
            local moved = movesKept()
            local out   = rentHers(player, true)
            assert(out.rented, tostring(out.refusal))
            partner.assert:hasEffect(xi.effect.MOUNTED)
            assert(partner:getGil() == 0 and player:getGil() == purse, 'someone paid')
            assert(moved[partner:getID()] == nil, 'the rental moved her: her trek crosses to the set-down')
        end)

        it('has one of his own pay from her gil', function()
            partner:setGil(purse)
            local out = rentHers(player, false)
            assert(out.rented, tostring(out.refusal))
            assert(partner:getGil() < purse and player:getGil() == purse, 'her gil did not pay')
        end)

        it('has his purse pay what hers cannot', function()
            local out = rentHers(player, false)
            assert(out.rented, tostring(out.refusal))
            assert(player:getGil() < purse, 'his purse did not pay')
        end)

        it('rents nothing when she cannot pay and he is not online', function()
            local out = rentHers(nil, false)
            assert(out.refusal ~= nil, 'she rented with no gil')
            partner.assert.no:hasEffect(xi.effect.MOUNTED)
        end)

        it('rents no second chocobo', function()
            partner:addStatusEffect(xi.effect.MOUNTED, { duration = 600, origin = partner, silent = true })
            local out = rentHers(player, true)
            assert(out.refusal ~= nil, 'she rented a second chocobo')
        end)
    end)
end)
