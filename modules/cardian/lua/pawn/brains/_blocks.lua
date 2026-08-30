-----------------------------------
-- Cardian pawn brains: building blocks
--
-- Each block is one gambit row { target, conditions, actions, retry } in
-- the trust ai.* vocabulary, or a list of rows. Job brains compose them;
-- a future in-game gambit editor offers the same blocks to the player.
-- Rows for spells or abilities the pawn lacks are inert until it learns
-- them, so blocks are written level-agnostic.
-----------------------------------
local b = {}

local spell  = xi.magic.spell
local family = xi.magic.spellFamily

-- Flatten rows and lists of rows into one ordered gambit list
b.list = function(items)
    local out = {}
    for _, item in ipairs(items) do
        if type(item[1]) == 'number' then
            table.insert(out, item)
        else
            for _, row in ipairs(item) do
                table.insert(out, row)
            end
        end
    end
    return out
end

-----------------------------------
-- Healing
-----------------------------------
b.cureParty = function(hpp)
    return { ai.t.PARTY, { ai.c.HPP_LT, hpp }, { ai.r.MA, ai.s.HIGHEST, family.CURE } }
end

b.cureSelf = function(hpp)
    return { ai.t.SELF, { ai.c.HPP_LT, hpp }, { ai.r.MA, ai.s.HIGHEST, family.CURE } }
end

b.raise = { ai.t.PARTY_DEAD, { ai.c.ALWAYS, 0 }, { ai.r.MA, ai.s.HIGHEST, family.RAISE } }

-----------------------------------
-- Status removal
-----------------------------------
b.wakeSleepers =
{
    { ai.t.PARTY, { ai.c.STATUS, xi.effect.SLEEP_I },  { ai.r.MA, ai.s.SPECIFIC, spell.CURE } },
    { ai.t.PARTY, { ai.c.STATUS, xi.effect.SLEEP_II }, { ai.r.MA, ai.s.SPECIFIC, spell.CURE } },
}

b.naSpells =
{
    { ai.t.PARTY, { ai.c.STATUS, xi.effect.POISON },        { ai.r.MA, ai.s.SPECIFIC, spell.POISONA } },
    { ai.t.PARTY, { ai.c.STATUS, xi.effect.PARALYSIS },     { ai.r.MA, ai.s.SPECIFIC, spell.PARALYNA } },
    { ai.t.PARTY, { ai.c.STATUS, xi.effect.BLINDNESS },     { ai.r.MA, ai.s.SPECIFIC, spell.BLINDNA } },
    { ai.t.PARTY, { ai.c.STATUS, xi.effect.SILENCE },       { ai.r.MA, ai.s.SPECIFIC, spell.SILENA } },
    { ai.t.PARTY, { ai.c.STATUS, xi.effect.PETRIFICATION }, { ai.r.MA, ai.s.SPECIFIC, spell.STONA } },
    { ai.t.PARTY, { ai.c.STATUS, xi.effect.DISEASE },       { ai.r.MA, ai.s.SPECIFIC, spell.VIRUNA } },
    { ai.t.PARTY, { ai.c.STATUS, xi.effect.CURSE_I },       { ai.r.MA, ai.s.SPECIFIC, spell.CURSNA } },
}

b.erase = { ai.t.PARTY, { ai.c.STATUS_FLAG, xi.effectFlag.ERASABLE }, { ai.r.MA, ai.s.SPECIFIC, spell.ERASE } }

-----------------------------------
-- Buffs
-----------------------------------
b.selfBuff = function(effect, fam)
    return { ai.t.SELF, { ai.c.NOT_STATUS, effect }, { ai.r.MA, ai.s.HIGHEST, fam } }
end

b.selfBuffSpell = function(effect, id)
    return { ai.t.SELF, { ai.c.NOT_STATUS, effect }, { ai.r.MA, ai.s.SPECIFIC, id } }
end

b.partyBuff = function(effect, fam)
    return { ai.t.PARTY, { ai.c.NOT_STATUS, effect }, { ai.r.MA, ai.s.HIGHEST, fam } }
end

b.buffOn = function(target, effect, fam)
    return { target, { ai.c.NOT_STATUS, effect }, { ai.r.MA, ai.s.HIGHEST, fam } }
end

-----------------------------------
-- Job abilities
-----------------------------------
b.jaSelf = function(effect, ja)
    return { ai.t.SELF, { ai.c.NOT_STATUS, effect }, { ai.r.JA, ai.s.SPECIFIC, ja } }
end

b.jaTarget = function(ja, retry)
    return { ai.t.TARGET, { ai.c.ALWAYS, 0 }, { ai.r.JA, ai.s.SPECIFIC, ja }, retry }
end

b.jaWhen = function(target, conditions, ja, retry)
    return { target, conditions, { ai.r.JA, ai.s.SPECIFIC, ja }, retry }
end

-----------------------------------
-- Debuffs
-----------------------------------
b.debuff = function(effect, fam, retry)
    return { ai.t.TARGET, { ai.c.NOT_STATUS, effect }, { ai.r.MA, ai.s.HIGHEST, fam }, retry or 60 }
end

b.debuffSpell = function(effect, id, retry)
    return { ai.t.TARGET, { ai.c.NOT_STATUS, effect }, { ai.r.MA, ai.s.SPECIFIC, id }, retry or 60 }
end

-----------------------------------
-- Nuking
-----------------------------------
-- Best known nuke against the target's weakest element, keeping minMpp% MP
b.nuke = function(minMpp)
    return { ai.t.TARGET, { ai.c.MPP_GTE, minMpp or 0 }, { ai.r.MA, ai.s.BEST_AGAINST_TARGET, 0 } }
end

b.magicBurst = { ai.t.TARGET, { ai.c.MB_AVAILABLE, 0 }, { ai.r.MA, ai.s.MB_ELEMENT, 0 } }

b.stunCaster = { ai.t.TARGET, { ai.c.CASTING_MA, 0 }, { ai.r.MA, ai.s.SPECIFIC, spell.STUN } }

-----------------------------------
-- Enmity
-----------------------------------
-- Provoke whenever someone else holds hate
b.provokeIfLoose = { ai.t.TARGET, { ai.c.NOT_HAS_TOP_ENMITY, 0 }, { ai.r.JA, ai.s.SPECIFIC, xi.ja.PROVOKE } }

-- Provoke off a member who is getting low
b.provokeToSave = function(target, hpp)
    return { target, { ai.c.HPP_LT, hpp }, { ai.r.JA, ai.s.SPECIFIC, xi.ja.PROVOKE } }
end

-----------------------------------
-- Ranged
-----------------------------------
b.shoot = { ai.t.TARGET, { ai.c.ALWAYS, 0 }, { ai.r.RATTACK, 0, 0 } }

return b
