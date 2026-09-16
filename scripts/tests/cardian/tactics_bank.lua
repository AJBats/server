-----------------------------------
-- Cardian: the MP bank's samplers
-- (modules/cardian/lua/tactics_bank.lua)
--
-- The bank prices Dia from the melee pDIF formula run against a stand-in
-- target at any defence, and every enfeeble's land chance from the resist
-- roll. The user's acceptance test (RESEARCH §12.13): the sampler must
-- agree with itself at one defence before a fight with Dia is trusted, and
-- must read harder at a lower defence.
--
-- The pawn module loads the samplers at init; under xi_test there is no
-- pawn module, so the test loads them itself.
-----------------------------------
require('modules/cardian/lua/tactics_bank')

describe('Cardian tactics bank', function()
    local kRolls = 400

    ---@type CClientEntityPair
    local player

    before_each(function()
        xi.test.world:setSeed(1)
        player = xi.test.world:spawnPlayer({ job = xi.job.WAR, level = 20 })
    end)

    it('is loaded', function()
        assert(xi.cardian and xi.cardian.bank, 'modules/cardian/lua/tactics_bank.lua did not load')
    end)

    it('expectedPdif agrees with itself at one defence', function()
        local a = xi.cardian.bank.expectedPdif(player, 60, 20, xi.skill.SWORD, kRolls)
        local b = xi.cardian.bank.expectedPdif(player, 60, 20, xi.skill.SWORD, kRolls)
        assert(a > 0 and b > 0, string.format('pDIF should be positive: %.3f, %.3f', a, b))
        local ratio = a / b
        assert(ratio > 0.95 and ratio < 1.05, string.format('two batches at one defence should agree within 5%%: %.3f', ratio))
    end)

    it('expectedPdif reads harder at a lower defence', function()
        local base = xi.cardian.bank.expectedPdif(player, 100, 20, xi.skill.SWORD, kRolls)
        local down = xi.cardian.bank.expectedPdif(player, 90, 20, xi.skill.SWORD, kRolls)
        assert(down > base, string.format('ten percent less defence should land harder: %.3f against %.3f', down, base))
        assert(down / base < 1.5, string.format('and not absurdly so: %.3f', down / base))
    end)

    it('expectedPdif reports its biggest roll beside the mean', function()
        local mean, biggest = xi.cardian.bank.expectedPdif(player, 60, 20, xi.skill.SWORD, kRolls)
        assert(biggest >= mean, string.format('the biggest roll is at least the mean: %.3f against %.3f', biggest, mean))
    end)

    it('swingBase is a positive number for a bare-handed swing at a player', function()
        local target = xi.test.world:spawnPlayer({ job = xi.job.WAR, level = 20 })
        local base   = xi.cardian.bank.swingBase(player, target, true)
        assert(type(base) == 'number' and base > 0, string.format('a swing has a base damage: %s', tostring(base)))
    end)

    it('landChance is a fraction, and one when the effect always lands', function()
        local target = xi.test.world:spawnPlayer({ job = xi.job.WAR, level = 20 })
        local fed    =
        {
            effectId       = xi.effect.PARALYSIS,
            magicalElement = xi.element.ICE,
            actorStat      = xi.mod.MND,
            skillType      = xi.skill.ENFEEBLING_MAGIC,
            spellGroup     = xi.magic.spellGroup.WHITE,
            bonusMacc      = -10,
            magicBurstTier = 0,
        }
        local chance, rate = xi.cardian.bank.landChance(player, target, fed, 50)
        assert(chance >= 0 and chance <= 1, string.format('a land chance is a fraction: %s', tostring(chance)))
        assert(rate >= 0 and rate <= 1, string.format('a resist rate is a fraction: %s', tostring(rate)))

        local always, full = xi.cardian.bank.landChance(player, target, { effectId = xi.effect.DIA, magicalElement = xi.element.LIGHT }, 0)
        assert(always == 1 and full == 1, 'zero rolls means the effect lands whenever the cast resolves')
    end)

    -- getCureFinal ends on a day/weather roll the cure formula flips one call
    -- in three. Pin it, or the sampler and the cast that checks it are each
    -- reading a different sky
    local function pinTheSky()
        stub('xi.spells.damage.calculateDayAndWeather', 1)
    end

    it('expectedCure is at least the tier\'s minimum and grows by tier', function()
        pinTheSky()

        local whm = xi.test.world:spawnPlayer({ job = xi.job.WHM, level = 30 })

        local cure    = xi.cardian.bank.expectedCure(whm, xi.magic.spell.CURE, xi.element.LIGHT)
        local cureII  = xi.cardian.bank.expectedCure(whm, xi.magic.spell.CURE_II, xi.element.LIGHT)
        local cureIII = xi.cardian.bank.expectedCure(whm, xi.magic.spell.CURE_III, xi.element.LIGHT)

        for tier, pair in pairs({ Cure = { cure, 10 }, ['Cure II'] = { cureII, 60 }, ['Cure III'] = { cureIII, 130 } }) do
            assert(type(pair[1]) == 'number', string.format('%s should sample to a number: %s', tier, tostring(pair[1])))
            assert(pair[1] >= pair[2], string.format('%s should heal at least its minimum %d: %d', tier, pair[2], pair[1]))
        end

        assert(cureII > cure, string.format('Cure II should out-heal Cure: %d against %d', cureII, cure))
        assert(cureIII > cureII, string.format('Cure III should out-heal Cure II: %d against %d', cureIII, cureII))
    end)

    it('expectedCure matches a cast', function()
        pinTheSky()

        local caster = xi.test.world:spawnPlayer({ job = xi.job.WHM, level = 30 })
        local target = xi.test.world:spawnPlayer({ job = xi.job.WAR, level = 30 })

        -- Skill is stored in tenths; above the level cap it reads back as
        -- the cap, enough to put the cast in a real power band rather than
        -- on the tier's floor
        caster:setSkillLevel(xi.skill.HEALING_MAGIC, 1500)
        caster:addSpell(xi.magic.spell.CURE_III)
        caster:setMP(caster:getMaxMP())
        caster:resetRecasts()

        target:setHP(1)

        local sampled = xi.cardian.bank.expectedCure(caster, xi.magic.spell.CURE_III, xi.element.LIGHT, target)
        assert(sampled and sampled > 130, string.format('the cast should land in a band, not on Cure III\'s floor: %s (healing skill %d, cure power %d, MND %d, VIT %d, old formula %s)',
            tostring(sampled), caster:getSkillLevel(xi.skill.HEALING_MAGIC), getCurePower(caster), caster:getStat(xi.mod.MND), caster:getStat(xi.mod.VIT), tostring(xi.settings.main.USE_OLD_CURE_FORMULA)))

        caster.actions:useSpell(target, xi.magic.spell.CURE_III)
        xi.test.world:tickEntity(caster)
        xi.test.world:skipTime(10)

        -- The sampler carries the server's multiplier and the target's own
        -- bonus; only the missing HP caps it after that
        local healed   = target:getHP() - 1
        local expected = math.min(sampled, target:getMaxHP() - 1)

        assert(healed == expected, string.format('the cast healed %d, the sampler said %d', healed, expected))
    end)
end)
