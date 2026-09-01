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

    -- Cardian Link: the direct TCP channel between the companion addon and
    -- this map server (RESEARCH.md §7). Newline text; the addon connects at
    -- load and both sides keep the link alive with pings. The link is
    -- load-bearing for the addon: when it cannot connect the addon says so
    -- in its UI rather than degrading.
    LINK_ENABLED = true,
    LINK_PORT    = 54250,
}
