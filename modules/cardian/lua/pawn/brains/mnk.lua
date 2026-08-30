-----------------------------------
-- Cardian pawn brain: MNK
-- Front-line puncher: stance buffs, Chakra when hurt, Counterstance when nobody tanks.
-----------------------------------
local b = require('modules/cardian/lua/pawn/brains/_blocks')

local spell  = xi.magic.spell
local family = xi.magic.spellFamily

return
{
    name = 'MNK',

    -- Weapon skills: as soon as TP allows, strongest known
    tp = { ai.tp.ASAP, ai.s.HIGHEST },

    gambits = b.list({
        b.jaSelf(xi.effect.FOCUS, xi.ja.FOCUS),
        b.jaSelf(xi.effect.DODGE, xi.ja.DODGE),
        { ai.t.SELF, { { ai.c.NOT_PT_HAS_TANK, 0 }, { ai.c.NOT_STATUS, xi.effect.COUNTERSTANCE } }, { ai.r.JA, ai.s.SPECIFIC, xi.ja.COUNTERSTANCE } },
        { ai.t.SELF, { ai.c.HPP_LT, 50 }, { ai.r.JA, ai.s.SPECIFIC, xi.ja.CHAKRA } },
        { ai.t.SELF, { ai.c.NOT_STATUS, xi.effect.BOOST }, { ai.r.JA, ai.s.SPECIFIC, xi.ja.BOOST }, 15 },
        b.provokeToSave(ai.t.MASTER, 40),
    }),
}
