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

    -- Milliseconds a pawn waits before answering a party invite. 0 answers on
    -- the next zone tick; a human takes seconds, and the retail client's
    -- party UI is being tested against that difference.
    INVITE_ACCEPT_DELAY = 0,

    -- Log every gambit action a pawn takes (spell, ability, weapon skill,
    -- ranged attack) with its target. Dev aid for tuning brains.
    GAMBIT_DEBUG = false,

    -- Hunt mode (!pawnhunt): a flagged pawn picks and pulls exp mobs on
    -- its own while the party is idle and healthy. Difficulty band uses
    -- the check scale: 2 = Easy Prey, 3 = Decent Challenge, 4 = Even
    -- Match, 5 = Tough, 6 = Very Tough, 7 = Incredibly Tough.
    HUNT_CHECK_MIN = 3,
    HUNT_CHECK_MAX = 5,

    -- Yalms from the PLAYER a hunt target may be picked; the player is
    -- the party's anchor -- walk away and the pulling stops.
    HUNT_RADIUS = 30,

    -- Yalms from the player beyond which an engaged pawn breaks off and
    -- comes home (runaway-train guard).
    HUNT_LEASH = 50,

    -- The party is "ready" for the next pull when everyone is above
    -- these thresholds (percent; MP applies only to characters with MP)
    -- and the post-fight breather has passed.
    HUNT_READY_HPP    = 75,
    HUNT_READY_MPP    = 50,
    HUNT_DOWNTIME_MS  = 6000,

    -- Formation: the hunter leads, holding a point this many yalms ahead
    -- of the player along their facing; the point is re-aimed only when
    -- the fresh projection drifts more than DEADBAND yalms from the held
    -- one, so the client's coarse position/heading updates don't twitch it.
    FORMATION_LEAD_DISTANCE = 5.0,
    FORMATION_DEADBAND      = 2.5,

    -- Extra lead while the player is moving: the server learns of the
    -- player's motion a packet bundle late, so the point is aimed further
    -- out to cover that lag. The deadband never applies while moving.
    FORMATION_LEAD_MOVING_BONUS = 3.0,
}
