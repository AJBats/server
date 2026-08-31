-----------------------------------
-- func: pawncreate <name>
-- desc: Cardian pawns - mint a generated pawn character (male Hume Warrior,
--       defaults) owned by this account. Spawn it with !pawnspawn <name>.
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
        player:printToPlayer('Usage: !pawncreate <name>')
        return
    end

    if player:pawnCreate(targetName) then
        player:printToPlayer(string.format('Pawn %s has been created. Use !pawnspawn %s to summon them.', targetName, targetName))
    else
        player:printToPlayer(string.format('Cannot create %s (invalid or taken name, or pawns disabled).', targetName))
    end
end

return commandObj
