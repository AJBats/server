-----------------------------------
-- Cardian: the party progresses together
--
-- Quests and missions are the player's doing -- a cardian cannot talk to
-- an NPC -- but she was there, so her log moves with the player's and she
-- earns her own reward. A cardian may enter any battlefield her player
-- enters: the registrant's registration already registers every party
-- member in the zone, and a member then only has to touch the circle to
-- go in, which she never can; so she goes in right behind the player.
-- Scope: cardians in the player's party and in the player's zone.
-----------------------------------
require('modules/module_utils')
-----------------------------------
local m = Module:new('cardian_party_progress')

-- The cardians in the player's party and zone; nothing for a cardian's
-- own completion (never mirror a mirror)
local function cardiansWith(player)
    local out = {}
    if not player:isPC() or player:isCardian() then
        return out
    end

    local party = player:getParty()
    if party == nil then
        return out
    end

    local zone = player:getZoneID()
    for _, member in pairs(party) do
        if member:isCardian() and member:getZoneID() == zone then
            out[#out + 1] = member
        end
    end
    return out
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

m:addOverride('npcUtil.completeQuest', function(player, area, quest, params)
    local ok = super(player, area, quest, params)
    if ok then
        for _, cardian in ipairs(cardiansWith(player)) do
            if not super(cardian, area, quest, params) then
                super(cardian, area, quest, withoutItems(params))
                print(string.format('[cardian] %s completes the quest with %s, no room for its item', cardian:getName(), player:getName()))
            end
        end
    end
    return ok
end)

m:addOverride('npcUtil.completeMission', function(player, logId, missionId, params)
    local ok = super(player, logId, missionId, params)
    if ok then
        for _, cardian in ipairs(cardiansWith(player)) do
            if not super(cardian, logId, missionId, params) then
                super(cardian, logId, missionId, withoutItems(params))
                print(string.format('[cardian] %s completes the mission with %s, no room for its item', cardian:getName(), player:getName()))
            end
        end
    end
    return ok
end)

-- Acceptance moves with the player's too, for the interaction framework's
-- quests and missions; a legacy script's quest catches up at completion
m:addOverride('Quest.begin', function(self, player)
    super(self, player)
    for _, cardian in ipairs(cardiansWith(player)) do
        super(self, cardian)
    end
end)

m:addOverride('Mission.begin', function(self, player)
    super(self, player)
    for _, cardian in ipairs(cardiansWith(player)) do
        super(self, cardian)
    end
end)

-- Into the battlefield right behind the player: registered with them by
-- the registration itself, entered here since she cannot touch the circle
m:addOverride('Battlefield.onEntryEventUpdate', function(self, player, csid, option, npc)
    local result = super(self, player, csid, option, npc)
    if player:getBattlefield() ~= nil then
        for _, cardian in ipairs(cardiansWith(player)) do
            if cardian:hasStatusEffect(xi.effect.BATTLEFIELD) and cardian:getBattlefield() == nil then
                cardian:enterBattlefield()
                print(string.format('[cardian] %s enters the battlefield with %s', cardian:getName(), player:getName()))
            end
        end
    end
    return result
end)

return m
