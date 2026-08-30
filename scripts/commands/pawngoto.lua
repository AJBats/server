-----------------------------------
-- func: pawngoto <charname> <zoneid>
-- desc: Cardian pawns - order a pawn to travel to a zone on its own,
--       overriding follow behavior. The order clears on arrival.
-----------------------------------
---@type TCommand
local commandObj = {}

commandObj.cmdprops =
{
    permission = 1,
    parameters = 's',
}

commandObj.onTrigger = function(player, args)
    local targetName, zoneArg = string.match(args or '', '^(%S+)%s+(%S+)')
    local zoneId = tonumber(zoneArg)

    if not targetName or not zoneId then
        player:printToPlayer('Usage: !pawngoto <charname> <zoneid>')
        return
    end

    if player:pawnGoto(targetName, zoneId) then
        player:printToPlayer(string.format('%s sets out for zone %d.', targetName, zoneId))
    else
        player:printToPlayer(string.format('Cannot send %s there (not a live pawn, or zone unavailable).', targetName))
    end
end

return commandObj
