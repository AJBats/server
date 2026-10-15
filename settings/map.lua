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

    -- Listings a player may have on sale at once: three pages of the game's
    -- own seven (the user, 2026-10-15). Over seven, the ah_pagination module
    -- (modules/init.txt) pages the game's own Sales Status; the Sell page of
    -- the Auction House screen shows them all
    AH_LIST_LIMIT = 21,
}
