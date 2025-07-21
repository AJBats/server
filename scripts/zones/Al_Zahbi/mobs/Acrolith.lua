-----------------------------------
-- Area: Al Zahbi
--  Mob: Acrolith
-----------------------------------
mixins = { require('scripts/mixins/families/acrolith') }
-----------------------------------
---@type TMobEntity
local entity = {}

entity.onMobWeaponSkillPrepare = function(mob)
    return mob:getLocalVar('acrolith_chosen_ws')
end

return entity
