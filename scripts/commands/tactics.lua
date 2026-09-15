-----------------------------------
-- func: tactics
-- desc: Cardian party tactics (RESEARCH §12) - the fight log as it stands:
--       your party's open and recent fights, this zone's averages per mob
--       type, every member's cure figures and the debuffs' records.
-----------------------------------
---@type TCommand
local commandObj = {}

commandObj.cmdprops =
{
    permission = 0,
    parameters = '',
}

commandObj.onTrigger = function(player)
    for _, line in ipairs(player:cardianTactics()) do
        player:printToPlayer(line)
    end
end

return commandObj
