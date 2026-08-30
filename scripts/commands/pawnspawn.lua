-----------------------------------
-- func: pawnspawn <charname>
-- desc: Cardian pawns - spawn an offline character from this account into
--       your zone as a session-less pawn.
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
        player:printToPlayer('Usage: !pawnspawn <charname>')
        return
    end

    if player:pawnSpawn(targetName) then
        player:printToPlayer(string.format('Pawn %s materializes.', targetName))
    else
        player:printToPlayer(string.format('Cannot spawn %s (unknown, online, already a pawn, wrong account, or pawns disabled).', targetName))
    end
end

return commandObj
