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
    -- this map server (RESEARCH.md §7). Binary messages
    -- (src/map/pawn/cardian_link_protocol.h); the addon connects at
    -- load and both sides keep the link alive with pings. The link is
    -- load-bearing for the addon: when it cannot connect the addon says so
    -- in its UI rather than degrading.
    LINK_ENABLED = true,
    LINK_PORT    = 54250,

    -- The wait before engaging a different mob than the one fought last, in
    -- seconds. A cardian counts it from leaving the fight (pawn::reengageWait;
    -- the mob she fought last waits her weapon's full delay). The player counts
    -- it from his last swing and waits no longer than upstream's retail
    -- lockout, which alone holds him off the mob of that swing (pawn/reengage.h).
    REENGAGE_SWITCH_DELAY = 2.0,

    -- The combat pause (ROADMAP B): the addon's pause button holds the whole
    -- simulation -- combat and movement, nothing else -- until the player who
    -- took it resumes. Off, the button is refused with a note.
    PAUSE_ENABLED = true,

    -- The game clock (Vana'diel time, and every lockout and NM window scripts
    -- keep) while the server is off. true: its time goes by, as upstream's
    -- does. false: the game carries on from the second it stopped.
    CLOCK_RUNS_OFFLINE = true,

    -- A player's major progression is his account's (ROADMAP N;
    -- src/map/pawn/account_wide.cpp): the support job, the level cap, the
    -- gate crystals and Limit Breaker, and the outposts' warps any character
    -- of an account earns are copied to every character of it. A copy is for
    -- good, so false stops further copies and takes back none. The maps are
    -- shared without being copied, each player's choice in the addon's
    -- Settings (Shared maps), and are not this setting's.
    ACCOUNT_WIDE_PROGRESSION = true,

    -- The exp formula (RESEARCH §15; modules/cardian/lua/exp_spread.lua).
    -- true: Cardian's rule. While the party's level spread -- highest less
    -- lowest among the members counted for a kill -- is within
    -- EXP_AVERAGE_SPREAD, every member is paid as if the whole party were
    -- its average level, rounded down; from there to EXP_CLASSIC_SPREAD
    -- each reward slides toward the server's own figure, which it reaches
    -- at that spread. false: the server's own formula at every spread, as
    -- LandSandBoat pays it with the era module or without.
    EXP_PARTY_AVERAGE  = true,
    EXP_AVERAGE_SPREAD = 3,
    EXP_CLASSIC_SPREAD = 10,
}
