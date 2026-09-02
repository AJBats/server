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

    -- Prediction: with the Cardian Link streaming the player's position,
    -- the lead aims at where the player will be this many milliseconds
    -- from now (straight-line from the stream's velocity, shortened while
    -- the player turns, capped at PREDICT_MAX yalms). Covers the rest of
    -- the loop -- the pawn tick, its travel, the client's render cadence.
    -- A stopped player collapses the prediction at once.
    FORMATION_PREDICT_MS  = 700,
    FORMATION_PREDICT_MAX = 6.0,

    -- The first follower (the pawn following the player, not another
    -- pawn) uses the same fresh position with this fraction of the lead's
    -- prediction -- enough to stay at the player's side, not out front.
    -- 0 = fresh position only, no prediction.
    FORMATION_FOLLOW_PREDICT_SCALE = 0.4,

    -- The first follower's slot: this many yalms from the player, this many
    -- degrees off straight-behind (a rear quarter, out of the camera line).
    -- Held with the same deadband as the lead's point while the player
    -- stands, so a player turning in place doesn't make the follower orbit.
    FORMATION_FOLLOW_DISTANCE  = 2.5,
    FORMATION_FOLLOW_ANGLE_DEG = 40,

    -- Catch-up: the lead runs at CATCHUP_SPEED while more than
    -- CATCHUP_DISTANCE yalms from its point, PAWN_SPEED otherwise. It only
    -- ever closes a gap to a point the player defines, so it nets out no
    -- faster than the player. map.SPEED_LIMIT must be at least the
    -- catch-up speed or the clamp eats it (see the local overrides).
    FORMATION_CATCHUP_DISTANCE = 3.0,
    FORMATION_CATCHUP_SPEED    = 135,

    -- Log, once a second per lead pawn: how old the Cardian Link's view of
    -- the player is versus the position packet's, the distance between the
    -- two (packet lag in yalms), the prediction applied and its error, and
    -- the lead's distance from its point. Dev aid for the formation work.
    FORMATION_DEBUG = false,
}
