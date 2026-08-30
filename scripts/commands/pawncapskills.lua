-----------------------------------
-- func: pawncapskills <charname>
-- desc: Cardian pawns - cap every combat and magic skill of a spawned pawn
--       for its current job and level (the by-name form of !capallskills).
-----------------------------------
---@type TCommand
local commandObj = {}

commandObj.cmdprops =
{
    permission = 1,
    parameters = 's',
}

commandObj.onTrigger = function(player, targetName)
    if targetName == nil or targetName == '' then
        player:printToPlayer('Usage: !pawncapskills <charname>')
        return
    end

    local targ = GetPlayerByName(targetName)
    if targ == nil then
        player:printToPlayer(string.format('%s is not in the world.', targetName))
        return
    end

    targ:capAllSkills()
    player:printToPlayer(string.format('All skills capped for %s.', targ:getName()))
end

return commandObj
