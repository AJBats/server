-----------------------------------
-- Cardian pawn brain: SCH
-- Scholar in Light Arts: healer-lite with Regen and protective buffs, Sublimation, a nuke when MP is plentiful.
-----------------------------------
local b = require('modules/cardian/lua/pawn/brains/_blocks')

local spell  = xi.magic.spell
local family = xi.magic.spellFamily

return
{
    name = 'SCH',

    -- Weapon skills: as soon as TP allows, strongest known
    tp = { ai.tp.ASAP, ai.s.HIGHEST },

    gambits = b.list({
        b.jaSelf(xi.effect.LIGHT_ARTS, xi.ja.LIGHT_ARTS),
        b.cureParty(40),
        b.wakeSleepers,
        b.naSpells,
        b.partyBuff(xi.effect.PROTECT, family.PROTECT),
        b.partyBuff(xi.effect.SHELL,   family.SHELL),
        b.buffOn(ai.t.MELEE, xi.effect.REGEN, family.REGEN),
        b.selfBuffSpell(xi.effect.STONESKIN, spell.STONESKIN),
        b.selfBuffSpell(xi.effect.BLINK,     spell.BLINK),
        b.jaSelf(xi.effect.SUBLIMATION_ACTIVATED, xi.ja.SUBLIMATION),
        b.debuff(xi.effect.DIA, family.DIA, 60),
        b.nuke(60),
    }),
}
