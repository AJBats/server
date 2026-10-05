-----------------------------------
-- Cardian: the conquest exchange by proxy
-- (modules/cardian/lua/conquest_exchange.lua, the Link's CP_SHOP and CP_BUY)
--
-- The Link finds the cardian and the guard; the library sells to her from
-- the guard's stock. Here a player stands in for her at her own nation's
-- guard, in her nation's city (the guard's messages are the zone's).
--
-- The pawn module loads the library at init; under xi_test there is no pawn
-- module, so the test loads it itself.
-----------------------------------
require('modules/cardian/lua/conquest_exchange')

describe('Cardian conquest exchange', function()
    local kInstantWarp = 32929 -- 10 CP, Rare
    local kReturnRing  = 32930 -- 2500 CP
    local kChariotBand = 32933 -- an experience ring

    ---@type CClientEntityPair
    local buyer

    before_each(function()
        buyer = xi.test.world:spawnPlayer({ job = xi.job.WAR, level = 30, zone = xi.zone.PORT_BASTOK })
        buyer:setNation(xi.nation.BASTOK)
        buyer:setRank(1)
        buyer:addCP(1000)
    end)

    it('is loaded', function()
        assert(xi.cardian and xi.cardian.exchange, 'modules/cardian/lua/conquest_exchange.lua did not load')
    end)

    it('lists her own guard\'s stock by option, with her points', function()
        local shop = xi.cardian.exchange.shop(buyer, xi.nation.BASTOK)
        assert(shop.cp == 1000, string.format('her points: %s', tostring(shop.cp)))
        assert(shop.nation == xi.nation.BASTOK and not shop.foreign and not shop.blocked, 'her own nation\'s guard sells to her')
        assert(#shop.items > 0, 'the guard sells something')
        for i = 2, #shop.items do
            assert(shop.items[i - 1].option < shop.items[i].option, 'the stock comes by option')
        end
        local warp
        for _, it in ipairs(shop.items) do
            if it.option == kInstantWarp then
                warp = it
            end
        end
        assert(warp ~= nil and warp.price == 10 and warp.rank == 0 and warp.place == 0, 'the Instant Warp scroll is sold for 10 CP to anyone')
    end)

    it('sells her a thing out of her own points', function()
        local sale = xi.cardian.exchange.buy(buyer, xi.nation.BASTOK, xi.conquest.guard.CITY, kInstantWarp)
        assert(sale.refusal == nil, string.format('refused: %s', tostring(sale.refusal)))
        assert(sale.cp == 990 and buyer:getCP() == 990, string.format('10 CP paid: %s', tostring(sale.cp)))
        assert(buyer:hasItem(xi.item.SCROLL_OF_INSTANT_WARP), 'she holds the scroll')
    end)

    it('names the guard\'s refusal when her points fall short', function()
        local sale = xi.cardian.exchange.buy(buyer, xi.nation.BASTOK, xi.conquest.guard.CITY, kReturnRing)
        assert(sale.refusal == 'TOO_FEW_POINTS', string.format('refusal: %s', tostring(sale.refusal)))
        assert(sale.have == 1000 and sale.need == 2500, string.format('have %s, need %s', tostring(sale.have), tostring(sale.need)))
        assert(buyer:getCP() == 1000, 'nothing was paid')
    end)

    it('refuses a Rare thing she holds already, in the guard\'s terms', function()
        xi.cardian.exchange.buy(buyer, xi.nation.BASTOK, xi.conquest.guard.CITY, kInstantWarp)
        local sale = xi.cardian.exchange.buy(buyer, xi.nation.BASTOK, xi.conquest.guard.CITY, kInstantWarp)
        assert(sale.refusal == 'GUARD_REFUSED', string.format('refusal: %s', tostring(sale.refusal)))
        assert(buyer:getCP() == 990, 'the second was not paid for')
    end)

    it('sells an experience ring by the guard\'s own rules: one held at a time', function()
        local sale = xi.cardian.exchange.buy(buyer, xi.nation.BASTOK, xi.conquest.guard.CITY, kChariotBand)
        assert(sale.refusal == nil, string.format('refusal: %s', tostring(sale.refusal)))
        assert(buyer:hasItem(xi.item.CHARIOT_BAND), 'she holds the ring')
        assert(buyer:getCP() == 500, string.format('her points: %s', tostring(buyer:getCP())))
        local again = xi.cardian.exchange.buy(buyer, xi.nation.BASTOK, xi.conquest.guard.CITY, kChariotBand)
        assert(again.refusal == 'GUARD_REFUSED', string.format('refusal: %s', tostring(again.refusal)))
        assert(buyer:getCP() == 500, 'the second was not paid for')
    end)

    it('sells nothing the guard does not stock', function()
        assert(xi.cardian.exchange.buy(buyer, xi.nation.BASTOK, xi.conquest.guard.CITY, 1).refusal == 'NOT_SOLD')
        assert(buyer:getCP() == 1000, 'nothing was paid')
    end)
end)
