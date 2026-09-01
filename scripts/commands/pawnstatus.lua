-----------------------------------
-- func: pawnstatus <charname>
-- desc: Cardian pawns - print a spawned pawn's job, level, HP/MP/TP and
--       loaded gambit count (the vanilla client hides party members' TP).
-----------------------------------
---@type TCommand
local commandObj = {}

commandObj.cmdprops =
{
    permission = 0,
    parameters = 's',
}

local jobShort =
{
    'WAR', 'MNK', 'WHM', 'BLM', 'RDM', 'THF', 'PLD', 'DRK', 'BST', 'BRD', 'RNG',
    'SAM', 'NIN', 'DRG', 'SMN', 'BLU', 'COR', 'PUP', 'DNC', 'SCH', 'GEO', 'RUN',
}

commandObj.onTrigger = function(player, targetName)
    if targetName == nil or targetName == '' then
        player:printToPlayer('Usage: !pawnstatus <charname>')
        return
    end

    local targ = GetPlayerByName(targetName)
    if targ == nil then
        player:printToPlayer(string.format('%s is not in the world.', targetName))
        return
    end

    local sub = targ:getSubJob()
    local subText = sub ~= 0 and string.format('/%s%d', jobShort[sub] or '?', targ:getSubLvl()) or ''

    player:printToPlayer(string.format('%s %s%d%s  HP %d/%d  MP %d/%d  TP %d  gambits %d  ws [%s]',
        targ:getName(),
        jobShort[targ:getMainJob()] or '?', targ:getMainLvl(), subText,
        targ:getHP(), targ:getMaxHP(),
        targ:getMP(), targ:getMaxMP(),
        targ:getTP(),
        targ:pawnGambitCount(),
        targ.pawnWeaponSkills and targ:pawnWeaponSkills() or '?'))
end

return commandObj
