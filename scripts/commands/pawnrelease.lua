-----------------------------------
-- func: pawnrelease <name>
-- desc: Cardian recruitment (ROADMAP H), the other way - one of yours goes
--       back to being one of the world's: her cardian_pawns row returns to
--       the world's account and the census offers her slots again. Standing,
--       she is handed back to the world where she stands, which takes her
--       out of your party with her body.
--
--       Release, not dismissal: dismissal is leaving your party, which
--       changes no tier at all.
--
--       GM only.
-----------------------------------
---@type TCommand
local commandObj = {}

commandObj.cmdprops =
{
    permission = 1,
    parameters = 's',
}

commandObj.onTrigger = function(player, name)
    if name == nil then
        player:printToPlayer('Usage: !pawnrelease <name>')
        return
    end

    local err = player:cardianRelease(name)
    if err ~= '' then
        player:printToPlayer(string.format('Cannot release %s: %s.', name, err))
    else
        player:printToPlayer(string.format('%s is one of the world\'s again.', name))
    end
end

return commandObj
