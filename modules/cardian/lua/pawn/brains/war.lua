-----------------------------------
-- Cardian pawn brain: WAR
-- Volker's pattern: damage dealer when the party has a tank, tank otherwise.
-----------------------------------
local b = require('modules/cardian/lua/pawn/brains/_blocks')

local spell  = xi.magic.spell
local family = xi.magic.spellFamily

return
{
    name = 'WAR',

    -- Weapon skills: close whatever skillchain the party opens; with nothing
    -- to close, open at 2000 TP rather than sit on it
    tp = { ai.tp.CLOSER_UNTIL_TP, ai.s.HIGHEST, 2000 },

    gambits = b.list({
        -- Damage-dealer stance when someone else tanks
        { ai.t.SELF, { { ai.c.PT_HAS_TANK, 0 }, { ai.c.NOT_STATUS, xi.effect.BERSERK } },   { ai.r.JA, ai.s.SPECIFIC, xi.ja.BERSERK } },
        { ai.t.SELF, { { ai.c.PT_HAS_TANK, 0 }, { ai.c.NOT_STATUS, xi.effect.AGGRESSOR } }, { ai.r.JA, ai.s.SPECIFIC, xi.ja.AGGRESSOR } },
        b.provokeToSave(ai.t.TANK, 50),

        -- Tank stance otherwise
        { ai.t.TARGET, { { ai.c.NOT_PT_HAS_TANK, 0 }, { ai.c.NOT_HAS_TOP_ENMITY, 0 } },     { ai.r.JA, ai.s.SPECIFIC, xi.ja.PROVOKE } },
        { ai.t.SELF,   { { ai.c.NOT_PT_HAS_TANK, 0 }, { ai.c.NOT_STATUS, xi.effect.DEFENDER } },    { ai.r.JA, ai.s.SPECIFIC, xi.ja.DEFENDER } },
        { ai.t.SELF,   { { ai.c.NOT_PT_HAS_TANK, 0 }, { ai.c.NOT_STATUS, xi.effect.RETALIATION } }, { ai.r.JA, ai.s.SPECIFIC, xi.ja.RETALIATION } },

        -- Always: peel the mob off the player, shout when fresh
        b.provokeToSave(ai.t.MASTER, 50),
        b.jaSelf(xi.effect.WARCRY, xi.ja.WARCRY),
    }),
}
