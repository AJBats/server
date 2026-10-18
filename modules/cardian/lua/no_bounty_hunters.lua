-----------------------------------
-- Cardian: no Goblin Bounty Hunters
--
-- Retail's Goblin Bounty Hunters patrol the rivers and shores of Batallia
-- Downs, Buburimu Peninsula, Korroloka Tunnel, Qufim Island, Sauromugue
-- Champaign and Valkurm Dunes as a deterrent to fishing bots: aggressive,
-- linking goblins well above the zone's level, running through its camps.
-- With no bots to deter, they are kept out of the world: each leaves as it
-- first spawns and never respawns. Their drops come from other goblins.
-- cardian.GOBLIN_BOUNTY_HUNTERS = true brings them back at the next start.
-----------------------------------
require('modules/module_utils')
require('scripts/globals/goblin_bounty_hunter')
-----------------------------------
local m = Module:new('cardian_no_bounty_hunters')

m:addOverride('xi.goblinBountyHunter.onMobSpawn', function(mob)
    if xi.settings.cardian.GOBLIN_BOUNTY_HUNTERS then
        return super(mob)
    end

    DisallowRespawn(mob:getID(), true)
    DespawnMob(mob:getID(), 1)
end)

return m
