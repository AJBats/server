-----------------------------------
-- Cardian: a venture's supplies from the guild shops
-- (modules/cardian/lua/venture_shops.lua, the map's pawn/venture_shop.cpp)
--
-- The library lists the guild shops open now that sell an item today, and
-- buys from one through the shop's own purchase, putting what was bought
-- back on the shelf. Here a player stands in for the cardian, and the test
-- sets the Vana'diel hour a shop's hours need.
--
-- The pawn module loads the library at init; under xi_test there is no pawn
-- module, so the test loads it itself.
-----------------------------------
require('modules/cardian/lua/venture_shops')

describe('Cardian venture shops', function()
    ---@type CClientEntityPair
    local buyer
    local realHolidays

    before_each(function()
        realHolidays = xi.settings.main.GUILD_SHOP_HOLIDAYS
        xi.settings.main.GUILD_SHOP_HOLIDAYS = false
        buyer = xi.test.world:spawnPlayer({ zone = xi.zone.SELBINA })
        buyer:addGil(10000)
    end)

    after_each(function()
        xi.settings.main.GUILD_SHOP_HOLIDAYS = realHolidays
    end)

    -- The guild shops open now selling Little Worms
    local function wormOffers()
        return xi.cardian.ventureShops.offers({ xi.item.LITTLE_WORM })[xi.item.LITTLE_WORM] or {}
    end

    -- A guild shop selling Little Worms, its hours made open now
    local function wormShop()
        for name, shop in pairs(xi.data.guildShops) do
            for _, cfg in ipairs(shop.stock or {}) do
                if cfg.id == xi.item.LITTLE_WORM then
                    xi.test.world:setVanaTime(shop.hours[1], 0)
                    for _, offer in ipairs(wormOffers()) do
                        if offer.shop == name then
                            return offer, shop
                        end
                    end
                end
            end
        end
    end

    it('is loaded', function()
        assert(xi.cardian and xi.cardian.ventureShops, 'modules/cardian/lua/venture_shops.lua did not load')
    end)

    it('lists the guild shops open now selling an item today, each with its price and stock', function()
        local offer = wormShop()
        assert(offer ~= nil, 'a guild shop sells Little Worms, open at its opening hour')
        for _, each in ipairs(wormOffers()) do
            local shop = xi.data.guildShops[each.shop]
            assert(shop ~= nil and shop.stock ~= nil, string.format('a shop with a shelf of its own: %s', tostring(each.shop)))
            assert(each.price > 0 and each.stock > 0, string.format('%s: price %s, stock %s', each.shop, tostring(each.price), tostring(each.stock)))
        end
        assert(next(xi.cardian.ventureShops.offers({ xi.item.CHARIOT_BAND })) == nil, 'an item no guild shop sells: none')
    end)

    it('buys at the shop\'s price, and puts what it bought back on the shelf', function()
        local offer = wormShop()
        local gil   = buyer:getGil()
        local sale  = xi.cardian.ventureShops.buy(buyer, offer.shop, xi.item.LITTLE_WORM, 12)
        assert(sale.bought == 12, string.format('bought: %s', tostring(sale.bought)))
        assert(sale.shelved, 'back on the shelf')
        assert(buyer:getGil() == gil - offer.price * 12, string.format('paid %s of %s', tostring(gil - buyer:getGil()), tostring(offer.price * 12)))
        assert(buyer:hasItem(xi.item.LITTLE_WORM), 'she holds the worms')
        for _, after in ipairs(wormOffers()) do
            if after.shop == offer.shop then
                assert(after.stock == offer.stock, string.format('the shelf as it was: %s, was %s', tostring(after.stock), tostring(offer.stock)))
            end
        end
    end)

    it('sells nothing, and is not listed, while the shop is closed', function()
        local offer, shop = wormShop()
        xi.test.world:setVanaTime(shop.hours[2] % 24, 0)
        local gil  = buyer:getGil()
        local sale = xi.cardian.ventureShops.buy(buyer, offer.shop, xi.item.LITTLE_WORM, 12)
        assert(sale.bought == 0, 'a closed shop sells nothing')
        assert(buyer:getGil() == gil, 'nothing was paid')
        for _, each in ipairs(wormOffers()) do
            assert(each.shop ~= offer.shop, 'a closed shop is not offered')
        end
    end)

    it('sells nothing when its shelf holds less than all of it', function()
        local offer = wormShop()
        local gil   = buyer:getGil()
        local sale  = xi.cardian.ventureShops.buy(buyer, offer.shop, xi.item.LITTLE_WORM, offer.stock + 1)
        assert(sale.bought == 0, 'short of all of it: nothing, not part')
        assert(buyer:getGil() == gil, 'nothing was paid')
    end)
end)
