-----------------------------------
-- Cardian pawn brain: BST
-- Melee for now. Charm, Reward and Sic wait on pet-aware building blocks.
-----------------------------------
local b = require('modules/cardian/lua/pawn/brains/_blocks')

local spell  = xi.magic.spell
local family = xi.magic.spellFamily

return
{
    name = 'BST',

    -- Weapon skills: as soon as TP allows, strongest known
    tp = { ai.tp.ASAP, ai.s.HIGHEST },

    gambits = b.list({
        b.provokeToSave(ai.t.MASTER, 40),
        b.defaults(),
    }),
}
