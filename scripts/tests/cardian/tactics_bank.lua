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

        -- Burn's INT down, asked of the seed, comes back as a second answer
        -- in the same pass: the mob's INT lowered as the effect lowers it,
        -- so the same seed as the mob with the mod on, never less than
        -- without it; the first answer is unmoved
        local plain, burned = xi.cardian.bank.nukeSeeds(blm, mob, spells, 9)
        mob:addMod(xi.mod.INT, -9)
        local modded = xi.cardian.bank.nukeSeeds(blm, mob, spells)
        mob:delMod(xi.mod.INT, -9)
        assert(select(2, xi.cardian.bank.nukeSeeds(blm, mob, spells)) == nil, 'no INT down asked, no second answer')
        for _, s in ipairs(spells) do
            assert(plain[s.id] == seeds[s.id], string.format('spell %d: the first answer moved with an INT down asked: %s against %s', s.id, tostring(plain[s.id]), tostring(seeds[s.id])))
            assert(burned[s.id] == modded[s.id], string.format('spell %d: INT down seeded %s, the mod %s', s.id, tostring(burned[s.id]), tostring(modded[s.id])))
            assert(burned[s.id] >= seeds[s.id], string.format('spell %d: INT down seeded %s, under %s', s.id, tostring(burned[s.id]), tostring(seeds[s.id])))
        end

        -- A spell the damage table does not know is left out
        local protect = xi.cardian.bank.nukeSeeds(blm, mob, { { id = xi.magic.spell.PROTECT, element = xi.element.LIGHT, skillType = xi.skill.ENHANCING_MAGIC, spellGroup = xi.magic.spellGroup.WHITE, family = 3 } })
        assert(protect[xi.magic.spell.PROTECT] == nil, 'Protect is not in the damage table')
    end)

    -- The burst reads the server's own two burst steps: a factor over one,
    -- growing with the chain's count, and nothing on the mob consumed
    it('burstFactors multiplies a nuke by the server burst steps, more for a longer chain', function()
        local blm = xi.test.world:spawnPlayer({ job = xi.job.BLM, level = 30, zone = xi.zone.EAST_RONFAURE })
        local mob = blm.entities:moveTo(17190976) -- a Forest Hare (data/zones/East_Ronfaure/mobs.yaml)
        mob:respawn()

        local spells =
        {
            { id = xi.magic.spell.FIRE, element = xi.element.FIRE, skillType = xi.skill.ELEMENTAL_MAGIC },
            { id = xi.magic.spell.STONE, element = xi.element.EARTH, skillType = xi.skill.ELEMENTAL_MAGIC },
        }
        local one = xi.cardian.bank.burstFactors(blm, mob, spells, 1)
        local two = xi.cardian.bank.burstFactors(blm, mob, spells, 2)
        for _, s in ipairs(spells) do
            assert(type(one[s.id]) == 'number' and one[s.id] > 1, string.format('spell %d: a burst should multiply it: %s', s.id, tostring(one[s.id])))
            assert(two[s.id] > one[s.id], string.format('spell %d: a longer chain bursts harder: %s against %s', s.id, tostring(two[s.id]), tostring(one[s.id])))
            local expected = xi.spells.damage.calculateIfMagicBurst(blm, mob, s.element, 1) *
                xi.spells.damage.calculateIfMagicBurstBonus(blm, mob, s.id, s.skillType, s.element)
            assert(math.abs(one[s.id] - expected) < 1e-9, string.format('spell %d: %s, the server says %s', s.id, tostring(one[s.id]), tostring(expected)))
        end

        -- Sengikori's bonus is read, never consumed
        mob:setMod(xi.mod.SENGIKORI_MB_DMG_DEBUFF, 25)
        local sengi = xi.cardian.bank.burstFactors(blm, mob, spells, 1)
        assert(mob:getMod(xi.mod.SENGIKORI_MB_DMG_DEBUFF) == 25, 'the bonus is left on the mob')
        assert(sengi[xi.magic.spell.FIRE] > one[xi.magic.spell.FIRE], 'and counted')
        mob:setMod(xi.mod.SENGIKORI_MB_DMG_DEBUFF, 0)
    end)

    -- The chain's worth reads the server's own skillchain damage: a share of
    -- the closing weapon skill that grows with the chain's level, and
    -- nothing on the mob taken or left behind
    it('chainShare is the server skillchain damage for each point of the closing weapon skill', function()
        local war = xi.test.world:spawnPlayer({ job = xi.job.WAR, level = 30, zone = xi.zone.EAST_RONFAURE })
        local mob = war.entities:moveTo(17190976) -- a Forest Hare (data/zones/East_Ronfaure/mobs.yaml)
        mob:respawn()
        local hp = mob:getHP()

        local share = xi.cardian.bank.chainShare
        local one   = share(war, mob, xi.skillchainType.LIQUEFACTION, 1, 1)
        local two   = share(war, mob, xi.skillchainType.FUSION, 2, 1)
        local three = share(war, mob, xi.skillchainType.LIGHT, 3, 1)
        assert(type(one) == 'number' and one > 0 and one < 1, string.format('a level 1 chain deals a share of its weapon skill: %s', tostring(one)))
        assert(two > one, string.format('a level 2 chain deals more: %s against %s', tostring(two), tostring(one)))
        assert(three > two, string.format('a level 3 chain more again: %s against %s', tostring(three), tostring(two)))
        assert(share(war, mob, xi.skillchainType.LIQUEFACTION, 1, 2) > one, 'a longer chain deals more')

        assert(mob:getHP() == hp, 'the mob takes nothing')
        assert(not mob:hasStatusEffect(xi.effect.SKILLCHAIN), 'and carries no chain after')
    end)

    -- The era module's skillchain damage (prod's) adds a resist roll, which
    -- halves the damage up to three times by chance: the share prices it at
    -- its average and never rolls, so it is the same whatever the dice say.
    -- xi_test runs the dev profile, which loads no era module: its override
    -- is read from the script itself and put in place for the test alone
    it('chainShare prices the era formula\'s resist roll at its average, the same every time', function()
        local before = #xi.module.registry
        require('modules/era/lua/combat/skillchain')
        local eraDamage = nil
        for i = #xi.module.registry, 1, -1 do
            local module = xi.module.registry[i]
            if module.name == 'era_skillchain' then
                for _, override in ipairs(module.overrides) do
                    if override.name == 'xi.combat.skillchain.calculateSkillchainDamage' then
                        eraDamage = override.func
                    end
                end
                if i > before then
                    table.remove(xi.module.registry, i) -- declared for this test only: no loader applies it
                end
            end
        end
        assert(eraDamage ~= nil, 'modules/era/lua/combat/skillchain.lua no longer overrides calculateSkillchainDamage')

        local war = xi.test.world:spawnPlayer({ job = xi.job.WAR, level = 30, zone = xi.zone.EAST_RONFAURE })
        local mob = war.entities:moveTo(17190976) -- a Forest Hare (data/zones/East_Ronfaure/mobs.yaml)
        mob:respawn()
        local hp      = mob:getHP()
        local current = xi.cardian.bank.chainShare(war, mob, xi.skillchainType.FUSION, 2, 1)

        local upstream = xi.combat.skillchain.calculateSkillchainDamage
        local roll     = xi.combat.magicHitRate.calculateResistRate
        xi.combat.skillchain.calculateSkillchainDamage = eraDamage
        local ok, err = pcall(function()
            local first = xi.cardian.bank.chainShare(war, mob, xi.skillchainType.FUSION, 2, 1)
            assert(type(first) == 'number' and first > 0, string.format('the era formula prices the chain: %s', tostring(first)))
            -- its resist at its average: at most the formula without one
            assert(first <= current + 1e-9, string.format('a resist never adds: %s against %s', tostring(first), tostring(current)))
            for seed = 1, 20 do
                xi.test.world:setSeed(seed)
                local again = xi.cardian.bank.chainShare(war, mob, xi.skillchainType.FUSION, 2, 1)
                assert(again == first, string.format('seed %d: %s, first %s: the resist was rolled', seed, tostring(again), tostring(first)))
            end
            -- and no die is thrown for it: the game's own fraction roll is
            -- never reached
            local randomFloat = math.randomFloat
            local thrown      = 0
            math.randomFloat  = function(...)
                thrown = thrown + 1
                return randomFloat(...)
            end
            local rolled, errRolled = pcall(xi.cardian.bank.chainShare, war, mob, xi.skillchainType.FUSION, 2, 1)
            math.randomFloat = randomFloat
            assert(rolled, errRolled)
            assert(thrown == 0, string.format('the share rolled the dice %d times', thrown))
        end)
        xi.combat.skillchain.calculateSkillchainDamage = upstream
        assert(ok, err)
        assert(xi.combat.magicHitRate.calculateResistRate == roll, 'the resist roll is put back')
        assert(mob:getHP() == hp, 'the mob takes nothing')
    end)

    -- A weapon skill's price (#496): the server's own weapon skill, its
    -- script and xi.weaponskills, run on stand-ins over the bank's seeded
    -- dice -- the same answer every time, the server's dice put back, and
    -- nothing on the actor or the mob spent, taken or added
    local function swordsman()
        local war = xi.test.world:spawnPlayer({ job = xi.job.WAR, level = 30, zone = xi.zone.EAST_RONFAURE })
        local mob = war.entities:moveTo(17190976) -- a Forest Hare (data/zones/East_Ronfaure/mobs.yaml)
        mob:respawn()
        war.entities:moveTo(17190976) -- beside it where it respawned
        war:setSkillLevel(xi.skill.SWORD, 1500)
        war:addItem(xi.item.BRONZE_SWORD)
        war:equipItem(xi.item.BRONZE_SWORD, nil, xi.slot.MAIN)
        return war, mob
    end

    it('wsExpected prices a weapon skill by its own script, the same every time, changing nothing', function()
        local war, mob = swordsman()
        war:setTP(1500)
        war:addStatusEffect(xi.effect.SNEAK_ATTACK, { power = 1, duration = 60, origin = war })
        war:addStatusEffect(xi.effect.BOOST, { power = 1, duration = 60, origin = war })
        local hp = mob:getHP()
        local ce = mob:getCE(war)

        local bank                           = xi.cardian.bank
        local random, randomInt, randomFloat = math.random, math.randomInt, math.randomFloat
        local take                           = xi.weaponskills.takeWeaponskillDamage
        local fast                           = bank.wsExpected(war, mob, xi.weaponskill.FAST_BLADE, 'fast_blade', 1000)
        local burning                        = bank.wsExpected(war, mob, xi.weaponskill.BURNING_BLADE, 'burning_blade', 1000)
        assert(math.random == random and math.randomInt == randomInt and math.randomFloat == randomFloat, 'the dice are put back')
        assert(xi.weaponskills.takeWeaponskillDamage == take, 'the game\'s own last step is put back')

        assert(type(fast) == 'number' and fast > 0, string.format('Fast Blade prices to a positive number: %s', tostring(fast)))
        assert(type(burning) == 'number' and burning > 0, string.format('Burning Blade (magical) prices to a positive number: %s', tostring(burning)))
        assert(bank.wsExpected(war, mob, xi.weaponskill.FAST_BLADE, 'fast_blade', 1000) == fast, 'the same answer twice')
        -- more TP, a bigger fTP: Fast Blade's 1.0 at 1000 rises to 1.5 at 2000
        local fast2000 = bank.wsExpected(war, mob, xi.weaponskill.FAST_BLADE, 'fast_blade', 2000)
        assert(fast2000 > fast, string.format('more TP deals more: %.1f at 2000 against %.1f at 1000', fast2000, fast))
        assert(bank.wsExpected(war, mob, 0, 'no_such_weaponskill', 1000) == nil, 'a weapon skill without a script prices nil')

        assert(mob:getHP() == hp, 'the mob takes nothing')
        assert(mob:getCE(war) == ce, 'and gains no enmity')
        assert(not mob:hasStatusEffect(xi.effect.SKILLCHAIN), 'and carries no opening')
        assert(war:getTP() == 1500, string.format('her TP is untouched: %d', war:getTP()))
        assert(war:hasStatusEffect(xi.effect.SNEAK_ATTACK), 'her Sneak Attack is not spent')
        assert(war:hasStatusEffect(xi.effect.BOOST), 'her Boost is not spent')
    end)

    -- The price against the real thing: the mean of what the weapon skill
    -- lands when used, within three standard errors of the two means -- the
    -- landed one's and the price's own runs' -- read off the landings' own
    -- spread, so the bound holds whatever the server's dice are seeded to
    it('wsExpected agrees with the weapon skill used for real', function()
        local war, mob = swordsman()
        -- a mob that outlasts every weapon skill: none is cut short at its
        -- HP (a mod is 16 bits; set, so a retry does not add it again)
        mob:setMod(xi.mod.HP, 30000)
        mob:updateHealth()
        mob:setHP(mob:getMaxHP())
        assert(mob:getMaxHP() > 20000, string.format('the mob should carry the HP: %d', mob:getMaxHP()))

        local landed = {}
        war:addListener('WEAPONSKILL_USE', 'CARDIAN_WS_PRICE_TEST', function(_, _, skill, _, _, damage)
            local id           = skill:getID()
            local dealt        = damage or 0
            landed[id]         = landed[id] or { sum = 0, squares = 0, uses = 0 }
            landed[id].sum     = landed[id].sum + dealt
            landed[id].squares = landed[id].squares + dealt * dealt
            landed[id].uses    = landed[id].uses + 1
        end)
        war.actions:engage(mob)
        xi.test.world:skipTime(2)

        local kUses = 80
        local ok, err = pcall(function()
            for _, ws in ipairs({ { xi.weaponskill.FAST_BLADE, 'fast_blade' }, { xi.weaponskill.BURNING_BLADE, 'burning_blade' } }) do
                local price = xi.cardian.bank.wsExpected(war, mob, ws[1], ws[2], 1000)
                -- one asked too soon after the last is refused: asked again
                for _ = 1, 3 * kUses do
                    if landed[ws[1]] ~= nil and landed[ws[1]].uses >= kUses then
                        break
                    end
                    -- beside it and fighting it, at full HP, with a weapon skill's TP
                    if war:checkDistance(mob) > 3 then
                        war.entities:moveTo(17190976)
                    end
                    if not war:isEngaged() then
                        war.actions:engage(mob)
                        xi.test.world:skipTime(1)
                    end
                    mob:setHP(mob:getMaxHP())
                    war:setTP(1000)
                    war.actions:useWeaponskill(mob, ws[1])
                    xi.test.world:skipTime(3)
                end
                local got = landed[ws[1]]
                assert(got ~= nil and got.uses == kUses, string.format('%s: used %s times, not %d', ws[2], tostring(got and got.uses), kUses))
                local mean      = got.sum / got.uses
                local spread    = math.sqrt(math.max(0, got.squares / got.uses - mean * mean))
                local tolerance = 3 * spread * math.sqrt(1 / kUses + 1 / xi.cardian.bank.wsRuns) + 1
                assert(math.abs(mean - price) <= tolerance,
                    string.format('%s: priced %.1f, landed %.1f on average over %d (spread %.1f, allowed %.1f)', ws[2], price, mean, kUses, spread, tolerance))
            end
        end)
        war:removeListener('CARDIAN_WS_PRICE_TEST')
        mob:setMod(xi.mod.HP, 0)
        mob:updateHealth()
        assert(ok, err)
    end)

    -- Every weapon skill script in the game, priced on stand-ins: none
    -- asks a stand-in something it cannot answer, and none changes the
    -- actor or the mob
    it('wsExpected prices every weapon skill script without error, changing nothing', function()
        local war, mob = swordsman()
        war:setTP(2000)
        local hp     = mob:getHP()
        local ce     = mob:getCE(war)
        local failed = {}
        local priced = 0
        for name, script in pairs(xi.actions.weaponskills) do
            if type(script) == 'table' and type(script.onUseWeaponSkill) == 'function' then
                local id         = xi.weaponskill[string.upper(name)] or 0
                local ok, answer = pcall(xi.cardian.bank.wsExpected, war, mob, id, name, 1000)
                if not ok or type(answer) ~= 'number' or answer < 0 then
                    table.insert(failed, string.format('%s: %s', name, tostring(answer)))
                else
                    priced = priced + 1
                end
            end
        end
        assert(#failed == 0, string.format('%d weapon skills would not price: %s', #failed, table.concat(failed, '; ')))
        assert(priced > 150, string.format('the game has its weapon skills: only %d priced', priced))
        assert(mob:getHP() == hp, 'the mob takes nothing')
        assert(mob:getCE(war) == ce, 'and gains no enmity')
        assert(not mob:hasStatusEffect(xi.effect.SKILLCHAIN), 'and carries no opening')
        assert(war:getTP() == 2000, string.format('her TP is untouched: %d', war:getTP()))
    end)

    it('debuffSubPower reads what Dia and Bio scripts put on a mob, overrides included', function()
        local bank = xi.cardian.bank
        assert(bank.debuffSubPower('dia', xi.effect.DIA) == 10, 'Dia lowers defence 10% in upstream\'s script')
        assert(bank.debuffSubPower('dia_ii', xi.effect.DIA) ~= nil, 'Dia II is read')
        assert(bank.debuffSubPower('bio', xi.effect.BIO) == 10, 'Bio lowers attack 10% in upstream\'s script')
        assert(bank.debuffSubPower('no_such_spell', xi.effect.DIA) == nil, 'an unknown script reads nil')

        -- A script overridden as the era module does is read as overridden,
        -- and the damage chain it borrowed is put back after
        local damage = xi.spells.damage.useDamageSpell
        local dia    = xi.actions.spells.white.dia.onSpellCast
        xi.actions.spells.white.dia.onSpellCast = function(caster, target, spell)
            local hit = xi.spells.damage.useDamageSpell(caster, target, spell)
            target:delStatusEffect(xi.effect.BIO)
            target:addStatusEffect(xi.effect.DIA, { power = 1 + caster:getMod(xi.mod.DIA_DOT), duration = 60, origin = caster, tick = 3, subPower = 5, tier = 1 })
            return hit
        end
        local read = bank.debuffSubPower('dia', xi.effect.DIA)
        xi.actions.spells.white.dia.onSpellCast = dia
        assert(read == 5, string.format('the overridden Dia read %s, not 5', tostring(read)))
        assert(xi.spells.damage.useDamageSpell == damage, 'the damage chain was not put back')
    end)
end)
