-----------------------------------
-- Cardian pawn brain: RNG
-- Shooter: Sharpshot and Velocity Shot up, Barrage when ready, then keep firing.
-----------------------------------
local b = require('modules/cardian/lua/pawn/brains/_blocks')

local spell  = xi.magic.spell
local family = xi.magic.spellFamily

return
{
    name = 'RNG',

    -- Weapon skills: as soon as TP allows, strongest known
    tp = { ai.tp.ASAP, ai.s.HIGHEST },

    gambits = b.list({
        b.jaSelf(xi.effect.SHARPSHOT,     xi.ja.SHARPSHOT),
        b.jaSelf(xi.effect.VELOCITY_SHOT, xi.ja.VELOCITY_SHOT),
        b.jaTarget(xi.ja.BARRAGE, 60),
        b.shoot,
    }),
}
