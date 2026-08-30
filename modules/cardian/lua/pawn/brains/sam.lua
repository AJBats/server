-----------------------------------
-- Cardian pawn brain: SAM
-- Samurai: Hasso and Third Eye up, Meditate for TP. Skillchain roles come with the coordination phase.
-----------------------------------
local b = require('modules/cardian/lua/pawn/brains/_blocks')

local spell  = xi.magic.spell
local family = xi.magic.spellFamily

return
{
    name = 'SAM',

    -- Weapon skills: as soon as TP allows, strongest known
    tp = { ai.tp.ASAP, ai.s.HIGHEST },

    gambits = b.list({
        b.jaSelf(xi.effect.HASSO,          xi.ja.HASSO),
        b.jaSelf(xi.effect.THIRD_EYE,      xi.ja.THIRD_EYE),
        { ai.t.SELF, { ai.c.TP_LT, 1000 }, { ai.r.JA, ai.s.SPECIFIC, xi.ja.MEDITATE } },
        b.jaSelf(xi.effect.WARDING_CIRCLE, xi.ja.WARDING_CIRCLE),
    }),
}
