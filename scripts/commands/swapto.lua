-----------------------------------
-- func: swapto <charname>
-- desc: Cardian charswap - rezone this client into another character on the
--       same account (target must be offline). Decides swap Design A vs B.
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
        player:printToPlayer('Usage: !swapto <charname>')
        return
    end

    if player:swapTo(targetName) then
        player:printToPlayer(string.format('Swapping to %s...', targetName))
    else
        player:printToPlayer(string.format('Cannot swap to %s (unknown name, online, wrong account, or charswap disabled).', targetName))
    end
end

return commandObj
