-----------------------------------
-- Cardian pawn brain: DNC
-- Dancer: waltzes the hurt, keeps a samba going, steps with spare TP.
-----------------------------------
local b = require('modules/cardian/lua/pawn/brains/_blocks')

local spell  = xi.magic.spell
local family = xi.magic.spellFamily

return
{
    name = 'DNC',

    -- Weapon skills: hold some TP for waltzes
    tp = { ai.tp.RANDOM, ai.s.HIGHEST, 1500 },

    gambits = b.list({
        { ai.t.PARTY, { ai.c.HPP_LT, 50 }, { ai.r.JA, ai.s.HIGHEST_WALTZ, 0 } },
        { ai.t.SELF,  { ai.c.NO_SAMBA, 0 }, { ai.r.JA, ai.s.BEST_SAMBA, 0 } },
        { ai.t.TARGET, { ai.c.TP_GTE, 300 }, { ai.r.JA, ai.s.SPECIFIC, xi.ja.QUICKSTEP }, 30 },
        b.defaults(),
    }),
}
