-----------------------------------
-- MAP SETTINGS (local overrides)
-----------------------------------

xi = xi or {}
xi.settings = xi.settings or {}

xi.settings.map =
{
    -- Server-computed PC speed cap; pawn movement (server-stepped) needs
    -- headroom above the retail 80 to hold pace with a client-rendered runner
    SPEED_LIMIT = 140, -- room for the formation lead's catch-up sprint (pawn.FORMATION_CATCHUP_SPEED)
}
