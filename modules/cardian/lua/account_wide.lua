-----------------------------------
-- Cardian: a grant of major progression reaches the whole account
--
-- What one character of an account earns, every character of it has
-- (ROADMAP N; src/map/pawn/account_wide.cpp keeps the list and does the
-- copying). These six entity calls are every path that writes those fields
-- once a character is loaded: unlockJob, changeJob and changesJob (the job
-- bits), setLevelCap (the level cap), addKeyItem (key items) and
-- addTeleport (the outposts' warps). After each, the account is told at once,
-- and whatever is new on the account's list reaches the rest of it. A real
-- player's zone-in is the safety net for anything that came another way.
-----------------------------------
require('modules/module_utils')
-----------------------------------
local m = Module:new('cardian_account_wide')

local function granted(player, what)
    if player:isPC() then
        player:cardianAccountGranted(what)
    end
end

m:addOverride('CBaseEntity.unlockJob', function(player, ...)
    super(player, ...)
    granted(player, 'unlockJob')
end)

m:addOverride('CBaseEntity.changeJob', function(player, ...)
    super(player, ...)
    granted(player, 'changeJob')
end)

m:addOverride('CBaseEntity.changesJob', function(player, ...)
    super(player, ...)
    granted(player, 'changesJob')
end)

m:addOverride('CBaseEntity.setLevelCap', function(player, ...)
    super(player, ...)
    granted(player, 'setLevelCap')
end)

m:addOverride('CBaseEntity.addKeyItem', function(player, ...)
    super(player, ...)
    granted(player, 'addKeyItem')
end)

m:addOverride('CBaseEntity.addTeleport', function(player, ...)
    super(player, ...)
    granted(player, 'addTeleport')
end)

return m
