-----------------------------------
-- Cardian: the party progresses together
--
-- Quests and missions are the player's doing -- a cardian cannot talk to
-- an NPC -- but she was there, so her log moves with the player's and she
-- earns her own reward. A cardian may enter any battlefield her player
-- enters: the registrant's registration already registers every party
-- member in the zone, and a member then only has to touch the circle to
-- go in, which she never can; so she goes in right behind the player.
-- Scope: cardians in the player's party and in the player's zone; a party
-- cardian elsewhere is named in the chat log as left out.
--
-- The player hears who moved with them, in the chat log, one line per
-- outcome: "Jevyak and Zapp accepted the mission - Bat Hunt!", and a line
-- of its own for each cardian who fared differently.
-----------------------------------
require('modules/module_utils')
-----------------------------------
local m = Module:new('cardian_party_progress')

local channel = xi.msg.channel.SYSTEM_3

-- One line to the player's chat log and to the map log
local function say(player, line)
    print(string.format('[cardian] to %s: %s', player:getName(), line))
    player:printToPlayer(line, channel)
end

-- "Jevyak", "Jevyak and Zapp", "Jevyak, Zapp and Kupo"
local function joinNames(names)
    local n = #names
    if n == 1 then
        return names[1]
    end
    return table.concat(names, ', ', 1, n - 1) .. ' and ' .. names[n]
end

-- The enum key as a title: SMASH_THE_ORCISH_SCOUTS -> Smash the Orcish Scouts
local smallWords =
{
    a = true, an = true, the = true, of = true, to = true, at = true, on = true,
    by = true, ['in'] = true, ['for'] = true, ['and'] = true, ['or'] = true,
    with = true, from = true,
}

local function titleFromKey(key)
    local words = {}
    for word in string.gmatch(string.lower(key), '[^_]+') do
        if #words > 0 and smallWords[word] then
            words[#words + 1] = word
        else
            words[#words + 1] = string.upper(string.sub(word, 1, 1)) .. string.sub(word, 2)
        end
    end
    return table.concat(words, ' ')
end

-- Shared with the link's command script (scripts/commands/cardian.lua),
-- which titles the finder's goals the same way
xi.cardian = xi.cardian or {}
xi.cardian.titleFromKey = titleFromKey

local function nameIn(ids, id)
    if ids ~= nil then
        for key, value in pairs(ids) do
            if value == id then
                return titleFromKey(key)
            end
        end
    end
    return string.format('#%d', id)
end

local function missionName(logId, missionId)
    return nameIn(xi.mission.id[xi.mission.area[logId]], missionId)
end

local function questName(logId, questId)
    return nameIn(xi.quest.id[xi.quest.area[logId]], questId)
end

-- The cardians in the player's party, split by whether they are in the
-- player's zone. Nothing for a cardian's own completion (never mirror a
-- mirror). The player's own cardians always; a wild cardian invited along
-- by her contract (ROADMAP H slice 3: what the shout recruited her for):
-- `contracts` is 'all', or the set of contracts that take part, e.g.
-- { mission = true }. She fights beside the player and enters the
-- battlefield with them whatever she came for; her log moves with the
-- player's only under the matching contract
local function partyCardians(player, contracts)
    local here, away = {}, {}
    if not player:isPC() or player:isCardian() then
        return here, away
    end

    local party = player:getParty()
    if party == nil then
        return here, away
    end

    local zone = player:getZoneID()
    for _, member in pairs(party) do
        if member:isCardian() then
            local name = member:getName()
            local take = player:cardianOwns(name) or contracts == 'all' or
                (type(contracts) == 'table' and contracts[member:cardianContract(player:getID())] == true)
            if take then
                if member:getZoneID() == zone then
                    here[#here + 1] = member
                else
                    away[#away + 1] = member
                end
            end
        end
    end
    return here, away
end

-- Outcomes grouped by their line, so cardians who fared the same share
-- one line: those who did first, then those who could not, each group in
-- the order its outcomes first appeared
local function newReport()
    return { lines = {}, byLine = {} }
end

local function report(r, cardian, line, ok)
    local entry = r.byLine[line]
    if entry == nil then
        entry = { line = line, ok = ok, names = {} }
        r.byLine[line]        = entry
        r.lines[#r.lines + 1] = entry
    end
    entry.names[#entry.names + 1] = cardian:getName()
end

local function send(r, player)
    for _, wantOk in ipairs({ true, false }) do
        for _, entry in ipairs(r.lines) do
            if entry.ok == wantOk then
                say(player, joinNames(entry.names) .. entry.line)
            end
        end
    end
end

-- Her own reward, the same as the player's. An item she has no room for
-- is the only thing left out; the log entry still lands
local function without(params, keys)
    if params == nil then
        return params
    end
    local copy, any = {}, false
    for k, v in pairs(params) do
        if keys[k] then
            any = true
        else
            copy[k] = v
        end
    end
    return any and copy or params
end

local function withoutItems(params)
    return without(params, { item = true, itemParams = true })
end

-- A nation mission's rank is the player's nation's; a cardian of another
-- nation takes the reward without it (catchUpRank is her rank's one gate)
local function rankFor(cardian, logId, params)
    if logId <= xi.mission.log_id.WINDURST and cardian:getNation() ~= logId then
        return without(params, { rank = true, rankPoints = true })
    end
    return params
end

-- Her rank follows her player's at every checkpoint she is present for:
-- some mission scripts set the player's rank directly, outside the reward
-- table the mirror passes on. Nation logs only; setRank writes her own
-- nation's slot
local function catchUpRank(player, cardian, logId)
    if logId > xi.mission.log_id.WINDURST or cardian:getNation() ~= logId then
        return nil
    end

    local theirs = player:getRank(logId)
    if cardian:getRank(logId) < theirs then
        cardian:setRank(theirs)
        return string.format('rank %d', theirs)
    end
    return nil
end

-- act(cardian) returns ok, detail: the reason when she could not, or a
-- note when she did with something left out or caught up
local did = { accept = 'accepted', complete = 'completed' }

-- The party memory at a completion (ROADMAP H slice 3, the contracts): a
-- wild cardian recruited for this earns affinity -- a mission counts
-- toward the pearl's lock, and a mission recruit earns on a quest too --
-- and one recruited for something else complains, in the chat log, and
-- nothing counts. The player's own cardians need no affinity
local complaints =
{
    exp   = { 'Worst exp party ever.', "This isn't what I signed up for.", 'Are we ever going to fight something?', 'One more of these and I\'m out.' },
    quest = { 'I came along for a quest, not this.', "This isn't what I signed up for." },
}

-- The missions that are a fight, by log and id: a battlefield, a
-- notorious monster, required kills, a starter's drop hunt (the party
-- fights for the item) or a trek as deadly as a fight -- the sweep of
-- the era's mission scripts, 2026-09-13, and the treks as a starting
-- guess (the user, the same evening: to be corrected from play, with the
-- trace below saying how each mission fired). Left out: every cutscene
-- mission (Promathia most of all), the Zeruhn Report, the Crystal Line,
-- the Rites of Life, Unraveling Reason. A mission not here
-- completes, her log moves, and nobody's affinity changes, nobody
-- complains. Quests all count until the pass on them
-- Built from the enum's own keys, so an upstream renumbering cannot
-- reclassify a mission; a key the enum no longer has is reported and skipped
local function missionIds(area, keys)
    local ids, out = xi.mission.id[area], {}
    for _, key in ipairs(keys) do
        local id = ids and ids[key]
        if id ~= nil then
            out[id] = true
        else
            print(string.format('[cardian] combat table: no mission %s.%s in the enum', area, key))
        end
    end
    return out
end

local combatMissions =
{
    [0] = missionIds('sandoria', {
        'SMASH_THE_ORCISH_SCOUTS',  -- (a drop hunt: the party fights for it)
        'BAT_HUNT',  -- (a drop hunt)
        'SAVE_THE_CHILDREN',
        'THE_RESCUE_DRILL',  -- (a trek: La Theine, Ordelle's Caves)
        'THE_DAVOI_REPORT',  -- (a trek: Davoi)
        'JOURNEY_ABROAD',  -- (a trek: the consulate run)
        'JOURNEY_TO_BASTOK',  -- (a trek: Palborough Mines)
        'JOURNEY_TO_WINDURST',  -- (a trek: Giddeus)
        'JOURNEY_TO_BASTOK2',
        'JOURNEY_TO_WINDURST2',
        'INFILTRATE_DAVOI',  -- (a trek: Davoi)
        'APPOINTMENT_TO_JEUNO',  -- (a trek: Lower Delkfutt's Tower)
        'MAGICITE',  -- (a trek as deadly as any fight; the user)
        'THE_RUINS_OF_FEI_YIN',
        'THE_SHADOW_LORD',
        'LEAUTES_LAST_WISHES',
        'RANPERRES_FINAL_REST',
        'PRESTIGE_OF_THE_PAPSQUE',
        'THE_SECRET_WEAPON',
        'COMING_OF_AGE',
        'LIGHTBRINGER',
        'BREAKING_BARRIERS',
        'THE_HEIR_TO_THE_LIGHT',
    }),
    [1] = missionIds('bastok', {
        'GEOLOGICAL_SURVEY',  -- (a trek: Dangruf Wadi)
        'FETICHISM',  -- (a drop hunt)
        'WADING_BEASTS',  -- (a drop hunt)
        'THE_EMISSARY',  -- (a trek: the consulate run)
        'THE_EMISSARY_SANDORIA',
        'THE_EMISSARY_WINDURST',  -- (a drop hunt)
        'THE_EMISSARY_SANDORIA2',
        'THE_EMISSARY_WINDURST2',
        'THE_FOUR_MUSKETEERS',
        'TO_THE_FORSAKEN_MINES',
        'JEUNO',  -- (a trek: Lower Delkfutt's Tower)
        'MAGICITE',  -- (a trek)
        'DARKNESS_RISING',
        'XARCABARD_LAND_OF_TRUTHS',
        'RETURN_OF_THE_TALEKEEPER',
        'THE_PIRATES_COVE',
        'THE_FINAL_IMAGE',
        'ON_MY_WAY',
        'THE_CHAINS_THAT_BIND_US',
        'ENTER_THE_TALEKEEPER',
        'THE_SALT_OF_THE_EARTH',
        'WHERE_TWO_PATHS_CONVERGE',
    }),
    [2] = missionIds('windurst', {
        'THE_HORUTOTO_RUINS_EXPERIMENT',  -- (a trek: Inner Horutoto)
        'THE_HEART_OF_THE_MATTER',  -- (a trek: Outer Horutoto)
        'THE_PRICE_OF_PEACE',  -- (a trek: Giddeus)
        'LOST_FOR_WORDS',  -- (a trek: the Maze of Shakhrami)
        'A_TESTING_TIME',
        'THE_THREE_KINGDOMS',  -- (a trek: the consulate run)
        'THE_THREE_KINGDOMS_BASTOK',  -- (a trek: Palborough Mines)
        'THE_THREE_KINGDOMS_SANDORIA',
        'THE_THREE_KINGDOMS_SANDORIA2',
        'THE_THREE_KINGDOMS_BASTOK2',
        'TO_EACH_HIS_OWN_RIGHT',  -- (a trek: Castle Oztroja to the top)
        'WRITTEN_IN_THE_STARS',  -- (a drop hunt)
        'A_NEW_JOURNEY',  -- (a trek: Lower Delkfutt's Tower)
        'MAGICITE',  -- (a trek)
        'THE_FINAL_SEAL',
        'THE_SHADOW_AWAITS',
        'FULL_MOON_FOUNTAIN',
        'SAINTLY_INVITATION',
        'THE_SIXTH_MINISTRY',  -- (the canal door opens on four kills)
        'AWAKENING_OF_THE_GODS',  -- (a drop hunt)
        'VAIN',  -- (a trek: fifteen zones, then Davoi)
        'THE_JESTER_WHOD_BE_KING',
        'DOLL_OF_THE_DEAD',  -- (a trek: the Boyahda Tree)
        'MOON_READING',
    }),
    [3] = missionIds('zilart', {
        'THE_NEW_FRONTIER',  -- (a trek: Sea Serpent Grotto to Norg)
        'THE_TEMPLE_OF_UGGALEPIH',
        'HEADSTONE_PILGRIMAGE',
        'THROUGH_THE_QUICKSAND_CAVES',
        'THE_CHAMBER_OF_ORACLES',  -- (a trek: the Quicksand Caves)
        'RETURN_TO_DELKFUTTS_TOWER',
        'THE_TEMPLE_OF_DESOLATION',  -- (a trek)
        'THE_HALL_OF_THE_GODS',  -- (a trek)
        'THE_MITHRA_AND_THE_CRYSTAL',
        'THE_GATE_OF_THE_GODS',  -- (a trek: Ru'Aun Gardens)
        'ARK_ANGELS',
        'THE_SEALED_SHRINE',  -- (a trek: the Shrine of Ru'Avitau)
        'THE_CELESTIAL_NEXUS',
    }),
    [4] = missionIds('toau', {
        'IMMORTAL_SENTRIES',  -- (a trek: four aggressive zones)
        'UNDERSEA_SCOUTING',  -- (a trek: Alzadaal Undersea Ruins)
        'LOST_KINGDOM',
        'THE_BLACK_COFFIN',
        'TEAHOUSE_TUMULT',  -- (a trek: Aydeewa Subterrane)
        'SHIELD_OF_DIPLOMACY',
        'MISPLACED_NOBILITY',  -- (a trek: Aydeewa Subterrane)
        'PUPPET_IN_PERIL',
        'PREVALENCE_OF_PIRATES',  -- (a trek: Arrapago Reef)
        'SHADES_OF_VENGEANCE',
        'TESTING_THE_WATERS',  -- (a trek: Arrapago Reef)
        'LEGACY_OF_THE_LOST',
        'GAZE_OF_THE_SABOTEUR',  -- (a trek: Hazhalm Testing Grounds)
        'PATH_OF_DARKNESS',
        'NASHMEIRAS_PLEA',
    }),
    [6] = missionIds('cop', {
        'BELOW_THE_ARKS',
        'THE_MOTHERCRYSTALS',
        'AN_INVITATION_WEST',  -- (a trek: Lufaise Meadows)
        'DISTANT_BELIEFS',
        'AN_ETERNAL_MELODY',  -- (a trek: Misareaux Coast)
        'ANCIENT_VOWS',
        'THE_ROAD_FORKS',
        'DARKNESS_NAMED',
        'SHELTERING_DOUBT',  -- (a trek: Misareaux Coast)
        'THE_SAVAGE',
        'THE_SECRETS_OF_WORSHIP',
        'SLANDEROUS_UTTERINGS',  -- (a trek: Sealion's Den)
        'THE_ENDURING_TUMULT_OF_WAR',
        'DESIRES_OF_EMPTINESS',
        'THREE_PATHS',
        'A_PLACE_TO_RETURN',
        'ONE_TO_BE_FEARED',
        'CHAINS_AND_BONDS',  -- (a trek: Sealion's Den)
        'FLAMES_IN_THE_DARKNESS',  -- (a trek: Sealion's Den)
        'FIRE_IN_THE_EYES_OF_MEN',  -- (a trek: Movalpolos)
        'CALM_BEFORE_THE_STORM',
        'THE_WARRIORS_PATH',
        'GARDEN_OF_ANTIQUITY',
        'A_FATE_DECIDED',
        'WHEN_ANGELS_FALL',
        'DAWN',
    }),
}

local function isCombatMission(logId, missionId)
    local log = combatMissions[logId]
    return log ~= nil and log[missionId] == true
end

-- The mission trace (the user, 2026-09-13: rich logging to dogfood the
-- combat table against): every mission added, completed or moved along,
-- every quest completed, with where it happened, how it was called, what
-- the table said and what each cardian's contract made of it. Always to
-- the map log as '[mission]'; to the player's chat log too under
-- pawn.MISSION_TRACE (dev)
local function traceOn()
    return xi.settings ~= nil and xi.settings.pawn ~= nil and xi.settings.pawn.MISSION_TRACE == true
end

local function trace(player, line)
    print('[mission] ' .. line)
    if traceOn() and player:isPC() and not player:isCardian() then
        player:printToPlayer('[mission] ' .. line, channel)
    end
end

local function logLabel(logId)
    local area = xi.mission.area[logId]
    return area ~= nil and area or ('log' .. tostring(logId))
end

local function questLabel(area)
    local name = xi.quest.area[area]
    return name ~= nil and name or ('area' .. tostring(area))
end

-- "Kolokbe (mission), Yapari (exp), Jevyak (yours)"
local function crewText(player)
    local here, away = partyCardians(player, 'all')
    local parts = {}
    for _, list in ipairs({ here, away }) do
        for _, cardian in ipairs(list) do
            local name     = cardian:getName()
            local contract = player:cardianOwns(name) and 'yours' or cardian:cardianContract(player:getID())
            if contract == '' then
                contract = 'no contract'
            end
            parts[#parts + 1] = string.format('%s (%s%s)', name, contract, list == away and ', away' or '')
        end
    end
    return #parts > 0 and table.concat(parts, ', ') or 'nobody'
end

local inHelper = false -- inside npcUtil.completeMission / completeQuest

-- The player hears the gain once for the party, the way exp is told, and
-- each complaint on its own
local gained =
{
    mission = 'Mission accomplished! Everyone in the party thinks a little more of you.',
    quest   = 'Quest complete! Everyone in the party thinks a little more of you.',
}

-- Returns the outcome per cardian as text, for the trace
local function settleContracts(player, kind, counts)
    if not counts then
        return 'nothing counts'
    end
    local here     = partyCardians(player, 'all')
    local bonded   = false
    local outcomes = {}
    for _, cardian in ipairs(here) do
        local name = cardian:getName()
        if not player:cardianOwns(name) then
            local contract = cardian:cardianContract(player:getID())
            if contract == kind or (kind == 'quest' and contract == 'mission') then
                player:cardianBond(name, 'a ' .. kind .. ' completed together', kind == 'mission')
                bonded = true
                outcomes[#outcomes + 1] = name .. ' +1 affinity'
            elseif contract ~= '' then
                local lines = complaints[contract] or complaints.exp
                say(player, name .. ': ' .. lines[(cardian:getID() % #lines) + 1])
                outcomes[#outcomes + 1] = name .. ' complains (' .. contract .. ')'
            else
                outcomes[#outcomes + 1] = name .. ' no contract'
            end
        end
    end
    if bonded then
        say(player, gained[kind])
    end
    return #outcomes > 0 and table.concat(outcomes, ', ') or 'no wild cardian here'
end

local kMissionCrew = { mission = true }
local kQuestCrew   = { quest = true, mission = true }

local function mirror(player, verb, kind, title, act, contracts)
    local here, away = partyCardians(player, contracts)
    local r          = newReport()
    for _, cardian in ipairs(here) do
        local ok, detail = act(cardian)
        if not ok then
            report(r, cardian, string.format(' could not %s the %s (%s).', verb, kind, detail), false)
        elseif detail ~= nil then
            report(r, cardian, string.format(' %s the %s - %s! (%s)', did[verb], kind, title, detail), true)
        else
            report(r, cardian, string.format(' %s the %s - %s!', did[verb], kind, title), true)
        end
    end
    for _, cardian in ipairs(away) do
        report(r, cardian, string.format(' could not %s the %s (not in your zone).', verb, kind), false)
    end
    send(r, player)
end

-- She takes the mission from the same state the player took it from: no
-- mission, or the log's resting value (a category on the Promathia log,
-- 0 on the later ones); a different current mission of her own stands
m:addOverride('Mission.begin', function(self, player)
    local before = player:getCurrentMission(self.areaId)
    super(self, player)
    mirror(player, 'accept', 'mission', missionName(self.areaId, self.missionId), function(cardian)
        local current = cardian:getCurrentMission(self.areaId)
        if current ~= before and current ~= self.missionId then
            return false, 'already on another mission'
        end
        super(self, cardian)
        return true, catchUpRank(player, cardian, self.areaId)
    end, kMissionCrew)
end)

-- A legacy script's quest is not accepted here; her log catches up at
-- completion
m:addOverride('Quest.begin', function(self, player)
    super(self, player)
    mirror(player, 'accept', 'quest', questName(self.areaId, self.questId), function(cardian)
        super(self, cardian)
        return true
    end, kQuestCrew)
end)

-- The helper's completions are marked as such for the trace; the flag
-- clears even when a script under the helper errors
local function viaHelper(f, ...)
    inHelper = true
    local results = { pcall(f, ...) }
    inHelper = false
    if not results[1] then
        error(results[2], 0)
    end
    return select(2, unpack(results))
end

m:addOverride('npcUtil.completeMission', function(player, logId, missionId, params)
    local ok = viaHelper(super, player, logId, missionId, params)
    if ok then
        mirror(player, 'complete', 'mission', missionName(logId, missionId), function(cardian)
            -- She was there at the end: her log catches up, since many
            -- scripts grant a mission with a bare addMission this module
            -- never sees, and a chain's next mission comes with the reward
            if cardian:getCurrentMission(logId) ~= missionId then
                cardian:addMission(logId, missionId)
            end
            local notes = {}
            local hers  = rankFor(cardian, logId, params)
            if not super(cardian, logId, missionId, hers) then
                super(cardian, logId, missionId, withoutItems(hers))
                notes[#notes + 1] = 'no room for the item'
            end
            notes[#notes + 1] = catchUpRank(player, cardian, logId)
            if #notes > 0 then
                return true, table.concat(notes, ', ')
            end
            return true
        end, kMissionCrew)
    end
    return ok
end)

m:addOverride('npcUtil.completeQuest', function(player, area, quest, params)
    local ok = viaHelper(super, player, area, quest, params)
    if ok then
        mirror(player, 'complete', 'quest', questName(area, quest), function(cardian)
            if super(cardian, area, quest, params) then
                return true
            end
            super(cardian, area, quest, withoutItems(params))
            return true, 'no room for the item'
        end, kQuestCrew)
    end
    return ok
end)

-- The entity's own mission and quest calls, under every script path --
-- the helper above and the direct calls a cutscene makes (a mission that
-- completes on zoning in, the next one added right after): the trace,
-- and the contracts settled here so no path is missed. A quest pays the
-- first time this player completes it: a repeatable one (a bought bat
-- wing handed in again and again) would farm affinity the way corn farms
-- fame in Selbina
m:addOverride('CBaseEntity.addMission', function(player, logId, missionId)
    super(player, logId, missionId)
    if player:isPC() and not player:isCardian() then
        trace(player, string.format('%s: %s #%d %s added%s in %s; party: %s',
            player:getName(), logLabel(logId), missionId, missionName(logId, missionId),
            inHelper and ' (by the helper)' or ' (by the script)', player:getZoneName(), crewText(player)))
    end
end)

m:addOverride('CBaseEntity.completeMission', function(player, logId, missionId)
    local mine  = player:isPC() and not player:isCardian()
    local first = mine and not player:hasCompletedMission(logId, missionId)
    super(player, logId, missionId)
    if mine then
        local combat  = isCombatMission(logId, missionId)
        local outcome = settleContracts(player, 'mission', combat and first)
        trace(player, string.format('%s: %s #%d %s completed%s in %s; combat table: %s; %s; %s; party: %s',
            player:getName(), logLabel(logId), missionId, missionName(logId, missionId),
            inHelper and ' (by the helper)' or ' (by the script)', player:getZoneName(),
            combat and 'yes' or 'no', first and 'first time' or 'done before', outcome, crewText(player)))
    end
end)

m:addOverride('CBaseEntity.setMissionStatus', function(player, logId, value, index)
    super(player, logId, value, index)
    if player:isPC() and not player:isCardian() then
        trace(player, string.format('%s: %s status%s = %s (on #%d %s) in %s',
            player:getName(), logLabel(logId), index ~= nil and ('[' .. tostring(index) .. ']') or '', tostring(value),
            player:getCurrentMission(logId), missionName(logId, player:getCurrentMission(logId)), player:getZoneName()))
    end
end)

m:addOverride('CBaseEntity.completeQuest', function(player, area, questId)
    local first = player:isPC() and not player:isCardian() and not player:hasCompletedQuest(area, questId)
    super(player, area, questId)
    if player:isPC() and not player:isCardian() then
        local outcome = settleContracts(player, 'quest', first)
        trace(player, string.format('%s: quest %s #%d %s completed%s in %s; %s; %s; party: %s',
            player:getName(), questLabel(area), questId, questName(area, questId),
            inHelper and ' (by the helper)' or ' (by the script)', player:getZoneName(),
            first and 'first time' or 'done before', outcome, crewText(player)))
    end
end)

-- Fame a script hands the player outside a completion -- a repeatable
-- quest's second hand-in pays addFame directly, the quest already being
-- complete -- reaches the cardians a completion would have: the player's
-- own and the quest and mission recruits in the zone. Inside the helper
-- the completion mirror has already paid them from the reward table
m:addOverride('CBaseEntity.addFame', function(player, area, fame)
    super(player, area, fame)
    if inHelper or not player:isPC() or player:isCardian() then
        return
    end
    local here, away = partyCardians(player, kQuestCrew)
    local names = {}
    for _, cardian in ipairs(here) do
        cardian:addFame(area, fame)
        names[#names + 1] = cardian:getName()
    end
    if #names > 0 then
        say(player, joinNames(names) .. ' shared in your good name.')
        local behind = {}
        for _, cardian in ipairs(away) do
            behind[#behind + 1] = cardian:getName()
        end
        trace(player, string.format('%s: fame +%d in area %d, shared with %s%s', player:getName(), fame, area, table.concat(names, ', '),
            #behind > 0 and ('; not in the zone: ' .. table.concat(behind, ', ')) or ''))
    end
end)

-- Into the battlefield right behind the player: registered with them by
-- the registration itself, entered here since she cannot touch the circle
m:addOverride('Battlefield.onEntryEventUpdate', function(self, player, csid, option, npc)
    local result = super(self, player, csid, option, npc)
    if player:getBattlefield() ~= nil then
        local here = partyCardians(player, 'all')
        local r    = newReport()
        for _, cardian in ipairs(here) do
            if cardian:getBattlefield() ~= nil then
                -- already inside
            elseif cardian:hasStatusEffect(xi.effect.BATTLEFIELD) then
                cardian:enterBattlefield()
                report(r, cardian, ' entered the battlefield.', true)
            else
                report(r, cardian, ' could not enter the battlefield (not registered).', false)
            end
        end
        send(r, player)
    end
    return result
end)

return m
