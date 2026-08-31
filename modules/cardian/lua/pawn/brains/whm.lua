-----------------------------------
-- Cardian pawn brain: WHM
-- Kupipi's pattern: triage cures, wake and cleanse, protect the party, keep melee hasted, light debuffs.
-----------------------------------
local b = require('modules/cardian/lua/pawn/brains/_blocks')

local spell  = xi.magic.spell
local family = xi.magic.spellFamily

return
{
    name = 'WHM',

    -- Weapon skills: as soon as TP allows, strongest known
    tp = { ai.tp.ASAP, ai.s.HIGHEST },

    gambits = b.list({
        -- Emergencies first
        b.cureParty(25),
        b.wakeSleepers,
        b.cureParty(50),
        b.raise,

        -- Cleanse
        b.naSpells,
        b.erase,

        -- Protect the party (the -ra versions take over once learned)
        b.partyBuff(xi.effect.PROTECT, family.PROTECTRA),
        b.partyBuff(xi.effect.SHELL,   family.SHELLRA),
        b.partyBuff(xi.effect.PROTECT, family.PROTECT),
        b.partyBuff(xi.effect.SHELL,   family.SHELL),

        -- Top up, then upkeep
        b.cureParty(75),
        b.buffOn(ai.t.MELEE, xi.effect.HASTE, family.HASTE),
        b.buffOn(ai.t.MELEE, xi.effect.REGEN, family.REGEN),
        b.selfBuffSpell(xi.effect.STONESKIN, spell.STONESKIN),
        b.selfBuffSpell(xi.effect.BLINK,     spell.BLINK),

        -- Debuffs when there is nothing better to do
        b.debuff(xi.effect.PARALYSIS, family.PARALYZE, 60),
        b.debuff(xi.effect.SLOW,      family.SLOW,     60),
        b.debuff(xi.effect.DIA,       family.DIA,      60),
        b.debuffSpell(xi.effect.FLASH, spell.FLASH,    45),
    }),
}
