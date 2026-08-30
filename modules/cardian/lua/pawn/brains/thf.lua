-----------------------------------
-- Cardian pawn brain: THF
-- Melee with sticky fingers. Sneak/Trick Attack wait on rear positioning.
-----------------------------------
local b = require('modules/cardian/lua/pawn/brains/_blocks')

local spell  = xi.magic.spell
local family = xi.magic.spellFamily

return
{
    name = 'THF',

    -- Weapon skills: as soon as TP allows, strongest known
    tp = { ai.tp.ASAP, ai.s.HIGHEST },

    gambits = b.list({
        { ai.t.TARGET, { ai.c.ALWAYS, 0 }, { ai.r.JA, ai.s.SPECIFIC, xi.ja.STEAL }, 300 },
        b.provokeToSave(ai.t.MASTER, 40),
    }),
}
