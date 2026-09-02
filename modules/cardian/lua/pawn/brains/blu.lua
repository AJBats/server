-----------------------------------
-- Cardian pawn brain: BLU
-- Blue mage: bursts skillchains and nukes with its set spells, keeping an MP reserve.
-----------------------------------
local b = require('modules/cardian/lua/pawn/brains/_blocks')

local spell  = xi.magic.spell
local family = xi.magic.spellFamily

return
{
    name = 'BLU',

    -- Weapon skills: as soon as TP allows, strongest known
    tp = { ai.tp.ASAP, ai.s.HIGHEST },

    gambits = b.list({
        b.magicBurst,
        b.cureParty(40),
        b.nuke(30),
        b.defaults(),
    }),
}
