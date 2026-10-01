-----------------------------------
-- Cardian: the exp formula, fair within a small level spread
-- (RESEARCH §15; ROADMAP "The road to subjob" item 3)
--
-- Upstream grades a kill at the highest level in the party and scales
-- every lower member down by TNL, so a mixed party pays its lowest
-- members a fraction of what level sync would. Here, while the party's
-- spread -- highest level less lowest, among the members the game counts
-- for the kill -- is within cardian.EXP_AVERAGE_SPREAD, every member is
-- paid as if the whole party stood at its average level, rounded down:
-- one number for all. From there to cardian.EXP_CLASSIC_SPREAD each
-- member's reward slides linearly toward upstream's figure, which it
-- reaches at that spread and keeps beyond it.
--
-- Both figures are upstream's own function's answers: once as the party
-- is, once with the base read at the average and no cut. The rule only
-- chooses between them. Lua, but for one marked line of C++; the base
-- table is read live, so prod's era table and dev's modern one each keep
-- their numbers.
--
-- Exp chains are the one-level party's too: the mob is judged at the
-- average for the chain gate, and the C++ that counts chains follows the
-- formula's judgement (one marked line in charutils::AddExperiencePoints).
-- Between the spreads the chain is the one-level figure's: it counts, and
-- the blend carries its share of it; at the classic spread the chain is
-- upstream's again, which may be none. One thing stays upstream's, judged
-- at the highest level in C++ before Lua is asked: the Too Weak gate, a
-- mob too weak for the top member pays nobody. Level sync needs nothing:
-- synced members stand at the sync level, and a member below it makes a
-- small spread like any other.
--
-- The rule may fail; the grant may not. Whatever goes wrong here,
-- upstream's formula pays.
-----------------------------------
require('modules/module_utils')
-----------------------------------
local m = Module:new('cardian_exp_spread')

-- Everything the rule keeps lives on this table, not in this chunk: a
-- re-run of the file (the map's file watcher, on an edit while it runs)
-- makes a new chunk and must find the thresholds and the records where
-- the old one left them
xi = xi or {}
xi.cardian = xi.cardian or {}
xi.cardian.exp = xi.cardian.exp or {}
local exp = xi.cardian.exp

-- The numbers behind each character's last grant, by id, one small table
-- per character ever paid: `paid`, the figure the formula paid; `granted`,
-- that figure after map.EXP_RATE, which is what the character receives
-- (a world body's census cap may still trim it, in C++); `classic`,
-- upstream's own figure for the same kill; `averaged`, the party as one
-- level's; the `spread` and `average` they came from; `t`, the blend (0
-- the party as one level, 1 upstream's formula); `witnessed`, whether a
-- real player was in the party; and the `mob` and the second (`at`). A
-- kill that pays nobody writes nothing.
exp.last = exp.last or {}

-- How often the rule gave way to upstream's formula on an error
exp.failures = exp.failures or 0

-- The difficulty thresholds as the profile hands them to C++ at start
-- (scripts/globals/exp_difficulty_curve.lua on dev, the era module's own
-- on prod), kept so a mob can be judged at the party's average level,
-- which no binding does. The hand-over is wrapped once, before either
-- caller runs: modules load inside luautils::init, the exp table after
-- it. Until a load, the chain gate keeps upstream's judgement
if not exp.wrapped and type(LoadExpDifficultyCurves) == 'function' then
    local handOver = LoadExpDifficultyCurves
    LoadExpDifficultyCurves = function(expToDifficulty, iepLevel, iepMinExp)
        local thresholds = {}
        for threshold, difficulty in pairs(expToDifficulty) do
            thresholds[#thresholds + 1] = { exp = threshold, difficulty = difficulty }
        end

        table.sort(thresholds, function(a, b)
            return a.exp > b.exp
        end)

        exp.curve = { thresholds = thresholds, iepLevel = iepLevel, iepMinExp = iepMinExp }
        return handOver(expToDifficulty, iepLevel, iepMinExp)
    end

    exp.wrapped = true
end

-- Upstream's judgement (charutils::CheckMob) of a base figure against a
-- mob of this level; nil while the thresholds are unknown
exp.difficultyOf = function(baseExp, mobLevel)
    local curve = exp.curve
    if curve == nil then
        return nil
    end

    if baseExp == 0 then
        return xi.mobDifficulty.TOO_WEAK
    end

    for _, entry in ipairs(curve.thresholds) do
        if baseExp >= entry.exp then
            return entry.difficulty
        end
    end

    if baseExp >= curve.iepMinExp and mobLevel >= curve.iepLevel then
        return xi.mobDifficulty.INCREDIBLY_EASY_PREY
    end

    return xi.mobDifficulty.TOO_WEAK
end

-- A member's level as the game reads it for exp (charutils::GetExpLevel):
-- her level, the sync level under level sync, and her job's true level
-- under a level restriction that says exp is paid by it
exp.levelOf = function(entity)
    local restriction = entity:getStatusEffect(xi.effect.LEVEL_RESTRICTION)
    if restriction and restriction:getSubPower() == 1 then
        return entity:getJobLevel(entity:getMainJob())
    end

    return entity:getMainLvl()
end

-- Upstream's lookup (charutils::GetBaseExp): the base table's row is the
-- mob's level less the member's, its column the member's five-level
-- bracket
exp.baseAt = function(level, mobLevel)
    level = math.min(level, 99)
    if level <= 0 then
        return 0
    end

    local row = xi.data.experiencePoints.baseTable[math.max(-44, math.min(15, mobLevel - level))]
    if row == nil then
        return 0
    end

    return row[math.floor((level - 1) / 5) + 1] or 0
end

-- The party's shape from its members' levels: the spread, highest less
-- lowest, and the average rounded down. `top` is the highest level as
-- C++ graded the kill, which remembers an outsider who hit the mob: he
-- widens the spread, and is no part of the average
exp.shapeOf = function(levels, top)
    local bottom = levels[1]
    local sum    = 0
    for _, level in ipairs(levels) do
        top    = math.max(top, level)
        bottom = math.min(bottom, level)
        sum    = sum + level
    end

    return top - bottom, math.floor(sum / #levels)
end

-- Where a spread sits between the party as one level (0) and upstream's
-- formula (1). With the rule off (cardian.EXP_PARTY_AVERAGE false), or
-- the classic spread at 0, it is upstream's formula at every spread
exp.blendFor = function(spread)
    if xi.settings.cardian.EXP_PARTY_AVERAGE == false then
        return 1
    end

    local averageSpread = xi.settings.cardian.EXP_AVERAGE_SPREAD or 3
    local classicSpread = xi.settings.cardian.EXP_CLASSIC_SPREAD or 10

    if spread >= classicSpread then
        return 1
    elseif spread <= averageSpread then
        return 0
    end

    return (spread - averageSpread) / (classicSpread - averageSpread)
end

-- The kill's shape, read once a kill. C++ pays the members one after
-- another and a grant can raise a level, so a shape read again for the
-- next member would not be the party that made the kill. The first
-- member's call walks the members the game counts
-- (charutils::DistributeExperiencePoints: the alliance, in the mob's
-- zone, within 100 yalms of it) and leaves the answer on the mob, whose
-- local variables are cleared as it spawns and so last exactly one life.
-- Also whether a real player is in the alliance at all, wherever he is:
-- his party's grants are written to the map log, a world camp's are not
local function shapeOfKill(member, mob, data)
    local kept = mob:getLocalVar('[cardian]expSpread')
    if kept > 0 then
        return kept - 1, mob:getLocalVar('[cardian]expAverage'), mob:getLocalVar('[cardian]expWitnessed') == 1
    end

    local levels    = {}
    local zone      = mob:getZoneID()
    local witnessed = false
    for _, ally in ipairs(member:getAlliance()) do
        if ally:getZoneID() == zone and ally:checkDistance(mob) < 100 then
            levels[#levels + 1] = exp.levelOf(ally)
        end

        if ally:isPC() and not ally:isCardian() then
            witnessed = true
        end
    end

    if #levels == 0 then
        levels[1] = data.memberLevel
    end

    local spread, average = exp.shapeOf(levels, data.highestMemberLevel)
    mob:setLocalVar('[cardian]expSpread', spread + 1)
    mob:setLocalVar('[cardian]expAverage', average)
    mob:setLocalVar('[cardian]expWitnessed', witnessed and 1 or 0)

    return spread, average, witnessed
end

local function copyOf(data)
    local copy = {}
    for key, value in pairs(data) do
        copy[key] = value
    end

    return copy
end

-- What C++ hands the formula, rewritten as the party at one level: the
-- base read at the average, this member her own top, so upstream takes
-- no cut, and the mob judged at the average, so a chain is the one-level
-- party's
local function asOneLevel(data, average, mob)
    local mobLevel = mob:getMainLvl() + mob:getMod(xi.mod.EXP_LVL_MOD)
    local asOne    = copyOf(data)
    asOne.baseExp            = exp.baseAt(average, mobLevel)
    asOne.highestMemberLevel = data.memberLevel
    asOne.highestMemberTNL   = data.memberTNL

    local judged = exp.difficultyOf(asOne.baseExp, mobLevel)
    if judged ~= nil then
        asOne.mobDifficulty = judged
    end

    return asOne
end

-- The member as upstream's formula may read her, with nothing it can
-- change. The formula spends Dedication as it counts it; here the charge
-- is counted from the same numbers, written nowhere, and reported (the
-- `spent` of the second value), so the caller can spend what is right.
-- The second value is nil without a Dedication
local function readOnly(member)
    local dedication = member:getStatusEffect(xi.effect.DEDICATION)
    local frozen     = dedication and
    {
        spent = 0,

        getPower = function()
            return dedication:getPower()
        end,

        getSubPower = function()
            return dedication:getSubPower()
        end,
    }

    if frozen then
        frozen.setSubPower = function(_, remaining)
            frozen.spent = dedication:getSubPower() - remaining
        end
    end

    local proxy = setmetatable({},
    {
        __index = function(_, name)
            if name == 'getStatusEffect' then
                return function(_, effect)
                    if effect == xi.effect.DEDICATION then
                        return frozen
                    end

                    return member:getStatusEffect(effect)
                end
            elseif name == 'delStatusEffect' then
                return function()
                end
            end

            return function(_, ...)
                return member[name](member, ...)
            end
        end,
    })

    return proxy, frozen
end

-- Dedication spent as upstream spends it (experience_points.lua's
-- handleDedicationBonus): the charge taken off, the effect gone when it
-- is used up
local function spendDedication(member, spent)
    local dedication = member:getStatusEffect(xi.effect.DEDICATION)
    if dedication == nil or spent <= 0 then
        return
    end

    local remaining = dedication:getSubPower() - spent
    dedication:setSubPower(remaining)
    if remaining <= 0 then
        member:delStatusEffect(xi.effect.DEDICATION)
    end
end

-- The rule gave way to upstream's formula: counted, and said once
local function gaveWay(what, err)
    exp.failures = exp.failures + 1
    if exp.failures == 1 then
        print(string.format('[cardian] exp: %s failed, so upstream\'s formula pays (said once; xi.cardian.exp.failures counts): %s', what, tostring(err)))
    end
end

-- The record and the map log's line, in what the character receives
-- (map.EXP_RATE applied, as C++ applies it to the figure). Never the
-- grant's business: an error here is swallowed
local function note(member, mob, record)
    local ok, err = pcall(function()
        local rate = xi.settings.map.EXP_RATE or 1
        record.granted = math.floor(record.paid * rate)
        record.mob     = mob:getID()
        record.at      = os.time()
        exp.last[member:getID()] = record

        if not record.witnessed then
            return
        end

        local beside = ''
        if record.t == 1 then
            beside = '; upstream\'s figure'
        elseif record.classic ~= nil then
            beside = string.format(', average %d; upstream alone %d', record.average, math.floor(record.classic * rate))
        end

        print(string.format('[cardian] exp: %s gets %d for a level %d mob (spread %d%s)',
            member:getName(), record.granted, mob:getMainLvl() + mob:getMod(xi.mod.EXP_LVL_MOD), record.spread, beside))
    end)

    if not ok then
        gaveWay('the record', err)
    end
end

-- The rule, run guarded by the override below. `upstream` is upstream's
-- function (the override's super), handed in so this runs as a plain
-- function under pcall
local function ruleFor(upstream, member, mob, data)
    -- Switched off (cardian.EXP_PARTY_AVERAGE false, or the classic spread
    -- at 0), this is the server's own formula and nothing else
    if xi.settings.cardian.EXP_PARTY_AVERAGE == false or (xi.settings.cardian.EXP_CLASSIC_SPREAD or 10) <= 0 then
        return upstream(member, mob, data)
    end

    local spread, average, witnessed = shapeOfKill(member, mob, data)
    local t = exp.blendFor(spread)

    -- Upstream's end of the blend: its inputs go through untouched
    if t >= 1 then
        local result = upstream(member, mob, data)
        if result ~= nil then
            note(member, mob, { paid = result.exp, classic = result.exp, spread = spread, average = average, t = 1, witnessed = witnessed })
        end

        return result
    end

    local asOne = asOneLevel(data, average, mob)

    -- The party as one level: upstream's function paid as it is, and its
    -- own figure asked beside it for the record, nothing spent on it
    if t == 0 then
        local result = upstream(member, mob, asOne)
        if result ~= nil then
            local asked, classic = pcall(function()
                return upstream(readOnly(member), mob, copyOf(data))
            end)

            note(member, mob,
            {
                paid      = result.exp,
                classic   = asked and classic ~= nil and classic.exp or nil,
                averaged  = result.exp,
                spread    = spread,
                average   = average,
                t         = 0,
                witnessed = witnessed,
            })
        end

        return result
    end

    -- Between the spreads: both figures asked with nothing spent, the
    -- reward their straight blend rounded down, and Dedication charged
    -- the blend of what each would have spent. The result is the
    -- one-level answer's -- its chain is the one that counts
    local viewForAveraged, dedicationForAveraged = readOnly(member)
    local viewForClassic, dedicationForClassic   = readOnly(member)
    local averaged = upstream(viewForAveraged, mob, asOne)
    local classic  = upstream(viewForClassic, mob, copyOf(data))
    if averaged == nil or classic == nil then
        return nil
    end

    local oneLevel = averaged.exp
    averaged.exp   = math.floor((1 - t) * oneLevel + t * classic.exp + 1e-9)
    if dedicationForAveraged ~= nil then
        spendDedication(member, math.floor((1 - t) * dedicationForAveraged.spent + t * dedicationForClassic.spent + 1e-9))
    end

    note(member, mob,
    {
        paid      = averaged.exp,
        classic   = classic.exp,
        averaged  = oneLevel,
        spread    = spread,
        average   = average,
        t         = t,
        witnessed = witnessed,
    })

    return averaged
end

m:addOverride('xi.experiencePoints.calculate', function(member, mob, data)
    local ok, result = pcall(ruleFor, super, member, mob, data)
    if ok then
        return result
    end

    gaveWay('the rule', result)
    return super(member, mob, data)
end)

return m
