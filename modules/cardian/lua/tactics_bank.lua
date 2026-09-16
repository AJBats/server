-----------------------------------
-- Cardian: the MP bank's samplers (RESEARCH §12.13)
--
-- The server's own formulas, asked once from Lua instead of a hundred round
-- trips from C++ (src/map/pawn/spell_bank.cpp). Two of them roll dice
-- inside, so the bank samples those here:
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
