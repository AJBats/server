-----------------------------------
-- func: pawnbrain <charname>
-- desc: Cardian pawns - reload a spawned pawn's gambit brain from disk
--       (edit modules/cardian/lua/pawn/brains/<job>.lua, then run this).
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
