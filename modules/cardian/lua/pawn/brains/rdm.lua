-----------------------------------
-- Cardian pawn brain: RDM
-- Support mage: cures in a pinch, keeps Refresh/Haste/Phalanx up, enfeebles, converts when dry, nukes with plenty of MP.
-----------------------------------
local b = require('modules/cardian/lua/pawn/brains/_blocks')

local spell  = xi.magic.spell
local family = xi.magic.spellFamily

return
{
    name = 'RDM',

    -- Weapon skills: as soon as TP allows, strongest known
    tp = { ai.tp.ASAP, ai.s.HIGHEST },

    gambits = b.list({
        b.cureParty(40),
        b.wakeSleepers,
        b.naSpells,

        b.selfBuff(xi.effect.REFRESH, family.REFRESH),
        b.buffOn(ai.t.CASTER, xi.effect.REFRESH, family.REFRESH),
        b.buffOn(ai.t.MELEE,  xi.effect.HASTE,   family.HASTE),
        b.selfBuff(xi.effect.PHALANX, family.PHALANX),
        b.selfBuffSpell(xi.effect.STONESKIN, spell.STONESKIN),
        b.selfBuffSpell(xi.effect.BLINK,     spell.BLINK),
        b.partyBuff(xi.effect.PROTECT, family.PROTECT),
        b.partyBuff(xi.effect.SHELL,   family.SHELL),
        { ai.t.SELF, { ai.c.TIMER, 180 }, { ai.r.MA, ai.s.EN_MOB_WEAKNESS, 0 } },

        b.debuff(xi.effect.DIA,       family.DIA,      60),
        b.debuff(xi.effect.PARALYSIS, family.PARALYZE, 60),
        b.debuff(xi.effect.SLOW,      family.SLOW,     60),
        b.debuff(xi.effect.BLINDNESS, family.BLIND,    60),
        b.debuff(xi.effect.WEIGHT,    family.GRAVITY,  60),

        { ai.t.SELF, { { ai.c.MPP_LT, 15 }, { ai.c.HPP_GTE, 60 } }, { ai.r.JA, ai.s.SPECIFIC, xi.ja.CONVERT } },
        b.nuke(50),
        b.defaults(),
    }),
}
