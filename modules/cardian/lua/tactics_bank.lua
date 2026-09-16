-----------------------------------
-- Cardian: the MP bank's samplers (RESEARCH §12.13)
--
-- Two of the server's formulas roll dice inside, so the bank samples them
-- here, one call each, instead of a hundred round trips from C++
-- (src/map/pawn/spell_bank.cpp):
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

xi.cardian.bank.expectedPdif = function(actor, defence, level, weaponType, rolls)
    local target    = standIn(defence, level)
    local corrected = xi.data.levelCorrection.isLevelCorrectedZone(actor)
    local sum       = 0
    for _ = 1, rolls do
        sum = sum + xi.combat.physical.calculateMeleePDIF(actor, target, weaponType, 1, false, corrected, false, 0, false, xi.slot.MAIN, false)
    end
    return sum / rolls
end
