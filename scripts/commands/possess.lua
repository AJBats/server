-----------------------------------
-- func: possess <charname>
-- desc: Cardian - take control of one of your cardians (or an offline
--       character on your account). The character you were playing stays
--       behind as a cardian where it stood; a live cardian keeps its spot.
-----------------------------------
---@type TCommand
local commandObj = {}

commandObj.cmdprops =
{
    permission = 0,
    parameters = 's',
}

commandObj.onTrigger = function(player, targetName)
    if targetName == nil or targetName == '' then
        player:printToPlayer('Usage: !possess <charname>')
        return
    end

    if player:possess(targetName) then
        player:printToPlayer(string.format('Possessing %s...', targetName))
    else
        player:printToPlayer(string.format('Cannot possess %s (unknown, not yours, being played, or not in this zone).', targetName))
    end
end

return commandObj
