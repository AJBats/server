-----------------------------------
-- Cardian pawn brain: DRK
-- Dark knight: stuns and bashes casters, Last Resort behind a tank, drains and absorbs.
-----------------------------------
local b = require('modules/cardian/lua/pawn/brains/_blocks')

local spell  = xi.magic.spell
local family = xi.magic.spellFamily

return
{
    name = 'DRK',

    -- Weapon skills: as soon as TP allows, strongest known
    tp = { ai.tp.ASAP, ai.s.HIGHEST },

    gambits = b.list({
        b.stunCaster,
        { ai.t.TARGET, { ai.c.CASTING_MA, 0 }, { ai.r.JA, ai.s.SPECIFIC, xi.ja.WEAPON_BASH } },
        { ai.t.SELF, { { ai.c.PT_HAS_TANK, 0 }, { ai.c.NOT_STATUS, xi.effect.LAST_RESORT } }, { ai.r.JA, ai.s.SPECIFIC, xi.ja.LAST_RESORT } },
        b.jaSelf(xi.effect.ARCANE_CIRCLE, xi.ja.ARCANE_CIRCLE),
        { ai.t.SELF, { ai.c.HPP_LT, 60 }, { ai.r.MA, ai.s.HIGHEST, family.DRAIN } },
        { ai.t.SELF, { ai.c.MPP_LT, 50 }, { ai.r.MA, ai.s.HIGHEST, family.ASPIR } },
        b.debuff(xi.effect.BIO, family.BIO, 60),
        { ai.t.TARGET, { ai.c.TIMER, 60 }, { ai.r.MA, ai.s.HIGHEST, family.ABSORB } },
        b.defaults(),
    }),
}
