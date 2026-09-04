-----------------------------------
-- func: cardian <verb> [args]
-- desc: Cardian companion-addon API. Requests arrive over the Cardian Link
--       ('cd <verb> ...' lines run this command for the bound character) and
--       structured replies go back the same way as 'cd ...' lines; with no
--       link bound (a human typing !cardian in chat) the replies print to
--       chat channel 31 instead. Machine-facing -- humans get terse errors,
--       the addon gets data.
--
--       list                             roster of your live cardians
--       sync <name>                      one cardian: stats + gear + inventory
--       inv <name>                       a cardian's inventory
--       gear <name>                      a cardian's equipment
--       give <name> <slot> <qty>         your inventory slot -> cardian
--       take <name> <slot> <qty>         cardian inventory slot -> you
--       wear <name> <invslot> <eqslot>   equip from the cardian's inventory
--       strip <name> <eqslot>            unequip
--       equipset <name> <eq:inv,...>     apply a whole loadout in one pass
--                                        (invslot 0 clears the slot)
--       use <name> <slot>                cardian uses the item on itself
--       drop <name> <slot> <qty>         destroy part of a cardian's stack
--       giveuse <name> <slot> <qty>      give from your inventory, then the
--                                        cardian uses it (the scroll flow)
--       homepoint <name>                 a KO'd cardian home points (yours)
--       gambits <name>                   the cardian's gambit rows (gb.b / g / gb.e)
--       gtoggle <name> <row> <on|off>    a row's switch
--       gmove <name> <from> <to>         reorder a row (1-based, as shown)
--       gdel <name> <row>                delete a row
--       gins <name> <row> <spec>         insert a row (the row grammar) at a position
--       gset <name> <row> <spec>         replace a row in place (keeps its switch)
--       gvocab <name>                    the pickers' catalogue for the cardian
--       owned                            every cardian of yours, spawned or not (own.b / o / own.e)
--       spawn <name> | despawn <name>    the Debug screen's spawn and despawn (creation stays !pawncreate)
--                                        (gv.b, gvt/gvc/gvs/gva chunks, gv.e)
--       orders                           the party's orders: st <strategy> <retreat> <min> <max> <pull>
--                                        <aggressive> <links> <name;name>
--       hunt <rule> <n>                  a hunt rule: min|max (check 0 Too Weak .. 7 Incredibly Tough),
--                                        pull (0 nearest, 1 easiest, 2 toughest), aggressive|links (0/1)
--       strategy next|<n>                the party strategy (0 Off, 1 Roam); every cardian follows
--       retreat [on|off]                 the "on me" switch, no arg toggles: disengage, engage nobody,
--                                        avoid nothing, hunting pauses, until it clears
--       engage <targid>                  every cardian fights your target (a cardian: talk comes later)
--       gmaster <name> <on|off>          the cardian's master gambit switch
--       greset <name>                    back to the job's default rows
-----------------------------------
---@type TCommand
local commandObj = {}

commandObj.cmdprops =
{
    permission = 0,
    parameters = 's',
}

local channel = xi.msg.channel.NS_LINKSHELL3

local function reply(player, line)
    if not player:cardianLinkSend(line) then
        player:printToPlayer(line, channel)
    end
end

local function sendInv(player, name)
    local inv = player:cardianInv(name)
    if inv == nil then
        reply(player, '#cd err inv no such cardian')
        return
    end

    reply(player, string.format('#cd inv.b %s %d %d', name, inv.size or 0, inv.free or 0))
    for _, chunk in ipairs(inv.chunks) do
        reply(player, string.format('#cd i %s %s', name, chunk))
    end
    reply(player, '#cd inv.e ' .. name)
end

local function sendGear(player, name)
    local chunks = player:cardianGear(name)
    if chunks == nil then
        reply(player, '#cd err gear no such cardian')
        return
    end

    reply(player, '#cd gear.b ' .. name)
    for _, chunk in ipairs(chunks) do
        reply(player, string.format('#cd e %s %s', name, chunk))
    end
    reply(player, '#cd gear.e ' .. name)
end

-- The conquest exchange by proxy -------------------------------------------
-- A cardian cannot talk to a gate guard, but her player can stand beside
-- one, and she stands beside them with conquest points of her own. The
-- guards are a fixed list; a slow check asks whether the player is within
-- reach of one, and its answer rides on the roster line so her menu grows
-- the Conquest exchange row only while it says yes. cpshop and cpbuy sell
-- to her from that guard's stock at that guard's prices, out of her own
-- points. The stock is the guard's own -- conquest.lua keeps it in
-- file-local tables -- read from the file once, when the commands load.

-- The city and embassy guards: grep overseerOnTrigger scripts/zones/*/npcs,
-- keeping guard types CITY and FOREIGN (the outpost and border overseers
-- sell nothing)
local guards =
{
    ['Achantere_TK']      = { nation = xi.nation.SANDORIA, type = xi.conquest.guard.CITY,    zone = 'Northern_San_dOria' },
    ['Alrauverat']        = { nation = xi.nation.OTHER,    type = xi.conquest.guard.CITY,    zone = 'Lower_Jeuno' },
    ['Aravoge_TK']        = { nation = xi.nation.SANDORIA, type = xi.conquest.guard.CITY,    zone = 'Southern_San_dOria' },
    ['Arpevion_TK']       = { nation = xi.nation.SANDORIA, type = xi.conquest.guard.CITY,    zone = 'Southern_San_dOria' },
    ['Chapal-Afal_WW']    = { nation = xi.nation.WINDURST, type = xi.conquest.guard.FOREIGN, zone = 'Northern_San_dOria' },
    ['Crying_Wind_IM']    = { nation = xi.nation.BASTOK,   type = xi.conquest.guard.CITY,    zone = 'Bastok_Mines' },
    ['Emitt']             = { nation = xi.nation.OTHER,    type = xi.conquest.guard.CITY,    zone = 'Upper_Jeuno' },
    ['Flying_Axe_IM']     = { nation = xi.nation.BASTOK,   type = xi.conquest.guard.CITY,    zone = 'Port_Bastok' },
    ['Glarociquet_TK']    = { nation = xi.nation.SANDORIA, type = xi.conquest.guard.FOREIGN, zone = 'Metalworks' },
    ['Harara_WW']         = { nation = xi.nation.WINDURST, type = xi.conquest.guard.CITY,    zone = 'Windurst_Woods' },
    ['Kochahy-Muwachahy'] = { nation = xi.nation.OTHER,    type = xi.conquest.guard.CITY,    zone = 'Port_Jeuno' },
    ['Lexun-Marixun_WW']  = { nation = xi.nation.WINDURST, type = xi.conquest.guard.FOREIGN, zone = 'Metalworks' },
    ['Milma-Hapilma_WW']  = { nation = xi.nation.WINDURST, type = xi.conquest.guard.CITY,    zone = 'Port_Windurst' },
    ['Morlepiche']        = { nation = xi.nation.OTHER,    type = xi.conquest.guard.CITY,    zone = 'RuLude_Gardens' },
    ['Panoquieur_TK']     = { nation = xi.nation.SANDORIA, type = xi.conquest.guard.FOREIGN, zone = 'Windurst_Woods' },
    ['Puroiko-Maiko_WW']  = { nation = xi.nation.WINDURST, type = xi.conquest.guard.CITY,    zone = 'Windurst_Waters' },
    ['Rabid_Wolf_IM']     = { nation = xi.nation.BASTOK,   type = xi.conquest.guard.CITY,    zone = 'Bastok_Markets' },
    ['Sachetan_IM']       = { nation = xi.nation.BASTOK,   type = xi.conquest.guard.FOREIGN, zone = 'Port_Windurst' },
    ['Yevgeny_IM']        = { nation = xi.nation.BASTOK,   type = xi.conquest.guard.FOREIGN, zone = 'Northern_San_dOria' },
}

-- The guards by zone, so a check only looks up the names that live there
local guardsByZone = {}
for name, g in pairs(guards) do
    guardsByZone[g.zone] = guardsByZone[g.zone] or {}
    table.insert(guardsByZone[g.zone], name)
end

local kGuardReach = 8 -- yalms

-- The nearest guard within reach of the player, or nil: { npc, name,
-- nation, type } -- nearest, because a consulate stands its guards
-- together. Asked once a second by the roster line, so the answer is kept
-- for two seconds per player; the zone resolves names from a cache of
-- its own.
local nearCache = {}
local function guardNear(player)
    local id  = player:getID()
    local now = os.time()
    local c   = nearCache[id]
    if c ~= nil and now - c.at < 2 then
        return c.guard
    end

    local found, nearest = nil, kGuardReach
    local names = guardsByZone[player:getZoneName()]
    if names ~= nil then
        local zone = player:getZone()
        for _, name in ipairs(names) do
            for _, npc in pairs(zone:queryEntitiesByName(name)) do
                local away = player:checkDistance(npc)
                if away <= nearest then
                    local g = guards[name]
                    found   = { npc = npc, name = name, nation = g.nation, type = g.type }
                    nearest = away
                end
            end
        end
    end
    nearCache[id] = { at = now, guard = found }
    return found
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

-- One roster line: her jobs, health, TP, how far she is from her next
-- level, and whether a gate guard is within the player's reach (the
-- Conquest exchange row on her page). Both the roster list and a single
-- sync send it, so a screen sees the same fields either way. Experience
-- comes from the Cardian binding -- upstream has no getter for it or for
-- the level's cost.
local function pawnLine(player, name, targ)
    local xp    = player:cardianExp(name)
    local guard = guardNear(player) ~= nil
    return string.format('#cd p %s %d %d %d %d %d %d %d %d %d %d %d %d',
        name,
        targ:getMainJob(), targ:getMainLvl(),
        targ:getSubJob(), targ:getSubLvl(),
        targ:getHP(), targ:getMaxHP(),
        targ:getMP(), targ:getMaxMP(),
        targ:getTP(),
        xp and xp.exp or 0, xp and xp.tnl or 0,
        guard and 1 or 0)
end

local function sendPawnLine(player, name)
    local targ = GetPlayerByName(name)
    if targ then
        reply(player, pawnLine(player, name, targ))
    end
end

local statMods = {
    xi.mod.STR, xi.mod.DEX, xi.mod.VIT, xi.mod.AGI,
    xi.mod.INT, xi.mod.MND, xi.mod.CHR,
}

-- Seven base stats as total:bonus pairs, then attack:defense, then gil --
-- the status pane of the companion equip screen
local function sendStatsLine(player, name)
    local targ = GetPlayerByName(name)
    if targ == nil then
        return
    end

    local combat = player:cardianCombatStats(name)
    local parts  = { '#cd s', name }
    for _, mod in ipairs(statMods) do
        parts[#parts + 1] = string.format('%d:%d', targ:getStat(mod), targ:getMod(mod))
    end
    parts[#parts + 1] = string.format('%d:%d', combat and combat.att or 0, combat and combat.def or 0)
    parts[#parts + 1] = tostring(targ:getGil())
    reply(player, table.concat(parts, ' '))
end

-- What she cannot do yet: 'rc <name> key=seconds;...' for every spell
-- and ability still on recast. Nothing listed means everything is ready.
local function sendRecasts(player, name)
    local recasts = player:cardianRecasts(name)
    if recasts == nil then
        reply(player, '#cd err recasts no such cardian')
        return
    end

    local parts = {}
    for key, seconds in pairs(recasts) do
        parts[#parts + 1] = string.format('%s=%.1f', key, seconds)
    end
    reply(player, '#cd rc ' .. name .. ' ' .. table.concat(parts, ';'))
end

-- The conquest exchange verbs: the gate guard within the player's reach
-- sells to her out of her own conquest points. 'cps.b <name> <cp> <rank> <nation> <guard>', one
-- 'cps <name> <option> <item> <price> <lvl> <rank>' per item, 'cps.e'.
local function ownedCardian(player, name)
    for _, owned in ipairs(player:cardianNames()) do
        if owned == name then
            return GetPlayerByName(name)
        end
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

-- The three read-only pages under the cardian's menu: her profile, her
-- job levels, her combat skills -- what the client's own screens show for
-- the player, read for a cardian instead

local function sendProfile(player, name)
    local p = player:cardianProfile(name)
    if p == nil then
        reply(player, '#cd err profile no such cardian')
        return
    end
    reply(player, string.format('#cd pf %s %d %d %d %d %d %s', name, p.title, p.nation, p.race, p.rank, p.rankpoints, p.home))
end

-- Every job she has a level in, as job:level
local function sendJobs(player, name)
    local targ = GetPlayerByName(name)
    if targ == nil or player:cardianGear(name) == nil then
        reply(player, '#cd err jobs no such cardian')
        return
    end
    local parts = {}
    for job = 1, 22 do
        local level = targ:getJobLevel(job)
        if level > 0 then
            parts[#parts + 1] = string.format('%d:%d', job, level)
        end
    end
    reply(player, '#cd jl ' .. name .. ' ' .. table.concat(parts, ','))
end

-- The combat skills her jobs can raise, as skill:level:cap -- the base
-- skill level the client's Combat Skills page shows (the server keeps
-- skills in tenths; the cap table is in whole levels), and the cap at
-- her level (the higher of main and support job)
local kCombatSkills = { 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 25, 26, 27, 28, 29, 30, 31 }
local kMagicSkills  = { 32, 33, 34, 35, 36, 37, 38, 39, 40, 41, 42, 43, 44, 45 }

-- A skill list: '<tag> <name> skill:level:cap,...' for every skill of the
-- set her jobs can raise
local function sendSkillList(player, name, verb, tag, skills)
    local targ = GetPlayerByName(name)
    if targ == nil or player:cardianGear(name) == nil then
        reply(player, '#cd err ' .. verb .. ' no such cardian')
        return
    end
    local mjob, mlvl = targ:getMainJob(), targ:getMainLvl()
    local sjob, slvl = targ:getSubJob(), targ:getSubLvl()
    local parts = {}
    for _, skill in ipairs(skills) do
        local cap = targ:getMaxSkillLevel(mlvl, mjob, skill)
        if sjob ~= 0 and slvl > 0 then
            cap = math.max(cap, targ:getMaxSkillLevel(slvl, sjob, skill))
        end
        if cap > 0 then
            parts[#parts + 1] = string.format('%d:%d:%d', skill, math.floor(targ:getCharSkillLevel(skill) / 10), cap)
        end
    end
    reply(player, '#cd ' .. tag .. ' ' .. name .. ' ' .. table.concat(parts, ','))
end

local function sendSkills(player, name)
    sendSkillList(player, name, 'skills', 'cs', kCombatSkills)
end

local function sendMagicSkills(player, name)
    sendSkillList(player, name, 'mskills', 'ms', kMagicSkills)
end

-- Strips first (freeing hands and slots), then equips main-hand upward so
-- sub-slot rules see the new main. Per-slot failures are collected, not
-- fatal: the reply names what refused and the gear block that follows is
-- the authoritative result.
local function applyEquipSet(player, name, manifest)
    local strips = {}
    local wears  = {}
    for pair in manifest:gmatch('[^,]+') do
        local eqslot, invslot = pair:match('^(%d+):(%d+)$')
        if eqslot then
            if tonumber(invslot) == 0 then
                strips[#strips + 1] = tonumber(eqslot)
            else
                wears[#wears + 1] = { eqslot = tonumber(eqslot), invslot = tonumber(invslot) }
            end
        end
    end
    table.sort(wears, function(a, b)
        return a.eqslot < b.eqslot
    end)

    local fails = {}
    for _, eqslot in ipairs(strips) do
        local err = player:cardianStrip(name, eqslot)
        -- a stale diff asking to clear an already-empty slot is not a failure
        if err ~= '' and err ~= 'nothing equipped' then
            fails[#fails + 1] = string.format('%d: %s', eqslot, err)
        end
    end
    for _, w in ipairs(wears) do
        local err = player:cardianWear(name, w.invslot, w.eqslot)
        if err ~= '' then
            fails[#fails + 1] = string.format('%d: %s', w.eqslot, err)
        end
    end

    if #fails > 0 then
        reply(player, '#cd err equipset ' .. table.concat(fails, '; '))
    else
        reply(player, '#cd ok equipset')
    end
    sendStatsLine(player, name)
    sendGear(player, name)
    sendInv(player, name)
end

local function sendList(player)
    local names = player:cardianNames()
    reply(player, '#cd list.b ' .. #names)
    for _, name in ipairs(names) do
        local targ = GetPlayerByName(name)
        if targ then
            reply(player, pawnLine(player, name, targ))
        end
    end
    reply(player, '#cd list.e')
end

-- The gambit rows: 'gb.b <name> <master>', one 'g <name> <index> <on> <spec> <label>'
-- per row, 'gb.e <name>'. The label is the rest of the line.
local function sendGambits(player, name)
    local g = player:cardianGambits(name)
    if g == nil then
        reply(player, '#cd err gambits no such cardian')
        return
    end
    reply(player, string.format('#cd gb.b %s %d', name, g.master and 1 or 0))
    for _, row in ipairs(g.rows) do
        reply(player, string.format('#cd g %s %d %d %s %s', name, row.index, row.on and 1 or 0, row.spec, row.label))
    end
    reply(player, '#cd gb.e ' .. name)
end

-- The pickers' catalogue: 'gv.b <name>', then chunked 'gvt|gvs <name> k=label;...',
-- 'gvc <name> <range|-> k=label;...' (range = min,max,step,default for a numeric
-- condition) and 'gva <name> <group> k=label;...' lines under the link's line cap,
-- then 'gv.e <name>'
local function sendVocab(player, name)
    local v = player:cardianGambitVocab(name)
    if v == nil then
        reply(player, '#cd err gvocab no such cardian')
        return
    end
    reply(player, '#cd gv.b ' .. name)
    local function chunked(tag, prefix, entries, groupOf)
        local buf, bufGroup = {}, nil
        local size = 0
        local function flush()
            if #buf > 0 then
                reply(player, string.format('#cd %s %s %s%s', tag, name, prefix(bufGroup), table.concat(buf, ';')))
            end
            buf, size = {}, 0
        end
        for _, e in ipairs(entries) do
            local group = groupOf and groupOf(e) or nil
            local pair  = e.key .. '=' .. e.label
            if #buf > 0 and (size + #pair > 1700 or group ~= bufGroup) then
                flush()
            end
            bufGroup = group
            buf[#buf + 1] = pair
            size = size + #pair + 1
        end
        flush()
    end
    chunked('gvt', function () return '' end, v.targets)
    chunked('gvc', function (g) return (g ~= '' and g or '-') .. ' ' end, v.conditions, function (e) return e.group end)
    chunked('gvs', function () return '' end, v.statuses)
    chunked('gva', function (g) return g .. ' ' end, v.actions, function (e) return e.group end)
    -- The valid-target mask of every action that has one, key=mask: what
    -- a command window may aim it at
    local masks = {}
    for _, e in ipairs(v.actions) do
        if e.targets ~= nil and e.targets > 0 then
            masks[#masks + 1] = { key = e.key, label = tostring(e.targets) }
        end
    end
    chunked('gvx', function () return '' end, masks)
    reply(player, '#cd gv.e ' .. name)
end

-- The party's orders, one line: 'st <strategy> <retreat> <name;name...>'
local function sendOrders(player)
    local o = player:cardianOrders()
    if o == nil then
        reply(player, '#cd err orders no character')
        return
    end
    reply(player, string.format('#cd st %d %d %d %d %d %d %d %s', o.strategy, o.retreat and 1 or 0,
                                o.hunt_min, o.hunt_max, o.pull_first, o.aggressive and 1 or 0, o.links and 1 or 0, table.concat(o.names, ';')))
end

-- A gambit edit: the reply is ok or err, then the authoritative rows either way
local function gambitEdit(player, name, verb, err)
    if err ~= '' then
        reply(player, '#cd err ' .. verb .. ' ' .. err)
    else
        reply(player, '#cd ok ' .. verb)
    end
    sendGambits(player, name)
end

commandObj.onTrigger = function(player, line)
    local args = {}
    for word in tostring(line or ''):gmatch('%S+') do
        args[#args + 1] = word
    end

    local verb = args[1]
    local name = args[2]

    if verb == 'list' then
        sendList(player)
    elseif verb == 'gambits' and name then
        sendGambits(player, name)
    elseif verb == 'gtoggle' and name and args[3] and args[4] then
        gambitEdit(player, name, verb, player:cardianGambitToggle(name, tonumber(args[3]) or 0, args[4] == 'on'))
    elseif verb == 'gmove' and name and args[3] and args[4] then
        gambitEdit(player, name, verb, player:cardianGambitMove(name, tonumber(args[3]) or 0, tonumber(args[4]) or 0))
    elseif verb == 'gdel' and name and args[3] then
        gambitEdit(player, name, verb, player:cardianGambitDelete(name, tonumber(args[3]) or 0))
    elseif verb == 'gins' and name and args[3] and args[4] then
        gambitEdit(player, name, verb, player:cardianGambitInsert(name, tonumber(args[3]) or 0, args[4]))
    elseif verb == 'gset' and name and args[3] and args[4] then
        gambitEdit(player, name, verb, player:cardianGambitReplace(name, tonumber(args[3]) or 0, args[4]))
    elseif verb == 'gvocab' and name then
        sendVocab(player, name)
    elseif verb == 'owned' then
        reply(player, '#cd own.b')
        for _, n in ipairs(player:cardianAccountPawns()) do
            reply(player, '#cd o ' .. n)
        end
        reply(player, '#cd own.e')
    elseif verb == 'spawn' and name then
        if player:pawnSpawn(name) then
            reply(player, '#cd ok spawn')
        else
            reply(player, '#cd err spawn cannot spawn (unknown, online, already out, wrong account, or pawns disabled)')
        end
        sendList(player)
    elseif verb == 'despawn' and name then
        local mine = false
        for _, n in ipairs(player:cardianAccountPawns()) do
            if n == name then mine = true end
        end
        if mine and player:pawnDespawn(name) then
            reply(player, '#cd ok despawn')
        else
            reply(player, '#cd err despawn not one of yours, or not out')
        end
        sendList(player)
    elseif verb == 'orders' then
        sendOrders(player)
    elseif verb == 'strategy' and args[2] then
        local o    = player:cardianOrders()
        local want = args[2] == 'next' and ((o.strategy + 1) % #o.names) or tonumber(args[2])
        local err  = want ~= nil and player:cardianSetStrategy(want) or 'usage: strategy next|<n>'
        if err == '' then
            reply(player, '#cd ok strategy')
        else
            reply(player, '#cd err strategy ' .. err)
        end
        sendOrders(player)
    elseif verb == 'hunt' and args[2] and args[3] then
        local err = player:cardianSetHunt(args[2], tonumber(args[3]) or -1)
        if err == '' then
            reply(player, '#cd ok hunt')
        else
            reply(player, '#cd err hunt ' .. err)
        end
        sendOrders(player)
    elseif verb == 'retreat' then
        local o   = player:cardianOrders()
        local on  = (args[2] == nil and not o.retreat) or args[2] == 'on'
        local err = player:cardianRetreat(on)
        if err == '' then
            reply(player, '#cd ok retreat')
        else
            reply(player, '#cd err retreat ' .. err)
        end
        sendOrders(player)
    elseif verb == 'engage' and args[2] then
        local err = player:cardianEngage(tonumber(args[2]) or 0)
        if err == '' then
            reply(player, '#cd ok engage')
        else
            reply(player, '#cd err engage ' .. err)
        end
    elseif verb == 'gmaster' and name and args[3] then
        gambitEdit(player, name, verb, player:cardianGambitMaster(name, args[3] == 'on'))
    elseif verb == 'greset' and name then
        gambitEdit(player, name, verb, player:cardianGambitReset(name))
    elseif verb == 'sync' and name then
        -- ownership gate: cardianGear returns nil for a pawn that isn't yours
        if player:cardianGear(name) == nil then
            reply(player, '#cd err sync no such cardian')
        else
            sendPawnLine(player, name)
            sendStatsLine(player, name)
            sendGear(player, name)
            sendInv(player, name)
        end
    elseif verb == 'equipset' and name and args[3] then
        applyEquipSet(player, name, args[3])
    elseif verb == 'inv' and name then
        sendInv(player, name)
    elseif verb == 'gear' and name then
        sendGear(player, name)
    elseif verb == 'recasts' and name then
        sendRecasts(player, name)
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
    elseif verb == 'profile' and name then
        sendProfile(player, name)
    elseif verb == 'jobs' and name then
        sendJobs(player, name)
    elseif verb == 'skills' and name then
        sendSkills(player, name)
    elseif verb == 'mskills' and name then
        sendMagicSkills(player, name)
    elseif verb == 'give' and name then
        local err = player:cardianGive(name, tonumber(args[3]) or 0, tonumber(args[4]) or 1)
        if err ~= '' then
            reply(player, '#cd err give ' .. err)
        else
            reply(player, '#cd ok give')
            sendInv(player, name)
        end
    elseif verb == 'take' and name then
        local err = player:cardianTake(name, tonumber(args[3]) or 0, tonumber(args[4]) or 1)
        if err ~= '' then
            reply(player, '#cd err take ' .. err)
        else
            reply(player, '#cd ok take')
            sendInv(player, name)
        end
    elseif verb == 'do' and name and args[3] then
        local err = player:cardianDo(name, args[3], tonumber(args[4]) or 0)
        if err ~= '' then
            reply(player, '#cd err do ' .. err)
        else
            reply(player, '#cd ok do')
        end
    elseif verb == 'rescue' and name then
        local err = player:cardianRescue(name)
        if err ~= '' then
            reply(player, '#cd err rescue ' .. err)
        else
            reply(player, '#cd ok rescue')
        end
    elseif verb == 'homepoint' and name then
        local err = player:cardianHomePoint(name)
        if err ~= '' then
            reply(player, '#cd err homepoint ' .. err)
        else
            reply(player, '#cd ok homepoint')
        end
    elseif verb == 'use' and name then
        local err = player:cardianUse(name, tonumber(args[3]) or 0)
        if err ~= '' then
            reply(player, '#cd err use ' .. err)
        else
            -- the item decrements only when the use completes; the addon
            -- re-syncs after the cast to see the result
            reply(player, '#cd ok use')
        end
    elseif verb == 'drop' and name then
        local err = player:cardianDrop(name, tonumber(args[3]) or 0, tonumber(args[4]) or 1)
        if err ~= '' then
            reply(player, '#cd err drop ' .. err)
        else
            reply(player, '#cd ok drop')
            sendInv(player, name)
        end
    elseif verb == 'giveuse' and name then
        local err = player:cardianGiveUse(name, tonumber(args[3]) or 0, tonumber(args[4]) or 1)
        if err ~= '' then
            reply(player, '#cd err giveuse ' .. err)
        else
            reply(player, '#cd ok giveuse')
            sendInv(player, name)
        end
    elseif verb == 'wear' and name then
        local err = player:cardianWear(name, tonumber(args[3]) or 0, tonumber(args[4]) or 0)
        if err ~= '' then
            reply(player, '#cd err wear ' .. err)
        else
            reply(player, '#cd ok wear')
            sendStatsLine(player, name)
            sendGear(player, name)
            sendInv(player, name)
        end
    elseif verb == 'strip' and name then
        local err = player:cardianStrip(name, tonumber(args[3]) or 0)
        if err ~= '' then
            reply(player, '#cd err strip ' .. err)
        else
            reply(player, '#cd ok strip')
            sendStatsLine(player, name)
            sendGear(player, name)
            sendInv(player, name)
        end
    else
        player:printToPlayer('Usage: !cardian list | sync <name> | inv <name> | gear <name> | give | take | wear | strip | equipset | use <name> <slot> | drop <name> <slot> <qty> | giveuse <name> <slot> <qty> | rescue <name> | do <name> <action> [targid]')
    end
end

return commandObj
