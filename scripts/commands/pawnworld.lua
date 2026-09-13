-----------------------------------
-- func: pawnworld <stand|ring|fade|farm|idle|slots|fill|slot|cap|faded> ...
-- desc: Cardian world (ROADMAP D0-D3) - the developer's hands on the world's
--       adventurers. Players never need it: a zone fills itself from its
--       slot table (modules/cardian/world/<Zone>.yaml).
--         stand <census name>   she stands where you are, minted on first use
--         ring <count>          count of them in a ring round you, a couple a tick, pinned:
--                               outside the waterfall and its caps (deprecated; the
--                               ladder never hears of them)
--         fade <name|all>       fades her, or all of them, out
--         farm <name|all>       she farms: mobs in her band within reach, else she
--                               heads for the nearest farther off and fights what she meets
--         idle <name|all>       farming off
--         slots                 this zone's slot table and who holds each slot (~ = faded)
--         fill                  this zone's bodies return to the pool; the table is
--                               re-read from its file and filled again
--         slot <farm|stand> <lo>-<hi> [count] [spread]
--                               author a slot where you stand: appended to the
--                               zone's file and filled at once (count 1, spread 20)
-----------------------------------
---@type TCommand
local commandObj = {}

commandObj.cmdprops =
{
    permission = 1,
    parameters = 'sssss',
}

local usage = 'Usage: !pawnworld <stand|ring|fade|farm|idle|slots|fill|faded> [name|count], cap [standing] [faded], or slot <farm|stand> <lo>-<hi> [count] [spread]'

commandObj.onTrigger = function(player, verb, arg, arg2, arg3, arg4)
    if verb == 'stand' and arg ~= nil and arg ~= '' then
        if player:worldSpawn(arg) then
            player:printToPlayer(string.format('%s stands here.', arg))
        else
            player:printToPlayer(string.format('%s does not stand yet: not in the census, already present, the world is off, or the caps have no room (!pawnworld cap).', arg))
        end
    elseif verb == 'ring' and tonumber(arg) ~= nil and tonumber(arg) >= 1 then
        local n = player:worldRing(math.floor(tonumber(arg)))
        player:printToPlayer(string.format('%d will stand in a ring around you, a couple a tick.', n))
    elseif verb == 'fade' and arg ~= nil and arg ~= '' then
        player:printToPlayer(string.format('%d faded.', player:worldDespawn(arg)))
    elseif verb == 'farm' and arg ~= nil and arg ~= '' then
        player:printToPlayer(string.format('%d farm.', player:worldFarm(arg, true)))
    elseif verb == 'idle' and arg ~= nil and arg ~= '' then
        player:printToPlayer(string.format('%d stopped farming.', player:worldFarm(arg, false)))
    elseif verb == 'slots' then
        for _, line in ipairs(player:worldSlots()) do
            player:printToPlayer(line)
        end
    elseif verb == 'faded' then
        -- Your cardians without a body, in chat: the !cardian verb of the
        -- same name answers the addon, which is silent about it
        local names = player:cardianFaded()
        player:printToPlayer(#names == 0 and 'None of yours is faded.' or ('Faded: ' .. table.concat(names, ', ')))
    elseif verb == 'cap' then
        -- The seat waterfall's two caps, live (ROADMAP H). Standing is
        -- server-wide, faded is per zone; no argument reports both plus
        -- where the standing budget has gone. 0 0 puts the settings back
        local standing = tonumber(arg) or 0
        local faded    = tonumber(arg2) or 0
        if arg == nil then
            player:printToPlayer(player:worldCaps())
        else
            player:printToPlayer(player:worldCap(math.floor(standing), math.floor(faded)))
        end
    elseif verb == 'fill' then
        player:printToPlayer(string.format('%d seat(s) queued; the zone refills a couple a tick.', player:worldFill()))
    elseif verb == 'slot' and (arg == 'farm' or arg == 'stand') and arg2 ~= nil then
        local lo, hi = string.match(arg2, '^(%d+)%-(%d+)$')
        if lo == nil then
            player:printToPlayer('The band is <lo>-<hi>, e.g. 2-6.')
            return
        end
        local count = tonumber(arg3) or 1
        local spread = tonumber(arg4) or 20
        if player:worldSlot(arg, tonumber(lo), tonumber(hi), math.floor(count), spread) then
            player:printToPlayer(string.format('Slot added here: %s L%s-%s x%d, spread %d. It fills a couple a tick.', arg, lo, hi, count, spread))
        else
            player:printToPlayer('Cannot add that slot (bad band or count, or the world is off).')
        end
    else
        player:printToPlayer(usage)
    end
end

return commandObj
