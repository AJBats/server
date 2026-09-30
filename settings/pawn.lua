-----------------------------------
-- PAWN SETTINGS (local overrides)
-----------------------------------

xi = xi or {}
xi.settings = xi.settings or {}

xi.settings.pawn =
{
    ENABLE_PAWNS = true,
    PAWN_SPEED   = 100,
    GAMBIT_DEBUG = true,
    FORMATION_DEBUG = true,
    INVITE_ACCEPT_DELAY = 2500, -- experiment: human-like accept latency

    -- D0 measurement (world.cpp): the debug ring and its timer
    WORLD_TICK_DEBUG = true,
    WORLD_DEBUG_RING = 0,
    WORLD_WHERE_LOG = 5,      -- D3 authoring: the player's position in the map log
    WORLD_LOAD_REPORT = 60,
    WORLD_DEBUG_FARM = false,   -- D1 test: the ring farms
    WORLD_HUNT_MIN = 2,     -- dogfood: a load line a minute

    -- The seat waterfall (ROADMAP H): dev runs the shipped caps, 100
    -- standing server-wide and 700 per zone (settings/default/pawn.lua),
    -- the same as prod (2026-09-23). For a shallow test world, where the
    -- seat-doling order shows in a few steps, !pawnworld cap <standing>
    -- <faded> moves both live (0 0 = back to the defaults).
}
