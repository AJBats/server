-----------------------------------
-- func: pawndespawn <charname>
-- desc: Cardian pawns - remove a spawned pawn from the world.
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
        player:printToPlayer('Usage: !pawndespawn <charname>')
        return
    end

    if player:pawnDespawn(targetName) then
        player:printToPlayer(string.format('Pawn %s dissolves.', targetName))
    else
        player:printToPlayer(string.format('%s is not an active pawn.', targetName))
    end
end

return commandObj
