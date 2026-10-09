-----------------------------------
-- Cardian: the errand table (src/map/pawn/errands.h; the Link's ERRANDS and
-- SEND_ERRAND): the quests a member of the player's linkshell can be sent to
-- do once he has done them, and each nation's missions a rank catch-up takes
-- her through -- the zones each has her cross and how long she is away doing
-- it (RESEARCH §11.13)
--
--   xi.cardian.errands.entries
--     -> the quests: each { kind = 'quest', log (the quest area), key (the
--        id's name in its log), minutes (on the game clock), zones (her
--        route, zone ids in order), job (the job it unlocks, xi.job.NONE for
--        the support jobs; nil for none) }
--   xi.cardian.errands.goals(player)
--     -> the quests the player has done, in the table's order, each
--        { kind, log, id, title, minutes, zones, job }
--   xi.cardian.errands.ranks
--     -> by nation (xi.nation), its missions in the log's order: each
--        { key, step (the rank she holds once its step is done; a step's
--        last mission grants it), minutes, zones }
--   xi.cardian.errands.ladder(nation)
--     -> that nation's ladder as the map reads it: each { id, step,
--        minutes, zones, title }, the id and the title the game's own
--
-- A library, not a module: it overrides nothing, so the pawn module loads it
-- at init (pawn::linkapi::loadLibraries), and xi_test requires it from the
-- test.
-----------------------------------
xi = xi or {}
xi.cardian = xi.cardian or {}
xi.cardian.errands = {}

-- The enum key as a title, the party progress module's own caser
local function titleFromKey(key)
    return xi.cardian.titleFromKey and xi.cardian.titleFromKey(key) or key
end

local function quest(log, key, minutes, zones, job)
    return { kind = 'quest', log = log, key = key, minutes = minutes, zones = zones, job = job }
end

local z = xi.zone

-- An hour or two each (the user, 2026-10-09): the job unlocks of the era and
-- the support jobs. A nation's missions go by its rank ladder (below)
xi.cardian.errands.entries =
{
    quest(xi.questLog.OTHER_AREAS, 'THE_OLD_LADY', 90, { z.MHAURA, z.SELBINA, z.MHAURA }, xi.job.NONE),
    quest(xi.questLog.SANDORIA, 'A_KNIGHTS_TEST', 120, { z.SOUTHERN_SAN_DORIA, z.WEST_RONFAURE, z.LA_THEINE_PLATEAU, z.JUGNER_FOREST, z.DAVOI }, xi.job.PLD),
    quest(xi.questLog.BASTOK, 'BLADE_OF_DARKNESS', 120, { z.BASTOK_MINES, z.ZERUHN_MINES, z.NORTH_GUSTABERG, z.KONSCHTAT_HIGHLANDS, z.PASHHOW_MARSHLANDS, z.BEADEAUX }, xi.job.DRK),
    quest(xi.questLog.BASTOK, 'AYAME_AND_KAEDE', 120, { z.PORT_BASTOK, z.KORROLOKA_TUNNEL, z.NORG, z.PORT_BASTOK }, xi.job.NIN),
    quest(xi.questLog.WINDURST, 'THE_FANGED_ONE', 90, { z.WINDURST_WOODS, z.EAST_SARUTABARUTA, z.TAHRONGI_CANYON, z.MERIPHATAUD_MOUNTAINS, z.SAUROMUGUE_CHAMPAIGN }, xi.job.RNG),
    quest(xi.questLog.WINDURST, 'I_CAN_HEAR_A_RAINBOW', 120, { z.WINDURST_WALLS, z.WEST_SARUTABARUTA, z.LA_THEINE_PLATEAU, z.WEST_RONFAURE, z.NORTH_GUSTABERG }, xi.job.SMN),
    quest(xi.questLog.JEUNO, 'PATH_OF_THE_BEASTMASTER', 60, { z.UPPER_JEUNO, z.LOWER_JEUNO }, xi.job.BST),
    quest(xi.questLog.JEUNO, 'PATH_OF_THE_BARD', 90, { z.LOWER_JEUNO, z.ROLANBERRY_FIELDS, z.PASHHOW_MARSHLANDS, z.KONSCHTAT_HIGHLANDS, z.VALKURM_DUNES }, xi.job.BRD),
    quest(xi.questLog.OUTLANDS, 'FORGE_YOUR_DESTINY', 120, { z.NORG, z.KONSCHTAT_HIGHLANDS, z.THE_SANCTUARY_OF_ZITAH }, xi.job.SAM),
}

-- A quest's id in its log, from its key; nil when the game has none by that name
local function idOf(entry)
    local ids = xi.quest.id[xi.quest.area[entry.log]]
    return ids and ids[entry.key]
end

xi.cardian.errands.goals = function(player)
    local found = {}
    for _, entry in ipairs(xi.cardian.errands.entries) do
        local id = idOf(entry)
        if id ~= nil and player:hasCompletedQuest(entry.log, id) then
            found[#found + 1] =
            {
                kind    = entry.kind,
                log     = entry.log,
                id      = id,
                title   = titleFromKey(entry.key),
                minutes = entry.minutes,
                zones   = entry.zones,
                job     = entry.job,
            }
        end
    end
    return found
end

-- Each nation's missions in the log's order, the era's story to the Shadow
-- Lord (rank 6), grouped by rank step: a step's missions are the ones a
-- player clears to hold that rank, its journeys abroad included, and its last
-- one grants it (the user, 2026-10-09: catch a recruit up to his rank)
local function m(key, step, minutes, zones)
    return { key = key, step = step, minutes = minutes, zones = zones }
end

xi.cardian.errands.ranks =
{
    [xi.nation.SANDORIA] =
    {
        m('SMASH_THE_ORCISH_SCOUTS', 2, 20, { z.NORTHERN_SAN_DORIA, z.WEST_RONFAURE }),
        m('BAT_HUNT', 2, 20, { z.NORTHERN_SAN_DORIA, z.EAST_RONFAURE, z.KING_RANPERRES_TOMB }),
        m('SAVE_THE_CHILDREN', 2, 30, { z.NORTHERN_SAN_DORIA, z.WEST_RONFAURE, z.GHELSBA_OUTPOST }),
        m('THE_RESCUE_DRILL', 3, 30, { z.NORTHERN_SAN_DORIA, z.WEST_RONFAURE, z.LA_THEINE_PLATEAU }),
        m('THE_DAVOI_REPORT', 3, 30, { z.LA_THEINE_PLATEAU, z.JUGNER_FOREST, z.DAVOI }),
        m('JOURNEY_ABROAD', 3, 20, { z.CHATEAU_DORAGUILLE, z.NORTHERN_SAN_DORIA }),
        m('JOURNEY_TO_BASTOK', 3, 30, { z.LA_THEINE_PLATEAU, z.KONSCHTAT_HIGHLANDS, z.NORTH_GUSTABERG, z.BASTOK_MINES, z.PALBOROUGH_MINES }),
        m('JOURNEY_TO_WINDURST', 3, 30, { z.BUBURIMU_PENINSULA, z.TAHRONGI_CANYON, z.WINDURST_WOODS, z.GIDDEUS }),
        m('JOURNEY_TO_BASTOK2', 3, 30, { z.BASTOK_MINES, z.NORTH_GUSTABERG, z.WAUGHROON_SHRINE }),
        m('JOURNEY_TO_WINDURST2', 3, 30, { z.WINDURST_WOODS, z.TAHRONGI_CANYON, z.BALGAS_DAIS, z.NORTHERN_SAN_DORIA }),
        m('INFILTRATE_DAVOI', 4, 45, { z.NORTHERN_SAN_DORIA, z.JUGNER_FOREST, z.DAVOI }),
        m('THE_CRYSTAL_SPRING', 4, 45, { z.NORTHERN_SAN_DORIA, z.LA_THEINE_PLATEAU, z.ORDELLES_CAVES }),
        m('APPOINTMENT_TO_JEUNO', 4, 60, { z.JUGNER_FOREST, z.BATALLIA_DOWNS, z.RULUDE_GARDENS, z.QUFIM_ISLAND, z.LOWER_DELKFUTTS_TOWER, z.QUBIA_ARENA }),
        m('MAGICITE', 5, 90, { z.RULUDE_GARDENS, z.DAVOI, z.BEADEAUX, z.CASTLE_OZTROJA, z.RULUDE_GARDENS }),
        m('THE_RUINS_OF_FEI_YIN', 6, 60, { z.BATALLIA_DOWNS, z.BEAUCEDINE_GLACIER, z.FEIYIN }),
        m('THE_SHADOW_LORD', 6, 90, { z.BEAUCEDINE_GLACIER, z.XARCABARD, z.CASTLE_ZVAHL_BAILEYS, z.CASTLE_ZVAHL_KEEP, z.THRONE_ROOM }),
    },
    [xi.nation.BASTOK] =
    {
        m('THE_ZERUHN_REPORT', 2, 20, { z.BASTOK_MINES, z.ZERUHN_MINES }),
        m('GEOLOGICAL_SURVEY', 2, 20, { z.PORT_BASTOK, z.SOUTH_GUSTABERG, z.DANGRUF_WADI }),
        m('FETICHISM', 2, 30, { z.BASTOK_MINES, z.NORTH_GUSTABERG, z.PALBOROUGH_MINES }),
        m('THE_CRYSTAL_LINE', 3, 30, { z.METALWORKS, z.NORTH_GUSTABERG, z.KONSCHTAT_HIGHLANDS }),
        m('WADING_BEASTS', 3, 30, { z.BASTOK_MINES, z.NORTH_GUSTABERG, z.PASHHOW_MARSHLANDS }),
        m('THE_EMISSARY', 3, 20, { z.METALWORKS, z.BASTOK_MINES }),
        m('THE_EMISSARY_SANDORIA', 3, 30, { z.KONSCHTAT_HIGHLANDS, z.LA_THEINE_PLATEAU, z.NORTHERN_SAN_DORIA, z.GHELSBA_OUTPOST }),
        m('THE_EMISSARY_WINDURST', 3, 30, { z.BUBURIMU_PENINSULA, z.TAHRONGI_CANYON, z.WINDURST_WOODS, z.GIDDEUS }),
        m('THE_EMISSARY_SANDORIA2', 3, 30, { z.NORTHERN_SAN_DORIA, z.WEST_RONFAURE, z.HORLAIS_PEAK }),
        m('THE_EMISSARY_WINDURST2', 3, 30, { z.WINDURST_WOODS, z.TAHRONGI_CANYON, z.BALGAS_DAIS, z.BASTOK_MINES }),
        m('THE_FOUR_MUSKETEERS', 4, 45, { z.METALWORKS, z.PASHHOW_MARSHLANDS, z.BEADEAUX }),
        m('TO_THE_FORSAKEN_MINES', 4, 45, { z.BASTOK_MINES, z.NORTH_GUSTABERG, z.GUSGEN_MINES }),
        m('JEUNO', 4, 60, { z.KONSCHTAT_HIGHLANDS, z.PASHHOW_MARSHLANDS, z.ROLANBERRY_FIELDS, z.RULUDE_GARDENS, z.QUFIM_ISLAND, z.LOWER_DELKFUTTS_TOWER, z.QUBIA_ARENA }),
        m('MAGICITE', 5, 90, { z.RULUDE_GARDENS, z.DAVOI, z.BEADEAUX, z.CASTLE_OZTROJA, z.RULUDE_GARDENS }),
        m('DARKNESS_RISING', 6, 60, { z.BATALLIA_DOWNS, z.BEAUCEDINE_GLACIER, z.FEIYIN }),
        m('XARCABARD_LAND_OF_TRUTHS', 6, 90, { z.BEAUCEDINE_GLACIER, z.XARCABARD, z.CASTLE_ZVAHL_BAILEYS, z.CASTLE_ZVAHL_KEEP, z.THRONE_ROOM }),
    },
    [xi.nation.WINDURST] =
    {
        m('THE_HORUTOTO_RUINS_EXPERIMENT', 2, 20, { z.WINDURST_WATERS, z.EAST_SARUTABARUTA, z.INNER_HORUTOTO_RUINS }),
        m('THE_HEART_OF_THE_MATTER', 2, 20, { z.WINDURST_WATERS, z.EAST_SARUTABARUTA, z.OUTER_HORUTOTO_RUINS }),
        m('THE_PRICE_OF_PEACE', 2, 30, { z.WINDURST_WOODS, z.EAST_SARUTABARUTA, z.TAHRONGI_CANYON, z.GIDDEUS }),
        m('LOST_FOR_WORDS', 3, 30, { z.WINDURST_WATERS, z.TAHRONGI_CANYON, z.MAZE_OF_SHAKHRAMI }),
        m('A_TESTING_TIME', 3, 30, { z.WINDURST_WATERS, z.WEST_SARUTABARUTA, z.EAST_SARUTABARUTA }),
        m('THE_THREE_KINGDOMS', 3, 20, { z.HEAVENS_TOWER, z.WINDURST_WALLS }),
        m('THE_THREE_KINGDOMS_SANDORIA', 3, 30, { z.BUBURIMU_PENINSULA, z.LA_THEINE_PLATEAU, z.NORTHERN_SAN_DORIA, z.GHELSBA_OUTPOST }),
        m('THE_THREE_KINGDOMS_BASTOK', 3, 30, { z.KONSCHTAT_HIGHLANDS, z.NORTH_GUSTABERG, z.BASTOK_MINES, z.PALBOROUGH_MINES }),
        m('THE_THREE_KINGDOMS_SANDORIA2', 3, 30, { z.NORTHERN_SAN_DORIA, z.WEST_RONFAURE, z.HORLAIS_PEAK }),
        m('THE_THREE_KINGDOMS_BASTOK2', 3, 30, { z.BASTOK_MINES, z.NORTH_GUSTABERG, z.WAUGHROON_SHRINE, z.WINDURST_WALLS }),
        m('TO_EACH_HIS_OWN_RIGHT', 4, 45, { z.WINDURST_WALLS, z.MERIPHATAUD_MOUNTAINS, z.CASTLE_OZTROJA }),
        m('WRITTEN_IN_THE_STARS', 4, 45, { z.WINDURST_WATERS, z.TAHRONGI_CANYON, z.SAUROMUGUE_CHAMPAIGN }),
        m('A_NEW_JOURNEY', 4, 60, { z.SAUROMUGUE_CHAMPAIGN, z.ROLANBERRY_FIELDS, z.RULUDE_GARDENS, z.QUFIM_ISLAND, z.LOWER_DELKFUTTS_TOWER, z.QUBIA_ARENA }),
        m('MAGICITE', 5, 90, { z.RULUDE_GARDENS, z.DAVOI, z.BEADEAUX, z.CASTLE_OZTROJA, z.RULUDE_GARDENS }),
        m('THE_FINAL_SEAL', 6, 60, { z.BATALLIA_DOWNS, z.BEAUCEDINE_GLACIER, z.FEIYIN }),
        m('THE_SHADOW_AWAITS', 6, 90, { z.BEAUCEDINE_GLACIER, z.XARCABARD, z.CASTLE_ZVAHL_BAILEYS, z.CASTLE_ZVAHL_KEEP, z.THRONE_ROOM }),
    },
}

-- A nation's ladder as the map reads it: each mission's id in the nation's
-- log and its title, the game's own; one the game has no id for is left out
xi.cardian.errands.ladder = function(nation)
    local out    = {}
    local ladder = xi.cardian.errands.ranks[nation]
    local ids    = xi.mission.id[xi.mission.area[nation]]
    if ladder == nil or ids == nil then
        return out
    end
    for _, mission in ipairs(ladder) do
        local id = ids[mission.key]
        if id ~= nil then
            out[#out + 1] =
            {
                id      = id,
                step    = mission.step,
                minutes = mission.minutes,
                zones   = mission.zones,
                title   = titleFromKey(mission.key),
            }
        end
    end
    return out
end
