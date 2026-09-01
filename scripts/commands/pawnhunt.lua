-----------------------------------
-- func: pawnhunt <charname> <on|off>
-- desc: Cardian pawns - hunt mode. The flagged cardian picks and pulls
--       exp mobs on its own whenever the party is idle, healthy and past
--       the post-fight breather; the rest of the party joins the pull.
--       One hunter per party is the intended shape (usually the tank).
-----------------------------------
---@type TCommand
local commandObj = {}

commandObj.cmdprops =
{
    permission = 0,
    parameters = 's',
}

commandObj.onTrigger = function(player, line)
    local args = {}
    for word in tostring(line or ''):gmatch('%S+') do
        args[#args + 1] = word
    end

    local name = args[1]
    local mode = args[2]

    if name == nil or (mode ~= 'on' and mode ~= 'off') then
        player:printToPlayer('Usage: !pawnhunt <charname> <on|off>')
        return
    end

    local err = player:cardianHunt(name, mode == 'on')
    if err ~= '' then
        player:printToPlayer(string.format('Cannot set hunt mode: %s.', err))
    else
        player:printToPlayer(string.format('%s hunt mode %s.', name, mode))
    end
end

return commandObj
