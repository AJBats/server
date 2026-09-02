-----------------------------------
-- Cardian pawn brain: BRD
-- Songs are sung on the bard and reach the party around it: melee songs, Ballad for casters, Paeon when hurt, Elegy/Requiem on the target.
-----------------------------------
local b = require('modules/cardian/lua/pawn/brains/_blocks')

local spell  = xi.magic.spell
local family = xi.magic.spellFamily

return
{
    name = 'BRD',

    -- Weapon skills: as soon as TP allows, strongest known
    tp = { ai.tp.ASAP, ai.s.HIGHEST },

    gambits = b.list({
        b.selfBuff(xi.effect.MINUET,   family.VALOR_MINUET),
        b.selfBuff(xi.effect.MADRIGAL, family.MADRIGAL),
        b.buffOn(ai.t.CASTER, xi.effect.BALLAD, family.MAGES_BALLAD),
        { ai.t.PARTY, { ai.c.HPP_LT, 60 }, { ai.r.MA, ai.s.HIGHEST, family.ARMYS_PAEON } },
        b.debuff(xi.effect.ELEGY,   family.ELEGY,       60),
        b.debuff(xi.effect.REQUIEM, family.FOE_REQUIEM, 60),
        b.cureParty(30),
        b.defaults(),
    }),
}
