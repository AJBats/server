-----------------------------------
-- Cardian: the linkshell's errand table
-- (modules/cardian/lua/errand_quests.lua, the Link's ERRANDS and SEND_ERRAND):
-- its quests, and each nation's rank ladder for a catch-up
--
-- The pawn module loads the library at init; under xi_test there is no pawn
-- module, so the test loads it itself.
-----------------------------------
require('modules/cardian/lua/errand_quests')

describe('Cardian errand table', function()
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
    end)

    it('is loaded', function()
        assert(xi.cardian and xi.cardian.errands and xi.cardian.errands.goals and xi.cardian.errands.ladder,
            'modules/cardian/lua/errand_quests.lua did not load')
    end)

    it('names a game quest, a route of real zones and an hour or two for every row', function()
        for _, entry in ipairs(xi.cardian.errands.entries) do
            local ids = xi.quest.id[xi.quest.area[entry.log]]
            assert(entry.kind == 'quest', string.format('%s: the table holds quests; missions go by the rank ladder', tostring(entry.key)))
            assert(ids ~= nil and ids[entry.key] ~= nil, string.format('quest %s is not an id of its log', tostring(entry.key)))
            assert(entry.minutes >= 60 and entry.minutes <= 120, string.format('%s takes %d minutes', entry.key, entry.minutes))
            assert(#entry.zones >= 2, string.format('%s crosses fewer than two zones', entry.key))
            for i, zone in ipairs(entry.zones) do
                assert(type(zone) == 'number' and zone > 0, string.format('%s: zone %d of its route is not a zone', entry.key, i))
            end
        end
    end)

    it('offers nothing he has not done', function()
        assert(#xi.cardian.errands.goals(player) == 0, 'a new character has done none of the table')
    end)

    it('offers a quest once he has completed it, with its job', function()
        local log, id = xi.questLog.SANDORIA, xi.quest.id.sandoria.A_KNIGHTS_TEST
        player:addQuest(log, id)
        assert(find(xi.cardian.errands.goals(player), log, id) == nil, 'a quest under way is not done')

        player:completeQuest(log, id)
        local goal = find(xi.cardian.errands.goals(player), log, id)
        assert(goal ~= nil, 'a completed quest of the table is offered')
        assert(goal.title ~= '', 'with a title')
        assert(goal.job == xi.job.PLD, 'A Knight\'s Test unlocks Paladin')
        assert(#goal.zones >= 2 and goal.minutes > 0, 'with its route and its time')
    end)

    it('offers the support jobs quest with job none', function()
        local log, id = xi.questLog.OTHER_AREAS, xi.quest.id.otherAreas.THE_OLD_LADY
        player:addQuest(log, id)
        player:completeQuest(log, id)
        local goal = find(xi.cardian.errands.goals(player), log, id)
        assert(goal ~= nil and goal.job == xi.job.NONE, 'The Old Lady unlocks the support jobs')
    end)

    it('gives each nation a ladder of its own missions, rank 2 to rank 6, in the log\'s order', function()
        for _, nation in ipairs({ xi.nation.SANDORIA, xi.nation.BASTOK, xi.nation.WINDURST }) do
            local ladder = xi.cardian.errands.ladder(nation)
            assert(#ladder == #xi.cardian.errands.ranks[nation], string.format('nation %d: every mission of its ladder is an id of its log', nation))
            local seen, lastId, lastStep = {}, -1, 0
            for _, mission in ipairs(ladder) do
                assert(mission.id > lastId, string.format('nation %d: mission %d is out of the log\'s order', nation, mission.id))
                assert(mission.step >= lastStep, string.format('nation %d: mission %d steps back', nation, mission.id))
                assert(mission.minutes > 0 and #mission.zones >= 2, string.format('nation %d: mission %d has its time and its route', nation, mission.id))
                assert(mission.title ~= '', 'with a title')
                for _, zone in ipairs(mission.zones) do
                    assert(type(zone) == 'number' and zone > 0, string.format('nation %d: mission %d crosses a zone that is none', nation, mission.id))
                end
                seen[mission.step] = true
                lastId, lastStep = mission.id, mission.step
            end
            for rank = 2, 6 do
                assert(seen[rank], string.format('nation %d: its ladder grants rank %d', nation, rank))
            end
        end
    end)

    it('starts each ladder with the nation\'s first mission', function()
        assert(xi.cardian.errands.ladder(xi.nation.SANDORIA)[1].id == xi.mission.id.sandoria.SMASH_THE_ORCISH_SCOUTS)
        assert(xi.cardian.errands.ladder(xi.nation.BASTOK)[1].id == xi.mission.id.bastok.THE_ZERUHN_REPORT)
        assert(xi.cardian.errands.ladder(xi.nation.WINDURST)[1].id == xi.mission.id.windurst.THE_HORUTOTO_RUINS_EXPERIMENT)
        assert(#xi.cardian.errands.ladder(99) == 0, 'no ladder for a nation that is none')
    end)
end)
