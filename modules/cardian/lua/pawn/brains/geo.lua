-----------------------------------
-- Cardian pawn brain: GEO
-- Geomancer: an Indi bubble suited to the player, a Geo debuff on the target, cures and nukes to fill.
-----------------------------------
local b = require('modules/cardian/lua/pawn/brains/_blocks')

local spell  = xi.magic.spell
local family = xi.magic.spellFamily

return
{
    name = 'GEO',

    -- Weapon skills: as soon as TP allows, strongest known
    tp = { ai.tp.ASAP, ai.s.HIGHEST },

    gambits = b.list({
        { ai.t.SELF,   { ai.c.TIMER, 300 }, { ai.r.MA, ai.s.BEST_INDI, 0 } },
        { ai.t.TARGET, { ai.c.TIMER, 120 }, { ai.r.MA, ai.s.HIGHEST, family.GEO_DEBUFF } },
        b.cureParty(40),
        b.nuke(50),
        b.defaults(),
    }),
}
