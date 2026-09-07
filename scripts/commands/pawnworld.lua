-----------------------------------
-- func: pawnworld <stand|ring|fade|farm|idle> [name|count]
-- desc: Cardian world (ROADMAP D0/D1) - the developer's hands on the world's
--       adventurers. Players never need it: a zone fills itself (D3).
--         stand <census name>   she stands where you are, minted on first use
--         ring <count>          count of them in a ring round you, a couple a tick, pinned
--         fade <name|all>       fades her, or all of them, out
--         farm <name|all>       she farms: mobs in her band within reach, else she
--                               heads for the nearest farther off and fights what she meets
--         idle <name|all>       farming off
-----------------------------------
---@type TCommand
local commandObj = {}

commandObj.cmdprops =
{
    permission = 1,
    parameters = 'ss',
}

local usage = 'Usage: !pawnworld <stand|ring|fade|farm|idle> [name|count]'

commandObj.onTrigger = function(player, verb, arg)
    if verb == 'stand' and arg ~= nil and arg ~= '' then
        if player:worldSpawn(arg) then
            player:printToPlayer(string.format('%s stands here.', arg))
        else
            player:printToPlayer(string.format('Cannot stand %s here (not in the census, already present, or the world is off).', arg))
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
    else
        player:printToPlayer(usage)
    end
end

return commandObj
