-----------------------------------
-- Cardian pawn brain: COR
-- Corsair: keeps two rolls up, then shoots.
-----------------------------------
local b = require('modules/cardian/lua/pawn/brains/_blocks')

local spell  = xi.magic.spell
local family = xi.magic.spellFamily

return
{
    name = 'COR',

    -- Weapon skills: as soon as TP allows, strongest known
    tp = { ai.tp.ASAP, ai.s.HIGHEST },

    gambits = b.list({
        { ai.t.SELF, { ai.c.NOT_STATUS, xi.effect.CHAOS_ROLL },    { ai.r.JA, ai.s.SPECIFIC, xi.ja.CHAOS_ROLL },    60 },
        { ai.t.SELF, { ai.c.NOT_STATUS, xi.effect.CORSAIRS_ROLL }, { ai.r.JA, ai.s.SPECIFIC, xi.ja.CORSAIRS_ROLL }, 60 },
        b.shoot,
        b.defaults(),
    }),
}
