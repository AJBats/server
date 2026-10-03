-----------------------------------
-- Cardian: the MP bank's samplers (RESEARCH §12.13)
--
-- The server's own formulas, asked once from Lua instead of a hundred round
-- trips from C++ (src/map/pawn/spell_bank.cpp). Where a formula rolls dice
-- inside, the bank samples it here or takes each die at its expectation:
--
--   xi.cardian.bank.landChance(caster, target, fed, rolls)
--     -> the fraction of rolls where the cast's own gates pass -- immunity,
--        an effect on the target that nullifies this one (fed.tier), the
--        status-resistance roll, then the resist rate clearing the effect's
--        threshold -- and the mean rate among those (the duration's factor).
--        Zero rolls means the effect lands whenever the cast resolves (Dia,
--        Bio): only immunity and nullification say no.
--   xi.cardian.bank.expectedPdif(actor, defence, level, weaponType, rolls)
--     -> the mean melee pDIF the actor lands on a target at that defence and
--        level. A stand-in target answers the three things the formula asks
--        of its target; the actor is real. If the formula ever asks for
--        more, the call errors and the bank logs it.
--   xi.cardian.bank.expectedCure(caster, spellId, element)
--     -> what a Cure tier heals off this caster, before the target's missing
--        HP caps it. No dice in this one: the server's cure helpers, with
--        each tier's power bands mirrored from its spell script.
--   xi.cardian.bank.nukeSeeds(caster, target, spells)
--     -> what each nuke deals the target on average, rolling nothing: the
--        server's damage chain with every die taken at its expectation.
--
-- A library, not a module: it overrides nothing, so the pawn module loads
-- it at init (bank::load) rather than init.txt, and xi_test -- which has
-- no pawn module -- requires it from the test. Pure reads: nothing here
-- casts or changes an entity.
-----------------------------------
xi = xi or {}
xi.cardian = xi.cardian or {}
xi.cardian.bank = {}

xi.cardian.bank.landChance = function(caster, target, fed, rolls)
    if
        xi.data.statusEffect.isTargetImmune(target, fed.effectId, fed.magicalElement) or
        xi.data.statusEffect.isEffectNullified(target, fed.effectId, fed.tier or 0)
    then
        return 0, 0
    end

    if rolls == 0 then
        return 1, 1
    end

    local landed  = 0
    local rateSum = 0
    for _ = 1, rolls do
        if not xi.data.statusEffect.isTargetResistant(caster, target, fed.effectId) then
            local rate = xi.combat.magicHitRate.calculateResistRate(caster, target, fed)
            if xi.data.statusEffect.isResistRateSuccessfull(fed.effectId, rate, 0) then
                landed  = landed + 1
                rateSum = rateSum + rate
            end
        end
    end

    return landed / rolls, landed > 0 and rateSum / landed or 0
end

-- What calculateMeleePDIF asks of its target, and nothing else
local function standIn(defence, level)
    return
    {
        getStat    = function(_, mod)
            if mod == xi.mod.DEF then
                return defence
            end
            return 0
        end,
        getMainLvl = function() return level end,
        getMod     = function() return 0 end,
        isPC       = function() return false end,
        isMob      = function() return true end,
    }
end

-- The mean of an ordinary swing, and the biggest roll a critical one
-- can land: what a swing does on average, and the worst one can
xi.cardian.bank.expectedPdif = function(actor, defence, level, weaponType, rolls)
    local target    = standIn(defence, level)
    local corrected = xi.data.levelCorrection.isLevelCorrectedZone(actor)
    local sum       = 0
    for _ = 1, rolls do
        sum = sum + xi.combat.physical.calculateMeleePDIF(actor, target, weaponType, 1, false, corrected, false, 0, false, xi.slot.MAIN, false)
    end
    local biggest = 0
    for _ = 1, math.max(1, math.floor(rolls / 5)) do
        local pdif = xi.combat.physical.calculateMeleePDIF(actor, target, weaponType, 1, true, corrected, false, 0, false, xi.slot.MAIN, false)
        if pdif > biggest then
            biggest = pdif
        end
    end
    return sum / rolls, biggest
end

-- One ordinary swing's damage before the pDIF roll, from the server's own
-- attack damage function with the ratio at one: the weapon, the stat
-- factor, a mob's hand-to-hand penalty. nil while the actor has Consume
-- Mana, which that function would spend
xi.cardian.bank.swingBase = function(actor, target, isH2H)
    if actor:hasStatusEffect(xi.effect.CONSUME_MANA) then
        return nil
    end
    return xi.combat.physical.calculateAttackDamage(actor, target, xi.slot.MAIN, xi.physicalAttackType.NORMAL, isH2H, false, false, false, 1)
end

-- Every Cure tier's power bands, transcribed from its spell script
-- (scripts/actions/spells/white/cure*.lua): `old` is the base pair plus the
-- `power > above` steps in the script's order, `new` the `power < below`
-- steps with math.huge for the catch-all. Cure VI has no old formula
local kCureBands =
{
    [xi.magic.spell.CURE] =
    {
        minCure = 10,
        old =
        {
            base  = { divisor = 1, constant = -10 },
            steps =
            {
                { above = 100, divisor = 57, constant = 29.125 },
                { above =  60, divisor =  2, constant =      5 },
            },
        },
        new =
        {
            { below =        20, divisor =      4, constant = 10, basepower =   0 },
            { below =        40, divisor = 1.3333, constant = 15, basepower =  20 },
            { below =       125, divisor =    8.5, constant = 30, basepower =  40 },
            { below =       200, divisor =     15, constant = 40, basepower = 125 },
            { below =       600, divisor =     20, constant = 40, basepower = 200 },
            { below = math.huge, divisor = 999999, constant = 65, basepower =   0 },
        },
    },

    [xi.magic.spell.CURE_II] =
    {
        minCure = 60,
        old =
        {
            base  = { divisor = 1, constant = 20 },
            steps =
            {
                { above = 170, divisor = 35.6666, constant = 87.62 },
                { above = 110, divisor =       2, constant =  47.5 },
            },
        },
        new =
        {
            { below =        70, divisor =      1, constant =  60, basepower =  40 },
            { below =       125, divisor =    5.5, constant =  90, basepower =  70 },
            { below =       200, divisor =    7.5, constant = 100, basepower = 125 },
            { below =       400, divisor =     10, constant = 110, basepower = 200 },
            { below =       700, divisor =     20, constant = 130, basepower = 400 },
            { below = math.huge, divisor = 999999, constant = 145, basepower =   0 },
        },
    },

    [xi.magic.spell.CURE_III] =
    {
        minCure = 130,
        old =
        {
            base  = { divisor = 1, constant = 70 },
            steps =
            {
                { above = 300, divisor = 15.6666, constant = 180.43 },
                { above = 180, divisor =       2, constant =    115 },
            },
        },
        new =
        {
            { below =       125, divisor =     2.2, constant = 130, basepower =  70 },
            { below =       200, divisor = 75 / 65, constant = 155, basepower = 125 },
            { below =       300, divisor =     2.5, constant = 220, basepower = 200 },
            { below =       700, divisor =       5, constant = 260, basepower = 300 },
            { below = math.huge, divisor =  999999, constant = 340, basepower =   0 },
        },
    },

    [xi.magic.spell.CURE_IV] =
    {
        minCure = 270,
        old =
        {
            base  = { divisor = 0.6666, constant = 165 },
            steps =
            {
                { above = 460, divisor = 6.5, constant = 354.6666 },
                { above = 220, divisor =   2, constant =      275 },
            },
        },
        new =
        {
            { below =       200, divisor =      1, constant = 270, basepower =  70 },
            { below =       300, divisor =      2, constant = 400, basepower = 200 },
            { below =       400, divisor = 10 / 7, constant = 450, basepower = 300 },
            { below =       700, divisor =    2.5, constant = 520, basepower = 400 },
            { below = math.huge, divisor = 999999, constant = 640, basepower =   0 },
        },
    },

    [xi.magic.spell.CURE_V] =
    {
        minCure = 450,
        old =
        {
            base  = { divisor = 0.6666, constant = 330 },
            steps =
            {
                { above = 560, divisor = 2.8333, constant = 591.2 },
                { above = 320, divisor =      1, constant =   410 },
            },
        },
        new =
        {
            { below =       150, divisor =    0.70, constant = 450, basepower =  80 },
            { below =       190, divisor =    1.25, constant = 550, basepower = 150 },
            { below =       260, divisor = 70 / 38, constant = 582, basepower = 190 },
            { below =       300, divisor =       2, constant = 620, basepower = 260 },
            { below =       500, divisor =     2.5, constant = 640, basepower = 300 },
            { below =       700, divisor =  10 / 3, constant = 720, basepower = 500 },
            { below = math.huge, divisor =  999999, constant = 780, basepower =   0 },
        },
    },

    [xi.magic.spell.CURE_VI] =
    {
        minCure = 600,
        old     = nil,
        new     =
        {
            { below =       210, divisor =    1.5, constant =  600, basepower =  90 },
            { below =       300, divisor =    0.9, constant =  680, basepower = 210 },
            { below =       400, divisor = 10 / 7, constant =  780, basepower = 300 },
            { below =       500, divisor =    2.5, constant =  850, basepower = 400 },
            { below =       700, divisor =  5 / 3, constant =  890, basepower = 500 },
            { below = math.huge, divisor = 999999, constant = 1010, basepower =   0 },
        },
    },
}

-- What a Cure tier would heal, before the target's missing HP caps it:
-- the server's own cure helpers run on the caster, with each tier's
-- power bands mirrored from its script (there is nothing to call for
-- those). nil while the caster has Rapture, since the final step would
-- consume it. spellId is one of the six Cure tiers; element is the
-- spell's element (Cure is light); target, when given, adds its own cure
-- bonus as the scripts do after the final step, and the server's CURE_POWER
-- applies either way
xi.cardian.bank.expectedCure = function(caster, spellId, element, target)
    if caster:hasStatusEffect(xi.effect.RAPTURE) then
        return nil
    end

    local bands = kCureBands[spellId]
    if not bands then
        return nil
    end

    local old      = xi.settings.main.USE_OLD_CURE_FORMULA and bands.old ~= nil
    local basecure = 0

    if old then
        local power    = getCurePowerOld(caster)
        local divisor  = bands.old.base.divisor
        local constant = bands.old.base.constant

        for _, step in ipairs(bands.old.steps) do
            if power > step.above then
                divisor  = step.divisor
                constant = step.constant
                break
            end
        end

        basecure = getBaseCureOld(power, divisor, constant)
    else
        local power = getCurePower(caster)

        for _, step in ipairs(bands.new) do
            if power < step.below then
                basecure = getBaseCure(power, step.divisor, step.constant, step.basepower)
                break
            end
        end
    end

    -- getCureFinal asks the spell for its element and nothing else
    local spell = { getElement = function() return element end }

    -- Its last step rolls the day and weather bonus (a third of casts on a
    -- matching day land ten percent more or less), so the answer is taken
    -- with that bonus at one: the expectation, not one roll of it. The
    -- fight log's self-check allows the roll when a cure lands
    local dayAndWeather = xi.spells.damage.calculateDayAndWeather
    xi.spells.damage.calculateDayAndWeather = function() return 1 end
    local ok, final = pcall(getCureFinal, caster, spell, basecure, bands.minCure, false)
    xi.spells.damage.calculateDayAndWeather = dayAndWeather
    if not ok then
        error(final)
    end
    if target ~= nil then
        final = final + (final * (target:getMod(xi.mod.CURE_POTENCY_RCVD) / 100))
    end
    return math.floor(final * xi.settings.main.CURE_POWER)
end

-----------------------------------
-- The nuke seed (RESEARCH §17.13, the Black Mage)
-----------------------------------
local unpack = unpack or table.unpack

local function pack(...)
    return { n = select('#', ...), ... }
end

-- A stand-in for an entity that asks it once per method and arguments and
-- answers from memory after. Pricing her nukes reads the same few stats for
-- every spell; through this each read crosses into the server once a
-- pricing. Every read the damage chain makes is pure, so nothing it
-- remembers goes stale within one pricing
local entityOf = setmetatable({}, { __mode = 'k' })
local kNil     = {} -- a nil argument, as a key
local kAnswer  = {} -- where a call's answer is kept, under its arguments

local function remembered(entity)
    local proxy = {}
    entityOf[proxy] = entity
    return setmetatable(proxy, {
        __index = function(self, method)
            local answers = {}
            local call    = function(_, ...)
                local n    = select('#', ...)
                local node = answers
                for i = 1, n do
                    local arg = select(i, ...)
                    if arg == nil then
                        arg = kNil
                    end
                    local nextNode = node[arg]
                    if nextNode == nil then
                        nextNode  = {}
                        node[arg] = nextNode
                    end
                    node = nextNode
                end
                local answer = node[kAnswer]
                if answer == nil then
                    -- another stand-in passed along goes in as its entity
                    local args = { ... }
                    for i = 1, n do
                        local real = args[i] ~= nil and entityOf[args[i]] or nil
                        if real ~= nil then
                            args[i] = real
                        end
                    end
                    answer        = pack(entity[method](entity, unpack(args, 1, n)))
                    node[kAnswer] = answer
                end
                return unpack(answer, 1, answer.n)
            end
            rawset(self, method, call)
            return call
        end,
    })
end

-- A step's expectation over its one percent roll (`math.randomInt(1, 100)
-- <= chance` in the server's code): the step run as if the roll hit and as
-- if it missed, the two weighed by the chance. The step rolls nothing else
local function overRoll(chance, step, ...)
    local p         = utils.clamp(chance, 0, 100) / 100
    local randomInt = math.randomInt
    local hit, miss = 0, 0
    local ok, err   = pcall(function(...)
        if p > 0 then
            math.randomInt = function()
                return 1
            end
            hit = step(...)
        end
        if p < 1 then
            math.randomInt = function(_, hi)
                return hi
            end
            miss = step(...)
        end
    end, ...)
    math.randomInt = randomInt
    if not ok then
        error(err, 0)
    end
    return p * hit + (1 - p) * miss
end

-- A step's answer with every percent roll in it missing: only a certain
-- proc (a chance of 100) happens. The nullification and the absorption
-- roll their chances; a partial chance is priced as if it never happens
local function certainOnly(step, ...)
    local randomInt = math.randomInt
    math.randomInt  = function(_, hi)
        return hi
    end
    local ok, result = pcall(step, ...)
    math.randomInt   = randomInt
    if not ok then
        error(result, 0)
    end
    return result
end

-- calculateMagicHitRate, a local of the resist roll's script: the hit rate
-- from an accuracy and an evasion. Found again if the script is reloaded
local hitRate, hitRateOf

local function magicHitRate(params)
    local roll = xi.combat.magicHitRate.calculateResistRate
    if hitRateOf ~= roll then
        hitRate, hitRateOf = nil, roll
        for i = 1, 64 do
            local name, value = debug.getupvalue(roll, i)
            if name == nil then
                break
            end
            if name == 'calculateMagicHitRate' then
                hitRate = value
                break
            end
        end
    end
    if hitRate == nil then
        error('calculateResistRate no longer reaches calculateMagicHitRate (scripts/combat/basic/magic_hit_rate.lua)', 0)
    end
    return hitRate(params)
end

-- The resist roll as calculateResistRate sets it up, read instead of rolled:
-- the hit rate it rolls against and the tiers it may roll, from its own
-- locals, so its expectation needs no copy of the formula. Its weather
-- accuracy is taken unproc'd (a third of casts gain or lose 5 to 10). A rate
-- it gives without a roll (Magic Shield, an auto-resist) is kept as given
local function resistOf(caster, target, fed)
    local seen
    local randomFloat, randomInt = math.randomFloat, math.randomInt
    math.randomInt = function(_, hi)
        return hi
    end
    math.randomFloat = function()
        seen = {}
        for i = 1, 32 do
            local name, value = debug.getlocal(2, i)
            if name == nil then
                break
            end
            seen[name] = value
        end
        return 0 -- the first roll lands, which ends the call
    end
    local ok, rate = pcall(xi.combat.magicHitRate.calculateResistRate, caster, target, fed)
    math.randomFloat, math.randomInt = randomFloat, randomInt
    if not ok then
        error(rate, 0)
    end
    if seen == nil then
        return { fixed = rate }
    end
    if type(seen.params) ~= 'table' or type(seen.maxResistTier) ~= 'number' then
        error('the resist roll no longer keeps params and maxResistTier (scripts/combat/basic/magic_hit_rate.lua)', 0)
    end
    return
    {
        macc    = seen.params.actorMagicAccuracy,
        meva    = seen.params.targetMagicEvasion,
        floored = seen.params.resistanceRank >= 10, -- the rate is held at its floor, whatever the accuracy
        rate    = seen.params.magicHitRate,
        tiers   = seen.maxResistTier,
    }
end

-- What the resist roll leaves of a nuke on average: it halves the damage
-- once for each roll that misses the hit rate, up to its tiers
local function expectedResist(p, tiers)
    local sum, reach = 0, 1
    for k = 0, tiers - 1 do
        sum   = sum + reach * p / 2 ^ k
        reach = reach * (1 - p)
    end
    return sum + reach / 2 ^ tiers
end

-- The resist's expectation for a spell with this accuracy bonus. The bonus
-- is added past the accuracy's food factor: exact without a food's magic
-- accuracy, a point or two off with one
local function resistFor(resist, bonusMacc)
    if resist.fixed ~= nil then
        return resist.fixed
    end
    local p = resist.rate
    if not resist.floored then
        p = magicHitRate({ actorMagicAccuracy = resist.macc + bonusMacc, targetMagicEvasion = resist.meva })
    end
    return expectedResist(p, resist.tiers)
end

-- What every nuke of one element, skill, group and stat shares: a certain
-- nullification or absorption, the resist roll's setup, and the chain's
-- steps that do not ask for the spell, each die at its expectation
local function kindOf(caster, target, s, statUsed, alwaysApply)
    local damage = xi.spells.damage
    if
        certainOnly(damage.calculateNullification, target, s.element, false, true, false, false) == 0 or
        certainOnly(damage.calculateAbsorption, target, s.element, false, true, false, false) < 0
    then
        return { none = true }
    end

    -- What the chain asks of the spell, for one target that is the primary
    local spell =
    {
        getTotalTargets    = function() return 1 end,
        getPrimaryTargetID = function() return target:getID() end,
    }

    local factor = damage.calculateMTDR(caster, spell) *
        damage.calculateElementalStaffBonus(caster, s.element) *
        damage.calculateElementalAffinityBonus(caster, s.element) *
        damage.calculateAdditionalResistTier(caster, target, s.element) *
        overRoll(33, damage.calculateDayAndWeather, caster, s.element, alwaysApply) *
        xi.combat.damage.calculateDamageAdjustment(target, false, true, false, false) *
        xi.combat.damage.magicalElementSDT(target, s.element) *
        xi.combat.damage.ecosystemMultiplier(caster, target, 0) *
        overRoll(caster:getMod(xi.mod.MAGIC_CRITHITRATE_II), damage.calculateMagicCriticalMultiplier, caster) *
        damage.calculateDivineSealMultiplier(caster, target, s.skillType) *
        damage.calculateDivineEmblemMultiplier(caster, s.skillType) *
        damage.calculateEnhancedElementalSealMultiplier(caster, s.skillType, s.element) *
        damage.calculateEbullienceMultiplier(caster, s.spellGroup) *
        damage.calculateSkillTypeMultiplier(s.skillType) *
        damage.calculateUndeadDivinePenalty(target, s.skillType) *
        xi.combat.damage.scarletDeliriumMultiplier(caster) *
        damage.calculateAreaOfEffectResistance(target, spell) *
        damage.calculateSpellActionTypeMultiplier(caster)

    local fed =
    {
        magicBurstTier = 0,
        magicalElement = s.element,
        actorStat      = statUsed,
        skillType      = s.skillType,
        spellGroup     = s.spellGroup,
        bonusMacc      = 0,
    }
    return { factor = factor, resist = resistOf(caster, target, fed) }
end

-- What each of her nukes deals the target on average (RESEARCH §17.13):
-- the chain of xi.spells.damage.useDamageSpell on the real caster and
-- target with every die at its expectation -- the resist roll, the day and
-- weather, the magic attack's crit, the magic crit -- and no floors between
-- steps. What the spells of an element share is worked out once, the base
-- damage and the accuracy bonus per spell. Left out, each for a reason: a
-- magic burst (no skillchains yet), Cardinal Chant's crit chance and the
-- nuke wall (locals of that script: a Geomancer's, and an NM's), the
-- Ninjutsu and automaton steps (never a nuke's), Phalanx, Stoneskin, a
-- damage cap, and a partial chance to nullify or absorb. What her nukes
-- really land against this is learned beside it (fight_log.cpp). spells:
-- { id, element, skillType, spellGroup, family } each; the answer is the
-- seed by spell id, 0 when the target certainly nullifies or absorbs the
-- element, nothing for a spell the damage table does not know
xi.cardian.bank.nukeSeeds = function(caster, target, spells)
    local damage = xi.spells.damage
    local c      = remembered(caster)
    local t      = remembered(target)
    local kinds  = {}
    local bonus  = {}
    local seeds  = {}

    for _, s in ipairs(spells) do
        local row = damage.pTable[s.id]
        if row then
            -- The damage table's columns (a local `column` in
            -- damage_spell.lua): the stat the spell rolls on, its accuracy
            -- bonus, whether the day and weather always apply
            local statUsed    = row[1]
            local bonusMacc   = row[2]
            local alwaysApply = row[3]

            local kindKey = table.concat({ s.element, s.skillType, s.spellGroup, statUsed, tostring(alwaysApply) }, ':')
            local kind    = kinds[kindKey]
            if kind == nil then
                kind           = kindOf(c, t, s, statUsed, alwaysApply)
                kinds[kindKey] = kind
            end

            if kind.none then
                seeds[s.id] = 0
            else
                -- The magic attack against the magic defence asks for the
                -- spell only by its family (the ancient magic's merits)
                local bonusKey = table.concat({ s.element, s.skillType, s.family }, ':')
                local mab      = bonus[bonusKey]
                if mab == nil then
                    mab             = overRoll(c:getMod(xi.mod.MAGIC_CRITHITRATE), damage.calculateMagicBonusDiff, c, t, s.id, s.skillType, s.element, 0)
                    bonus[bonusKey] = mab
                end

                seeds[s.id] = damage.calculateBaseDamage(c, t, s.id, s.spellGroup, s.skillType, statUsed) *
                    kind.factor *
                    resistFor(kind.resist, bonusMacc) *
                    mab *
                    damage.calculateHelixMeritMultiplier(c, s.id)
            end
        end
    end

    return seeds
end
