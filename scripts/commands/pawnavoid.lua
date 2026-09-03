-----------------------------------
-- func: pawnavoid <charname> <on|off>
-- desc: Cardian pawns - aggro avoidance. On (the default), the cardian
--       keeps itself, its formation slot and its paths outside every
--       nearby mob's detection range and is pushed away as mobs roam
--       toward it; the hunter also refuses pulls with other aggressive
--       mobs near the target. Off lets it walk anywhere. This checks or
--       unchecks the cardian's Avoid aggro gambit row.
-----------------------------------
---@type TCommand
local commandObj = {}

commandObj.cmdprops =
{
    permission = 0,
    parameters = 's',
}

commandObj.onTrigger = function(player, line)
    local name, mode = string.match(line or '', '^(%S+)%s+(%S+)')

    if name == nil or (mode ~= 'on' and mode ~= 'off') then
        player:printToPlayer('Usage: !pawnavoid <charname> <on|off>')
        return
    end

    local err = player:cardianAvoid(name, mode == 'on')
    if err ~= '' then
        player:printToPlayer(string.format('Cannot set aggro avoidance: %s.', err))
    else
        player:printToPlayer(string.format('%s aggro avoidance %s.', name, mode))
    end
end

return commandObj
