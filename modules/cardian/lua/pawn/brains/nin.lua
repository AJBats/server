-----------------------------------
-- Cardian pawn brain: NIN
-- Ninja: shadows first, Yonin while holding hate and Innin otherwise, ninjutsu debuffs (needs tools).
-----------------------------------
local b = require('modules/cardian/lua/pawn/brains/_blocks')

local spell  = xi.magic.spell
local family = xi.magic.spellFamily

return
{
    name = 'NIN',

    -- Weapon skills: as soon as TP allows, strongest known
    tp = { ai.tp.ASAP, ai.s.HIGHEST },

    gambits = b.list({
        b.selfBuff(xi.effect.COPY_IMAGE, family.UTSUSEMI),
        { ai.t.TARGET, { { ai.c.HAS_TOP_ENMITY, 0 },     { ai.c.NOT_STATUS, xi.effect.YONIN } }, { ai.r.JA, ai.s.SPECIFIC, xi.ja.YONIN }, 60 },
        { ai.t.TARGET, { { ai.c.NOT_HAS_TOP_ENMITY, 0 }, { ai.c.NOT_STATUS, xi.effect.INNIN } }, { ai.r.JA, ai.s.SPECIFIC, xi.ja.INNIN }, 60 },
        b.provokeIfLoose,
        b.debuff(xi.effect.SLOW,      family.HOJO,     60),
        b.debuff(xi.effect.PARALYSIS, family.JUBAKU,   60),
        b.debuff(xi.effect.BLINDNESS, family.KURAYAMI, 60),
    }),
}
