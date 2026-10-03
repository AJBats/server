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

    -- The nuke seed reads the resist roll's own locals rather than copying
    -- its formula; an upstream rename of them fails here, not in play
    it('nukeSeeds prices nukes on a mob without rolling, each tier above the last', function()
        local blm = xi.test.world:spawnPlayer({ job = xi.job.BLM, level = 30, zone = xi.zone.EAST_RONFAURE })
        local mob = blm.entities:moveTo(17190976) -- a Forest Hare (data/zones/East_Ronfaure/mobs.yaml)
        mob:respawn()

        local function nuke(id, element, family)
            return { id = id, element = element, skillType = xi.skill.ELEMENTAL_MAGIC, spellGroup = xi.magic.spellGroup.BLACK, family = family }
        end

        local spells =
        {
            nuke(xi.magic.spell.FIRE, xi.element.FIRE, 1),
            nuke(xi.magic.spell.FIRE_II, xi.element.FIRE, 1),
            nuke(xi.magic.spell.STONE, xi.element.EARTH, 2),
        }

        local randomInt, randomFloat = math.randomInt, math.randomFloat
        local seeds                  = xi.cardian.bank.nukeSeeds(blm, mob, spells)
        assert(math.randomInt == randomInt and math.randomFloat == randomFloat, 'the dice are put back')

        for _, s in ipairs(spells) do
            assert(type(seeds[s.id]) == 'number' and seeds[s.id] > 0, string.format('spell %d should seed to a positive number: %s', s.id, tostring(seeds[s.id])))
        end
        assert(seeds[xi.magic.spell.FIRE_II] > seeds[xi.magic.spell.FIRE], string.format('Fire II should out-deal Fire: %.1f against %.1f', seeds[xi.magic.spell.FIRE_II], seeds[xi.magic.spell.FIRE]))

        -- Nothing rolled, so the same answer twice
        local again = xi.cardian.bank.nukeSeeds(blm, mob, spells)
        for _, s in ipairs(spells) do
            assert(again[s.id] == seeds[s.id], string.format('spell %d seeded %s, then %s', s.id, tostring(seeds[s.id]), tostring(again[s.id])))
        end

        -- A partial chance to nullify is priced as if it never happens (so
        -- nothing is rolled); a certain one zeroes the element and no other
        local earthNull = xi.data.element.getElementalNullificationModifier(xi.element.EARTH)
        mob:setMod(earthNull, 50)
        local partial = xi.cardian.bank.nukeSeeds(blm, mob, spells)
        assert(partial[xi.magic.spell.STONE] == seeds[xi.magic.spell.STONE], string.format('a half chance to nullify earth moved Stone: %s against %s', tostring(partial[xi.magic.spell.STONE]), tostring(seeds[xi.magic.spell.STONE])))
        mob:setMod(earthNull, 100)
        local certain = xi.cardian.bank.nukeSeeds(blm, mob, spells)
        assert(certain[xi.magic.spell.STONE] == 0, string.format('earth nullified, Stone should seed 0: %s', tostring(certain[xi.magic.spell.STONE])))
        assert(certain[xi.magic.spell.FIRE] == seeds[xi.magic.spell.FIRE], 'earth nullified, Fire should not move')
        mob:setMod(earthNull, 0)

        -- A spell the damage table does not know is left out
        local protect = xi.cardian.bank.nukeSeeds(blm, mob, { { id = xi.magic.spell.PROTECT, element = xi.element.LIGHT, skillType = xi.skill.ENHANCING_MAGIC, spellGroup = xi.magic.spellGroup.WHITE, family = 3 } })
        assert(protect[xi.magic.spell.PROTECT] == nil, 'Protect is not in the damage table')
    end)
end)
