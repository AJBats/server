-----------------------------------
-- Cardian mission hints: San d'Oria rank 2
-- 2-2 The Davoi Report and 2-3 Journey Abroad (with Journey to Bastok and
-- Journey to Windurst, in either order), up to rank 3.
--
-- Data only (modules/cardian/lua/mission_hints.lua reads it; so does the
-- MUD bench, tools/hints). A step: `id`; `when`, the conditions that make
-- it the player's step (the first step, top down, whose conditions all
-- hold); `say`, what a cardian volunteers in party chat when he first
-- reaches it -- a nudge, no more than the step needs; `more`, optional,
-- the specifics she gives when asked (`!cardian hint`); and `notes`,
-- optional, keyed by zone id -- said on entering that zone, on reaching
-- the step there, and when asked there. The conditions: nation, rank,
-- current, status, done, notDone, ki, noKi, item, under, zone, notZone,
-- var, level -- see mission_hints.lua. At most two lines a hint, each at
-- most ~110 characters, so the chat log shows it whole. A line may be
-- { when = { conditions }, "text" }, said only while they hold: a warp tip
-- (`warps = true`: the era's crystals don't warp), a line about a map we
-- may have bought since.
--
-- `goal`, optional: where in a zone the step is done, and what Zapp calls
-- it -- `pos`, its { x, z } in the game (from the geography research), and
-- for the bench a map square (`at`) or someone or something there
-- (`target`). `route`, optional: the zones on the road there, in order, or
-- a list of such roads. Asked, she answers from where we stand with them
-- (mission_hints.lua); the bench also plays hot and cold with the goal.
--
-- How the words are chosen (the playtests, tools/hints/playtest):
--  - `say` gives the direction and the stakes, in her voice: which zones
--    to cross, who we're looking for, whether we're ready. No map squares,
--    no puzzle steps, and nothing the NPC has just said.
--  - `more` has the squares and the steps, for whoever asks.
--  - notes only where the game's own map doesn't settle the way: forks,
--    and the zones we have no map of.
--  - readiness follows the party's level, and a step that turns on level
--    has her react once we reach it.
--
-- The statuses and key items follow the server's own scripts,
-- scripts/missions/sandoria/2_2_The_Davoi_Report.lua and 2_3_*.lua. The
-- map squares come from tools/hints/research/geography.yaml (the client's
-- own map grid). Misha owns the area maps and Davoi's, not Palborough
-- Mines' or Giddeus', and Heavens Tower, Waughroon Shrine and Balga's
-- Dais have no map at all.
-----------------------------------
local SANDORIA = xi.mission.log_id.SANDORIA
local NONE     = xi.mission.id.nation.NONE
local mission  = xi.mission.id.sandoria
local zone     = xi.zone

-- Where the road forks, or we have no map

local valkurmFork =
{
    [zone.VALKURM_DUNES] = { "Bastok lies past Konschtat Highlands; Windurst means the ferry from Selbina." },
}

local toWindurstByFerry =
{
    [zone.VALKURM_DUNES] = { "Selbina's gate is west across the dunes. The ferry leaves from its south end." },
    [zone.MHAURA]        = { "Windurst is west over land from here: Buburimu Peninsula, then Tahrongi Canyon, then East Sarutabaruta." },
}

local toBastokFromMhaura =
{
    [zone.MHAURA]        = { "Only some of these ships sail to Selbina: the ones at midnight, 8:00 and 16:00." },
    [zone.VALKURM_DUNES] = { "Bastok is south-east, through Konschtat Highlands and North Gustaberg." },
}

local outOfWindurstWalls  = { "West Sarutabaruta is through Windurst Waters: west from the Walls, then out the Waters' north-west gate." }
local outOfWindurstWaters = { "West Sarutabaruta is out the north-west gate." }
local outOfWindurstWoods  = { "West Sarutabaruta: through Port Windurst, south-west, and out its north-west gate." }

local giddeusDown =
{
    [zone.WINDURST_WALLS]  = outOfWindurstWalls,
    [zone.WINDURST_WATERS] = outOfWindurstWaters,
    [zone.WINDURST_WOODS]  = outOfWindurstWoods,
    [zone.GIDDEUS]         =
    {
        { when = { noKi = xi.ki.MAP_OF_GIDDEUS }, "No map of Giddeus." },
        "Their holy places would be below ground: watch the west side for a way down as we go south.",
    },
}

local giddeusSouth =
{
    [zone.WINDURST_WALLS]  = outOfWindurstWalls,
    [zone.WINDURST_WATERS] = outOfWindurstWaters,
    [zone.WINDURST_WOODS]  = outOfWindurstWoods,
    [zone.GIDDEUS]         =
    {
        { when = { noKi = xi.ki.MAP_OF_GIDDEUS }, "No map here." },
        "The Dais is at the far south end: keep to the west side and follow the valley to its end.",
    },
    [zone.BALGAS_DAIS]     = { "The Burning Circle is just ahead. Touch it when you're ready; our buffs won't come in with us." },
}

local rexSellsMaps  = { { when = { noKi = xi.ki.MAP_OF_THE_PALBOROUGH_MINES }, "Rex, in the middle of the port, sells maps. We've none of the mines." } }
local roundTheHill  = { "The mines are at the far north. The way round goes up the west side of the big hill." }

local toPalborough =
{
    [zone.PORT_BASTOK]      = rexSellsMaps,
    [zone.NORTH_GUSTABERG]  = roundTheHill,
    [zone.PALBOROUGH_MINES] =
    {
        { when = { noKi = xi.ki.MAP_OF_THE_PALBOROUGH_MINES }, "No map of these mines." },
        "Grohm's seam should be close to the entrance.",
    },
}

local toShrine =
{
    [zone.PORT_BASTOK]      = rexSellsMaps,
    [zone.NORTH_GUSTABERG]  = roundTheHill,
    [zone.PALBOROUGH_MINES] = { "The shrine is off the top floor." },
    [zone.WAUGHROON_SHRINE] = { "The Burning Circle is just ahead. Touch it when you're ready; our buffs won't come in with us." },
}

local homeFromAbroad =
{
    [zone.METALWORKS]     = { { when = { warps = true }, "The nearest crystal is in Bastok Markets, by the South Gustaberg gate." } },
    [zone.WINDURST_WOODS] = { { when = { warps = true }, "The Woods' own crystal is just behind the Manustery." } },
}

-- The roads, zone by zone, as the hints send us. A route is one road, or a
-- list of roads (from either city, or forking toward either nation): asked,
-- she names the next zone on the first road that runs on from here.

local toDavoi                =
{
    { zone.SOUTHERN_SAN_DORIA, zone.WEST_RONFAURE, zone.LA_THEINE_PLATEAU, zone.JUGNER_FOREST, zone.DAVOI },
    { zone.NORTHERN_SAN_DORIA, zone.WEST_RONFAURE, zone.LA_THEINE_PLATEAU, zone.JUGNER_FOREST, zone.DAVOI },
}

-- Out from the castle to the fork in Valkurm Dunes (its note says which
-- road is whose), then on toward either nation
local toFirstConsul =
{
    { zone.CHATEAU_DORAGUILLE, zone.NORTHERN_SAN_DORIA, zone.WEST_RONFAURE, zone.LA_THEINE_PLATEAU, zone.VALKURM_DUNES },
    { zone.VALKURM_DUNES, zone.KONSCHTAT_HIGHLANDS, zone.NORTH_GUSTABERG, zone.PORT_BASTOK, zone.BASTOK_MARKETS, zone.METALWORKS },
    { zone.VALKURM_DUNES, zone.SELBINA, zone.MHAURA, zone.BUBURIMU_PENINSULA, zone.TAHRONGI_CANYON, zone.EAST_SARUTABARUTA, zone.WINDURST_WOODS },
}

local homeToHalver =
{
    {
        zone.METALWORKS, zone.BASTOK_MARKETS, zone.PORT_BASTOK, zone.NORTH_GUSTABERG, zone.KONSCHTAT_HIGHLANDS,
        zone.VALKURM_DUNES, zone.LA_THEINE_PLATEAU, zone.WEST_RONFAURE, zone.NORTHERN_SAN_DORIA,
    },
    {
        zone.WINDURST_WOODS, zone.EAST_SARUTABARUTA, zone.TAHRONGI_CANYON, zone.BUBURIMU_PENINSULA, zone.MHAURA, zone.SELBINA,
        zone.VALKURM_DUNES, zone.LA_THEINE_PLATEAU, zone.WEST_RONFAURE, zone.NORTHERN_SAN_DORIA,
    },
}
local fromDavoi              = { zone.DAVOI, zone.JUGNER_FOREST, zone.LA_THEINE_PLATEAU, zone.WEST_RONFAURE, zone.NORTHERN_SAN_DORIA }
local metalworksToPalborough = { zone.METALWORKS, zone.BASTOK_MARKETS, zone.PORT_BASTOK, zone.NORTH_GUSTABERG, zone.PALBOROUGH_MINES }
local palboroughToMetalworks = { zone.PALBOROUGH_MINES, zone.NORTH_GUSTABERG, zone.PORT_BASTOK, zone.BASTOK_MARKETS, zone.METALWORKS }
local shrineToMetalworks     = { zone.WAUGHROON_SHRINE, zone.PALBOROUGH_MINES, zone.NORTH_GUSTABERG, zone.PORT_BASTOK, zone.BASTOK_MARKETS, zone.METALWORKS }
local towerToGiddeus         = { zone.HEAVENS_TOWER, zone.WINDURST_WALLS, zone.WINDURST_WATERS, zone.WEST_SARUTABARUTA, zone.GIDDEUS }
local giddeusToWoods         = { zone.GIDDEUS, zone.WEST_SARUTABARUTA, zone.EAST_SARUTABARUTA, zone.WINDURST_WOODS }

local bastokToWindurst =
{
    zone.METALWORKS, zone.BASTOK_MARKETS, zone.PORT_BASTOK, zone.NORTH_GUSTABERG, zone.KONSCHTAT_HIGHLANDS, zone.VALKURM_DUNES,
    zone.SELBINA, zone.MHAURA, zone.BUBURIMU_PENINSULA, zone.TAHRONGI_CANYON, zone.EAST_SARUTABARUTA, zone.WINDURST_WOODS,
}

local windurstToBastok =
{
    zone.WINDURST_WOODS, zone.EAST_SARUTABARUTA, zone.TAHRONGI_CANYON, zone.BUBURIMU_PENINSULA, zone.MHAURA, zone.SELBINA,
    zone.VALKURM_DUNES, zone.KONSCHTAT_HIGHLANDS, zone.NORTH_GUSTABERG, zone.PORT_BASTOK, zone.BASTOK_MARKETS, zone.METALWORKS,
}

return
{
    title = "San d'Oria rank 2: The Davoi Report, Journey Abroad",

    steps =
    {
        -----------------------------------
        -- 2-2 The Davoi Report
        -----------------------------------
        {
            id    = 'sandoria.davoi.go-green',
            when  = { current = { SANDORIA, mission.THE_DAVOI_REPORT }, status = 0, level = { 1, 15 } },
            say   =
            {
                "Davoi is the Orcs' stronghold, past La Theine Plateau and Jugner Forest. At our level they're far stronger.",
                "I wonder if we can slip in and out without any trouble...",
            },
            more  =
            {
                "West Ronfaure south to La Theine (F-12), east across the plateau to Jugner (M-8), then east along",
                "Jugner's southern wall to Davoi (G-12). The Temple Knight hides just inside, by a tree (J-7).",
            },
            notes = { [zone.DAVOI] = { "Quietly now. The knight should be close to the entrance." } },
            goal  = { zone = zone.DAVOI, target = 'Zantaviat', pos = { 212, -9 }, name = "the Temple Knight" },
            route = toDavoi,
        },
        {
            id    = 'sandoria.davoi.go',
            when  = { current = { SANDORIA, mission.THE_DAVOI_REPORT }, status = 0 },
            say   =
            {
                "We're sturdier now. Davoi, then: past La Theine and Jugner Forest. Its Orcs still outclass us; no lingering.",
            },
            more  =
            {
                "West Ronfaure south to La Theine (F-12), east across the plateau to Jugner (M-8), then east along",
                "Jugner's southern wall to Davoi (G-12). The Temple Knight hides just inside, by a tree (J-7).",
            },
            notes = { [zone.DAVOI] = { "Quietly now. The knight should be close to the entrance." } },
            goal  = { zone = zone.DAVOI, target = 'Zantaviat', pos = { 212, -9 }, name = "the Temple Knight" },
            route = toDavoi,
        },
        {
            id   = 'sandoria.davoi.find-document',
            when = { current = { SANDORIA, mission.THE_DAVOI_REPORT }, status = 1 },
            say  =
            {
                "I'll watch for Orcs while you look.",
            },
            more =
            {
                "There's a pond just south of him, around (J-8). The papers should be on its south bank.",
            },
            goal = { zone = zone.DAVOI, at = 'J-8', pos = { 211, -104 }, name = "the water he mentioned" },
        },
        {
            id   = 'sandoria.davoi.return-document',
            when = { current = { SANDORIA, mission.THE_DAVOI_REPORT }, status = 2, ki = xi.ki.LOST_DOCUMENT },
            say  =
            {
                "That's it! Back to Zantaviat before the Orcs notice.",
            },
            more =
            {
                "He's by the entrance, north of the pond (J-7).",
            },
            goal = { zone = zone.DAVOI, target = 'Zantaviat', pos = { 212, -9 }, name = "Zantaviat" },
        },
        {
            id    = 'sandoria.davoi.report',
            when  = { current = { SANDORIA, mission.THE_DAVOI_REPORT }, status = 3, ki = xi.ki.TEMPLE_KNIGHTS_DAVOI_REPORT },
            say   =
            {
                "Home to San d'Oria with it. A Temple Knight's report belongs with the Church, I'd think, not a gate guard.",
            },
            more  =
            {
                "The Papal Chambers: the door on the top floor of the Cathedral, east side of Northern San d'Oria (M-6).",
            },
            goal  = { zone = zone.NORTHERN_SAN_DORIA, target = 'Door: Papal Chambers', pos = { 130, 122 }, name = "the Church" },
            route = fromDavoi,
        },

        -----------------------------------
        -- 2-3 Journey Abroad: the orders, the road between, the report home
        -----------------------------------
        {
            id   = 'sandoria.journey.halver',
            when = { current = { SANDORIA, mission.JOURNEY_ABROAD }, status = 0 },
            say  =
            {
                "Chateau d'Oraguille is the castle at the north end of Northern San d'Oria.",
            },
            more =
            {
                "The castle gate is at the north end of the main avenue (I-6). Halver waits just inside.",
            },
            goal = { zone = zone.NORTHERN_SAN_DORIA, at = 'I-6', pos = { -21, 140 }, name = "the castle" },
        },
        {
            id    = 'sandoria.journey.first-consul',
            when  = { current = { SANDORIA, mission.JOURNEY_ABROAD }, status = 2, ki = xi.ki.LETTER_TO_THE_CONSULS_SANDORIA },
            say   =
            {
                "Bastok and Windurst, then! Both roads start south, through La Theine Plateau to Valkurm Dunes.",
                { when = { warps = true }, "Let's touch the home point crystal in every town we pass. We can warp between them later." },
            },
            more  =
            {
                "Bastok: on foot from Valkurm, through Konschtat Highlands and North Gustaberg; Savae is in the Metalworks.",
                "Windurst: the ferry from Selbina to Mhaura, then west over land; Mourices is in Windurst Woods.",
            },
            notes = valkurmFork,
            route = toFirstConsul,
        },
        {
            id    = 'sandoria.journey.go-windurst',
            when  = { current = { SANDORIA, mission.JOURNEY_ABROAD }, status = 6 },
            say   =
            {
                "Windurst now: across the sea from Selbina, then west from Mhaura.",
                { when = { warps = true }, "If we touched Selbina's crystal, warp there." },
            },
            more  =
            {
                "Back to Valkurm Dunes, west to Selbina and the ferry; from Mhaura, Buburimu, Tahrongi, East Sarutabaruta.",
                "Mourices is at the San d'Orian Consulate in Windurst Woods (G-10).",
            },
            notes = toWindurstByFerry,
            goal  = { zone = zone.WINDURST_WOODS, target = 'Mourices', pos = { -51, -28 }, name = "our consulate" },
            route = bastokToWindurst,
        },
        {
            id    = 'sandoria.journey.go-bastok',
            when  = { current = { SANDORIA, mission.JOURNEY_ABROAD }, status = 7 },
            say   =
            {
                "Bastok now: the ferry from Mhaura back to Selbina, then east over land.",
                { when = { warps = true }, "A warp to Selbina saves the sailing." },
            },
            more  =
            {
                "From Selbina: Valkurm Dunes, south-east to Konschtat Highlands, North Gustaberg, Port Bastok.",
                "Savae E Paleade is at the San d'Orian Consulate, on the Metalworks' upper floor (I-9).",
            },
            notes = toBastokFromMhaura,
            goal  = { zone = zone.METALWORKS, target = 'Savae E Paleade', pos = { 24, -43 }, name = "our consulate" },
            route = windurstToBastok,
        },
        {
            id    = 'sandoria.journey.home',
            when  = { current = { SANDORIA, mission.JOURNEY_ABROAD }, status = 11, ki = xi.ki.KINDRED_REPORT },
            say   =
            {
                "Both consuls are satisfied! Halver will want this report.",
                { when = { warps = true }, "A home point warp gets us home fastest." },
            },
            more  =
            {
                "Halver is just inside Chateau d'Oraguille, at the north end of Northern San d'Oria (I-6).",
            },
            notes = homeFromAbroad,
            route = homeToHalver,
            goal  = { zone = zone.NORTHERN_SAN_DORIA, at = 'I-6', pos = { -21, 140 }, name = "the castle" },
        },
        {
            id   = 'sandoria.journey.rank3',
            when = { nation = SANDORIA, rank = 3, current = { SANDORIA, NONE }, done = { SANDORIA, mission.JOURNEY_ABROAD } },
            say  =
            {
                "Rank 3! I'm proud to be traveling with you.",
            },
        },

        -----------------------------------
        -- Journey to Bastok (first nation): mythril sand
        -----------------------------------
        {
            id   = 'sandoria.bastok1.pius',
            when = { current = { SANDORIA, mission.JOURNEY_TO_BASTOK }, status = 3 },
            say  =
            {
                "The Department of Industry should be on this same upper floor.",
            },
            more =
            {
                "East along this floor, up a few steps (J-8). Pius is inside.",
            },
            goal = { zone = zone.METALWORKS, target = 'Pius', pos = { 100, -13 }, name = "the Department of Industry" },
        },
        {
            id   = 'sandoria.bastok1.grohm',
            when = { current = { SANDORIA, mission.JOURNEY_TO_BASTOK }, status = 4 },
            say  =
            {
                "The Craftsmen's Eatery -- back toward the lifts, I think.",
            },
            more =
            {
                "Back west, beside the lifts (H-9). Grohm is inside.",
            },
            goal = { zone = zone.METALWORKS, target = 'Grohm', pos = { -18, -28 }, name = "the Craftsmen's Eatery" },
        },
        {
            id    = 'sandoria.bastok1.hand-in',
            when  = { current = { SANDORIA, mission.JOURNEY_TO_BASTOK }, status = 5, item = { xi.item.ONZ_OF_MYTHRIL_SAND, 1 } },
            say   =
            {
                "Mythril sand! Pius asked for it, but it's our consul who speaks for San d'Oria here.",
            },
            more  =
            {
                "Trade the sand to Savae E Paleade, at the San d'Orian Consulate on the Metalworks' upper floor (I-9).",
            },
            goal  = { zone = zone.METALWORKS, target = 'Savae E Paleade', pos = { 24, -43 }, name = "our consulate" },
            route = palboroughToMetalworks,
        },
        {
            id   = 'sandoria.bastok1.bottom-lever',
            when = { current = { SANDORIA, mission.JOURNEY_TO_BASTOK }, status = 5, var = { 'refiner_output', 1 } },
            say  =
            {
                "It's running!",
            },
            more =
            {
                "The bottom lever is under the machine, a floor down: drop off the ledge, and pull it.",
            },
            goal = { zone = zone.PALBOROUGH_MINES, target = 'Refiner Lever (bottom)', pos = { 176, 176 }, name = "where that noise came from" },
        },
        {
            id   = 'sandoria.bastok1.top-lever',
            when = { current = { SANDORIA, mission.JOURNEY_TO_BASTOK }, status = 5, var = { 'refiner_input', 1 } },
            say  =
            {
                "In it goes.",
            },
            more =
            {
                "Pull the lever right beside the lid.",
            },
        },
        {
            id   = 'sandoria.bastok1.refine',
            when = { current = { SANDORIA, mission.JOURNEY_TO_BASTOK }, status = 5, item = { xi.item.CHUNK_OF_MINE_GRAVEL, 1 } },
            say  =
            {
                "Gravel! Now for that refiner. It'll be higher up in the mines.",
            },
            more =
            {
                "Keep right to the lift on the east side of this floor and ride it up. The Refiner Lid is just north:",
                "trade it the gravel, pull the lever beside it, then the one underneath, a floor down.",
            },
            goal = { zone = zone.PALBOROUGH_MINES, target = 'Refiner Lid', pos = { 180, 170 }, name = "the refiner" },
        },
        {
            id    = 'sandoria.bastok1.mine',
            when  = { current = { SANDORIA, mission.JOURNEY_TO_BASTOK }, status = 5, item = { xi.item.PICKAXE, 1 } },
            say   =
            {
                "Palborough Mines is up in North Gustaberg, beyond Port Bastok's gate. Pickaxes break; mind the count.",
            },
            more  =
            {
                "The entrance is at the far north of North Gustaberg (K-3), round the west side of the big hill.",
                "The seam is one room in, north, on the left-hand wall: trade it a pickaxe.",
                { when = { noKi = xi.ki.MAP_OF_THE_PALBOROUGH_MINES }, "Rex in Port Bastok sells a map." },
            },
            notes = toPalborough,
            goal  = { zone = zone.PALBOROUGH_MINES, target = 'Mythril Seam', pos = { -70, 173 }, name = "a mythril seam" },
            route = metalworksToPalborough,
        },
        {
            id    = 'sandoria.bastok1.out-of-pickaxes',
            when  = { current = { SANDORIA, mission.JOURNEY_TO_BASTOK }, status = 5 },
            say   =
            {
                "No pickaxes left. Someone in Bastok must sell them.",
            },
            more  =
            {
                "Numa, on the west side of Port Bastok (E-7), sells pickaxes for 200 gil.",
            },
            goal  = { zone = zone.PORT_BASTOK, target = 'Numa', pos = { -135, -17 }, name = "someone who sells pickaxes" },
            route = { zone.PALBOROUGH_MINES, zone.NORTH_GUSTABERG, zone.PORT_BASTOK },
        },

        -----------------------------------
        -- Journey to Windurst (first nation): the offering and two shields
        -----------------------------------
        {
            id    = 'sandoria.windurst1.tower',
            when  = { current = { SANDORIA, mission.JOURNEY_TO_WINDURST }, status = { 3, 4 } },
            say   =
            {
                "The Heavens Tower rises in the middle of Windurst Walls, north-west of here.",
            },
            more  =
            {
                "Through the Woods' north-west passage (G-7) into the Walls, then over the bridge (H-7).",
                "Kupipi's desk is straight ahead inside.",
            },
            goal  = { zone = zone.WINDURST_WALLS, at = 'H-7', pos = { -1, 120 }, name = "the Heavens Tower" },
            route = { zone.WINDURST_WOODS, zone.WINDURST_WALLS },
        },
        {
            id   = 'sandoria.windurst1.giddeus-with-shields',
            when = { current = { SANDORIA, mission.JOURNEY_TO_WINDURST }, status = 5, ki = xi.ki.SHIELD_OFFERING, item = { xi.item.PARANA_SHIELD, 2 } },
            say  =
            {
                "Two shields! Now the offering, for Uu Zhoumo.",
            },
            more =
            {
                "He stands by the great doors, west of the underground hall.",
            },
            goal = { zone = zone.GIDDEUS, target = 'Uu Zhoumo', pos = { -180, 158 }, name = "Uu Zhoumo" },
        },
        {
            id    = 'sandoria.windurst1.giddeus',
            when  = { current = { SANDORIA, mission.JOURNEY_TO_WINDURST }, status = 5, ki = xi.ki.SHIELD_OFFERING },
            say   =
            {
                "Giddeus is where the Yagudo live. Uu Zhoumo's there -- and maybe Mourices' two missing shields.",
            },
            more  =
            {
                "West Sarutabaruta, then Giddeus to its west (F-8). Below ground, Zhuu Buxu the Silent carries a Parana",
                "shield; he's back about 5 minutes after each kill. Uu Zhoumo stands by the great doors.",
            },
            notes = giddeusDown,
            goal  = { zone = zone.GIDDEUS, at = 'G-8', pos = { -122, 40 }, name = "a way below ground" },
            route = towerToGiddeus,
        },
        {
            id    = 'sandoria.windurst1.hand-in',
            when  = { current = { SANDORIA, mission.JOURNEY_TO_WINDURST }, status = 6, item = { xi.item.PARANA_SHIELD, 2 } },
            say   =
            {
                "The offering's made and both shields are ours. Mourices will be glad.",
            },
            more  =
            {
                "Back to the San d'Orian Consulate in Windurst Woods (G-10): trade him both shields.",
            },
            goal  = { zone = zone.WINDURST_WOODS, target = 'Mourices', pos = { -51, -28 }, name = "our consulate" },
            route = giddeusToWoods,
        },
        {
            id   = 'sandoria.windurst1.shields',
            when = { current = { SANDORIA, mission.JOURNEY_TO_WINDURST }, status = 6 },
            say  =
            {
                "The Yagudo accepted it. Now, those two shields.",
            },
            more =
            {
                "Zhuu Buxu the Silent, in the underground hall, carries one each time. He's back about 5 minutes after.",
            },
            goal = { zone = zone.GIDDEUS, target = 'Zhuu Buxu the Silent', pos = { -42, 120 }, name = "whoever carries those shields" },
        },

        -----------------------------------
        -- Journey to Bastok (second nation): the dragon at Waughroon Shrine
        -----------------------------------
        {
            id   = 'sandoria.bastok2.pius',
            when = { current = { SANDORIA, mission.JOURNEY_TO_BASTOK2 }, status = 8 },
            say  =
            {
                "The Department of Industry should be on this same upper floor.",
            },
            more =
            {
                "East along this floor, up a few steps (J-8). Pius is inside.",
            },
            goal = { zone = zone.METALWORKS, target = 'Pius', pos = { 100, -13 }, name = "the Department of Industry" },
        },
        {
            id   = 'sandoria.bastok2.grohm',
            when = { current = { SANDORIA, mission.JOURNEY_TO_BASTOK2 }, status = 9 },
            say  =
            {
                "The Craftsmen's Eatery -- back toward the lifts, I think.",
            },
            more =
            {
                "Back west, beside the lifts (H-9). Grohm is inside.",
            },
            goal = { zone = zone.METALWORKS, target = 'Grohm', pos = { -18, -28 }, name = "the Craftsmen's Eatery" },
        },
        {
            id    = 'sandoria.bastok2.shrine-green',
            when  = { current = { SANDORIA, mission.JOURNEY_TO_BASTOK2 }, status = 10, level = { 1, 19 } },
            say   =
            {
                "A dragon, and the fight holds us to level 25. We're not ready.",
                "Let's train to 20 first. Valkurm Dunes or Jugner Forest will get us there.",
            },
            more  =
            {
                "Valkurm Dunes is back past Konschtat Highlands. Then Waughroon Shrine: Palborough Mines' top floor,",
                "by the lift on the east side of its first floor, then south-west.",
            },
            notes = toShrine,
        },
        {
            id    = 'sandoria.bastok2.shrine',
            when  = { current = { SANDORIA, mission.JOURNEY_TO_BASTOK2 }, status = 10 },
            say   =
            {
                "We're ready for the dragon. Waughroon Shrine is up at the top of Palborough Mines.",
            },
            more  =
            {
                "Palborough is at the far north of North Gustaberg (K-3). Ride the lift on the east side of the first floor",
                "up, then go south-west to the shrine. Touch the Burning Circle when you're ready.",
            },
            notes = toShrine,
            goal  = { zone = zone.PALBOROUGH_MINES, at = 'H-10', pos = { 118, -120 }, name = "the way to the shrine" },
            route = metalworksToPalborough,
        },
        {
            id    = 'sandoria.bastok2.report',
            when  = { current = { SANDORIA, mission.JOURNEY_TO_BASTOK2 }, status = 11, ki = xi.ki.KINDRED_CREST },
            say   =
            {
                "The Kindred crest! Savae will want to see it.",
            },
            more  =
            {
                "The San d'Orian Consulate, on the Metalworks' upper floor (I-9).",
            },
            goal  = { zone = zone.METALWORKS, target = 'Savae E Paleade', pos = { 24, -43 }, name = "our consulate" },
            route = shrineToMetalworks,
        },

        -----------------------------------
        -- Journey to Windurst (second nation): the dragon at Balga's Dais
        -----------------------------------
        {
            id    = 'sandoria.windurst2.kupipi',
            when  = { current = { SANDORIA, mission.JOURNEY_TO_WINDURST2 }, status = 7 },
            say   =
            {
                "The Heavens Tower rises in the middle of Windurst Walls, north-west of here.",
            },
            more  =
            {
                "Through the Woods' north-west passage (G-7) into the Walls, then over the bridge (H-7).",
                "Kupipi's desk is straight ahead inside.",
            },
            goal  = { zone = zone.WINDURST_WALLS, at = 'H-7', pos = { -1, 120 }, name = "the Heavens Tower" },
            route = { zone.WINDURST_WOODS, zone.WINDURST_WALLS },
        },
        {
            id    = 'sandoria.windurst2.dais-green',
            when  = { current = { SANDORIA, mission.JOURNEY_TO_WINDURST2 }, status = 8, ki = xi.ki.DARK_KEY, level = { 1, 19 } },
            say   =
            {
                "A dragon, and the fight holds us to level 25. We're not ready.",
                "Let's train to 20 first. Buburimu Peninsula, past Mhaura's gate, will get us there.",
            },
            more  =
            {
                { when = { warps = true }, "Mhaura's crystal is the quick way to Buburimu." },
                "Balga's Dais: West Sarutabaruta, Giddeus, and south along its west side to the tunnel at the far end.",
            },
            notes = giddeusSouth,
        },
        {
            id    = 'sandoria.windurst2.dais',
            when  = { current = { SANDORIA, mission.JOURNEY_TO_WINDURST2 }, status = 8, ki = xi.ki.DARK_KEY },
            say   =
            {
                "We're ready for the dragon. Balga's Dais is past Giddeus; we follow its west wall south.",
            },
            more  =
            {
                "West Sarutabaruta, then Giddeus to its west (F-8). Keep south past the terrace and the dip in the ground,",
                "down the narrow path to the tunnel. Touch the Burning Circle when you're ready.",
            },
            notes = giddeusSouth,
            goal  = { zone = zone.GIDDEUS, at = 'G-12', pos = { -122, -280 }, name = "the way south to the Dais" },
            route = towerToGiddeus,
        },
        {
            id    = 'sandoria.windurst2.report',
            when  = { current = { SANDORIA, mission.JOURNEY_TO_WINDURST2 }, status = { 9, 10 }, ki = xi.ki.KINDRED_CREST },
            say   =
            {
                "The Kindred crest! Kupipi asked for proof, but it's Mourices who reports home for us.",
            },
            more  =
            {
                "The San d'Orian Consulate in Windurst Woods (G-10).",
            },
            goal  = { zone = zone.WINDURST_WOODS, target = 'Mourices', pos = { -51, -28 }, name = "our consulate" },
            route = { zone.BALGAS_DAIS, zone.GIDDEUS, zone.WEST_SARUTABARUTA, zone.EAST_SARUTABARUTA, zone.WINDURST_WOODS },
        },
    },
}
