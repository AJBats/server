-----------------------------------
-- Cardian: the party progresses together
--
-- Quests and missions are the player's doing -- a cardian cannot talk to
-- an NPC -- but she was there, so her log moves with the player's and she
-- earns her own reward. A cardian may enter any battlefield her player
-- enters: the registrant's registration already registers every party
-- member in the zone, and a member then only has to touch the circle to
-- go in, which she never can; so she goes in right behind the player.
-- Scope: cardians in the player's party and in the player's zone; a party
-- cardian elsewhere is named in the chat log as left out.
--
-- The player hears who moved with them, in the chat log, one line per
-- outcome: "Jevyak and Zapp accepted the mission - Bat Hunt!", and a line
-- of its own for each cardian who fared differently.
-----------------------------------
require('modules/module_utils')
-----------------------------------
local m = Module:new('cardian_party_progress')

local channel = xi.msg.channel.SYSTEM_3

-- One line to the player's chat log and to the map log
local function say(player, line)
    print(string.format('[cardian] to %s: %s', player:getName(), line))
    player:printToPlayer(line, channel)
end

-- "Jevyak", "Jevyak and Zapp", "Jevyak, Zapp and Kupo"
local function joinNames(names)
    local n = #names
    if n == 1 then
        return names[1]
    end
    return table.concat(names, ', ', 1, n - 1) .. ' and ' .. names[n]
end

-- The enum key as a title: SMASH_THE_ORCISH_SCOUTS -> Smash the Orcish Scouts
local smallWords =
{
    a = true, an = true, the = true, of = true, to = true, at = true, on = true,
    by = true, ['in'] = true, ['for'] = true, ['and'] = true, ['or'] = true,
    with = true, from = true,
}

local function titleFromKey(key)
    local words = {}
    for word in string.gmatch(string.lower(key), '[^_]+') do
        if #words > 0 and smallWords[word] then
            words[#words + 1] = word
        else
            words[#words + 1] = string.upper(string.sub(word, 1, 1)) .. string.sub(word, 2)
        end
    end
    return table.concat(words, ' ')
end

local function nameIn(ids, id)
    if ids ~= nil then
        for key, value in pairs(ids) do
            if value == id then
                return titleFromKey(key)
            end
        end
    end
    return string.format('#%d', id)
end

local function missionName(logId, missionId)
    return nameIn(xi.mission.id[xi.mission.area[logId]], missionId)
end

local function questName(logId, questId)
    return nameIn(xi.quest.id[xi.quest.area[logId]], questId)
end

-- The cardians in the player's party, split by whether they are in the
-- player's zone. Nothing for a cardian's own completion (never mirror a
-- mirror)
local function partyCardians(player)
    local here, away = {}, {}
    if not player:isPC() or player:isCardian() then
        return here, away
    end

    local party = player:getParty()
    if party == nil then
        return here, away
    end

    local zone = player:getZoneID()
    for _, member in pairs(party) do
        if member:isCardian() then
            if member:getZoneID() == zone then
                here[#here + 1] = member
            else
                away[#away + 1] = member
            end
        end
    end
    return here, away
end

-- Outcomes grouped by their line, so cardians who fared the same share
-- one line: those who did first, then those who could not, each group in
-- the order its outcomes first appeared
local function newReport()
    return { lines = {}, byLine = {} }
end

local function report(r, cardian, line, ok)
    local entry = r.byLine[line]
    if entry == nil then
        entry = { line = line, ok = ok, names = {} }
        r.byLine[line]        = entry
        r.lines[#r.lines + 1] = entry
    end
    entry.names[#entry.names + 1] = cardian:getName()
end

local function send(r, player)
    for _, wantOk in ipairs({ true, false }) do
        for _, entry in ipairs(r.lines) do
            if entry.ok == wantOk then
                say(player, joinNames(entry.names) .. entry.line)
            end
        end
    end
end

-- Her own reward, the same as the player's. An item she has no room for
-- is the only thing left out; the log entry still lands
local function withoutItems(params)
    if params == nil or params.item == nil then
        return params
    end

    local copy = {}
    for k, v in pairs(params) do
        if k ~= 'item' and k ~= 'itemParams' then
            copy[k] = v
        end
    end
    return copy
end

-- act(cardian) returns ok, detail: the reason when she could not, or a
-- note when she did with something left out
local did = { accept = 'accepted', complete = 'completed' }

local function mirror(player, verb, kind, title, act)
    local here, away = partyCardians(player)
    local r          = newReport()
    for _, cardian in ipairs(here) do
        local ok, detail = act(cardian)
        if not ok then
            report(r, cardian, string.format(' could not %s the %s (%s).', verb, kind, detail), false)
        elseif detail ~= nil then
            report(r, cardian, string.format(' %s the %s - %s! (%s)', did[verb], kind, title, detail), true)
        else
            report(r, cardian, string.format(' %s the %s - %s!', did[verb], kind, title), true)
        end
    end
    for _, cardian in ipairs(away) do
        report(r, cardian, string.format(' could not %s the %s (not in your zone).', verb, kind), false)
    end
    send(r, player)
end

-- She takes the mission from the same state the player took it from: no
-- mission, or the log's resting value (a category on the Promathia log,
-- 0 on the later ones); a different current mission of her own stands
m:addOverride('Mission.begin', function(self, player)
    local before = player:getCurrentMission(self.areaId)
    super(self, player)
    mirror(player, 'accept', 'mission', missionName(self.areaId, self.missionId), function(cardian)
        local current = cardian:getCurrentMission(self.areaId)
        if current ~= before and current ~= self.missionId then
            return false, 'already on another mission'
        end
        super(self, cardian)
        return true
    end)
end)

-- A legacy script's quest is not accepted here; her log catches up at
-- completion
m:addOverride('Quest.begin', function(self, player)
    super(self, player)
    mirror(player, 'accept', 'quest', questName(self.areaId, self.questId), function(cardian)
        super(self, cardian)
        return true
    end)
end)

m:addOverride('npcUtil.completeMission', function(player, logId, missionId, params)
    local ok = super(player, logId, missionId, params)
    if ok then
        mirror(player, 'complete', 'mission', missionName(logId, missionId), function(cardian)
            -- She was there at the end: her log catches up, since many
            -- scripts grant a mission with a bare addMission this module
            -- never sees, and a chain's next mission comes with the reward
            if cardian:getCurrentMission(logId) ~= missionId then
                cardian:addMission(logId, missionId)
            end
            if super(cardian, logId, missionId, params) then
                return true
            end
            super(cardian, logId, missionId, withoutItems(params))
            return true, 'no room for the item'
        end)
    end
    return ok
end)

m:addOverride('npcUtil.completeQuest', function(player, area, quest, params)
    local ok = super(player, area, quest, params)
    if ok then
        mirror(player, 'complete', 'quest', questName(area, quest), function(cardian)
            if super(cardian, area, quest, params) then
                return true
            end
            super(cardian, area, quest, withoutItems(params))
            return true, 'no room for the item'
        end)
    end
    return ok
end)

-- Into the battlefield right behind the player: registered with them by
-- the registration itself, entered here since she cannot touch the circle
m:addOverride('Battlefield.onEntryEventUpdate', function(self, player, csid, option, npc)
    local result = super(self, player, csid, option, npc)
    if player:getBattlefield() ~= nil then
        local here = partyCardians(player)
        local r    = newReport()
        for _, cardian in ipairs(here) do
            if cardian:getBattlefield() ~= nil then
                -- already inside
            elseif cardian:hasStatusEffect(xi.effect.BATTLEFIELD) then
                cardian:enterBattlefield()
                report(r, cardian, ' entered the battlefield.', true)
            else
                report(r, cardian, ' could not enter the battlefield (not registered).', false)
            end
        end
        send(r, player)
    end
    return result
end)

return m
