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
    -- follow stepping underpaces the client's run rate, so pawns need a
    -- higher value to keep formation with a running player.
    PAWN_SPEED = 80,
}
