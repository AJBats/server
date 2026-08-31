-----------------------------------
-- func: pawnjob <charname> <job> (level)
-- desc: Cardian pawns - change a spawned pawn's main job (and level).
--       The pawn's brain reloads for the new job on its next tick.
-----------------------------------
---@type TCommand
local commandObj = {}

commandObj.cmdprops =
{
    permission = 1,
    parameters = 'ssi',
}

commandObj.onTrigger = function(player, targetName, jobName, level)
    if targetName == nil or targetName == '' or jobName == nil or jobName == '' then
        player:printToPlayer('Usage: !pawnjob <charname> <job> (level)')
        return
    end

    local targ = GetPlayerByName(targetName)
    if targ == nil then
        player:printToPlayer(string.format('%s is not in the world.', targetName))
        return
    end

    local jobId = tonumber(jobName) or xi.job[string.upper(jobName)]
    if jobId == nil or jobId <= 0 or jobId > xi.job.RUN then
        player:printToPlayer('Invalid job. Use a short name such as WHM.')
        return
    end

    targ:changeJob(jobId)

    if level ~= nil and level >= 1 and level <= 99 then
        targ:setLevel(level)
    end

    player:printToPlayer(string.format('%s is now a level %d %s.', targ:getName(), targ:getMainLvl(), string.upper(jobName)))
end

return commandObj
