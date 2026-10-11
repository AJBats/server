-----------------------------------
-- Cardian: a venture's supplies from the guild shops (the map's
-- src/map/pawn/venture_shop.cpp)
--
-- A fishing venture buys her rod and bait where they cost least: the
-- auction house, or a guild shop open now. Her walk to the shop is
-- simulated: she buys from any guild shop in the world as she sets out,
-- though a shop's own hours and holiday still hold (OPEN_ISSUES #431: where
-- a venture may go, and so where she shops, once cardians travel). The shop
-- is the game's own -- its stock and today's price (xi.guildShops,
-- scripts/globals/guild_shops.lua, over xi.data.guildShops) and its own
-- purchase -- and what she buys goes back on its shelf after, so ventures
-- never empty a shop the player buys from (the user, 2026-10-11).
--
--   xi.cardian.ventureShops.offers(itemIds)
--     -> { [itemId] = { { shop, price, stock } ... } }: for each item, every
--        guild shop open now that sells it today, its price for one and how
--        many it holds.
--   xi.cardian.ventureShops.buy(buyer, shop, itemId, quantity)
--     -> { bought, shelved }: bought, quantity or 0 -- the shop open, its
--        shelf holding all of it and her purse the price, or nothing is
--        bought; shelved, what was bought is back on the shelf.
--
-- Two pieces of guild_shops.lua it leans on, compared at every merge from
-- base (CLAUDE.md): its opening check, file-local there and copied here
-- (open), and its shelf, xi.guildShops.state[shop].items[id].stock, which
-- the put-back writes.
--
-- A library, not a module: it overrides nothing, so the pawn module loads it
-- at init (pawn::linkapi::loadLibraries), and xi_test requires it from the
-- test.
-----------------------------------
xi = xi or {}
xi.cardian = xi.cardian or {}
xi.cardian.ventureShops = {}

-- A guild shop as the guild shop's own functions know it: by its name
local function shopNpc(name)
    return { getName = function(_) return name end }
end

-- Open now, as guild_shops.lua's guildShopIsOpen and isHolidayToday judge it
local function open(shop)
    if xi.settings.main.GUILD_SHOP_HOLIDAYS and shop.holiday ~= nil and shop.holiday == VanadielDayOfTheWeek() then
        return false
    end

    local hour = VanadielHour()
    return hour >= shop.hours[1] and hour < shop.hours[2]
end

-- Of the items asked about, those a shop's stock list carries
local function carried(shop, wanted)
    local out = {}
    for _, cfg in ipairs(shop.stock or {}) do
        if wanted[cfg.id] then
            out[cfg.id] = true
        end
    end

    return out
end

-- An item on a shop's shelf today: { id, count, price } (the day rolled as a
-- player's visit rolls it), nil when it is not offered
local function onShelf(name, itemId)
    for _, item in ipairs(xi.guildShops.onBuyList(nil, shopNpc(name))) do
        if item.id == itemId then
            return item
        end
    end
end

xi.cardian.ventureShops.offers = function(itemIds)
    local wanted = {}
    for _, id in ipairs(itemIds) do
        wanted[id] = true
    end

    local out = {}
    for name, shop in pairs(xi.data.guildShops or {}) do
        -- an alias (sharedStock) sells from another shop's shelf, listed under that one
        local here = carried(shop, wanted)
        if next(here) ~= nil and open(shop) then
            for _, item in ipairs(xi.guildShops.onBuyList(nil, shopNpc(name))) do
                if here[item.id] and item.count > 0 then
                    out[item.id] = out[item.id] or {}
                    table.insert(out[item.id], { shop = name, price = item.price, stock = item.count })
                end
            end
        end
    end

    return out
end

xi.cardian.ventureShops.buy = function(buyer, shop, itemId, quantity)
    local cfg  = xi.data.guildShops[shop]
    local item = cfg ~= nil and cfg.stock ~= nil and open(cfg) and onShelf(shop, itemId) or nil
    if item == nil or item.count < quantity then
        return { bought = 0, shelved = false } -- closed, or short of all of it: the shop's own sale would sell part
    end

    local sale = xi.guildShops.onPlayerBuy(buyer, shopNpc(shop), itemId, quantity)
    local got  = sale ~= nil and sale.tradeCode or 0
    if got == nil or got <= 0 then
        return { bought = 0, shelved = false }
    end

    -- back on the shelf: a venture never empties a shop
    local state = xi.guildShops.state[shop]
    local entry = state ~= nil and state.items[itemId] or nil
    if entry ~= nil then
        entry.stock = entry.stock + got
    end

    return { bought = got, shelved = entry ~= nil }
end
