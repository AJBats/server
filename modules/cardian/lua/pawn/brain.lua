-----------------------------------
-- Cardian pawn brains
--
-- A brain is data: an ordered list of gambit rows in the trust ai.*
-- vocabulary, { target, conditions, actions, retry }, plus TP-skill
-- settings. First matching row wins each think, exactly like FF12.
--
-- xi.pawn.brain.select() is the seam for player-authored gambit sets: today
-- it returns the job default from brains/<job>.lua, later it will prefer a
-- per-pawn set persisted in a Cardian-owned table. Files are re-read from
-- disk on every load so a brain edit applies on the next !pawnbrain.
-----------------------------------
require('modules/module_utils')
-----------------------------------
xi = xi or {}
xi.pawn = xi.pawn or {}
xi.pawn.brain = xi.pawn.brain or {}

-- Cardian-only gambit vocabulary on top of the trust ai.* tables, published
-- by the pawn module (pawn_module.cpp) from the interpreter's definitions:
-- xi.pawn.r.BEHAVIOR and xi.pawn.behavior.*. A BEHAVIOR action flips a
-- controller switch instead of acting: the row is applied while its
-- conditions hold and never consumes the think, so
-- { ai.t.SELF, { ai.c.HPP_LT, 50 }, { xi.pawn.r.BEHAVIOR, xi.pawn.behavior.AVOID_AGGRO, 1 } }
-- reads "avoid aggro while under half health".

local brainDir  = 'modules/cardian/lua/pawn/brains/'
local blocksPath = brainDir .. '_blocks'

local jobFiles =
{
    [xi.job.WAR] = 'war',
    [xi.job.MNK] = 'mnk',
    [xi.job.WHM] = 'whm',
    [xi.job.BLM] = 'blm',
    [xi.job.RDM] = 'rdm',
    [xi.job.THF] = 'thf',
    [xi.job.PLD] = 'pld',
    [xi.job.DRK] = 'drk',
    [xi.job.BST] = 'bst',
    [xi.job.BRD] = 'brd',
    [xi.job.RNG] = 'rng',
    [xi.job.SAM] = 'sam',
    [xi.job.NIN] = 'nin',
    [xi.job.DRG] = 'drg',
    [xi.job.SMN] = 'smn',
    [xi.job.BLU] = 'blu',
    [xi.job.COR] = 'cor',
    [xi.job.PUP] = 'pup',
    [xi.job.DNC] = 'dnc',
    [xi.job.SCH] = 'sch',
    [xi.job.GEO] = 'geo',
    [xi.job.RUN] = 'run',
}

local function requireFresh(path)
    package.loaded[path] = nil
    local ok, result = pcall(require, path)
    if not ok then
        printf('pawn: brain file %s failed: %s', path, tostring(result))
        return nil
    end
    return result
end

-- The gambit set this pawn should run
xi.pawn.brain.select = function(pawn)
    local file = jobFiles[pawn:getMainJob()]
    if file == nil then
        return nil
    end

    package.loaded[blocksPath] = nil
    return requireFresh(brainDir .. file)
end

-- Replace the pawn's gambits with its selected set. Returns the row count.
xi.pawn.brain.load = function(pawn)
    pawn:pawnClearGambits()

    local brain = xi.pawn.brain.select(pawn)
    if brain == nil then
        printf('pawn: no brain for %s (job %d)', pawn:getName(), pawn:getMainJob())
        return 0
    end

    if brain.tp then
        pawn:pawnSetTPSkillSettings(brain.tp[1], brain.tp[2], brain.tp[3])
    end

    local count = 0
    for _, row in ipairs(brain.gambits or {}) do
        if pawn:pawnAddGambit(row[1], row[2], row[3], row[4]) ~= '' then
            count = count + 1
        end
    end

    printf('pawn: brain %s loaded for %s (%d gambits)', brain.name or '?', pawn:getName(), count)
    return count
end
