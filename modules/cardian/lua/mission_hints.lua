-----------------------------------
-- Cardian: mission hints
--
-- A cardian in the player's party chimes in, in party chat, when he
-- reaches a new step of a mission: where to go next and who to see. The
-- steps and their words are data, the hint files in
-- modules/cardian/lua/hints/; each step names the state it covers (the
-- current mission, its status, key items, items carried, the zone, his
-- level), and the first step, top down, whose conditions all hold is the
-- player's step. A step's `say` is said once, when he first reaches it --
-- a nudge -- with its note for the zone he stands in; each zone note once
-- more on entering that zone. `!cardian hint` asks her outright: she
-- answers for where he stands (which way the step's `goal` lies, this
-- zone's note, or the next zone on the step's `route`); asked again without
-- moving, the specifics, `more`. How much she volunteers is the
-- player's (`!cardian hints nudge|full|off`, a char var): nudge (the
-- default) says `say`; full says `more` where a step has it; off says
-- nothing unasked.
--
-- A line is a string, or { when = { conditions }, "text" }: said only
-- while those conditions hold, the step's own kinds plus `warps` (home
-- point crystals warp on this server): a warp tip, or a line about a map
-- we may have bought since.
--
-- The check runs after every event, trade and NPC click, after every
-- zone-in and at every level up -- the moments a step can change. A hint needs a cardian in his
-- party and his zone to say it; with none, nothing is marked said, so the
-- first cardian to join hears the step out. cardian.MISSION_HINTS off
-- silences the chiming in; `!cardian hint` still answers.
--
-- tools/hints reads the same files with the same rules (hintcore.py, and
-- guidance.py for asking), so the MUD bench that playtests the words tests
-- what is said here; the bench also plays hot and cold, which is not here.
-----------------------------------
require('modules/module_utils')
-----------------------------------
local m = Module:new('cardian_mission_hints')

local hintFiles = { 'sandoria_rank2' }

xi.cardian       = xi.cardian or {}
xi.cardian.hints = xi.cardian.hints or {}
local hints      = xi.cardian.hints

hints.steps = {}
for _, name in ipairs(hintFiles) do
    local data = require('modules/cardian/lua/hints/' .. name)
    for _, step in ipairs(data.steps) do
        hints.steps[#hints.steps + 1] = step
    end
end

local stepVar  = '[Hint]Step'  -- char var: the step last said, by its id's hash
local levelVar = '[Hint]Level' -- char var: how much she volunteers (levels below)

-- The hint levels, as the char var holds them: 0, the default, is nudge
local levels = { nudge = 0, full = 1, off = 2 }
hints.levels = levels

-- A step id as a char var's value: a stable 31-bit hash, never 0
local function hashOf(text)
    local h = 5381
    for i = 1, #text do
        h = (h * 33 + string.byte(text, i)) % 2147483647
    end
    return h == 0 and 1 or h
end

local function list(v)
    if v == nil then
        return {}
    end
    return type(v) == 'table' and v or { v }
end

local function contains(t, value)
    for _, v in ipairs(t) do
        if v == value then
            return true
        end
    end
    return false
end

-- How many of the item he carries: his inventory, not the Mog Safe or his
-- other bags (a spare in storage is not in hand)
local function carried(player, itemId)
    local count = 0
    for _, item in ipairs(player:findItems(itemId, xi.inv.INVENTORY)) do
        count = count + item:getQuantity()
    end
    return count
end

-- Whether a step's conditions all hold (hintcore.py's holds, the same rules)
function hints.holds(when, player)
    local nation = player:getNation()
    if when.nation ~= nil and nation ~= when.nation then
        return false
    end
    if when.rank ~= nil and player:getRank(nation) ~= when.rank then
        return false
    end
    local log = nation
    if when.current ~= nil then
        log = when.current[1]
        if player:getCurrentMission(log) ~= when.current[2] then
            return false
        end
    end
    if when.status ~= nil then
        local status = player:getMissionStatus(log)
        local lo, hi = when.status, when.status
        if type(when.status) == 'table' then
            lo, hi = when.status[1], when.status[2]
        end
        if status < lo or status > hi then
            return false
        end
    end
    if when.done ~= nil and not player:hasCompletedMission(when.done[1], when.done[2]) then
        return false
    end
    if when.notDone ~= nil and player:hasCompletedMission(when.notDone[1], when.notDone[2]) then
        return false
    end
    for _, ki in ipairs(list(when.ki)) do
        if not player:hasKeyItem(ki) then
            return false
        end
    end
    for _, ki in ipairs(list(when.noKi)) do
        if player:hasKeyItem(ki) then
            return false
        end
    end
    if when.item ~= nil and carried(player, when.item[1]) < when.item[2] then
        return false
    end
    if when.under ~= nil and carried(player, when.under[1]) >= when.under[2] then
        return false
    end
    local zone = player:getZoneID()
    if when.zone ~= nil and not contains(list(when.zone), zone) then
        return false
    end
    if when.notZone ~= nil and contains(list(when.notZone), zone) then
        return false
    end
    if when.var ~= nil and player:getCharVar(when.var[1]) < when.var[2] then
        return false
    end
    if when.level ~= nil then
        local level = player:getMainLvl()
        if level < when.level[1] or level > when.level[2] then
            return false
        end
    end
    if when.warps ~= nil and when.warps ~= hints.warps() then
        return false
    end
    return true
end

-- The player's step: the first whose conditions hold
function hints.stepOf(player)
    for _, step in ipairs(hints.steps) do
        if hints.holds(step.when or {}, player) then
            return step
        end
    end
    return nil
end

-- Who says it: a cardian in his party, in his zone
local function speakerFor(player)
    local party = player:getParty()
    if party == nil then
        return nil
    end
    local zone = player:getZoneID()
    for _, member in pairs(party) do
        if member:isCardian() and member:getZoneID() == zone then
            return member:getName()
        end
    end
    return nil
end

-- Whether home point crystals warp on this server: the setting, less the
-- era module's crystal before Seekers of Adoulin, which only sets the home
-- point (modules/era/lua/globals/homepoint.lua, by the same test)
function hints.warps()
    return xi.settings.main.HOMEPOINT_TELEPORT == 1 and not xi.pre(xi.expansion.SOA)
end

-- A step's lines as said to this player: each conditional line kept only
-- while it holds
function hints.lines(lines, player)
    local out = {}
    for _, line in ipairs(lines or {}) do
        if type(line) ~= 'table' then
            out[#out + 1] = line
        elseif hints.holds(line.when or {}, player) then
            out[#out + 1] = line[1]
        end
    end
    return out
end

local function say(player, speaker, lines)
    for _, line in ipairs(hints.lines(lines, player)) do
        player:printToPlayer(line, xi.msg.channel.PARTY, speaker)
    end
end

-- The zone notes each player has heard, by entity id: { ['step@zone'] = true }.
-- Kept here, not in local vars, which a zone change clears with the rest
-- of his entity; so a note is said once while the map server runs
local noted = {}

-- The step's note for the zone he stands in, if it has one; marked heard
-- once any of its lines is said, so entering the zone again does not repeat it
local function noteHere(player, step)
    if step.notes == nil then
        return nil
    end
    local zone = player:getZoneID()
    local note = step.notes[zone]
    if note ~= nil and #hints.lines(note, player) > 0 then
        local heard = noted[player:getID()] or {}
        heard[step.id .. '@' .. tostring(zone)] = true
        noted[player:getID()] = heard
    end
    return note
end

local function noteSaid(player, step)
    local heard = noted[player:getID()]
    return heard ~= nil and heard[step.id .. '@' .. tostring(player:getZoneID())] == true
end

-- A real player's step, said if it is new to him, with the note for where
-- he stands; on entering a zone (`zoned`), the step's note for it, once
function hints.check(player, zoned)
    -- A settings file of one's own may replace the whole cardian table, so
    -- only an explicit false turns the hints off
    local settings = xi.settings.cardian or {}
    if player == nil or not player:isPC() or player:isCardian() or settings.MISSION_HINTS == false then
        return
    end
    local step = hints.stepOf(player)
    if step == nil then
        return
    end
    local speaker = speakerFor(player)
    if speaker == nil then
        return
    end
    local hash  = hashOf(step.id)
    local level = player:getCharVar(levelVar)
    if player:getCharVar(stepVar) ~= hash then
        player:setCharVar(stepVar, hash)
        if level == levels.off then
            return
        end
        print(string.format('[cardian] hint for %s: %s (%s)', player:getName(), step.id, speaker))
        say(player, speaker, level == levels.full and step.more or step.say)
        local note = noteHere(player, step)
        if note ~= nil then
            say(player, speaker, note)
        end
    elseif zoned and level ~= levels.off and not noteSaid(player, step) then
        local note = noteHere(player, step)
        if note ~= nil then
            say(player, speaker, note)
        end
    end
end

-- Asking twice counts as "again" within this many yalms of the first ask
local askedRadius = 10
-- The goal is "right around here" within this many yalms
local hereRadius  = 15

-- Where each player last asked, by entity id: { step, zone, x, z }
local asked = {}

-- The zone's name as a player reads it, from its xi.zone key, loaded or
-- not: SOUTHERN_SAN_DORIA as "Southern San d'Oria"
local zoneKeys = nil

local function zoneName(zoneId)
    if zoneKeys == nil then
        zoneKeys = {}
        for key, id in pairs(xi.zone) do
            zoneKeys[id] = key
        end
    end
    local name = (zoneKeys[zoneId] or tostring(zoneId)):lower():gsub('_', ' '):gsub('(%a)([%w]*)', function(first, rest)
        return first:upper() .. rest
    end)
    name = name:gsub(' Dor', " d'Or"):gsub('Balgas ', "Balga's "):gsub('Ranperres ', "Ranperre's ")
    return (name:gsub('Carpenters ', "Carpenters' "))
end

-- The compass from one spot to another, north being +z ("north-east")
local function compass(dx, dz)
    local ns = dz > 0 and 'north' or (dz < 0 and 'south' or '')
    local ew = dx > 0 and 'east' or (dx < 0 and 'west' or '')
    if math.abs(dx) >= 2 * math.abs(dz) then
        ns = ''
    elseif math.abs(dz) >= 2 * math.abs(dx) then
        ew = ''
    end
    if ns ~= '' and ew ~= '' then
        return ns .. '-' .. ew
    end
    return ns ~= '' and ns or ew
end

-- The next zone from here on the step's road (the first of its roads that
-- runs on from here), or nil off the road
local function nextOnRoute(route, zone)
    if route == nil or route[1] == nil then
        return nil
    end
    local roads = type(route[1]) == 'table' and route or { route }
    for _, road in ipairs(roads) do
        for i = 1, #road - 1 do
            if road[i] == zone then
                local nextZone, last = road[i + 1], road[#road]
                if nextZone == last then
                    return string.format("On to %s from here; that's where we're headed.", zoneName(nextZone))
                end
                return string.format('On to %s from here, on the road to %s.', zoneName(nextZone), zoneName(last))
            end
        end
    end
    return nil
end

-- Asked, the answer for where he stands: which way the step's goal lies,
-- in its zone; else the step's note for this zone; else the next zone on
-- its road; nil if none of those
local function answerHere(player, step)
    local zone = player:getZoneID()
    local goal = step.goal
    if goal ~= nil and goal.zone == zone and goal.pos ~= nil then
        local dx = goal.pos[1] - player:getXPos()
        local dz = goal.pos[2] - player:getZPos()
        if dx * dx + dz * dz <= hereRadius * hereRadius then
            return { string.format("We're looking for %s. It should be right around here.", goal.name) }
        end
        return { string.format("We're looking for %s. I think it's %s of here.", goal.name, compass(dx, dz)) }
    end
    local note = noteHere(player, step)
    if note ~= nil and #hints.lines(note, player) > 0 then
        return note
    end
    local line = nextOnRoute(step.route, zone)
    return line and { line } or nil
end

-- `!cardian hint`: asked outright, she answers for where he stands (else
-- the step's nudge again); asked again without moving, the specifics
-- (`more`, else `say`) and the note for where he stands
function hints.remind(player)
    local step = hints.stepOf(player)
    local speaker = speakerFor(player)
    if speaker == nil then
        return false
    end
    if step == nil then
        say(player, speaker, { "I'm not sure what's next. Let's check the mission log." })
        return true
    end
    player:setCharVar(stepVar, hashOf(step.id))

    local zone, x, z = player:getZoneID(), player:getXPos(), player:getZPos()
    local last       = asked[player:getID()]
    local again      = last ~= nil and last.step == step.id and last.zone == zone and
        (x - last.x) ^ 2 + (z - last.z) ^ 2 <= askedRadius * askedRadius
    asked[player:getID()] = { step = step.id, zone = zone, x = x, z = z }

    if not again then
        say(player, speaker, answerHere(player, step) or step.say)
        return true
    end
    say(player, speaker, step.more or step.say)
    local note = noteHere(player, step)
    if note ~= nil then
        say(player, speaker, note)
    end
    return true
end

-- `!cardian hints nudge|full|off`: how much she volunteers from now on.
-- Her answer is in her voice when she is in the party, else a system line
local levelWords =
{
    nudge = "I'll point the way when we reach something new. Ask me if you want the details.",
    full  = "I'll tell you everything I know as we go.",
    off   = "I'll keep quiet unless you ask.",
}

function hints.setLevel(player, word)
    if levels[word] == nil then
        return false
    end
    player:setCharVar(levelVar, levels[word])
    local speaker = speakerFor(player)
    if speaker ~= nil then
        say(player, speaker, { levelWords[word] })
    else
        player:printToPlayer(string.format('Mission hints: %s.', word), xi.msg.channel.SYSTEM_3)
    end
    return true
end

-- The moments a mission moves: an event's end, a trade, a click, a zone-in.
-- Each runs the game's own handling first, then looks
m:addOverride('InteractionGlobal.onEventFinish', function(player, csid, option, npc, fallbackFn)
    local result = super(player, csid, option, npc, fallbackFn)
    hints.check(player, false)
    return result
end)

m:addOverride('InteractionGlobal.onTrade', function(player, npc, trade, fallbackFn)
    local result = super(player, npc, trade, fallbackFn)
    hints.check(player, false)
    return result
end)

m:addOverride('InteractionGlobal.onTrigger', function(player, npc, fallbackFn)
    local result = super(player, npc, fallbackFn)
    hints.check(player, false)
    return result
end)

m:addOverride('xi.player.onPlayerLevelUp', function(player)
    super(player)
    hints.check(player, false)
end)

m:addOverride('InteractionGlobal.afterZoneIn', function(player, fallbackFn)
    local result = super(player, fallbackFn)
    hints.check(player, true)
    return result
end)

return m
