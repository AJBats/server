# The zone slot tables (ROADMAP D3, RESEARCH.md §11.5)

One YAML per zone, named as the map server names the zone
(`West_Ronfaure.yaml`, `East_Ronfaure.yaml`). A slot says what happens
there, never who: the census fills it. The map reads a zone's file on the
zone's first tick and gives every occupant presence at once (a session row,
so `/sea` lists her at her job and level); her body loads when a player is
in the zone or one zone line away, and fades when nobody is.

```yaml
# West_Ronfaure: the slot table -- what happens where, never who.
slots:
  - activity: farm        # farm (the errand: hunts her band round the point), stand (idles there),
    band: [2, 6]          #   or camp (a party of `party`, a healer dealt first when the band has one)
    count: 3              # seats (camps: parties; seats = count x party)
    at: [-387.0, -52.0, 230.0]
    spread: 30            # yalms round the point the seats scatter over
    roam: 200             # optional, the home pull: the farther she drifts from the point,
                          #   the more the errand favours prey back toward it -- at this many
                          #   yalms out the pull weighs as much as the walk itself. Drift is
                          #   allowed (user); 0 or absent means she roams anywhere
    party: 2              # camps only: members per party
```

Editing the file is enough: a zone re-reads its table within ten seconds of
a change, sends its people back to the pool and fills again. A file that
does not parse, or a change that is only comments, leaves the zone as it is.

## Town seats (ROADMAP D4)

A field seat is a job, held for good. A town seat is a turnstile: a `stand`
slot with a `dwell` is held that long, then she walks to one of the zone's
exits and fades, and the seat refills with another face after a gap
(`pawn.WORLD_TOWN_GAP_MIN/MAX`). The zone names its exits once, at the top:

```yaml
exits:
  - name: west_gate
    at: [-110.0, -4.0, -57.0]
slots:
  - activity: stand
    band: [1, 75]
    count: 2
    at: [8.9, 1.7, -34.0]
    spread: 1.5
    face: [8.9, 1.7, -31.0]   # she faces this point at her seat
    dwell: [20, 90]           # seconds, min and max; a dwell makes the seat a turnstile
    enter: any                # the exit she walks in from: a name, nearest (default), any
    exit: west_gate           # the exit she leaves by, the same words (any avoids the one she came in by)
    hours: [12, 2]            # the player's local clock, [from, to); to before from wraps midnight
    vhours: [3, 18]           # Vana'diel's clock (the guilds keep it)
    holiday: iceday           # a Vana'diel weekday the seat stands empty
    prefer: sellers           # the names in the player's own auction history first
    pose: kneel               # she kneels at her seat
    party: 3                  # three hold the seat and come and go together
    cliques: [1, 4]           # the seats laid out as little groups of 1-4 facing each other
    via: [[-182.9, -1.0, 29.4]]  # points walked in order on the way in, in reverse on the way out
```

A town walk keeps to a lane of her own -- the mesh's route slid a step
to one side, snapped back where that lands in a wall -- so a crowd sent
down one street does not walk it in single file.

`via` is for a doorway the mesh's shortest line would miss: the Tanners'
Guild has an open outer doorway on its north side and an inner door that
opens for her, but the mesh leaks through the building's east wall, so a
seat inside names the doorway as its via and she goes round by it. She
comes in from the exit nearest her first via point and leaves the same
way back.

`cliques` is for a crowd: group sizes are drawn in the range until the
seats are covered, group centres are spread over the slot by
best-candidate sampling (each new centre the farthest of twenty random
tries from the ones placed), and a group is a conversation circle -- its
members a yalm or so out from the middle, evenly spaced with a little
jitter, each facing the middle; a body alone faces a heading of her own.
The layout is drawn from the zone and the slot, so it is the same every
visit, and the seats are dealt in order as bodies come and go.

Placed while nobody is in the zone, she is at her seat already and the
clock runs unseen; placed while someone is, she appears at her exit point
and walks (`world: ... fades in at ... and walks to her seat`), and on
arrival `takes her seat for N s`. A walk that gets no nearer for
`pawn.WORLD_TOWN_STALL` seconds is given up where she stands, with a
warning naming both ends: that is the route to fix, not her. A turnstile
mixes its turn into the fill's hash and passes over the last six faces it
showed, so the next one differs. Town keys are for `stand` only; a
turnstile needs the zone's exits. `!pawnworld slots` shows each seat's
dwell, hours, turn and whether it is closed now.

Author by walking: stand where the slot is, `!pos`, and write the line (the
user's way, 2026-09-07), or `!pawnworld slot farm 2-6 3 30` to append and
fill it from in game. `!pawnworld slots` lists the table and who holds each
seat (`~` marks a faded body), `!pawnworld fill` forces the refill. A slot
is appended, never reordered: placed bodies keep their seats across a
re-read.

Filling is a query: in the world (not in the bank), level in band, not
recruited, not placed in another zone; cohort rows first, then by a hash of
zone, slot and seed, so the same faces come back on the next visit. Each
occupant stands at a spot in the spread drawn from her name, so she is in
the same place each time.

## The brains: `brains.yaml`

What a world body does is data too. `brains.yaml` holds gambit rows you can
read -- `party: hp < 60 -> cast best cure` -- compiled on load to the numeric
form the addon saves for your own cardians (the file's header has the
grammar): a `common` block every body runs (avoid aggro, rest with the
leader, rest under 60 % HP), a block per `role` and a block per `job`. A body's rows are common, then her job's,
then her role's, top to bottom; the first row whose conditions hold acts,
and a row she cannot use (a spell she doesn't know, an ability on recast)
is passed over. Roles: a Warrior is the **tank** when she is the highest
Warrior of her party and leads the camp; other Warriors and the fighters
are **melee**; White, Black and Red Mage are **mages**. The file is re-read
when it changes; a body picks it up on `!pawnreloadbrain <name>` or her
next stand. The header of the file explains the grammar's numbers.

## Candidate camps from the mob spawn data (2026-09-07)

Centroids of the placed spawns, for reference while authoring. Levels are
the mobs', not the band -- a band a step or two under the mobs' top is
easy prey, at it a fair fight.

West Ronfaure

| mob | levels | camps (x, y, z) x spawns |
|---|---|---|
| Forest Hare | 2-6 | (-387, -52, 230) x6; (-272, -41, 99) x6; (-442, -36, 114) x6; (-341, -31, -3) x6; (-489, -30, -25) x6 |
| Wild Sheep | 5-8 | (-136, -6, -458) x6; (-277, -19, -260) x5 |
| Ding Bats | 1-5 | (-351, -52, 278) x4 |
| Wild Rabbit | 1 | (-303, -52, 293) x12; (-327, -54, 394) x5 |
| Orcish Grappler / Mesmerizer | 3-8 | (-553, -60, 494) x4; (-545, -60, 480) x4 |
| River Crab | 5-6 | (-401, -10, -430) x6 |
| Tunnel Worm | 1 | (-286, -60, 442) x5; (-296, -50, 242) x4 |

East Ronfaure

| mob | levels | camps (x, y, z) x spawns |
|---|---|---|
| Forest Hare | 2-6 | (345, -42, 33) x7; (271, -60, 409) x5; (270, -40, 27) x4; (424, -19, -216) x4; (184, -16, -340) x4 |
| Ding Bats | 1-5 | (132, -56, 139) x5 |
| Wild Sheep | 6-8 | (484, -36, -18) x4 |
| Wild Rabbit | 1 | (71, -56, 154) x7; (183, -60, 407) x6; (192, -53, 181) x5; (223, -57, 271) x5 |
| Pugil | 1-5 | (350, -38, 23) x5; (234, -58, 397) x4 |
| Goblin Fisher | 3-6 | (375, -39, 21) x4 |
