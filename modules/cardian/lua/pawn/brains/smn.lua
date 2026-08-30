-----------------------------------
-- Cardian pawn brain: SMN
-- Avatars wait on pet-aware building blocks (summon, assault, blood pacts, release). Cures via a healing support job.
-----------------------------------
local b = require('modules/cardian/lua/pawn/brains/_blocks')

local spell  = xi.magic.spell
local family = xi.magic.spellFamily

return
{
    name = 'SMN',

    -- Weapon skills: as soon as TP allows, strongest known
    tp = { ai.tp.ASAP, ai.s.HIGHEST },

    gambits = b.list({
        b.cureParty(40),
    }),
}
