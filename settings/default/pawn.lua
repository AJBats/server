-----------------------------------
-- PAWN SETTINGS
-----------------------------------
-- Cardian pawn system: AI party members that are real player characters,
-- loaded session-less from character rows on the same account.
-----------------------------------

xi = xi or {}
xi.settings = xi.settings or {}

xi.settings.pawn =
{
    -- Allow !pawnspawn <charname> / !pawndespawn <charname>: spawn an
    -- offline character from your account into your zone as a session-less
    -- pawn. M2 experiment feature.
    ENABLE_PAWNS = false,

    -- Pawn movement speed (base entity speed; players are 50). Server-side
    -- stepping moves speed/50 yalms per 400ms tick, while the client renders
    -- a player's nominal 50 at roughly 5.3 yalms/sec -- pawns need ~105 to
    -- hold formation with a running player. NOTE: PC-type entities are
    -- clamped to map.SPEED_LIMIT (default 80) each step; raise that limit
    -- above this value or the pawn runs at the limit instead.
    PAWN_SPEED = 107,
}
