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
--       orders                           the party's orders (st <strategy> <retreat> <name;name>)
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

local function sendPawnLine(player, name)
    local targ = GetPlayerByName(name)
    if targ then
        reply(player, string.format('#cd p %s %d %d %d %d %d %d %d %d %d',
            name,
            targ:getMainJob(), targ:getMainLvl(),
            targ:getSubJob(), targ:getSubLvl(),
            targ:getHP(), targ:getMaxHP(),
            targ:getMP(), targ:getMaxMP(),
            targ:getTP()))
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
            reply(player, string.format('#cd p %s %d %d %d %d %d %d %d %d',
                name,
                targ:getMainJob(), targ:getMainLvl(),
                targ:getSubJob(), targ:getSubLvl(),
                targ:getHP(), targ:getMaxHP(),
                targ:getMP(), targ:getMaxMP()))
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
    reply(player, '#cd gv.e ' .. name)
end

-- The party's orders, one line: 'st <strategy> <retreat> <name;name...>'
local function sendOrders(player)
    local o = player:cardianOrders()
    if o == nil then
        reply(player, '#cd err orders no character')
        return
    end
    reply(player, string.format('#cd st %d %d %s', o.strategy, o.retreat and 1 or 0, table.concat(o.names, ';')))
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
        player:printToPlayer('Usage: !cardian list | sync <name> | inv <name> | gear <name> | give | take | wear | strip | equipset | use <name> <slot> | drop <name> <slot> <qty> | giveuse <name> <slot> <qty>')
    end
end

return commandObj
