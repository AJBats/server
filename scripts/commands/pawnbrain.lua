-----------------------------------
-- func: pawnbrain <charname>
-- desc: Cardian pawns - reload a spawned pawn's gambit rows: her saved set,
--       else her job's defaults. modules/cardian/world/brains.yaml is read
--       again first if it changed, so every world body's world layer follows
--       it at once rather than at the next poll.
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
        player:printToPlayer('Usage: !pawnbrain <charname>')
        return
    end

    if player:pawnReloadBrain(targetName) then
        player:printToPlayer(string.format('Brain reloaded for %s.', targetName))
    else
        player:printToPlayer(string.format('%s is not a spawned pawn.', targetName))
    end
end

return commandObj
