-----------------------------------
-- CARDIAN SETTINGS
-----------------------------------
-- Settings for Cardian single-player features (pawn system, pause, charswap).
-----------------------------------

xi = xi or {}
xi.settings = xi.settings or {}

xi.settings.cardian =
{
    -- Allow !swapto <charname>: rezone the client into another character on
    -- the same account (target must be offline). M1 experiment feature.
    ENABLE_CHARSWAP = false,
}
