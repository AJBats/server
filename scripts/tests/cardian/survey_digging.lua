-----------------------------------
-- Cardian: the yield survey, chocobo digging (ROADMAP O; OPEN_ISSUES #293)
--
-- OFF BY DEFAULT: registers nothing unless CARDIAN_SURVEY=1, the way
-- upstream's benchmarks are kept out of the ordinary run. It measures, it
-- does not test: for every zone with digging, a beginner (rank Amateur) on a
-- rental chocobo digs CARDIAN_SURVEY_DIGS times (default 2000) through the
-- game's own dig packet, the moon walked through its cycle, and every find
-- is counted. The dig's rules -- each find rolled at its rate, the rank, the
-- moon, the finds that compete, the treasure layer first -- are the game's
-- own (scripts/globals/hobbies/chocobo_digging/logic.lua). The survey resets
-- only what would make the next dig wait or change who digs (the cooldown,
-- the position a dig must move from, the day's fatigue, the digging skill a
-- dig raises), hands over the Gysahl Greens a dig eats and counts them, and
-- takes each find off as it is given, so the bag never fills. A zone the game
-- does not let anyone dig in (logic.lua's diggingZoneList) refuses its first
-- dig and is recorded as such.
--
-- Not measured yet: the weather. xi_test clears every zone's weather before a
-- test, so the crystals and clusters a dig turns up in weather never come
-- up, and the file says "weather": "none". A rental chocobo is surveyed
-- only: a raised one's Burrow layer can find Gysahl Greens, which this
-- counting would miss.
--
-- Writes CARDIAN_SURVEY_OUT (default log/survey_digging.json): per zone, the
-- digs, the digs that found nothing, the greens eaten, and each find's count.
-----------------------------------
if os.getenv('CARDIAN_SURVEY') ~= '1' then
    return
end

local ffi           = require('ffi')
local raisingClient = require('scripts.tests.systems.chocobo_raising.client')

-- A whole number above zero from the environment, or the default
local function envCount(name, default)
    local value = tonumber(os.getenv(name) or '')
    return (value and value >= 1) and math.floor(value) or default
end

local kDigs = envCount('CARDIAN_SURVEY_DIGS', 2000)
local kOut  = os.getenv('CARDIAN_SURVEY_OUT') or 'log/survey_digging.json'

local greens = xi.item.BUNCH_OF_GYSAHL_GREENS

-- The game's names for zones and items, for a file people read
local function namesOf(enum)
    local names = {}
    for name, id in pairs(enum) do
        if type(id) == 'number' then
            names[id] = name:lower()
        end
    end

    return names
end

local function jsonString(s)
    return '"' .. tostring(s):gsub('[%c"\\]', function(c)
        return string.format('\\u%04x', c:byte())
    end) .. '"'
end

describe('Cardian survey: chocobo digging', function()
    it('measures every zone\'s dig for a beginner', function()
        xi.test.world:setSetting('main.DIG_FATIGUE', 0)

        local zoneNames = namesOf(xi.zone)
        local itemNames = namesOf(xi.item)

        -- The moon the dig reads, walked through its cycle dig by dig
        local moon = 0
        stub('VanadielMoonPhase', function()
            return moon
        end)

        -- A find, counted as the dig hands it over and never put in the bag
        local digging = false
        local found   = nil
        local originalAddItem = CBaseEntity.addItem
        stub('CBaseEntity.addItem', function(self, itemId, ...)
            if digging and itemId ~= greens then
                found = itemId
                return true
            end

            return originalAddItem(self, itemId, ...)
        end)

        local player = xi.test.world:spawnPlayer({ zone = xi.zone.SOUTHERN_SAN_DORIA, level = 30 })
        player:setGMLevel(0)
        player:addKeyItem(xi.keyItem.CHOCOBO_LICENSE)

        local results = {}
        local zones   = {}
        for zoneId in pairs(xi.chocoboDig.digInfo) do
            zones[#zones + 1] = zoneId
        end

        table.sort(zones)

        for _, zoneId in ipairs(zones) do
            raisingClient.gotoZone(player, zoneId)
            local entry = { digs = 0, nothing = 0, greens = 0, finds = {}, skipped = nil }
            if player:getZoneID() ~= zoneId then
                entry.skipped = 'could not go there'
            else
                player:addStatusEffect(xi.effect.MOUNTED, { power = xi.mount.CHOCOBO, duration = 86400, origin = player, silent = true })
            end

            -- One dig the game counts, or the zone is one it lets nobody dig in
            local function dig(n)
                moon = (n * 37) % 101
                player:setSkillLevel(xi.skill.DIG, 0)
                player:setLocalVar('ZoneInTime', 0)
                player:setLocalVar('[DIG]LastDigTime', 0)
                player:setLocalVar('[DIG]LastXPos', 5000 + n % 7)
                player:setLocalVar('[DIG]LastXPosSign', 0)
                if not player:hasItem(greens) then
                    player:addItem(greens)
                end

                local packet = ffi.new('uint8_t[28]')
                local id     = player:getID()
                for byte = 0, 3 do
                    packet[4 + byte] = bit.band(bit.rshift(id, byte * 8), 0xFF)
                end

                packet[8]  = bit.band(player:getTargID(), 0xFF)
                packet[9]  = bit.rshift(player:getTargID(), 8)
                packet[10] = 0x11

                found   = nil
                digging = true
                player.packets:send(0x01A, packet, assert(ffi.sizeof(packet)))
                digging = false

                return player:getLocalVar('[DIG]LastDigTime') ~= 0
            end

            if not entry.skipped and not dig(0) then
                entry.skipped = 'digging disabled here'
            end

            if not entry.skipped then
                for n = 1, kDigs do
                    if dig(n) then
                        entry.digs = entry.digs + 1
                        if not player:hasItem(greens) then
                            entry.greens = entry.greens + 1
                        end

                        if found then
                            local name = itemNames[found] or tostring(found)
                            entry.finds[name] = (entry.finds[name] or 0) + 1
                        else
                            entry.nothing = entry.nothing + 1
                        end
                    end
                end
            end

            results[#results + 1] = { zone = zoneNames[zoneId] or tostring(zoneId), entry = entry }
            print(string.format('[SURVEY] %s: %d digs, %d found nothing, %d greens eaten%s', zoneNames[zoneId] or zoneId, entry.digs, entry.nothing, entry.greens,
                entry.skipped and (' (' .. entry.skipped .. ')') or ''))
        end

        -- The counts hold together, or the survey measured nothing: every dig in
        -- a zone that allows digging went ahead and ate its greens, and each
        -- found something or nothing
        local surveyed = 0
        for _, r in ipairs(results) do
            local e = r.entry
            if not e.skipped then
                surveyed = surveyed + 1
                local finds = 0
                for _, count in pairs(e.finds) do
                    finds = finds + count
                end

                assert(e.digs == kDigs, string.format('%s: expected %d digs, got %d', r.zone, kDigs, e.digs))
                assert(e.greens == e.digs, string.format('%s: a rental eats a bunch of greens a dig, %d for %d digs', r.zone, e.greens, e.digs))
                assert(finds + e.nothing == e.digs, string.format('%s: %d finds and %d nothing for %d digs', r.zone, finds, e.nothing, e.digs))
            end
        end

        assert(surveyed > 0, 'Expected a zone that allows digging')

        -- The file, written by hand: no JSON library in the test runtime
        local lines = { '{', string.format('  "rank": "AMATEUR", "digs_per_zone": %d, "chocobo": "rental", "weather": "none",', kDigs), '  "zones": {' }
        for i, r in ipairs(results) do
            local e     = r.entry
            local finds = {}
            for name, count in pairs(e.finds) do
                finds[#finds + 1] = string.format('%s: %d', jsonString(name), count)
            end

            table.sort(finds)
            lines[#lines + 1] = string.format('    %s: {"digs": %d, "nothing": %d, "greens": %d, "skipped": %s, "finds": {%s}}%s', jsonString(r.zone), e.digs, e.nothing,
                e.greens, e.skipped and jsonString(e.skipped) or 'null', table.concat(finds, ', '), i < #results and ',' or '')
        end

        lines[#lines + 1] = '  }'
        lines[#lines + 1] = '}'
        local f = assert(io.open(kOut, 'w'), 'Expected to write ' .. kOut)
        f:write(table.concat(lines, '\n'), '\n')
        f:close()
        print('[SURVEY] written: ' .. kOut)
    end)
end)
