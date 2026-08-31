-----------------------------------
-- Cardian pawn brain: PLD
-- Tank: holds hate with Provoke and Flash, bashes casters, Sentinel when low, cures itself and the party.
-----------------------------------
local b = require('modules/cardian/lua/pawn/brains/_blocks')

local spell  = xi.magic.spell
local family = xi.magic.spellFamily

return
{
    name = 'PLD',

    -- Weapon skills: as soon as TP allows, strongest known
    tp = { ai.tp.ASAP, ai.s.HIGHEST },

    gambits = b.list({
        b.provokeIfLoose,
        b.debuffSpell(xi.effect.FLASH, spell.FLASH, 45),
        { ai.t.TARGET, { ai.c.CASTING_MA, 0 }, { ai.r.JA, ai.s.SPECIFIC, xi.ja.SHIELD_BASH } },
        { ai.t.SELF, { ai.c.HPP_LT, 40 }, { ai.r.JA, ai.s.SPECIFIC, xi.ja.SENTINEL } },
        { ai.t.SELF, { { ai.c.HPP_LT, 60 }, { ai.c.NOT_STATUS, xi.effect.RAMPART } }, { ai.r.JA, ai.s.SPECIFIC, xi.ja.RAMPART } },
        b.cureSelf(50),
        b.cureParty(35),
        b.selfBuff(xi.effect.PROTECT, family.PROTECT),
        b.selfBuff(xi.effect.SHELL,   family.SHELL),
        b.provokeToSave(ai.t.MASTER, 50),
    }),
}
