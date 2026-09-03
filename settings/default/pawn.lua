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
    -- Match, 5 = Tough, 6 = Very Tough, 7 = Incredibly Tough. Hunting is
    -- the party's strategy, not a gambit (!pawnhunt until the strategy
    -- channel exists).
    HUNT_CHECK_MIN = 3,
    HUNT_CHECK_MAX = 5,

    -- Yalms from the PLAYER a hunt target may be picked; the player is
    -- the party's anchor -- walk away and the pulling stops.
    HUNT_RADIUS = 30,

    -- How far around a cardian the party's aggro is answered and hate on
    -- her is noticed (yalms). Not a leash: a fight is never broken off by
    -- distance -- retreat is how the player calls the party back.
    HUNT_LEASH = 50,


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

    -- The rear-quarter seats: this many yalms from the player, this many
    -- degrees off straight-behind on each side (out of the camera line).
    -- Every seat is held with the same deadband as the lead's point while
    -- the player stands, so a player turning in place makes nobody orbit.
    FORMATION_FOLLOW_DISTANCE  = 2.5,
    FORMATION_FOLLOW_ANGLE_DEG = 40,

    -- The ring's other seats. Without a Formation row a cardian is seated
    -- by job (the silent default): melee jobs take the flanks first --
    -- FLANK_ANGLE_DEG off straight-ahead, FLANK_DISTANCE out -- then the
    -- rear quarters, then behind; everyone else takes the rear quarters
    -- first, then behind (REAR_DISTANCE, a step further back so it is not
    -- in the quarters' way), then the flanks. Within a kind the side with
    -- fewer cardians wins, ties to the right. A Formation row claims its
    -- seat ahead of all of this.
    FORMATION_FLANK_ANGLE_DEG = 90,
    FORMATION_FLANK_DISTANCE  = 2.5,
    FORMATION_REAR_DISTANCE   = 3.5,

    -- Holding for the player's strike (a weapon drawn on a mob commits the
    -- party; the first hit is the player's), the cardians walk in with
    -- them in this same formation, but no point is placed within melee
    -- reach of the mob plus this many yalms: a lead point aimed into the
    -- mob stops at that ring, on the player's side. Wider than the follow
    -- tolerance (2 y) so the walk to the point never crosses into reach.
    FORMATION_STANDOFF = 2.5,

    -- Catch-up: the lead runs at CATCHUP_SPEED while more than
    -- CATCHUP_DISTANCE yalms from its point, PAWN_SPEED otherwise. It only
    -- ever closes a gap to a point the player defines, so it nets out no
    -- faster than the player -- and it is a hair above run speed, not a
    -- sprint: enough to stay ahead of the predicted spot, never enough to
    -- whip around. A cardian never speeds up at all while a mob holds
    -- hate on her (she would be a kiting exploit otherwise).
    -- map.SPEED_LIMIT must be at least the catch-up speed or the clamp
    -- eats it (see the local overrides).
    FORMATION_CATCHUP_DISTANCE = 3.0,
    FORMATION_CATCHUP_SPEED    = 118,

    -- Aggro avoidance (M3.87). Every detection type a mob has counts as a
    -- circle of that type's range plus AVOID_BUFFER yalms -- sight and sound
    -- always, low-HP while the cardian is under 75%, magic only while it is
    -- casting, ambush at 3 y (the conditions the game itself applies); a cardian keeps its slots, its paths
    -- and itself outside those circles, and is pushed away as a mob roams
    -- toward it. Cardians move on the server with the mobs, so nothing
    -- surprises them: they stand boldly just outside. Avoidance is a gambit
    -- row (on in every cardian's default rows; !pawnavoid checks and
    -- unchecks it). AVOID_SCAN is how far around itself a cardian looks.
    AVOID_BUFFER = 1.5,
    AVOID_SCAN   = 30,

    -- Linking: the idle kin of any mob fighting a cardian are circles for
    -- that cardian alone -- link range plus AVOID_TAIL, the distance the mob
    -- keeps behind her as it follows -- so she leads her fight away from the
    -- kin and no one else has to. A kin that joins is a fight, not a danger,
    -- and drops out on its own. A mob that both aggroes and links is the
    -- larger of its two circles. The same pass runs mid-fight: a target
    -- parked inside another mob's circle is not approached; she waits at
    -- the rim for the tank to bring it.
    AVOID_LINKS = true,
    AVOID_TAIL  = 3.0,

    -- The settle rule (the itch): a cardian whose spot lies inside a circle
    -- takes the best clear spot on offer when she arrives, then stands
    -- there. Every second the itch grows by how much better than her spot
    -- the best spot on offer now is, minus AVOID_ITCH_TOLERANCE yalms (an
    -- improvement under that is never worth walking for, and drains it);
    -- when it reaches AVOID_ITCH_PATIENCE (yalm-seconds) she moves once, in
    -- one go, and the itch resets. Four yalms better with a tolerance of
    -- three fires after twenty seconds; eight yalms better, after four.
    -- Escapes and fights never wait on it.
    AVOID_ITCH_TOLERANCE = 3.0,
    AVOID_ITCH_PATIENCE  = 20,

    -- Company around a pull is the party's call (the Party page: aggressive
    -- company and linking company, saved per player in cardian_orders).
    -- CLEAN_PULLS is only the default for a player with no row yet: true
    -- avoids both. CLEAN_RADIUS is the link distance: a linking family
    -- member within it makes a target unclean when links are avoided.
    HUNT_CLEAN_PULLS  = true,
    HUNT_CLEAN_RADIUS = 10,

    -- Log, once a second per lead pawn: how old the Cardian Link's view of
    -- the player is versus the position packet's, the distance between the
    -- two (packet lag in yalms), the prediction applied and its error, and
    -- the lead's distance from its point. Dev aid for the formation work.
    FORMATION_DEBUG = false,
}
