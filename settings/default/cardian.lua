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

    -- Re-engaging after a disengage. Retail once charged a short fixed
    -- wait; when players found that a slow two-hander could swing faster
    -- by disengaging and re-engaging than by standing and fighting, the
    -- wait became the weapon's FULL delay, which punished every slow
    -- weapon for the trick. Here the nerf applies only where the trick
    -- lived -- re-engaging the very mob just fought -- while a switch to
    -- a different mob takes this pre-nerf wait, in seconds. Cardians
    -- drawing on their own hunt target obey the same rule.
    REENGAGE_SWITCH_DELAY = 2.0,

    -- An order from the command window given a little early -- she is
    -- mid-action, or the spell is still on recast -- is held and fires the
    -- moment it can, as long as that moment is within this many seconds of
    -- the press. Later than that the order is refused with the wait, and a
    -- held order the grace runs out on is let go with a note: a spell
    -- pressed twice is one cast, not two.
    ORDER_GRACE = 3.0,
}
