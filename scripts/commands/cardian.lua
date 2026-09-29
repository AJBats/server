-----------------------------------
-- func: cardian <verb> [args]
-- desc: The Cardian addon's requests not yet converted to Cardian Link
--       messages (src/map/pawn/cardian_link_protocol.h), whose logic is Lua: a
--       LEGACY_CD message runs this command for the bound character, and its
--       replies go back the same way. Nothing here is for typing: Cardian's
--       features are the addon's menus and buttons (CLAUDE.md, UI first). The
--       file leaves once these have messages of their own (ROADMAP, the Link).
--
--       goals                            what the player could recruit for: current missions, quests under way
--       cpshop <name> | cpbuy <name> <option>   the conquest exchange, for a cardian of yours
-----------------------------------
---@type TCommand
local commandObj = {}

commandObj.cmdprops =
{
    permission = 0,
    parameters = 's',
}

local function reply(player, line)
    player:cardianLinkSend(line)
end

-- Her inventory after a sale, told to the addon as the Link's own INVENTORY
local function sendInv(player, name)
    player:cardianTellInventory(name, 0)
end

-- The conquest exchange by proxy -------------------------------------------
-- A cardian cannot talk to a gate guard, but her player can stand beside
-- one, and she stands beside them with conquest points of her own. The
-- guards are a fixed list; a slow check asks whether the player is within
-- reach of one, and its answer rides on the roster (the Link's ROSTER) so
-- her menu grows the Conquest exchange row only while it says yes. cpshop and cpbuy sell
-- to her from that guard's stock at that guard's prices, out of her own
-- points. The stock is the guard's own -- conquest.lua keeps it in
-- file-local tables -- read from the file once, when the commands load.

-- The guard within the player's reach, { name, nation, type }, or nil: the
-- fixed list of guards that sell, and the reach, are pawn/gate_guards.cpp's,
-- which the roster judges by too
local function guardNear(player)
    return player:cardianGuardNear()
end

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
                local entry    = {
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

local function count(t)
    local n = 0
    for _ in pairs(t) do
        n = n + 1
    end
    return n
end
print(string.format('[cardian] conquest exchange: %d common items, %d nations stocked', count(common), count(nations)))

-- What this guard sells to this buyer: the common stock plus the guard's
-- nation's, or the buyer's own at a nationless overseer (getStock)
local function cpStockFor(guardNation, buyerNation)
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

local nationNames = { [xi.nation.SANDORIA] = "San d'Oria", [xi.nation.BASTOK] = 'Bastok', [xi.nation.WINDURST] = 'Windurst' }

-- A foreign counter: another nation's guard, not Jeuno's
local function foreignGuard(buyerNation, guardNation)
    return guardNation ~= xi.nation.OTHER and guardNation ~= buyerNation
end

-- The guard's own refusals (overseerOnEventUpdate): another nation's guard
-- sells only to a buyer whose nation outranks the guard's in the conquest
-- tally, and never its place-ranked gear; a nation's own place-ranked gear
-- needs the nation ranked at or above the place. nil when the sale stands.
local function cpRefusal(entry, buyerNation, guardNation)
    if foreignGuard(buyerNation, guardNation) then
        if GetNationRank(guardNation) <= GetNationRank(buyerNation) then
            return string.format('%s does not outrank %s in conquest; its guards sell her nothing', nationNames[buyerNation], nationNames[guardNation])
        end
        if entry.place ~= nil then
            return "another nation's guard does not sell that"
        end
    elseif entry.place ~= nil and GetNationRank(buyerNation) > entry.place then
        return string.format('that needs %s ranked %d or better in conquest', nationNames[buyerNation], entry.place)
    end
    return nil
end

-- The guard's price for this buyer: another nation's guard charges more
-- for its nation's own gear (the overseer's rule)
local function cpPrice(entry, buyerNation, guardNation)
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

-- The conquest exchange verbs: the gate guard within the player's reach
-- sells to her out of her own conquest points. 'cps.b <name> <cp> <rank> <nation> <guard>', one
-- 'cps <name> <option> <item> <price> <lvl> <rank>' per item, 'cps.e'.
local function ownedCardian(player, name)
    if player:cardianOwns(name) then
        return GetPlayerByName(name)
    end
    return nil
end

local function guardFor(player)
    local g = guardNear(player)
    if g == nil then
        return nil, 'no gate guard within reach'
    end
    return g
end

local function sendCpShop(player, name)
    local targ = ownedCardian(player, name)
    if targ == nil then
        reply(player, '#cd err cpshop no such cardian')
        return
    end
    local g, why = guardFor(player)
    if g == nil then
        reply(player, '#cd err cpshop ' .. why)
        return
    end

    local nation  = targ:getNation()
    local stock   = cpStockFor(g.nation, nation)
    local foreign = foreignGuard(nation, g.nation)
    local blocked = foreign and GetNationRank(g.nation) <= GetNationRank(nation)
    reply(player, string.format('#cd cps.b %s %d %d %d %d %d %d %d %s', name, targ:getCP(), targ:getRank(nation), nation, g.nation,
        GetNationRank(nation), foreign and 1 or 0, blocked and 1 or 0, g.name))
    local options = {}
    for option in pairs(stock) do
        options[#options + 1] = option
    end
    table.sort(options)
    for _, option in ipairs(options) do
        local entry = stock[option]
        reply(player, string.format('#cd cps %s %d %d %d %d %d %d', name, option, entry.item, cpPrice(entry, nation, g.nation), entry.lvl, entry.rank or 0, entry.place or 0))
    end
    reply(player, '#cd cps.e ' .. name)
end

local function cpBuy(player, name, option)
    local targ = ownedCardian(player, name)
    if targ == nil then
        return 'no such cardian'
    end
    local g, why = guardFor(player)
    if g == nil then
        return why
    end
    local nation = targ:getNation()
    local entry  = cpStockFor(g.nation, nation)[option]
    if entry == nil then
        return 'the guard does not sell that'
    end
    if option >= 32933 and option <= 32935 then
        return 'the experience rings are not sold by proxy'
    end

    -- The guard's own judgement and its own sale, her cutscene answered for
    -- her: overseerOnEventUpdate weighs the item the way the menu would --
    -- job, level, points, rank, the nations' standing, the place -- and arms
    -- the sale; overseerOnEventFinish makes it, charging her and handing
    -- her the item. The rules stay upstream's. Refused, nothing changes
    -- hands, and cpRefusal only puts the guard's reason into words.
    local before = targ:getCP()
    xi.conquest.overseerOnEventUpdate(targ, 0, option, g.nation)
    xi.conquest.overseerOnEventFinish(targ, 0, option, g.nation, g.type, nil)
    if targ:getCP() == before then
        if targ:getFreeSlotsCount() < 1 then
            return string.format('%s has no room', name)
        end
        return cpRefusal(entry, nation, g.nation)
            or (targ:getCP() < cpPrice(entry, nation, g.nation) and string.format('%s has %d conquest points, that costs %d', name, targ:getCP(), cpPrice(entry, nation, g.nation)))
            or (entry.rank ~= nil and targ:getRank(nation) < entry.rank and string.format('%s is rank %d, that needs rank %d', name, targ:getRank(nation), entry.rank))
            or 'the guard would not sell that to her'
    end
    return ''
end

-- The enum key as a title, the party progress module's own caser
local function titleFromKey(key)
    return xi.cardian and xi.cardian.titleFromKey and xi.cardian.titleFromKey(key) or key
end

local function keyOf(ids, id)
    if ids ~= nil then
        for key, value in pairs(ids) do
            if value == id then
                return key
            end
        end
    end
    return nil
end

-- What the player could be recruiting for (the party finder's ring): the
-- current mission on each log that has one, and every quest under way.
-- 'gl.b', 'gl mission <log> <id> <title>' / 'gl quest <area> <id> <title>',
-- 'gl.e'. Logs the era plays: the three nations, Zilart, Promathia, Aht
-- Urhgan; the nation logs rest on NONE, the later ones on 0
-- The nation logs rest on NONE; the later ones the game resets to 0 at
-- a completion, so 0 reads as none there too (Zilart's and Aht Urhgan's
-- first missions carry id 0 and cannot be told from an empty log) --
-- and Promathia's rests on a category
local goalLogs =
{
    { log = xi.mission.log_id.SANDORIA, none = { [xi.mission.id.nation.NONE] = true } },
    { log = xi.mission.log_id.BASTOK,   none = { [xi.mission.id.nation.NONE] = true } },
    { log = xi.mission.log_id.WINDURST, none = { [xi.mission.id.nation.NONE] = true } },
    { log = xi.mission.log_id.ZILART,   none = { [xi.mission.id.nation.NONE] = true, [0] = true } },
    { log = xi.mission.log_id.COP,      none = { [xi.mission.id.nation.NONE] = true, [0] = true, [xi.mission.id.cop.ANCIENT_FLAMES_BECKON] = true } },
    { log = xi.mission.log_id.TOAU,     none = { [xi.mission.id.nation.NONE] = true, [0] = true } },
}

-- ... and 'gl.c <log> <n>', how many missions on each log are complete,
-- for the panel's Completed line
local function sendGoals(player)
    reply(player, '#cd gl.b')
    for _, entry in ipairs(goalLogs) do
        local ids     = xi.mission.id[xi.mission.area[entry.log]]
        local current = player:getCurrentMission(entry.log)
        if not entry.none[current] then
            local key = keyOf(ids, current)
            reply(player, string.format('#cd gl mission %d %d %s', entry.log, current, key and titleFromKey(key) or ('#' .. current)))
        end
        local done = 0
        if ids ~= nil then
            for _, id in pairs(ids) do
                if id ~= xi.mission.id.nation.NONE and player:hasCompletedMission(entry.log, id) then
                    done = done + 1
                end
            end
        end
        reply(player, string.format('#cd gl.c %d %d', entry.log, done))
    end
    for area = 0, xi.questLog.COALITION do
        local ids = xi.quest.id[xi.quest.area[area]]
        if ids ~= nil then
            local keys = {}
            for key in pairs(ids) do
                keys[#keys + 1] = key
            end
            table.sort(keys)
            for _, key in ipairs(keys) do
                if player:getQuestStatus(area, ids[key]) == xi.questStatus.QUEST_ACCEPTED then
                    reply(player, string.format('#cd gl quest %d %d %s', area, ids[key], titleFromKey(key)))
                end
            end
        end
    end
    reply(player, '#cd gl.e')
end

commandObj.onTrigger = function(player, line)
    -- Run by the Link for the addon alone: typed, it answers nothing
    if not player:cardianByLink() then
        return
    end

    local args = {}
    for word in tostring(line or ''):gmatch('%S+') do
        args[#args + 1] = word
    end

    local verb = args[1]
    local name = args[2]

    if verb == 'goals' then
        sendGoals(player)
    elseif verb == 'cpshop' and name then
        sendCpShop(player, name)
    elseif verb == 'cpbuy' and name and args[3] then
        local err = cpBuy(player, name, tonumber(args[3]) or 0)
        if err ~= '' then
            reply(player, '#cd err cpbuy ' .. err)
        else
            reply(player, '#cd ok cpbuy')
            sendCpShop(player, name)
            sendInv(player, name)
        end
    else
        reply(player, '#cd err ' .. (verb or '?') .. ' no such request')
    end
end

return commandObj
