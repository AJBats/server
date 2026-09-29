-----------------------------------
-- Cardian: what the player could recruit for (the Link's GOALS,
-- src/map/pawn/link_api.cpp): the party finder's goals besides experience
--
--   xi.cardian.finder.goals(player)
--     -> { missions, quests, completed }: missions, the current mission on
--        each log the era plays that has one; quests, every quest under way;
--        each { log, id, title }, log the mission log or the quest area;
--        completed, by mission log, how many of its missions are done.
--
-- A library, not a module: it overrides nothing, so the pawn module loads it
-- at init (pawn::linkapi::loadLibraries), and xi_test requires it from the
-- test.
-----------------------------------
xi = xi or {}
xi.cardian = xi.cardian or {}
xi.cardian.finder = {}

-- The enum key as a title, the party progress module's own caser
local function titleFromKey(key)
    return xi.cardian.titleFromKey and xi.cardian.titleFromKey(key) or key
end

local function keyOf(ids, id)
    if ids ~= nil then
        for key, value in pairs(ids) do
            if value == id then
                return key
            end
        end
    end
    return nil
end

-- The logs the era plays: the three nations, Zilart, Promathia, Aht Urhgan.
-- The nation logs rest on NONE; the later ones the game resets to 0 at a
-- completion, so 0 reads as none there too (Zilart's and Aht Urhgan's first
-- missions carry id 0 and cannot be told from an empty log) -- and
-- Promathia's rests on a category
local goalLogs =
{
    { log = xi.mission.log_id.SANDORIA, none = { [xi.mission.id.nation.NONE] = true } },
    { log = xi.mission.log_id.BASTOK,   none = { [xi.mission.id.nation.NONE] = true } },
    { log = xi.mission.log_id.WINDURST, none = { [xi.mission.id.nation.NONE] = true } },
    { log = xi.mission.log_id.ZILART,   none = { [xi.mission.id.nation.NONE] = true, [0] = true } },
    { log = xi.mission.log_id.COP,      none = { [xi.mission.id.nation.NONE] = true, [0] = true, [xi.mission.id.cop.ANCIENT_FLAMES_BECKON] = true } },
    { log = xi.mission.log_id.TOAU,     none = { [xi.mission.id.nation.NONE] = true, [0] = true } },
}

xi.cardian.finder.goals = function(player)
    local found = { missions = {}, quests = {}, completed = {} }

    for _, entry in ipairs(goalLogs) do
        local ids     = xi.mission.id[xi.mission.area[entry.log]]
        local current = player:getCurrentMission(entry.log)
        if not entry.none[current] then
            local key = keyOf(ids, current)
            found.missions[#found.missions + 1] =
            {
                log   = entry.log,
                id    = current,
                title = key and titleFromKey(key) or ('#' .. current),
            }
        end
        local done = 0
        if ids ~= nil then
            for _, id in pairs(ids) do
                if id ~= xi.mission.id.nation.NONE and player:hasCompletedMission(entry.log, id) then
                    done = done + 1
                end
            end
        end
        found.completed[entry.log] = done
    end

    for area = 0, xi.questLog.COALITION do
        local ids = xi.quest.id[xi.quest.area[area]]
        if ids ~= nil then
            local keys = {}
            for key in pairs(ids) do
                keys[#keys + 1] = key
            end
            table.sort(keys)
            for _, key in ipairs(keys) do
                if player:getQuestStatus(area, ids[key]) == xi.questStatus.QUEST_ACCEPTED then
                    found.quests[#found.quests + 1] = { log = area, id = ids[key], title = titleFromKey(key) }
                end
            end
        end
    end
    return found
end
