-- Acrolith family mixin
-- Customization:
--   Acrolith family of mobs have a behavior of losing body parts based on what skills
--   they perform. All acrolith type mobs can mix in this lua.

require('scripts/globals/mixins')

g_mixins = g_mixins or {}
g_mixins.families = g_mixins.families or {}

local LOST_L_ARM = 'lost_l_arm'
local LOST_R_ARM = 'lost_r_arm'
local LOST_BODY = 'lost_Body'

g_mixins.families.acrolith = function(acrolithMob)
    acrolithMob:addListener('WEAPONSKILL_PREPARE', 'ACROLITH_WEAPONSKILL_PREPARE', function(mob)
        -- From wiki:
        -- They fall apart piece-by-piece whenever they use "Dismemberment" in the following order:
        -- left arm, right arm, and then the body, leaving only the legs left to fight.
        local lostLeftArm = mob:getLocalVar(LOST_L_ARM)
        local lostRightArm = mob:getLocalVar(LOST_R_ARM)
        local lostBody = mob:getLocalVar(LOST_BODY)

        local skills =
        {
            xi.mobSkill.DIRE_STRAIGHT,
            xi.mobSkill.EARTH_SHATTER,
            xi.mobSkill.SINKER_DRILL,
        }

        if lostLeftArm == 0 or lostRightArm == 0 or lostBody == 0 then
            table.insert(skills, xi.mobSkill.DISMEMBERMENT)
        end

        if lostRightArm == 0 then
            table.insert(skills, xi.mobSkill.DETONATING_GRIP)
        end

        mob:setLocalVar('acrolith_chosen_ws', utils.randomEntry(skills))
    end)

    acrolithMob:addListener('WEAPONSKILL_USE', 'ACROLITH_WEAPONSKILL_USE', function(mob, target, wsid, tp, action)
        if wsid == xi.mobSkill.DETONATING_GRIP then
            print("losing right arm...")
            mob:setLocalVar(LOST_R_ARM, 1);            
            return
        end

        if wsid == xi.mobSkill.DISMEMBERMENT then
            -- First we lose left arm on Dismemberment
            if mob:getLocalVar(LOST_L_ARM) == 0 then
                print("losing left arm...")
                mob:setLocalVar(LOST_L_ARM, 1)
                -- TODO Set animation to left arm dismemberment
                action:setAnimation(target:getID(), 1411) 
                return
            end

            -- If we still have a right arm, we lose that next
            if mob:getLocalVar(LOST_R_ARM) == 0 then
                print("losing right arm...")
                mob:setLocalVar(LOST_R_ARM, 1)
                action:setAnimation(target:getID(), 1411)
                return
            end

            -- Nothing left to lose but body
            print("losing body...")
            mob:setLocalVar(LOST_BODY, 1)
            -- TODO Set animation to body dismemberment
            action:setAnimation(target:getID(), 1411)
        end
    end)    
end

return g_mixins.families.acrolith
