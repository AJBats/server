-----------------------------------
-- func: pawnworld <stand|ring|walk|fade> [name|count]
-- desc: Cardian world (ROADMAP D0) - the developer's hands on the world's
--       adventurers. Players never need it: a zone fills itself (D3).
--         stand <census name>   she stands where you are, minted on first use
--         ring <count>          count of them in a ring round you, a couple a tick, pinned
--         walk <name|all>       walks between her spot and where you stand, back and forth
--         fade <name|all>       fades her, or all of them, out
-----------------------------------
---@type TCommand
local commandObj = {}

commandObj.cmdprops =
{
    permission = 1,
    parameters = 'ss',
}

local usage = 'Usage: !pawnworld <stand|ring|walk|fade> [name|count]'

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
    elseif verb == 'walk' and arg ~= nil and arg ~= '' then
        player:printToPlayer(string.format('%d walk.', player:worldWalk(arg)))
    elseif verb == 'fade' and arg ~= nil and arg ~= '' then
        player:printToPlayer(string.format('%d faded.', player:worldDespawn(arg)))
    else
        player:printToPlayer(usage)
    end
end

return commandObj
