-----------------------------------
-- Cardian pawn brain: RUN
-- Rune fencer: runes of the day, Vallation once runed, Swordplay, Lunge into light/dark chains, Flash and Provoke to hold hate.
-----------------------------------
local b = require('modules/cardian/lua/pawn/brains/_blocks')

local spell  = xi.magic.spell
local family = xi.magic.spellFamily

return
{
    name = 'RUN',

    -- Weapon skills: as soon as TP allows, strongest known
    tp = { ai.tp.ASAP, ai.s.HIGHEST },

    gambits = b.list({
        { ai.t.SELF, { ai.c.NO_MAX_RUNE, 0 }, { ai.r.JA, ai.s.RUNE_DAY, 0 } },
        { ai.t.SELF, { { ai.c.HAS_RUNES, 0 }, { ai.c.NOT_STATUS, xi.effect.VALLATION } }, { ai.r.JA, ai.s.SPECIFIC, xi.ja.VALLATION } },
        b.jaSelf(xi.effect.SWORDPLAY, xi.ja.SWORDPLAY),
        { ai.t.TARGET, { ai.c.LUNGE_MB_AVAILABLE, 0 }, { ai.r.JA, ai.s.SPECIFIC, xi.ja.LUNGE } },
        b.debuffSpell(xi.effect.FLASH, spell.FLASH, 45),
        b.provokeIfLoose,
    }),
}
