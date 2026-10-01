-----------------------------------
-- Cardian: the exp formula, fair within a small level spread
-- (modules/cardian/lua/exp_spread.lua; RESEARCH §15)
--
-- The rule's arithmetic is pinned on its pure parts, then proved on real
-- kills: a party kills a mob graded at a chosen level, and each grant is
-- read off the EXPERIENCE_POINTS listener the game fires as it pays (the
-- number after map.EXP_RATE, 1 under xi_test). Upstream's own figures
-- come from the game itself, by running the same kill with the rule
-- switched off, so the module's copies of upstream's lookups are held
-- against the C++ they copy; the one-level figure is read from the base
-- table and the share the game reads, so the tests hold on either
-- profile's table.
--
-- Two things about the harness: a spawned player stands one point under
-- her next level (setLevel), so a grant dings her -- every player has
-- some exp taken off first, but for the test that wants the ding; and
-- some mobs grade below their level (EXP_LVL_MOD, set by the mob's
-- script), so the mob's level is set to land its grade.
-----------------------------------
describe('Cardian exp spread', function()
    -- The module's table; an empty one if it did not load, so the first
    -- test can say so rather than every test tripping over nil
    local rule = xi.cardian and xi.cardian.exp or {}

    -- The suite runs on the shipped spreads whatever the folder's settings
    -- say, and puts back what was loaded
    local loadedAverage = xi.settings.cardian.EXP_AVERAGE_SPREAD
    local loadedClassic = xi.settings.cardian.EXP_CLASSIC_SPREAD
    local shippedTable  = xi.data.experiencePoints.baseTable
    local tableSwapped  = false

    local function ruleOn()
        xi.settings.cardian.EXP_AVERAGE_SPREAD = 3
        xi.settings.cardian.EXP_CLASSIC_SPREAD = 10
    end

    local function ruleOff()
        xi.settings.cardian.EXP_CLASSIC_SPREAD = 0
    end

    before_each(function()
        ruleOn()
        rule.last     = {}
        rule.failures = 0
    end)

    -- The rule's own parts, put back after a test that breaks one
    local shapeOf = rule.shapeOf
    local baseAt  = rule.baseAt

    after_each(function()
        xi.settings.cardian.EXP_AVERAGE_SPREAD = loadedAverage
        xi.settings.cardian.EXP_CLASSIC_SPREAD = loadedClassic
        rule.shapeOf = shapeOf
        rule.baseAt  = baseAt
        if tableSwapped then
            xi.data.experiencePoints.baseTable = shippedTable
            ReloadExperienceData()
            tableSwapped = false
        end
    end)

    -- charutils::GetBaseExp, for the one-level figure
    local function baseExpAt(level, gradedMobLevel)
        local row = xi.data.experiencePoints.baseTable[math.max(-44, math.min(15, gradedMobLevel - level))]
        return row[math.floor((level - 1) / 5) + 1]
    end

    local function share(size)
        return xi.experiencePoints.getExperienceShare(size, false)
    end

    -- The module's own rounding of a blend
    local function blendOf(t, averaged, classic)
        return math.floor((1 - t) * averaged + t * classic + 1e-9)
    end

    -- A party at these levels, gathered on a rabbit in West Ronfaure; the
    -- first is the leader and the one who kills. `onTheEdge` leaves them a
    -- point under their next level
    local function partyOf(levels, onTheEdge)
        local members = {}
        for i, level in ipairs(levels) do
            local member = xi.test.world:spawnPlayer({ job = xi.job.WAR, level = level, zone = xi.zone.WEST_RONFAURE })
            if not onTheEdge then
                member:delExp(500)
            end

            assert(member:getMainLvl() == level, 'precondition: she stands at her level')
            members[i] = member
        end

        local leader = members[1]
        local mob    = leader.entities:moveTo('Wild_Rabbit')
        for i = 2, #members do
            members[i]:setPos(leader:getXPos(), leader:getYPos(), leader:getZPos())
            leader.actions:inviteToParty(members[i])
            members[i].actions:acceptPartyInvite()
        end

        return members, mob
    end

    -- The mob up again at the level that grades as `grade`
    local function gradeAt(mob, grade)
        mob:setLevelRange(grade, grade)
        mob:respawn()
        local modifier = mob:getMod(xi.mod.EXP_LVL_MOD)
        if modifier ~= 0 then
            mob:setLevelRange(grade - modifier, grade - modifier)
            mob:respawn()
        end

        assert(mob:getMainLvl() + mob:getMod(xi.mod.EXP_LVL_MOD) == grade,
            string.format('precondition: the mob grades at %d', grade))
    end

    -- The leader kills the mob; what each member was paid, by her place in
    -- the party (nothing for a member the game paid nothing)
    local function kill(members, mob)
        local paid = {}
        for i, member in ipairs(members) do
            member:addListener('EXPERIENCE_POINTS', 'CARDIAN_EXP_SPREAD_TEST', function(_, _, amount)
                paid[i] = amount
            end)
        end

        members[1]:claimAndKillMob(mob)
        return paid
    end

    -- A party at these levels kills a mob graded at `grade`
    local function partyKills(levels, grade)
        local members, mob = partyOf(levels)
        gradeAt(mob, grade)
        return kill(members, mob), members, mob
    end

    -- The same kill with the rule off: upstream itself
    local function upstreamPays(levels, grade)
        ruleOff()
        local paid = partyKills(levels, grade)
        ruleOn()
        return paid
    end

    local function recordOf(member)
        return rule.last[member:getID()]
    end

    describe('the arithmetic', function()
        it('is loaded', function()
            assert(rule and rule.blendFor and rule.shapeOf and rule.baseAt and rule.levelOf,
                'modules/cardian/lua/exp_spread.lua did not load')
        end)

        it('is one level through a spread of 3, a straight line to 10, upstream from there', function()
            for spread = 0, 3 do
                assert(rule.blendFor(spread) == 0, string.format('spread %d is one level', spread))
            end

            for spread = 4, 9 do
                local expected = (spread - 3) / 7
                assert(math.abs(rule.blendFor(spread) - expected) < 1e-9,
                    string.format('spread %d blends %.4f, expected %.4f', spread, rule.blendFor(spread), expected))
            end

            for spread = 10, 30 do
                assert(rule.blendFor(spread) == 1, string.format('spread %d is upstream', spread))
            end
        end)

        it('never blends backwards as the spread widens', function()
            local last = 0
            for spread = 0, 30 do
                local t = rule.blendFor(spread)
                assert(t >= last and t >= 0 and t <= 1, string.format('spread %d blends %.4f after %.4f', spread, t, last))
                last = t
            end
        end)

        it('follows its two settings', function()
            xi.settings.cardian.EXP_AVERAGE_SPREAD = 2
            xi.settings.cardian.EXP_CLASSIC_SPREAD = 6
            assert(rule.blendFor(2) == 0 and rule.blendFor(6) == 1, 'the ends moved with the settings')
            assert(math.abs(rule.blendFor(4) - 0.5) < 1e-9, 'and the line between them')
        end)

        it('is upstream at every spread with the classic spread at 0', function()
            ruleOff()
            for spread = 0, 12 do
                assert(rule.blendFor(spread) == 1, string.format('spread %d is upstream', spread))
            end
        end)

        it('survives the two settings meeting', function()
            xi.settings.cardian.EXP_AVERAGE_SPREAD = 5
            xi.settings.cardian.EXP_CLASSIC_SPREAD = 5
            assert(rule.blendFor(4) == 0 and rule.blendFor(5) == 1 and rule.blendFor(6) == 1, 'one level below the meeting point, upstream from it')
        end)

        it('reads a party\'s shape: the spread, and the average rounded down', function()
            local function shape(levels, top)
                local spread, average = rule.shapeOf(levels, top)
                return string.format('%d/%d', spread, average)
            end

            assert(shape({ 15, 18 }, 18) == '3/16', 'two members, 16.5 rounds down: ' .. shape({ 15, 18 }, 18))
            assert(shape({ 15, 18, 18, 17 }, 18) == '3/17', 'four members: ' .. shape({ 15, 18, 18, 17 }, 18))
            assert(shape({ 15, 12, 12, 13 }, 15) == '3/13', 'the player on top: ' .. shape({ 15, 12, 12, 13 }, 15))
            assert(shape({ 20 }, 20) == '0/20', 'alone: ' .. shape({ 20 }, 20))
            assert(shape({ 9, 9, 9 }, 9) == '0/9', 'one level: ' .. shape({ 9, 9, 9 }, 9))
        end)

        it('widens the spread by an outsider the game graded the kill at, and leaves him out of the average', function()
            -- A passer-by of 25 hit the mob: the grade is his
            local spread, average = rule.shapeOf({ 15, 16 }, 25)
            assert(spread == 10 and average == 15, string.format('spread %d, average %d; expected 10, 15', spread, average))
        end)

        it('judges a mob at any level by the thresholds the profile handed the game', function()
            -- xi_test runs the dev profile: scripts/globals/exp_difficulty_curve.lua's
            assert(rule.difficultyOf(200, 16) ~= nil, 'the thresholds were caught as the profile handed them over')
            assert(rule.difficultyOf(200, 16) == xi.mobDifficulty.EVEN_MATCH, 'two hundred is an Even Match')
            assert(rule.difficultyOf(160, 16) == xi.mobDifficulty.DECENT_CHALLENGE, 'one sixty a Decent Challenge')
            assert(rule.difficultyOf(220, 16) == xi.mobDifficulty.TOUGH, 'two twenty Tough')
            assert(rule.difficultyOf(60, 16) == xi.mobDifficulty.EASY_PREY, 'sixty Easy Prey')
            assert(rule.difficultyOf(59, 16) == xi.mobDifficulty.TOO_WEAK, 'under sixty Too Weak')
            assert(rule.difficultyOf(0, 16) == xi.mobDifficulty.TOO_WEAK, 'nothing is Too Weak')
            assert(rule.difficultyOf(1, 60) == xi.mobDifficulty.INCREDIBLY_EASY_PREY, 'a point from a mob of 60 is Incredibly Easy Prey')
        end)

        it('reads the base table by the mob\'s level against hers and her bracket', function()
            local base = xi.data.experiencePoints.baseTable
            assert(rule.baseAt(16, 17) == base[1][4], 'a mob one above a 16: row +1, the 16-20 bracket')
            assert(rule.baseAt(5, 5) == base[0][1] and rule.baseAt(6, 6) == base[0][2], 'the bracket turns between 5 and 6')
            assert(rule.baseAt(10, 60) == base[15][2], 'a mob far above clamps to the top row')
            assert(rule.baseAt(60, 1) == base[-44][12], 'a mob far below clamps to the bottom row')
            assert(rule.baseAt(0, 5) == 0, 'no level, no exp')
        end)
    end)

    describe('on a kill', function()
        it('pays a lone player exactly what upstream does, across levels and brackets', function()
            -- The rule reads the base at her own level, so this holds its
            -- lookup against the game's
            for _, case in ipairs({ { 5, 5 }, { 6, 6 }, { 10, 13 }, { 11, 9 }, { 20, 24 }, { 30, 27 } }) do
                local level, grade = case[1], case[2]
                local paid     = partyKills({ level }, grade)
                local upstream = upstreamPays({ level }, grade)
                assert(upstream[1] and upstream[1] > 0, string.format('precondition: a %d is paid for a level %d mob', level, grade))
                assert(paid[1] == upstream[1],
                    string.format('a lone %d on a level %d mob was paid %s, upstream pays %d', level, grade, tostring(paid[1]), upstream[1]))
            end
        end)

        it('pays a party of one level exactly what upstream does', function()
            local paid     = partyKills({ 15, 15, 15 }, 16)
            local upstream = upstreamPays({ 15, 15, 15 }, 16)
            for i = 1, 3 do
                assert(paid[i] == upstream[i], string.format('member %d was paid %s, upstream pays %s', i, tostring(paid[i]), tostring(upstream[i])))
            end
        end)

        it('pays a duo three levels apart as one level, the average rounded down', function()
            local paid, members = partyKills({ 18, 15 }, 17)

            -- The average of 18 and 15 is 16
            local expected = math.floor(baseExpAt(16, 17) * share(2))
            assert(expected > 0, 'precondition: the mob pays at the average')
            assert(paid[1] == expected and paid[2] == expected,
                string.format('they were paid %s and %s, as one level %d', tostring(paid[1]), tostring(paid[2]), expected))

            for i = 1, 2 do
                local record = recordOf(members[i])
                assert(record and record.spread == 3 and record.average == 16 and record.t == 0 and record.paid == expected and record.averaged == expected,
                    string.format('member %d\'s record says spread %s, average %s, blend %s, paid %s; expected 3, 16, 0, %d', i,
                        tostring(record and record.spread), tostring(record and record.average), tostring(record and record.t),
                        tostring(record and record.paid), expected))
            end
        end)

        it('pays a party of four within the spread one figure', function()
            local paid = partyKills({ 18, 15, 18, 17 }, 17)

            -- 68 over four is 17
            local expected = math.floor(baseExpAt(17, 17) * share(4))
            for i = 1, 4 do
                assert(paid[i] == expected, string.format('member %d was paid %s, as one level %d', i, tostring(paid[i]), expected))
            end
        end)

        it('on an even fight, pays the lower members more than upstream and no member less', function()
            local levels   = { 18, 15, 18, 17 }
            local paid     = partyKills(levels, 17)
            local upstream = upstreamPays(levels, 17)

            for i = 1, 4 do
                assert(paid[i] >= upstream[i], string.format('member %d (level %d) was paid %d, upstream %d', i, levels[i], paid[i], upstream[i]))
            end

            assert(paid[2] > upstream[2], 'the 15 gains on upstream')
        end)

        it('records upstream\'s own figure beside what it pays', function()
            local _, members = partyKills({ 18, 15 }, 17)
            local classicHigh = recordOf(members[1]).classic
            local classicLow  = recordOf(members[2]).classic

            local upstream = upstreamPays({ 18, 15 }, 17)
            assert(upstream[2] < upstream[1], 'precondition: upstream cuts the lower member')
            assert(classicHigh == upstream[1], string.format('the higher member\'s classic figure is %s, upstream paid %d', tostring(classicHigh), upstream[1]))
            assert(classicLow == upstream[2], string.format('the lower member\'s classic figure is %s, upstream paid %d', tostring(classicLow), upstream[2]))
        end)

        it('is the straight blend of the two figures at every spread from 3 to 12', function()
            -- A duo, the higher at 25 on a mob of 25, the lower falling
            -- behind a level at a time. At each step both members' pay is
            -- the blend of the one-level figure and upstream's own
            for _, lowLevel in ipairs({ 22, 21, 20, 19, 18, 17, 16, 15, 14, 13 }) do
                local spread  = 25 - lowLevel
                local average = math.floor((25 + lowLevel) / 2)
                local t       = spread <= 3 and 0 or spread >= 10 and 1 or (spread - 3) / 7

                local paid, members = partyKills({ 25, lowLevel }, 25)
                local upstream      = upstreamPays({ 25, lowLevel }, 25)
                local averaged      = math.floor(baseExpAt(average, 25) * share(2))

                for i = 1, 2 do
                    local expected = blendOf(t, averaged, upstream[i])
                    assert(paid[i] == expected,
                        string.format('25 and %d (spread %d): member %d was paid %s; the blend of %d and %d at %.3f is %d',
                            lowLevel, spread, i, tostring(paid[i]), averaged, upstream[i], t, expected))

                    local record = recordOf(members[i])
                    assert(record.spread == spread and record.average == average and math.abs(record.t - t) < 1e-9 and record.classic == upstream[i],
                        string.format('25 and %d: member %d\'s record says spread %d, average %d, blend %.3f, classic %s',
                            lowLevel, i, record.spread, record.average, record.t, tostring(record.classic)))
                end

                if spread >= 10 then
                    assert(paid[1] == upstream[1] and paid[2] == upstream[2], 'ten levels apart and more, both are paid the figures upstream pays')
                end
            end
        end)

        it('blends a party of four the same way', function()
            -- 20, 14, 18 and 16: spread 6, average 17
            local levels        = { 20, 14, 18, 16 }
            local paid, members = partyKills(levels, 19)
            local upstream      = upstreamPays(levels, 19)
            local averaged      = math.floor(baseExpAt(17, 19) * share(4))

            for i = 1, 4 do
                local expected = blendOf(3 / 7, averaged, upstream[i])
                assert(paid[i] == expected,
                    string.format('member %d (level %d) was paid %s; the blend of %d and %d is %d', i, levels[i], tostring(paid[i]), averaged, upstream[i], expected))
            end

            assert(recordOf(members[2]).spread == 6 and recordOf(members[2]).average == 17, 'the record says spread 6, average 17')
        end)

        it('keeps the party\'s shape when a grant raises a level mid-kill', function()
            -- Both stand a point under their next level, so whoever the
            -- game pays first dings before the other is reckoned
            local members, mob = partyOf({ 18, 15 }, true)
            gradeAt(mob, 17)
            local paid = kill(members, mob)

            assert(members[1]:getMainLvl() == 19 and members[2]:getMainLvl() == 16, 'precondition: both dinged on the grant')

            local expected = math.floor(baseExpAt(16, 17) * share(2))
            assert(paid[1] == expected and paid[2] == expected,
                string.format('they were paid %s and %s; the party that made the kill is one level, %d', tostring(paid[1]), tostring(paid[2]), expected))
            for i = 1, 2 do
                local record = recordOf(members[i])
                assert(record.spread == 3 and record.average == 16,
                    string.format('member %d\'s record says spread %d, average %d; the kill was made at 3 and 16', i, record.spread, record.average))
            end
        end)

        it('reads the shape afresh for the mob\'s next life', function()
            local members, mob = partyOf({ 18, 15 })
            gradeAt(mob, 17)
            kill(members, mob)
            assert(recordOf(members[1]).spread == 3, 'precondition: the first kill was made at a spread of 3')

            -- The 15 walks off before the same mob is killed again
            members[2]:setPos(members[1]:getXPos() + 150, members[1]:getYPos(), members[1]:getZPos())
            gradeAt(mob, 17)
            kill(members, mob)
            local record = recordOf(members[1])
            assert(record.spread == 0 and record.average == 18,
                string.format('the second kill\'s record says spread %d, average %d; the 18 made it alone', record.spread, record.average))
        end)

        it('leaves a member out of range out of the average', function()
            local function farKill()
                local members, mob = partyOf({ 18, 15 })
                gradeAt(mob, 17)
                members[2]:setPos(members[1]:getXPos() + 150, members[1]:getYPos(), members[1]:getZPos())
                return kill(members, mob), members
            end

            local paid, members = farKill()
            ruleOff()
            local upstream = farKill()

            assert(upstream[1] and upstream[1] > 0 and upstream[2] == nil, 'precondition: upstream pays the 18 and not the member out of range')
            assert(paid[1] == upstream[1], string.format('the 18 was paid %s, upstream pays her %d', tostring(paid[1]), upstream[1]))
            assert(paid[2] == nil, 'the member out of range is paid nothing')

            local record = recordOf(members[1])
            assert(record.spread == 0 and record.average == 18, string.format('the record says spread %d, average %d', record.spread, record.average))
        end)

        it('pays nobody for a mob too weak for the top member, as upstream', function()
            -- Too weak for the 25; the average, 23, would have been paid
            local grade = 14
            while baseExpAt(25, grade) > 0 do
                grade = grade - 1
            end

            assert(baseExpAt(23, grade) > 0, 'precondition: the mob would pay at the average')
            local paid, members = partyKills({ 25, 22 }, grade)
            assert(paid[1] == nil and paid[2] == nil,
                string.format('a level %d mob paid %s and %s', grade, tostring(paid[1]), tostring(paid[2])))
            assert(recordOf(members[1]) == nil, 'and leaves no record')
        end)

        it('counts a level-restricted member at her restricted level', function()
            local members, mob = partyOf({ 18, 25 })
            members[2]:addStatusEffect(xi.effect.LEVEL_RESTRICTION, { power = 15, origin = members[2] })
            assert(members[2]:getMainLvl() == 15, 'precondition: the restriction holds her at 15')
            gradeAt(mob, 17)

            local paid = kill(members, mob)
            local expected = math.floor(baseExpAt(16, 17) * share(2))
            assert(paid[1] == expected and paid[2] == expected,
                string.format('they were paid %s and %s, as one level %d', tostring(paid[1]), tostring(paid[2]), expected))
        end)

        it('counts her at her true level when the restriction pays by it', function()
            local members, mob = partyOf({ 18, 25 })
            members[2]:addStatusEffect(xi.effect.LEVEL_RESTRICTION, { power = 15, subPower = 1, origin = members[2] })
            assert(members[2]:getMainLvl() == 15, 'precondition: the restriction holds her at 15')
            gradeAt(mob, 22)

            kill(members, mob)

            -- The game grades her at 25: spread 7, average 21
            local record = recordOf(members[1])
            assert(record and record.spread == 7 and record.average == 21,
                string.format('the record says spread %s, average %s; expected 7, 21', tostring(record and record.spread), tostring(record and record.average)))
        end)

        it('chains on a mob the average finds Even Match, though the top does not', function()
            -- Even Match to the average, 16; Decent Challenge to the 18,
            -- so upstream alone never chains it
            local oneLevel = baseExpAt(16, 16) * share(2)
            assert(rule.difficultyOf(baseExpAt(16, 16), 16) == xi.mobDifficulty.EVEN_MATCH and
                rule.difficultyOf(baseExpAt(18, 16), 16) < xi.mobDifficulty.EVEN_MATCH,
                'precondition: Even Match to a 16, under it to an 18')

            local members, mob = partyOf({ 18, 15 })
            local paid = {}
            for chain = 0, 2 do
                gradeAt(mob, 16)
                paid[chain] = kill(members, mob)
            end

            local multipliers = xi.experiencePoints.chainMultipliers
            for chain = 0, 2 do
                local expected = math.floor(oneLevel * multipliers[chain + 1])
                assert(paid[chain][1] == expected and paid[chain][2] == expected,
                    string.format('kill %d paid %s and %s; chain %d on %d is %d', chain + 1, tostring(paid[chain][1]), tostring(paid[chain][2]), chain, math.floor(oneLevel), expected))
            end

            -- The same mob, the rule off: no chain for anyone
            ruleOff()
            local alone, aloneMob = partyOf({ 18, 15 })
            local first = (function()
                gradeAt(aloneMob, 16)
                return kill(alone, aloneMob)
            end)()
            gradeAt(aloneMob, 16)
            local second = kill(alone, aloneMob)
            assert(second[1] == first[1] and second[2] == first[2],
                string.format('upstream alone paid %s then %s to the 18: no chain', tostring(first[1]), tostring(second[1])))
        end)

        it('chains as upstream does on a mob Tough to the top', function()
            -- Tough to the 18, so the game counts a chain
            local members, mob = partyOf({ 18, 15 })
            gradeAt(mob, 19)
            local first = kill(members, mob)

            gradeAt(mob, 19)
            local second = kill(members, mob)

            local oneLevel = baseExpAt(16, 19) * share(2)
            local chained  = xi.experiencePoints.chainMultipliers[2]
            assert(chained > 1, 'precondition: chain 1 multiplies')
            assert(first[1] == math.floor(oneLevel) and first[2] == math.floor(oneLevel),
                string.format('the first kill paid %s and %s, as one level %d', tostring(first[1]), tostring(first[2]), math.floor(oneLevel)))
            assert(second[1] == math.floor(oneLevel * chained) and second[2] == math.floor(oneLevel * chained),
                string.format('the second kill paid %s and %s; chain 1 pays %d', tostring(second[1]), tostring(second[2]), math.floor(oneLevel * chained)))
        end)

        it('never pays past the per-monster cap, and knows upstream\'s capped figure', function()
            local cap           = xi.experiencePoints.perMonsterCaps[1].cap
            local paid, members = partyKills({ 13, 10 }, 26)
            local upstream      = upstreamPays({ 13, 10 }, 26)

            assert(baseExpAt(11, 26) * share(2) > cap and upstream[1] == cap, 'precondition: one level would pay past the cap, and upstream caps the 13')
            assert(paid[1] == cap and paid[2] == cap, string.format('they were paid %s and %s, the cap is %d', tostring(paid[1]), tostring(paid[2]), cap))
            assert(recordOf(members[1]).classic == upstream[1] and recordOf(members[2]).classic == upstream[2],
                string.format('the records say upstream alone pays %s and %s; it pays %d and %d',
                    tostring(recordOf(members[1]).classic), tostring(recordOf(members[2]).classic), upstream[1], upstream[2]))
        end)

        it('spends Dedication once, on what was paid', function()
            local members, mob = partyOf({ 18, 15 })
            for _, member in ipairs(members) do
                member:addStatusEffect(xi.effect.DEDICATION, { power = 50, subPower = 1000, duration = 3600, origin = member })
            end

            gradeAt(mob, 17)
            local paid = kill(members, mob)

            local oneLevel = math.floor(baseExpAt(16, 17) * share(2))
            local bonus    = math.floor(oneLevel * 50 / 100)
            for i = 1, 2 do
                local left = members[i]:getStatusEffect(xi.effect.DEDICATION):getSubPower()
                assert(paid[i] == oneLevel + bonus, string.format('member %d was paid %s; %d and half again is %d', i, tostring(paid[i]), oneLevel, oneLevel + bonus))
                assert(left == 1000 - bonus, string.format('member %d has %d of her Dedication left; one grant of %d leaves %d', i, left, bonus, 1000 - bonus))
            end
        end)

        it('charges Dedication the blend of what each figure would have spent', function()
            -- 25 and 18: spread 7, four sevenths of the way to upstream;
            -- the average is 21. Both carry a band at half again, 1000 left
            local function kills()
                local members, mob = partyOf({ 25, 18 })
                for _, member in ipairs(members) do
                    member:addStatusEffect(xi.effect.DEDICATION, { power = 50, subPower = 1000, duration = 3600, origin = member })
                end

                gradeAt(mob, 20)
                local paid  = kill(members, mob)
                local spent = {}
                for i, member in ipairs(members) do
                    spent[i] = 1000 - member:getStatusEffect(xi.effect.DEDICATION):getSubPower()
                end

                return paid, spent
            end

            local paid, spent = kills()
            ruleOff()
            local upstream, upstreamSpent = kills()
            ruleOn()

            local t              = 4 / 7
            local oneLevelBefore = baseExpAt(21, 20) * share(2)
            local oneLevelBonus  = math.floor(oneLevelBefore * 50 / 100)
            local oneLevel       = math.floor(oneLevelBefore + oneLevelBonus)
            assert(upstreamSpent[1] > 0 and upstreamSpent[2] > 0 and upstreamSpent[1] ~= oneLevelBonus, 'precondition: upstream spends, and differently from the one-level figure')

            for i = 1, 2 do
                local expectedPaid  = blendOf(t, oneLevel, upstream[i])
                local expectedSpent = math.floor((1 - t) * oneLevelBonus + t * upstreamSpent[i] + 1e-9)
                assert(paid[i] == expectedPaid,
                    string.format('member %d was paid %s; the blend of %d and %d is %d', i, tostring(paid[i]), oneLevel, upstream[i], expectedPaid))
                assert(spent[i] == expectedSpent,
                    string.format('member %d spent %d of her band; the blend of %d and %d is %d', i, spent[i], oneLevelBonus, upstreamSpent[i], expectedSpent))
            end
        end)

        it('grades an outsider who hit the mob as the top, as upstream does', function()
            -- A 25 outside the party lands one hit; the kill is a level 25
            -- kill in C++'s eyes, so the spread is 10 and upstream pays
            local function withStranger()
                local members, mob = partyOf({ 18, 15 })
                gradeAt(mob, 17)

                -- The stranger lands a blow on the unclaimed mob, as the pause
                -- tests land one: engage through the client's action, a sure
                -- hit, a second at a time until TP says it landed. His claim
                -- and his hate are then dropped, as they would be once he
                -- walked away, and the party takes the mob
                local stranger = xi.test.world:spawnPlayer({ job = xi.job.WAR, level = 25, zone = xi.zone.WEST_RONFAURE })
                stranger:setPos(members[1]:getXPos(), members[1]:getYPos(), members[1]:getZPos())
                stranger:setMod(xi.mod.ACC, 1000)
                mob:updateEnmity(stranger)
                stranger.actions:engage(mob)
                for _ = 1, 20 do
                    if stranger:getTP() > 0 then
                        break
                    end

                    xi.test.world:skipTime(1)
                end

                assert(stranger:getTP() > 0 and mob:getHP() > 0, 'precondition: the stranger landed a blow and the mob lives')
                stranger:disengage()
                mob:resetEnmity(stranger)
                mob:updateClaim(nil)

                return kill(members, mob), members
            end

            local paid, members = withStranger()
            ruleOff()
            local upstream = withStranger()
            ruleOn()

            assert(upstream[1] and upstream[2], 'precondition: upstream pays both')
            assert(paid[1] == upstream[1] and paid[2] == upstream[2],
                string.format('they were paid %s and %s; upstream, graded at the stranger\'s 25, pays %d and %d', tostring(paid[1]), tostring(paid[2]), upstream[1], upstream[2]))

            local record = recordOf(members[1])
            assert(record.spread == 10 and record.average == 16 and record.t == 1,
                string.format('the record says spread %d, average %d, blend %.2f; expected 10, 16, upstream', record.spread, record.average, record.t))
        end)

        it('needs nothing for level sync: a synced member stands at the sync level', function()
            local members, mob = partyOf({ 25, 15 })
            members[1]:addStatusEffect(xi.effect.LEVEL_SYNC, { power = 15, origin = members[1] })
            assert(members[1]:getMainLvl() == 15, 'precondition: synced down to 15')
            gradeAt(mob, 16)

            local paid = kill(members, mob)
            local expected = math.floor(baseExpAt(15, 16) * share(2))
            assert(paid[1] == expected and paid[2] == expected,
                string.format('they were paid %s and %s; a party of 15s is paid %d', tostring(paid[1]), tostring(paid[2]), expected))

            local record = recordOf(members[2])
            assert(record.spread == 0 and record.average == 15, string.format('the record says spread %d, average %d', record.spread, record.average))
        end)

        it('says whether a real player was in the party', function()
            local _, members = partyKills({ 18, 15 }, 17)
            assert(recordOf(members[1]).witnessed == true, 'two players: witnessed')

            -- A world camp: every member a cardian
            stub('CBaseEntity.isCardian', function()
                return true
            end)

            local _, camp = partyKills({ 18, 15 }, 17)
            assert(recordOf(camp[1]).witnessed == false, 'a party of cardians: nobody\'s to log')
        end)

        it('pays upstream\'s figure when reading the party fails', function()
            local upstream = upstreamPays({ 18, 15 }, 17)

            rule.shapeOf = function()
                error('the party could not be read')
            end

            local paid = partyKills({ 18, 15 }, 17)
            assert(paid[1] == upstream[1] and paid[2] == upstream[2],
                string.format('they were paid %s and %s; upstream pays %d and %d', tostring(paid[1]), tostring(paid[2]), upstream[1], upstream[2]))
            assert(rule.failures == 2, string.format('the failure is counted once a member, %d times', rule.failures))
        end)

        it('pays upstream\'s figure when the one-level figure cannot be built', function()
            local upstream = upstreamPays({ 18, 15 }, 17)

            rule.baseAt = function()
                error('the base table could not be read')
            end

            local paid = partyKills({ 18, 15 }, 17)
            assert(paid[1] == upstream[1] and paid[2] == upstream[2],
                string.format('they were paid %s and %s; upstream pays %d and %d', tostring(paid[1]), tostring(paid[2]), upstream[1], upstream[2]))
            assert(rule.failures == 2, string.format('the failure is counted once a member, %d times', rule.failures))
        end)

        it('reads the base table live, whichever the profile loaded', function()
            -- A table paying half, as prod's era table roughly does
            local halved = {}
            for difference, row in pairs(shippedTable) do
                halved[difference] = {}
                for column, value in ipairs(row) do
                    halved[difference][column] = math.floor(value / 2)
                end
            end

            local shippedFigure = math.floor(shippedTable[1][4] * share(2))

            xi.data.experiencePoints.baseTable = halved
            tableSwapped = true
            ReloadExperienceData()

            local paid     = partyKills({ 18, 15 }, 17)
            local expected = math.floor(halved[1][4] * share(2))
            assert(expected ~= shippedFigure, 'precondition: the halved table pays a different figure')
            assert(paid[1] == expected and paid[2] == expected,
                string.format('they were paid %s and %s; the halved table pays %d, the shipped one %d', tostring(paid[1]), tostring(paid[2]), expected, shippedFigure))
        end)
    end)
end)
