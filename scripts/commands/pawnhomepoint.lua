-----------------------------------
-- func: pawnhomepoint <charname>
-- desc: Cardian pawns - a KO'd cardian home points: revived at full HP at
--       your home point (cardians always share yours), then it walks back
--       to the party through the world. The companion addon's Home point
--       menu entry runs the same thing.
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
        player:printToPlayer('Usage: !pawnhomepoint <charname>')
        return
    end

    local err = player:cardianHomePoint(targetName)
    if err ~= '' then
        player:printToPlayer(string.format('%s cannot home point: %s.', targetName, err))
    else
        player:printToPlayer(string.format('%s home points.', targetName))
    end
end

return commandObj
