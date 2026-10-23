-----------------------------------
-- Cardian: a cardian walks through a zone's trigger areas without their scripts
--
-- A trigger area's script is for a player's client: most start an event --
-- the Chateau d'Oraguille's guards at its gate, a quest's scene on a street
-- corner -- and a cardian has no client to play one through to its end. She
-- would stand in it for good, and the game's own guards against pulling
-- someone out of an event then refuse her: a Teleport someone else casts on
-- her skips her, as do the menus that ask whether she is free. So the zone
-- checks her in and out of its areas as ever, and neither script runs for
-- her. Her Lua says so with isCardian (src/map/pawn/pawn_module.cpp).
-----------------------------------
require('modules/module_utils')
-----------------------------------
local m = Module:new('cardian_no_trigger_areas')

m:addOverride('InteractionGlobal.onTriggerAreaEnter', function(player, triggerArea, instance, fallbackFn)
    if player:isCardian() then
        return
    end

    return super(player, triggerArea, instance, fallbackFn)
end)

m:addOverride('InteractionGlobal.onTriggerAreaLeave', function(player, triggerArea, instance, fallbackFn)
    if player:isCardian() then
        return
    end

    return super(player, triggerArea, instance, fallbackFn)
end)

return m
