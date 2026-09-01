-----------------------------------
-- func: cardian <verb> [args]
-- desc: Cardian companion-addon API. Structured replies go out as '#cd ...'
--       lines on chat channel 31 (NS_LINKSHELL3); the addon parses and
--       swallows them before the chat log renders them, so this command is
--       machine-facing -- humans get terse errors, the addon gets data.
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
    player:printToPlayer(line, channel)
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

commandObj.onTrigger = function(player, line)
    local args = {}
    for word in tostring(line or ''):gmatch('%S+') do
        args[#args + 1] = word
    end

    local verb = args[1]
    local name = args[2]

    if verb == 'list' then
        sendList(player)
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
