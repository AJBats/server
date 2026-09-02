-----------------------------------
-- Cardian pawn brain: DRG
-- Dragoon: jumps on cooldown, Ancient Circle up. The wyvern waits on pet-aware building blocks.
-----------------------------------
local b = require('modules/cardian/lua/pawn/brains/_blocks')

local spell  = xi.magic.spell
local family = xi.magic.spellFamily

return
{
    name = 'DRG',

    -- Weapon skills: as soon as TP allows, strongest known
    tp = { ai.tp.ASAP, ai.s.HIGHEST },

    gambits = b.list({
        b.jaSelf(xi.effect.ANCIENT_CIRCLE, xi.ja.ANCIENT_CIRCLE),
        b.jaTarget(xi.ja.JUMP),
        b.jaTarget(xi.ja.HIGH_JUMP),
        b.defaults(),
    }),
}
