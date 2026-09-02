-----------------------------------
-- Cardian pawn brain: BLM
-- Nuker: bursts skillchains, stuns casters, nukes the target's weak element with an MP reserve.
-----------------------------------
local b = require('modules/cardian/lua/pawn/brains/_blocks')

local spell  = xi.magic.spell
local family = xi.magic.spellFamily

return
{
    name = 'BLM',

    -- Weapon skills: as soon as TP allows, strongest known
    tp = { ai.tp.ASAP, ai.s.HIGHEST },

    gambits = b.list({
        b.magicBurst,
        b.stunCaster,
        b.selfBuffSpell(xi.effect.STONESKIN, spell.STONESKIN),
        b.selfBuffSpell(xi.effect.BLINK,     spell.BLINK),
        b.debuff(xi.effect.BIO,       family.BIO,    60),
        b.debuff(xi.effect.BLINDNESS, family.BLIND,  60),
        b.debuff(xi.effect.POISON,    family.POISON, 60),
        b.nuke(30),
        b.defaults(),
    }),
}
