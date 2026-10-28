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
--        level. A stand-in target answers what the formula asks of its
--        target; the actor is real. If the formula ever asks for more, the
--        call errors and the bank logs it.
--   xi.cardian.bank.expectedCure(caster, spellId, element)
--     -> what a Cure tier heals off this caster, before the target's missing
--        HP caps it. No dice in this one: the server's cure helpers, with
--        each tier's power bands mirrored from its spell script.
--   xi.cardian.bank.nukeSeeds(caster, target, spells, intDown)
--     -> what each nuke deals the target on average, rolling nothing: the
--        server's damage chain with every die taken at its expectation;
--        with intDown, a second table in the same pass, as if the target's
--        INT were that much lower (Burn). Where its read of the resist roll
--        fails, the spells' own scripts run on stand-ins over seeded dice,
--        and 'sampled' and why follow the two tables.
--   xi.cardian.bank.burstFactors(caster, target, spells, count)
--     -> what a magic burst multiplies each nuke by, the server's own two
--        burst steps at the chain's count, read without consuming anything.
--   xi.cardian.bank.chainShare(closer, target, chain, tier, count)
--     -> what a skillchain deals for each point of the weapon skill that
--        closes it: the server's own skillchain damage, run on a stand-in of
--        the target that carries the chain and changes nothing.
--   xi.cardian.bank.wsExpected(attacker, target, wsId, name, tp)
--     -> what a weapon skill deals the target on average, and how: its own
--        script and xi.weaponskills run on stand-ins that change nothing,
--        its dice read rather than rolled -- each hit's chance to land and to
--        crit off its calcParams, every other die by the comparison it is
--        rolled into -- the same answer every time ('read'); where that
--        read fails, the mean of 64 runs over seeded dice ('sampled', and
--        why).
--   xi.cardian.bank.debuffSubPower(scriptName, effectId)
--     -> the subPower a debuff's own script puts on its effect (Dia's
--        defence down, Bio's attack down, in percent), read by running the
--        script's onSpellCast on stand-ins: whatever script is in force,
--        the era module's overrides included. nil when it cannot be read.
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

-- What calculateMeleePDIF asks of its target, and nothing else: a mob under
-- no effect (Signet's defence bonus is a player's)
local function standIn(defence, level)
    return
    {
        getStat         = function(_, mod)
            if mod == xi.mod.DEF then
                return defence
            end
            return 0
        end,
        getMainLvl      = function() return level end,
        getMod          = function() return 0 end,
        hasStatusEffect = function() return false end,
        isPC            = function() return false end,
        isMob           = function() return true end,
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

-- A stand-in whose INT reads lower by intDown (higher, when negative),
-- everything else as the stand-in it wraps: the target as it would be under
-- a stat-down debuff (Burn), or with one lifted, for both the damage formula
-- and the resist roll, which read the stat through getStat. Passed into a
-- server call, it goes in as its entity
local function withIntDown(proxy, intDown)
    if intDown == nil or intDown == 0 then
        return proxy
    end
    local wrapper = {}
    entityOf[wrapper] = entityOf[proxy]
    return setmetatable(wrapper, {
        __index = function(self, method)
            local call = proxy[method]
            if method == 'getStat' then
                call = function(_, stat, ...)
                    local value = proxy:getStat(stat, ...)
                    if stat == xi.mod.INT then
                        -- held in the server's own range (CBattleEntity::INT)
                        value = utils.clamp(value - intDown, 0, 999)
                    end
                    return value
                end
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
-- roll their chances; a partial chance is priced as if it never happens.
-- A fraction roll (math.randomFloat) is pinned just under one, so a
-- `randomFloat() < chance` misses too, and the answer is the same every
-- time it is asked
local function certainOnly(step, ...)
    local randomInt, randomFloat = math.randomInt, math.randomFloat
    math.randomInt   = function(_, hi)
        return hi
    end
    math.randomFloat = function()
        return 0.999999
    end
    local ok, result = pcall(step, ...)
    math.randomInt, math.randomFloat = randomInt, randomFloat
    if not ok then
        error(result, 0)
    end
    return result
end

-- calculateMagicHitRate, a local of the resist roll's script: the hit rate
-- from an accuracy and an evasion, less the level correction between the
-- caster and the target. Found again if the script is reloaded
local hitRate, hitRateOf

local function magicHitRate(caster, target, params)
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
    return hitRate(caster, target, params)
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
        caster  = caster,
        target  = target,
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
        p = magicHitRate(resist.caster, resist.target, { actorMagicAccuracy = resist.macc + bonusMacc, targetMagicEvasion = resist.meva })
    end
    return expectedResist(p, resist.tiers)
end

-- What every nuke of one element, skill, group and stat shares: a certain
-- nullification or absorption, the resist roll's setup, and the chain's
-- steps that do not ask for the spell, each die at its expectation. With
-- `moved`, the target with its INT moved (withIntDown), the resist roll's
-- setup on it as well: the only shared step that reads the INT
local function kindOf(caster, target, s, statUsed, alwaysApply, moved)
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
    local kind = { factor = factor, resist = resistOf(caster, target, fed) }
    if moved ~= nil then
        kind.movedResist = resistOf(caster, moved, fed)
    end
    return kind
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
-- { id, element, skillType, spellGroup, family } each. The answer is the
-- seed by spell id, 0 when the target certainly nullifies or absorbs the
-- element, nothing for a spell the damage table does not know. With
-- intDown, a second answer in the same pass: each seed with the target's
-- INT that much lower, as a Burn would leave it (higher when negative, as
-- if one on it were lifted) -- only the base damage and the resist roll
-- read the INT, so everything else is worked out once for both
local function readNukeSeeds(caster, target, spells, intDown)
    local damage = xi.spells.damage
    local c      = remembered(caster)
    local t      = remembered(target)
    local moved  = nil
    local kinds  = {}
    local bonus  = {}
    local seeds  = {}
    local seedsMoved = nil
    if intDown ~= nil and intDown ~= 0 then
        moved      = withIntDown(t, intDown)
        seedsMoved = {}
    end

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
                kind           = kindOf(c, t, s, statUsed, alwaysApply, moved)
                kinds[kindKey] = kind
            end

            if kind.none then
                seeds[s.id] = 0
                if seedsMoved ~= nil then
                    seedsMoved[s.id] = 0
                end
            else
                -- The magic attack against the magic defence asks for the
                -- spell only by its family (the ancient magic's merits)
                local bonusKey = table.concat({ s.element, s.skillType, s.family }, ':')
                local mab      = bonus[bonusKey]
                if mab == nil then
                    mab             = overRoll(c:getMod(xi.mod.MAGIC_CRITHITRATE), damage.calculateMagicBonusDiff, c, t, s.id, s.skillType, s.element, 0)
                    bonus[bonusKey] = mab
                end

                local rest  = kind.factor * mab * damage.calculateHelixMeritMultiplier(c, s.id)
                seeds[s.id] = damage.calculateBaseDamage(c, t, s.id, s.spellGroup, s.skillType, statUsed) *
                    resistFor(kind.resist, bonusMacc) *
                    rest
                if seedsMoved ~= nil then
                    seedsMoved[s.id] = damage.calculateBaseDamage(c, moved, s.id, s.spellGroup, s.skillType, statUsed) *
                        resistFor(kind.movedResist, bonusMacc) *
                        rest
                end
            end
        end
    end

    return seeds, seedsMoved
end

-- The nukes' fallback where the read fails (sampledNukes, with the weapon
-- skills' below)
local sampledNukes

-- What each nuke deals the target on average, read (readNukeSeeds). Where
-- the read cannot find what it reads -- an upstream change to the resist
-- roll (scripts/combat/basic/magic_hit_rate.lua) -- each spell is priced by
-- kWsRuns dry runs of its own script over the bank's seeded dice instead,
-- and the answer says so: the seeds, the moved seeds, then 'sampled' and
-- why the read failed. spells: as readNukeSeeds takes them, with each
-- spell's name, which finds its script
xi.cardian.bank.nukeSeeds = function(caster, target, spells, intDown)
    local ok, seeds, seedsMoved = pcall(readNukeSeeds, caster, target, spells, intDown)
    if ok then
        return seeds, seedsMoved
    end
    local sampled, sampledMoved = sampledNukes(caster, target, spells, intDown)
    return sampled, sampledMoved, 'sampled', tostring(seeds)
end

-- What a magic burst multiplies each nuke by (ROADMAP, Road to subjob item
-- 6, slice 3h): the two burst steps of xi.spells.damage.useDamageSpell, the
-- server's own -- calculateIfMagicBurst at the chain's count (the
-- Skillchain effect's subPower: 1 for a chain of two weapon skills) and
-- calculateIfMagicBurstBonus -- for this caster on this target. Left out:
-- the burst's accuracy bonus to the resist roll, so the factor is the low
-- side of a burst. Read only: the Sengikori bonus the first step would
-- consume is not consumed. spells: { id, element, skillType } each; the
-- answer is the factor by spell id
xi.cardian.bank.burstFactors = function(caster, target, spells, count)
    local damage = xi.spells.damage
    local c      = remembered(caster)
    local t      = remembered(target)
    rawset(t, 'setMod', function() end)

    local factors = {}
    for _, s in ipairs(spells) do
        factors[s.id] = damage.calculateIfMagicBurst(c, t, s.element, count) *
            damage.calculateIfMagicBurstBonus(c, t, s.id, s.skillType, s.element)
    end

    return factors
end

-- What a skillchain deals for each point of the weapon skill that closes it
-- (ROADMAP, Road to subjob item 6, slice 3h: the chain's worth, which the
-- tactician weighs before the plan's opening): the server's own
-- xi.combat.skillchain.calculateSkillchainDamage, for this closer, on a
-- stand-in of this target that carries the chain as its Skillchain effect
-- (the server's chain number, its tier, and its count: 1 for a chain of two
-- weapon skills) and nothing else, and that changes nothing -- no damage
-- taken, no HP absorbed, no Sengikori bonus consumed. Every percent roll in
-- it misses (certainOnly): the day and weather bonus is left out, and a
-- partial chance to nullify or absorb is taken as never happening. A resist
-- roll in it (the era module's skillchain damage asks
-- xi.combat.magicHitRate.calculateResistRate, which halves the damage up to
-- three times by chance) is priced at its average, as the nukes' is
-- (resistOf, resistFor), never rolled: the answer is the same every time,
-- whichever formula is in force. The closer's flat magic damage bonus is
-- spread over a 1000-point weapon skill. 0 when the target absorbs or
-- nullifies the chain's element
xi.cardian.bank.chainShare = function(closer, target, chain, tier, count)
    local base     = 1000
    local absorbed = false
    local effect   =
    {
        getPower    = function() return chain end,
        getTier     = function() return tier end,
        getSubPower = function() return count end,
    }
    local t = remembered(target)
    rawset(t, 'getStatusEffect', function(_, id)
        if id == xi.effect.SKILLCHAIN then
            return effect
        end

        return nil
    end)
    rawset(t, 'setMod', function() end)
    rawset(t, 'takeDamage', function() end)
    rawset(t, 'addHP', function()
        absorbed = true
    end)

    -- The resist roll, asked by the formula, answered with its average: the
    -- real roll read for its setup (resistOf) with the real function back in
    -- place meanwhile, the stand-in in place while the formula runs
    local roll     = xi.combat.magicHitRate.calculateResistRate
    local averaged
    averaged = function(actor, tgt, params)
        xi.combat.magicHitRate.calculateResistRate = roll
        local ok, rate = pcall(function()
            return resistFor(resistOf(actor, tgt, params), 0)
        end)
        xi.combat.magicHitRate.calculateResistRate = averaged
        if not ok then
            error(rate, 0)
        end

        return rate
    end

    xi.combat.magicHitRate.calculateResistRate = averaged
    local ok, dealt = pcall(certainOnly, xi.combat.skillchain.calculateSkillchainDamage, remembered(closer), t, base)
    xi.combat.magicHitRate.calculateResistRate = roll
    if not ok then
        error(dealt, 0)
    end

    if absorbed or type(dealt) ~= 'number' or dealt <= 0 then
        return 0
    end

    return dealt / base
end

-----------------------------------
-- The weapon skill's price (#496)
-----------------------------------

-- Methods of an entity or an effect that only read, by name: what a weapon
-- skill's damage chain asks of its actor and its target. Anything else it
-- calls -- TP added, an effect spent, damage taken, a skill-up, a message --
-- does nothing on a stand-in and answers nil. The three damage reductions
-- go through to the server: they read the target's mods (and would spend a
-- mob's Migawari or Fan Dance, which mobs do not carry)
local kReads = { physicalDmgTaken = true, rangedDmgTaken = true, handleSevereDamage = true }

local function reads(method)
    return kReads[method] or method:find('^get') ~= nil or method:find('^has') ~= nil or method:find('^is') ~= nil or
        method:find('^can') ~= nil or method:find('^check') ~= nil
end

-- Any object of the server's -- an effect, a spell -- as a run may hold it:
-- its reads go through, anything else does nothing. Passed into a server
-- call, it goes in as itself
local function inertObject(object)
    local proxy = {}
    entityOf[proxy] = object
    return setmetatable(proxy, {
        __index = function(self, method)
            local call = function() return nil end
            if reads(method) then
                call = function(_, ...)
                    return object[method](object, ...)
                end
            end
            rawset(self, method, call)
            return call
        end,
    })
end

-- An entity a run of the server's code may ask anything and change nothing:
-- its reads asked once (remembered), its effects handed back inert, every
-- other method a no-op. `answers` replaces some reads outright: a value, or
-- a function answering the call
local function inert(entity, answers)
    local seen  = remembered(entity)
    local proxy = {}
    entityOf[proxy] = entity
    return setmetatable(proxy, {
        __index = function(self, method)
            local call
            if answers ~= nil and answers[method] ~= nil then
                local answer = answers[method]
                call = type(answer) == 'function' and answer or function()
                    return answer
                end
            elseif method == 'getStatusEffect' then
                call = function(_, ...)
                    local effect = seen:getStatusEffect(...)
                    return effect ~= nil and inertObject(effect) or nil
                end
            elseif reads(method) then
                call = seen[method]
            else
                call = function()
                    return nil
                end
            end
            rawset(self, method, call)
            return call
        end,
    })
end

-- The bank's own dice for runs of the server's code that roll many times:
-- the minimal standard generator (Park and Miller), seeded the same every
-- time, so its answer is the same every time it is asked. math.random,
-- randomInt, randomFloat and randomNormal as the server binds them
-- (src/map/lua/luautils.cpp), the server's own put back after
local kSeed = 20261017

local function onSeededDice(step)
    local x = kSeed
    local function u()
        x = (x * 16807) % 2147483647
        return x / 2147483647
    end

    local function int(lo, hi)
        lo, hi = math.floor(lo + 0.5), math.floor(hi + 0.5)
        return lo + math.floor(u() * (hi - lo + 1))
    end

    local random, randomInt, randomFloat, randomNormal = math.random, math.randomInt, math.randomFloat, math.randomNormal
    math.random = function(lo, hi)
        if lo == nil then
            return u()
        end
        if hi == nil then
            return int(1, lo)
        end
        return int(lo, hi)
    end
    math.randomInt   = int
    math.randomFloat = function(lo, hi)
        return lo + u() * (hi - lo)
    end
    math.randomNormal = function(mean, sd, lower, upper)
        local z = math.sqrt(-2 * math.log(u())) * math.cos(2 * math.pi * u())
        return utils.clamp(mean + sd * z, lower or -math.huge, upper or math.huge)
    end
    local ok, result = pcall(step)
    math.random, math.randomInt, math.randomFloat, math.randomNormal = random, randomInt, randomFloat, randomNormal
    if not ok then
        error(result, 0)
    end
    return result
end

-- Runs of the server's code a sampled price is the mean of (the fallback,
-- below): its error is about the price's own spread over eight
local kWsRuns = 64
xi.cardian.bank.wsRuns = kWsRuns

-- The stand-ins a weapon skill's script runs on: the target answers its
-- maximum HP for what it has left, so the price is the whole weapon skill
-- whatever the mob has left (the chain's worth weighs that itself)
local function wsStage(attacker, target)
    return
    {
        actor  = inert(attacker),
        foe    = inert(target, { getHP = target:getMaxHP() }),
        action = setmetatable({}, {
            __index = function()
                return function() end
            end,
        }),
    }
end

-- One run of a weapon skill's script on its stage: what it would have
-- dealt, taken where the game's own last step (takeWeaponskillDamage)
-- would take it
local function wsRun(stage, script, wsId, tp)
    local take  = xi.weaponskills.takeWeaponskillDamage
    local dealt = nil
    xi.weaponskills.takeWeaponskillDamage = function(_, _, _, _, _, results)
        dealt = results.finalDmg or 0
        return dealt
    end
    local ok, damage = pcall(function()
        local _, _, _, d = script.onUseWeaponSkill(stage.actor, stage.foe, wsId, tp, true, stage.action, nil)
        return d
    end)
    xi.weaponskills.takeWeaponskillDamage = take
    if not ok then
        error(damage, 0)
    end
    return math.max(0, dealt or damage or 0)
end

-- The sampled price: kWsRuns runs over the bank's seeded dice, their mean
-- and their spread
local function wsSampled(attacker, target, wsId, script, tp)
    local stage = wsStage(attacker, target)
    return onSeededDice(function()
        local sum, squares = 0, 0
        for _ = 1, kWsRuns do
            local run = wsRun(stage, script, wsId, tp)
            sum       = sum + run
            squares   = squares + run * run
        end
        local mean = sum / kWsRuns
        return { mean = mean, spread = math.sqrt(math.max(0, squares / kWsRuns - mean * mean)) }
    end)
end

-----------------------------------
-- Reading the dice (#496; the user, 2026-10-17): a run of the server's
-- code with each die it rolls answered by what it comes to on average --
-- the way resistOf reads the resist roll, never by sampling. A die rolled
-- into a comparison on its own line (`math.randomInt(1, 100) <= rate`,
-- `x * 100 >= math.randomInt(1, 10000)`, `math.randomInt(0, 1) == 0`) is
-- read there: its threshold worked out in the caller's own frame, and the
-- run goes both ways, each weighed by its chance. A weapon skill hit's miss
-- and critical rolls are read off its calcParams (wsRead, below). A die in
-- no comparison is taken at its midpoint -- right for one used linearly,
-- as pDIF's range and its random factor are -- and a die of 1 to 100 or to
-- 10000 in no comparison the bank can read is an error: a percent die is a
-- chance, and its odds unread would be priced wrong. A die of two values is
-- run both ways
-----------------------------------

-- The server's scripts' lines, read once a file
local sourceLines = {}

local function lineOf(path, line)
    local lines = sourceLines[path]
    if lines == nil then
        lines = {}
        local file = io.open(path, 'r')
        if file ~= nil then
            for text in file:lines() do
                lines[#lines + 1] = text
            end
            file:close()
        end
        sourceLines[path] = lines
    end
    return lines[line]
end

-- A frame as a die finds it, `level` counted as its caller counts: its
-- function, where it stands, and its locals and upvalues (boxed, so a nil
-- local still hides a global of its name)
local function frameOf(level)
    local at   = level + 1
    local info = debug.getinfo(at, 'Slf')
    if info == nil then
        return nil
    end
    local frame = { func = info.func, vars = {}, where = '?' }
    if type(info.source) == 'string' and info.source:sub(1, 1) == '@' then
        frame.path  = info.source:sub(2)
        frame.line  = info.currentline
        frame.where = string.format('%s:%d', frame.path, frame.line or 0)
    end
    for i = 1, 250 do
        local name, value = debug.getlocal(at, i)
        if name == nil then
            break
        end
        if name:sub(1, 1) ~= '(' then
            frame.vars[name] = { value }
        end
    end
    if info.func ~= nil then
        for i = 1, 250 do
            local name, value = debug.getupvalue(info.func, i)
            if name == nil then
                break
            end
            if frame.vars[name] == nil then
                frame.vars[name] = { value }
            end
        end
    end
    return frame
end

-- The index of the bracket closing the one at `open`
local function closing(text, open)
    local depth = 0
    for i = open, #text do
        local c = text:sub(i, i)
        if c == '(' or c == '[' or c == '{' then
            depth = depth + 1
        elseif c == ')' or c == ']' or c == '}' then
            depth = depth - 1
            if depth == 0 then
                return i
            end
        end
    end
    return nil
end

-- The operand after a comparison, to the end of its expression
local function operandAfter(text)
    text = text:match('^%s*(.-)$')
    local depth = 0
    local i     = 1
    while i <= #text do
        local c = text:sub(i, i)
        if c == '(' or c == '[' or c == '{' then
            depth = depth + 1
        elseif c == ')' or c == ']' or c == '}' then
            if depth == 0 then
                break
            end
            depth = depth - 1
        elseif depth == 0 then
            local rest = text:sub(i)
            if
                c == ',' or rest:find('^%-%-') or rest:find('^%s+then%f[%W]') or rest:find('^%s+or%f[%W]') or
                rest:find('^%s+and%f[%W]') or rest:find('^%s+do%f[%W]')
            then
                break
            end
        end
        i = i + 1
    end
    return (text:sub(1, i - 1):gsub('%s+$', ''))
end

-- The operand before a comparison, back to the start of its expression
local kBefore = { 'if', 'elseif', 'or', 'and', 'not', 'return', 'then', 'while', 'until', 'local' }

local function operandBefore(text)
    text = text:gsub('%s+$', '')
    local depth = 0
    local i     = #text
    while i >= 1 do
        local c = text:sub(i, i)
        if c == ')' or c == ']' or c == '}' then
            depth = depth + 1
        elseif c == '(' or c == '[' or c == '{' then
            if depth == 0 then
                break
            end
            depth = depth - 1
        elseif depth == 0 then
            if c == ',' or c == '=' then
                break
            end
            if c:find('%s') then
                local head = text:sub(1, i - 1)
                local stop = false
                for _, word in ipairs(kBefore) do
                    if head:find('%f[%w_]' .. word .. '$') then
                        stop = true
                    end
                end
                if stop then
                    break
                end
            end
        end
        i = i - 1
    end
    return (text:sub(i + 1):gsub('^%s+', ''))
end

-- A die's call as the server writes it, by kind
local kCalls  = { int = 'math%.randomInt%s*%(', float = 'math%.randomFloat%s*%(', any = 'math%.random%s*%(' }
local kOps    = { '<=', '>=', '==', '~=', '<', '>' }
local kMirror = { ['<='] = '>=', ['>='] = '<=', ['<'] = '>', ['>'] = '<', ['=='] = '==', ['~='] = '~=' }

-- The comparison a die is rolled into on its line, as `die op expr` (the
-- die's divisor folded in), by where it is; false for none
local comparisons = {}

local function comparisonAt(frame, kind)
    local key   = frame.where .. ':' .. kind
    local known = comparisons[key]
    if known ~= nil then
        return known
    end
    known      = false
    local text = frame.path ~= nil and lineOf(frame.path, frame.line) or nil
    if text ~= nil then
        text          = text:gsub('%-%-.*$', '')
        local s, open = text:find(kCalls[kind])
        if s ~= nil and text:find(kCalls[kind], open + 1) == nil then
            local close = closing(text, open)
            if close ~= nil then
                local after   = text:sub(close + 1)
                local divisor = 1
                local d, rest = after:match('^%s*/%s*([%d%.]+)(.*)$')
                if d ~= nil then
                    divisor = tonumber(d)
                    after   = rest
                end
                for _, op in ipairs(kOps) do
                    local operand = after:match('^%s*' .. op .. '(.*)$')
                    if operand ~= nil then
                        known = { op = op, expr = operandAfter(operand), divisor = divisor }
                        break
                    end
                end
                if not known then
                    local before = text:sub(1, s - 1)
                    for _, op in ipairs(kOps) do
                        local head = before:match('^(.-)%s*' .. op .. '%s*$')
                        if head ~= nil then
                            known = { op = kMirror[op], expr = operandBefore(head), divisor = 1 }
                            break
                        end
                    end
                end
            end
        end
    end
    comparisons[key] = known
    return known
end

-- A comparison's operand worked out in the die's own frame
local compiled = {}

local function valueIn(frame, expr)
    local chunk = compiled[expr]
    if chunk == nil then
        chunk = loadstring('return ' .. expr)
        if chunk == nil then
            error(string.format('%s: the bank cannot read "%s"', frame.where, expr), 0)
        end
        compiled[expr] = chunk
    end
    setfenv(chunk, setmetatable({}, {
        __index = function(_, name)
            local box = frame.vars[name]
            if box ~= nil then
                return box[1]
            end
            return _G[name]
        end,
    }))
    local ok, value = pcall(chunk)
    if not ok or type(value) ~= 'number' then
        error(string.format('%s: the bank cannot read "%s": %s', frame.where, expr, tostring(value)), 0)
    end
    return value
end

-- The two ways a die rolled into `die op threshold` goes, as { value, chance }:
-- whole numbers lo to hi for an integer die, lo up to hi for a fraction
local function waysOf(whole, lo, hi, op, threshold)
    local yes, no, chance
    if whole then
        local n     = hi - lo + 1
        local count = 0
        if op == '<=' then
            count = math.floor(threshold) - lo + 1
        elseif op == '<' then
            count = math.ceil(threshold) - lo
        elseif op == '>=' then
            count = hi - math.ceil(threshold) + 1
        elseif op == '>' then
            count = hi - math.floor(threshold)
        else
            local hits = threshold == math.floor(threshold) and threshold >= lo and threshold <= hi
            count = hits and 1 or 0
            if op == '~=' then
                count = n - count
            end
        end
        chance = utils.clamp(count, 0, n) / n
        if op == '<=' or op == '<' then
            yes, no = lo, hi
        elseif op == '>=' or op == '>' then
            yes, no = hi, lo
        else
            local other = threshold == lo and hi or lo
            if op == '==' then
                yes, no = threshold, other
            else
                yes, no = other, threshold
            end
        end
    else
        local top = hi - (hi - lo) * 1e-9
        local below = utils.clamp((threshold - lo) / (hi - lo), 0, 1)
        if op == '<=' or op == '<' then
            chance, yes, no = below, lo, top
        elseif op == '>=' or op == '>' then
            chance, yes, no = 1 - below, top, lo
        else
            chance = 0
            yes, no = threshold, (threshold == lo and top or lo)
            if op == '~=' then
                chance, yes, no = 1, no, yes
            end
        end
    end
    return { { yes, chance }, { no, 1 - chance } }
end

-- The walk a die answers to: every way its dice can fall, each run from the
-- start, weighed by the chances of the ways it took
local walking = nil

local function walkOver(step, limit)
    local outer   = walking
    local pending = { {} }
    local total   = 0
    local walked  = 0
    local ok, err = pcall(function()
        while #pending > 0 do
            walked = walked + 1
            if walked > limit then
                error(string.format('the dice fall more than %d ways', limit), 0)
            end
            local walk = { prefix = table.remove(pending), choices = {}, weight = 1, pending = pending }
            walking    = walk
            local value = step(walk)
            walking     = outer
            total       = total + walk.weight * value
        end
    end)
    walking = outer
    if not ok then
        error(err, 0)
    end
    return total
end

-- A die's ways, { value, chance } each: the way this run takes
local function branch(ways)
    local live = {}
    for _, way in ipairs(ways) do
        if way[2] > 0 then
            live[#live + 1] = way
        end
    end
    if #live == 0 then
        error('a die that cannot fall', 0)
    end
    local walk = walking
    if walk == nil then
        return live[1][1]
    end
    local at   = #walk.choices + 1
    local pick = walk.prefix[at]
    if pick == nil then
        pick = 1
        for j = 2, #live do
            local prefix = { unpack(walk.choices) }
            prefix[at]   = j
            walk.pending[#walk.pending + 1] = prefix
        end
    end
    walk.choices[at] = pick
    walk.weight      = walk.weight * live[pick][2]
    return live[pick][1]
end

-- A weapon skill hit's own two rolls, read off what the hit step holds as
-- it rolls them (scripts/globals/weaponskills.lua getSingleHitDamage): the
-- miss (`math.randomFloat(0, 1) > calcParams.hitRate`, never with
-- guaranteedHit, always with mustMiss), then the critical
-- (`math.randomFloat(0, 1) <= calcParams.critRate` where wsParams.critVaries,
-- always with forcedFirstCrit or mightyStrikesApplicable). The step being
-- read and the rolls it has made
local hitStep = nil
local hitSeen = nil
local kHitWhere = 'scripts/globals/weaponskills.lua getSingleHitDamage'

local function hitRoll(frame)
    if hitSeen == nil then
        error(kHitWhere .. ': a roll outside a hit the bank reads, at ' .. frame.where, 0)
    end
    local calc = frame.vars.calcParams and frame.vars.calcParams[1]
    if type(calc) ~= 'table' then
        error(kHitWhere .. ': no local calcParams at its roll', 0)
    end
    if frame.vars.missChance == nil then
        if type(calc.hitRate) ~= 'number' then
            error(kHitWhere .. ': no calcParams.hitRate at its miss roll', 0)
        end
        hitSeen.miss = true
        local lands = calc.mustMiss and 0 or (calc.guaranteedHit and 1 or utils.clamp(calc.hitRate, 0, 1))
        return branch({ { 0, lands }, { 1, 1 - lands } })
    end
    if frame.vars.critChance == nil then
        local ws = frame.vars.wsParams and frame.vars.wsParams[1]
        if type(calc.critRate) ~= 'number' then
            error(kHitWhere .. ': no calcParams.critRate at its critical roll', 0)
        end
        if type(ws) ~= 'table' then
            error(kHitWhere .. ': no local wsParams at its critical roll', 0)
        end
        hitSeen.crit = true
        local crits = (calc.forcedFirstCrit or calc.mightyStrikesApplicable) and 1 or (ws.critVaries and utils.clamp(calc.critRate, 0, 1) or 0)
        return branch({ { 0, crits }, { 1, 1 - crits } })
    end
    error(kHitWhere .. ': a third roll the bank does not know', 0)
end

-- A die's answer in a read: a hit's own roll; one rolled into a comparison,
-- both ways by its read chance; a die of two values, both ways; any other
-- at its midpoint
local function dieValue(kind, lo, hi, frame)
    if frame == nil then
        return (lo + hi) / 2
    end
    if kind == 'float' and hitStep ~= nil and frame.func == hitStep then
        return hitRoll(frame)
    end
    local whole = kind ~= 'float' and kind ~= 'unit'
    if whole then
        lo, hi = math.floor(lo + 0.5), math.floor(hi + 0.5)
    end
    local cmp = comparisonAt(frame, kind == 'unit' and 'any' or kind)
    if cmp then
        local threshold = valueIn(frame, cmp.expr) * cmp.divisor
        return branch(waysOf(whole, lo, hi, cmp.op, threshold))
    end
    if whole then
        if lo == 1 and (hi == 100 or hi == 10000) then
            error(string.format('%s: a die of 1 to %d in no comparison the bank reads', frame.where, hi), 0)
        end
        if hi - lo == 1 then
            return branch({ { lo, 0.5 }, { hi, 0.5 } })
        end
    end
    return (lo + hi) / 2
end

-- A step run with the read dice in place of the server's, put back after
local function onReadDice(step)
    local random, randomInt, randomFloat, randomNormal = math.random, math.randomInt, math.randomFloat, math.randomNormal
    math.randomInt = function(lo, hi)
        local value = dieValue('int', lo, hi, frameOf(2))
        return value
    end
    math.randomFloat = function(lo, hi)
        local value = dieValue('float', lo, hi, frameOf(2))
        return value
    end
    math.random = function(lo, hi)
        local value
        if lo == nil then
            value = dieValue('unit', 0, 1, frameOf(2))
        elseif hi == nil then
            value = dieValue('any', 1, lo, frameOf(2))
        else
            value = dieValue('any', lo, hi, frameOf(2))
        end
        return value
    end
    math.randomNormal = function(mean, _, lower, upper)
        return utils.clamp(mean, lower or -math.huge, upper or math.huge)
    end
    local ok, result = pcall(step)
    math.random, math.randomInt, math.randomFloat, math.randomNormal = random, randomInt, randomFloat, randomNormal
    if not ok then
        error(result, 0)
    end
    return result
end

-- calculateRawWSDmg's own hit step, found by its name (an upvalue of it),
-- with where it sits; an error naming the file when it is gone
local function hitStepOf()
    local raw = xi.weaponskills.calculateRawWSDmg
    if type(raw) ~= 'function' then
        error('scripts/globals/weaponskills.lua: no xi.weaponskills.calculateRawWSDmg', 0)
    end
    for i = 1, 250 do
        local name, value = debug.getupvalue(raw, i)
        if name == nil then
            break
        end
        if name == 'getSingleHitDamage' and type(value) == 'function' then
            return raw, i, value
        end
    end
    error('scripts/globals/weaponskills.lua: xi.weaponskills.calculateRawWSDmg no longer reaches getSingleHitDamage', 0)
end

-- calculateRawWSDmg with the read's own hit in place of its hit step: a twin
-- of it (its own bytecode, loaded again) sharing every upvalue of it but
-- that one, which the twin holds alone, put in xi.weaponskills for the read
-- and taken out after. The server's own closure is never touched: LuaJIT
-- compiles a closure's unchanging upvalues into its traces, so a hit step
-- swapped in place there could go on being called, or not, from compiled
-- code. Made once for each calculateRawWSDmg; the hit it calls is the read's
-- (readingHit)
local readingHit = nil
local twins      = setmetatable({}, { __mode = 'k' })

local function twinOf(raw, at)
    local twin = twins[raw]
    if twin ~= nil then
        return twin
    end
    local dumped = loadstring(string.dump(raw))
    if dumped == nil then
        error('scripts/globals/weaponskills.lua: xi.weaponskills.calculateRawWSDmg cannot be loaded again', 0)
    end
    setfenv(dumped, getfenv(raw))
    for i = 1, 250 do
        local name = debug.getupvalue(raw, i)
        if name == nil then
            break
        end
        if i ~= at then
            debug.upvaluejoin(dumped, i, raw, i)
        end
    end
    debug.setupvalue(dumped, at, function(...)
        return readingHit(...)
    end)
    twins[raw] = dumped
    return dumped
end

-- Ways a hit's own dice can fall, and a weapon skill's outside its hits
local kHitWays = 128
local kWsWays  = 512

-- The read price (the user's design, 2026-10-17): the weapon skill's own
-- script on the stand-ins, its dice read. Each hit is its expectation --
-- hit chance x (crit chance x its critical's damage + the rest x its
-- ordinary damage), the hit step run every way its own dice fall (its miss
-- and critical read off calcParams; pDIF's spike, its coin, a parry, a
-- shadow, by their lines) -- worked out once a kind of hit and handed on in
-- the hit's place, so what the script does with it after (the target's
-- reductions, the weapon skill's bonuses, its floors) is the server's own.
-- Outside its hits the run goes every way its dice fall -- a multi-attack's
-- procs, a magical weapon skill's day and weather -- and the resist roll is
-- taken at its average as the nukes' is (resistOf, resistFor). The same
-- answer every time; an error naming what it could not find
local function wsRead(attacker, target, wsId, script, tp)
    local raw, at, step = hitStepOf()
    local stage = wsStage(attacker, target)
    local known = {}

    local function expectedHit(actor, foe, dmg, ftp, wsParams, calcParams)
        local info = calcParams.attackInfo or {}
        local key  = table.concat({ tostring(dmg), tostring(ftp), tostring(calcParams.hitRate), tostring(calcParams.critRate), tostring(calcParams.guaranteedHit),
            tostring(calcParams.mustMiss), tostring(calcParams.forcedFirstCrit), tostring(calcParams.mightyStrikesApplicable), tostring(info.slot),
            tostring(info.weaponType), tostring(calcParams.attackType) }, ':')
        local hit = known[key]
        if hit == nil then
            local pdif, landed = 0, 0
            local value = walkOver(function(walk)
                local copy = {}
                for k, v in pairs(calcParams) do
                    copy[k] = v
                end
                local outer = hitSeen
                hitSeen     = { miss = false, crit = false }
                local ok, d = pcall(step, actor, foe, dmg, ftp, wsParams, copy)
                local seen  = hitSeen
                hitSeen     = outer
                if not ok then
                    error(d, 0)
                end
                if not seen.miss then
                    error(kHitWhere .. ': no longer rolls its miss with math.randomFloat', 0)
                end
                if d > 0 and not seen.crit then
                    error(kHitWhere .. ': no longer rolls its critical with math.randomFloat', 0)
                end
                if d > 0 then
                    pdif   = pdif + walk.weight * (copy.pdif or 0)
                    landed = landed + walk.weight
                end
                return d
            end, kHitWays)
            hit        = { value = value, landed = landed, pdif = landed > 0 and pdif / landed or nil }
            known[key] = hit
        end
        -- What the script reads of the hit after it: the pDIF a Sneak
        -- Attack's DEX bonus is weighed by, and that it landed
        if hit.pdif ~= nil then
            calcParams.pdif = hit.pdif
        end
        if hit.landed > 0 then
            calcParams.hitsLanded = (calcParams.hitsLanded or 0) + 1
        end
        return hit.value, calcParams
    end

    local resist = xi.combat.magicHitRate.calculateResistRate
    local averaged
    averaged = function(actor, foe, params)
        xi.combat.magicHitRate.calculateResistRate = resist
        local ok, rate = pcall(function()
            return resistFor(resistOf(actor, foe, params), 0)
        end)
        xi.combat.magicHitRate.calculateResistRate = averaged
        if not ok then
            error(rate, 0)
        end
        return rate
    end

    local twin = twinOf(raw, at)
    xi.weaponskills.calculateRawWSDmg          = twin
    readingHit                                 = expectedHit
    hitStep                                    = step
    xi.combat.magicHitRate.calculateResistRate = averaged
    local ok, price = pcall(onReadDice, function()
        return walkOver(function()
            return wsRun(stage, script, wsId, tp)
        end, kWsWays)
    end)
    xi.combat.magicHitRate.calculateResistRate = resist
    hitStep                                    = nil
    readingHit                                 = nil
    xi.weaponskills.calculateRawWSDmg          = raw
    if not ok then
        error(price, 0)
    end
    return price
end

local function wsScriptOf(name)
    local scripts = xi.actions and xi.actions.weaponskills
    local script  = scripts ~= nil and scripts[name] or nil
    if script == nil or type(script.onUseWeaponSkill) ~= 'function' then
        return nil
    end
    return script
end

-- What a weapon skill deals the target on average (#496; ROADMAP, Road to
-- subjob item 6, slice 3h: the chain's worth weighs each member's weapon
-- skill): its read price (wsRead), the same every time. Where the read
-- cannot find what it reads -- an upstream change to weaponskills.lua, a
-- die the bank cannot read -- the weapon skill is priced by kWsRuns runs of
-- its own script over the bank's seeded dice instead, and the answer says
-- so: the price, 'read' or 'sampled', and for a sampled one why the read
-- failed. Left out: what the actor's state would add when it lands (a Sneak
-- Attack not yet up, the TP she holds over `tp`) and the game's last step
-- (a mob's Phalanx, Stoneskin, damage cap). nil when the weapon skill has
-- no script
xi.cardian.bank.wsExpected = function(attacker, target, wsId, name, tp)
    local script = wsScriptOf(name)
    if script == nil then
        return nil
    end
    local ok, price = pcall(wsRead, attacker, target, wsId, script, tp)
    if ok then
        return price, 'read'
    end
    return wsSampled(attacker, target, wsId, script, tp).mean, 'sampled', tostring(price)
end

-- Each way alone, for the tests: the read price, erroring as it fails; the
-- sampled one, its mean and its runs' spread
xi.cardian.bank.wsRead = function(attacker, target, wsId, name, tp)
    local script = wsScriptOf(name)
    return script ~= nil and wsRead(attacker, target, wsId, script, tp) or nil
end

xi.cardian.bank.wsSampled = function(attacker, target, wsId, name, tp)
    local script = wsScriptOf(name)
    if script == nil then
        return nil
    end
    local sampled = wsSampled(attacker, target, wsId, script, tp)
    return sampled.mean, sampled.spread
end

-- The nukes' fallback (nukeSeeds, where its read of the resist roll fails):
-- each spell's own script (xi.actions.spells, found by its name) run
-- kWsRuns times on stand-ins over the bank's seeded dice, its damage chain
-- whole, the mean of what each run would deal. The target carries no
-- skillchain for it, so no run bursts; with intDown, the same runs on a
-- target whose INT reads that much lower
local function spellScriptOf(name)
    local folders = xi.actions and xi.actions.spells
    if folders == nil or name == nil then
        return nil
    end
    for _, folder in pairs(folders) do
        local script = type(folder) == 'table' and folder[name] or nil
        if type(script) == 'table' and type(script.onSpellCast) == 'function' then
            return script
        end
    end
    return nil
end

local function spellStage(s, target)
    local answers =
    {
        getID              = s.id,
        getElement         = s.element,
        getSkillType       = s.skillType,
        getSpellGroup      = s.spellGroup,
        getSpellFamily     = s.family,
        getTotalTargets    = 1,
        getPrimaryTargetID = target:getID(),
        getFlag            = 0,
        getAoE             = 0,
        getRadius          = 0,
        getMPCost          = 0,
        getCastTime        = 0,
        getKnockback       = 0,
        isAoE              = false,
        isConal            = false,
        isCritical         = false,
    }
    return setmetatable({}, {
        __index = function(self, method)
            local answer = answers[method]
            local call   = function()
                return answer
            end
            rawset(self, method, call)
            return call
        end,
    })
end

sampledNukes = function(caster, target, spells, intDown)
    local actor = inert(caster)
    local foe   = inert(target,
    {
        getHP           = target:getMaxHP(),
        getStatusEffect = function(_, id, ...)
            if id == xi.effect.SKILLCHAIN then
                return nil
            end
            local effect = target:getStatusEffect(id, ...)
            return effect ~= nil and inertObject(effect) or nil
        end,
        hasStatusEffect = function(_, id, ...)
            if id == xi.effect.SKILLCHAIN then
                return false
            end
            return target:hasStatusEffect(id, ...)
        end,
    })
    local moved      = intDown ~= nil and intDown ~= 0 and withIntDown(foe, intDown) or nil
    local seeds      = {}
    local seedsMoved = moved ~= nil and {} or nil
    local function mean(script, on, spell)
        return onSeededDice(function()
            local sum = 0
            for _ = 1, kWsRuns do
                local dealt = script.onSpellCast(actor, on, spell)
                sum         = sum + math.max(0, type(dealt) == 'number' and dealt or 0)
            end
            return sum / kWsRuns
        end)
    end
    for _, s in ipairs(spells) do
        local script = spellScriptOf(s.name)
        if script ~= nil and xi.spells.damage.pTable[s.id] ~= nil then
            local spell = spellStage(s, target)
            seeds[s.id] = mean(script, foe, spell)
            if seedsMoved ~= nil then
                seedsMoved[s.id] = mean(script, moved, spell)
            end
        end
    end
    return seeds, seedsMoved
end

-- A debuff spell's own script, run on stand-ins: the caster answers 0 to
-- every question, the target carries nothing and keeps what it is given,
-- and the opening hit's damage chain answers 0. What the script hands
-- addStatusEffect for the effect is what it would put on a mob
xi.cardian.bank.debuffSubPower = function(scriptName, effectId)
    local script = nil
    for _, school in pairs(xi.actions and xi.actions.spells or {}) do
        if type(school) == 'table' and type(school[scriptName]) == 'table' then
            script = school[scriptName]
            break
        end
    end

    if script == nil or type(script.onSpellCast) ~= 'function' then
        return nil
    end

    local function inert()
        return 0
    end

    local function none()
        return nil
    end

    local function no()
        return false
    end

    local recorded = nil
    local caster   = setmetatable({ getStatusEffect = none, hasStatusEffect = no }, { __index = function() return inert end })
    local target   = setmetatable({
        getStatusEffect = none,
        hasStatusEffect = no,
        addStatusEffect = function(_, effect, params)
            if effect == effectId and type(params) == 'table' then
                recorded = params.subPower
            end

            return true
        end,
    }, { __index = function() return inert end })
    local spell    = setmetatable({}, { __index = function() return inert end })

    local damage = xi.spells.damage.useDamageSpell
    xi.spells.damage.useDamageSpell = inert
    local ok = pcall(script.onSpellCast, caster, target, spell)
    xi.spells.damage.useDamageSpell = damage
    if not ok or type(recorded) ~= 'number' then
        return nil
    end

    return recorded
end

-- What a Regen tier would put on a member: its own script run with the real
-- caster, whose gear, merits and job points the enhancing chain reads, on a
-- stand-in of the target that answers as the target does and keeps nothing.
-- The power each tick heals and the duration in seconds, or nil
xi.cardian.bank.expectedRegen = function(caster, target, s)
    local script = spellScriptOf(s.name)
    if script == nil then
        return nil
    end

    local power    = nil
    local duration = nil
    local standIn  = setmetatable({
        addStatusEffect = function(_, effect, params)
            if effect == xi.effect.REGEN and type(params) == 'table' then
                power    = params.power
                duration = params.duration
            end

            return true
        end,

        delStatusEffect       = function() return true end,
        delStatusEffectSilent = function() return true end,
    }, {
        __index = function(self, method)
            local call = function(_, ...)
                return target[method](target, ...)
            end
            rawset(self, method, call)
            return call
        end,
    })

    local ok, err = pcall(script.onSpellCast, caster, standIn, spellStage(s, target))
    if not ok then
        error(err)
    end

    if type(power) ~= 'number' or type(duration) ~= 'number' then
        return nil
    end

    return power, duration
end
