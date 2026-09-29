-----------------------------------
-- Cardian: the conquest exchange by proxy (the Link's CP_SHOP and CP_BUY,
-- src/map/pawn/link_api.cpp)
--
-- A cardian cannot talk to a gate guard, but her player can stand beside
-- one, and she stands beside them with conquest points of her own. The Link
-- finds the guard within the player's reach (src/map/pawn/gate_guards.cpp)
-- and the cardian; this sells to her from that guard's stock, at that
-- guard's prices, out of her own points:
--
--   xi.cardian.exchange.shop(buyer, guardNation)
--     -> { cp, rank, nation, nationRank, foreign, blocked, items }: her
--        points, her rank, her nation and its place in the conquest tally;
--        foreign, another nation's guard; blocked, that guard's nation
--        outranks hers, so he sells her nothing; items, by option, each
--        { option, item, price, level, rank, place } -- rank the rank in her
--        nation it needs, place the conquest place her nation must hold for
--        it, 0 for none.
--   xi.cardian.exchange.buy(buyer, guardNation, guardType, option)
--     -> { refusal, cp, have, need }: refusal nil when she bought it, else
--        its name, which the Link turns into its outcome (CL_S_<refusal>);
--        cp her points after; have and need the two numbers a refusal's
--        words need (her points and the price, her rank and the item's, her
--        nation's place and the item's).
--
-- The stock is the guard's own -- conquest.lua keeps it in file-local tables
-- -- read from the file once, as this loads. The sale is the guard's own too:
-- his event handlers weigh it and make it.
--
-- A library, not a module: it overrides nothing, so the pawn module loads it
-- at init (pawn::linkapi::loadLibraries), and xi_test requires it from the
-- test.
-----------------------------------
xi = xi or {}
xi.cardian = xi.cardian or {}
xi.cardian.exchange = {}

-- The guard's stock tables, as conquest.lua writes them: one entry per
-- line, '[option] = { cp = N, lvl = N, item = xi.item.NAME, rank = N }',
-- the common table then one block per nation
local function readStock()
    local common, nations = {}, {}
    local f = io.open('scripts/globals/conquest.lua', 'r')
    if f == nil then
        print('[cardian] conquest exchange: scripts/globals/conquest.lua is not readable, the shop is empty')
        return common, nations
    end

    local section, nation = nil, nil
    for line in f:lines() do
        if line:match('^local overseerInvCommon') then
            section = 'common'
        elseif line:match('^local overseerInvNation') then
            section = 'nation'
        elseif section ~= nil and line:match('^}') then
            section = nil
        elseif section == 'nation' then
            local n = line:match('%[xi%.nation%.(%u+)%]%s*=')
            if n ~= nil then
                nation          = xi.nation[n]
                nations[nation] = nations[nation] or {}
            end
        end

        if section ~= nil then
            local option, body = line:match('^%s*%[(%d+)%]%s*=%s*{(.-)}')
            if option ~= nil then
                local itemName = body:match('item%s*=%s*xi%.item%.([%w_]+)')
                local entry    =
                {
                    cp    = tonumber(body:match('cp%s*=%s*(%d+)')) or 0,
                    lvl   = tonumber(body:match('lvl%s*=%s*(%d+)')) or 1,
                    rank  = tonumber(body:match('rank%s*=%s*(%d+)')),
                    place = tonumber(body:match('place%s*=%s*(%d+)')),
                    item  = itemName ~= nil and xi.item[itemName] or nil,
                }
                if entry.item ~= nil then
                    if section == 'common' then
                        common[tonumber(option)] = entry
                    elseif nation ~= nil then
                        nations[nation][tonumber(option)] = entry
                    end
                end
            end
        end
    end
    f:close()
    return common, nations
end

local common, nations = readStock()

-- What this guard sells to this buyer: the common stock plus the guard's
-- nation's, or the buyer's own at a nationless overseer (getStock)
local function stockFor(guardNation, buyerNation)
    local out = {}
    for option, entry in pairs(common) do
        out[option] = entry
    end
    local nation = guardNation ~= xi.nation.OTHER and guardNation or buyerNation
    for option, entry in pairs(nations[nation] or {}) do
        out[option] = entry
    end
    return out
end

-- A foreign counter: another nation's guard, not Jeuno's
local function foreignGuard(buyerNation, guardNation)
    return guardNation ~= xi.nation.OTHER and guardNation ~= buyerNation
end

-- The guard's price for this buyer: another nation's guard charges more
-- for its nation's own gear (the overseer's rule)
local function priceFor(entry, buyerNation, guardNation)
    local price = entry.cp
    if entry.rank and buyerNation ~= guardNation and guardNation ~= xi.nation.OTHER then
        if price <= 8000 then
            price = price * 2
        else
            price = price + 8000
        end
    end
    return price
end

xi.cardian.exchange.shop = function(buyer, guardNation)
    local nation  = buyer:getNation()
    local stock   = stockFor(guardNation, nation)
    local foreign = foreignGuard(nation, guardNation)
    local shop    =
    {
        cp         = buyer:getCP(),
        rank       = buyer:getRank(nation),
        nation     = nation,
        nationRank = GetNationRank(nation),
        foreign    = foreign,
        blocked    = foreign and GetNationRank(guardNation) <= GetNationRank(nation),
        items      = {},
    }

    local options = {}
    for option in pairs(stock) do
        options[#options + 1] = option
    end
    table.sort(options)
    for _, option in ipairs(options) do
        local entry = stock[option]
        shop.items[#shop.items + 1] =
        {
            option = option,
            item   = entry.item,
            price  = priceFor(entry, nation, guardNation),
            level  = entry.lvl,
            rank   = entry.rank or 0,
            place  = entry.place or 0,
        }
    end
    return shop
end

-- The guard's reason for a sale that did not happen, in his own terms:
-- another nation's guard sells only to a buyer whose nation outranks his in
-- the conquest tally, and never what a conquest place buys; a nation's own
-- place-ranked stock needs the nation placed at or above it; then her
-- points, then her rank
local function refusalOf(buyer, entry, nation, guardNation)
    if buyer:getFreeSlotsCount() < 1 then
        return 'NO_SPACE', 0, 0
    end
    if foreignGuard(nation, guardNation) then
        if GetNationRank(guardNation) <= GetNationRank(nation) then
            return 'OUTRANKED', 0, 0
        end
        if entry.place ~= nil then
            return 'FOREIGN_PLACE', 0, 0
        end
    elseif entry.place ~= nil and GetNationRank(nation) > entry.place then
        return 'NATION_PLACE', GetNationRank(nation), entry.place
    end
    local price = priceFor(entry, nation, guardNation)
    if buyer:getCP() < price then
        return 'TOO_FEW_POINTS', buyer:getCP(), price
    end
    if entry.rank ~= nil and buyer:getRank(nation) < entry.rank then
        return 'RANK_TOO_LOW', buyer:getRank(nation), entry.rank
    end
    return 'GUARD_REFUSED', 0, 0
end

xi.cardian.exchange.buy = function(buyer, guardNation, guardType, option)
    local nation = buyer:getNation()
    local entry  = stockFor(guardNation, nation)[option]
    if entry == nil then
        return { refusal = 'NOT_SOLD', cp = buyer:getCP(), have = 0, need = 0 }
    end
    if option >= 32933 and option <= 32935 then
        return { refusal = 'NOT_BY_PROXY', cp = buyer:getCP(), have = 0, need = 0 }
    end

    -- The guard's own judgement and his own sale, her cutscene answered for
    -- her: overseerOnEventUpdate weighs the item the way the menu would --
    -- job, level, points, rank, the nations' standing, the place -- and arms
    -- the sale; overseerOnEventFinish makes it, charging her and handing her
    -- the item. Refused, nothing changes hands, and refusalOf only names the
    -- guard's reason.
    local before = buyer:getCP()
    xi.conquest.overseerOnEventUpdate(buyer, 0, option, guardNation)
    xi.conquest.overseerOnEventFinish(buyer, 0, option, guardNation, guardType, nil)
    if buyer:getCP() ~= before then
        return { cp = buyer:getCP(), have = 0, need = 0 }
    end
    local refusal, have, need = refusalOf(buyer, entry, nation, guardNation)
    return { refusal = refusal, cp = buyer:getCP(), have = have, need = need }
end
