-----------------------------------
-- Cardian: what the player could recruit for
-- (modules/cardian/lua/finder_goals.lua, the Link's GOALS)
--
-- The pawn module loads the library at init; under xi_test there is no pawn
-- module, so the test loads it itself.
-----------------------------------
require('modules/cardian/lua/finder_goals')

describe('Cardian party finder goals', function()
    ---@type CClientEntityPair
    local player

    local function find(list, log, id)
        for _, goal in ipairs(list) do
            if goal.log == log and goal.id == id then
                return goal
            end
        end
        return nil
    end

    before_each(function()
        player = xi.test.world:spawnPlayer()
        player:setNation(xi.nation.BASTOK)
    end)

    it('is loaded', function()
        assert(xi.cardian and xi.cardian.finder, 'modules/cardian/lua/finder_goals.lua did not load')
    end)

    it('names a quest under way, and forgets it once it is done', function()
        local area, id = xi.questLog.SANDORIA, xi.quest.id.sandoria.GROWING_FLOWERS
        player:addQuest(area, id)
        local goal = find(xi.cardian.finder.goals(player).quests, area, id)
        assert(goal ~= nil and goal.title ~= '', 'the quest under way is a goal, with a title')

        player:completeQuest(area, id)
        assert(find(xi.cardian.finder.goals(player).quests, area, id) == nil, 'a finished quest is no goal')
    end)

    it('names the current mission on a log, and counts the missions done', function()
        local log, id = xi.mission.log_id.BASTOK, xi.mission.id.bastok.THE_ZERUHN_REPORT
        local before  = xi.cardian.finder.goals(player).completed[log]

        player:addMission(log, id)
        local found = xi.cardian.finder.goals(player)
        assert(find(found.missions, log, id) ~= nil, 'the current mission is a goal')
        assert(found.completed[log] == before, 'nothing more is done yet')

        player:completeMission(log, id)
        found = xi.cardian.finder.goals(player)
        assert(find(found.missions, log, id) == nil, 'a finished mission is no goal')
        assert(found.completed[log] == before + 1, string.format('one more done on the log: %s', tostring(found.completed[log])))
    end)
end)
