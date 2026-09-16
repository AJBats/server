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
end)
