-----------------------------------
-- Cardian: what the player has earned the right to buy
--
-- The simulated auction house crowd (tools/economy/market.py) sells a
-- notorious monster's drops once the player's party has killed it, and a
-- battlefield tier's loot once any fight of that tier is won. This module
-- records those events as char vars the crowd reads:
--
--   [MKT]nm:<mob entity id>            the NM died with the player's party there
--   [MKT]tier:<orb item id>/<cap>      an orb fight at that level cap was won
--   [MKT]quest:<log id>/<quest id>     a quest was completed (its rewards)
--   [MKT]chest:<zone id>/<1 chest|2 coffer>  a key was traded to a container in that zone
--
-- The price book (tools/economy/compile.py) names the same vars on each
-- gated item. Cardians take part in the kill but the var is the player's;
-- a cardian's var would never be read.
-----------------------------------
require('modules/module_utils')
require('scripts/globals/battlefield')
require('scripts/globals/treasure')
-----------------------------------
local m = Module:new('cardian_market_unlocks')

local function record(player, var)
    if player ~= nil and player:isPC() and not player:isCardian() then
        player:setCharVar(var, 1)
    end
end

m:addOverride('npcUtil.completeQuest', function(player, area, quest, params)
    local ok = super(player, area, quest, params)
    if ok then
        record(player, string.format('[MKT]quest:%d/%d', area, quest))
    end
    return ok
end)

-- A key traded to a chest or coffer opens the zone's chest loot for the
-- crowd, mimic or not: the key was the achievement
m:addOverride('xi.treasure.onTrade', function(player, npc, trade, bypassType, bypassReward)
    local result = super(player, npc, trade, bypassType, bypassReward)
    local container = ({ Treasure_Chest = 1, Treasure_Coffer = 2 })[npc:getName()]
    if container ~= nil and trade ~= nil and trade:getItemCount() >= 1 then
        record(player, string.format('[MKT]chest:%d/%d', player:getZoneID(), container))
    end
    return result
end)

m:addOverride('xi.mob.onMobDeathEx', function(mob, player, isKiller, isWeaponSkillKill)
    super(mob, player, isKiller, isWeaponSkillKill)
    if mob ~= nil and mob:isNM() then
        record(player, string.format('[MKT]nm:%d', mob:getID()))
    end
end)

m:addOverride('Battlefield.onBattlefieldWin', function(self, player, battlefield)
    super(self, player, battlefield)
    if self.isMission then
        return
    end

    -- the tier is the orb and the cap; a fight with no orb (the memory-cluster
    -- ENMs) is tier 0 at its cap, which the book names the same way
    local orb = self.requiredItems and self.requiredItems[1] or 0
    if type(orb) ~= 'number' then
        orb = 0
    end
    record(player, string.format('[MKT]tier:%d/%d', orb, self.levelCap or 0))
end)
