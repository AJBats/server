-- CARDIAN: the census -- the adventurers of the world (RESEARCH.md §11.2-11.4).
-- One row per name, written by tools/world/census.py; the map server mints
-- a body from a row on first need and the auction house crowd reads levels.
-- A name in the bank (anchor 'bank') is not in the world yet: the reserve
-- the next cohort draws on.
CREATE TABLE IF NOT EXISTS `cardian_census` (
  `name`      varchar(15)         NOT NULL,
  `race`      tinyint(3) unsigned NOT NULL,                 -- the client's race enum, 1-8: race and sex in one
  `sex`       tinyint(1) unsigned NOT NULL DEFAULT '0',     -- 0 male, 1 female (implied by race; kept for queries)
  `face`      tinyint(3) unsigned NOT NULL DEFAULT '0',     -- 0-15: eight faces by two hairstyles
  `size`      tinyint(3) unsigned NOT NULL DEFAULT '1',     -- 0 small, 1 medium, 2 large
  `nation`    tinyint(3) unsigned NOT NULL DEFAULT '0',     -- 0 San d'Oria, 1 Bastok, 2 Windurst
  `job`       tinyint(3) unsigned NOT NULL DEFAULT '1',     -- main job id
  `target`    tinyint(3) unsigned NOT NULL DEFAULT '0',     -- what the ladder says she should be now (D6): her cap while she is in sight, what she levels to at her
                                                          --   cohort's pace out of sight. Her level itself is her character row's (char_stats.mlvl): the census never copies it (user, 2026-09-08)
  `sub`       tinyint(3) unsigned NOT NULL DEFAULT '0',     -- support job id at her target (jobs.yaml's convention), 0 for none: her character rows carry the one at her level
  `sublevel`  tinyint(3) unsigned NOT NULL DEFAULT '0',     --   at the level the game gives a support job (map.SUBJOB_RATIO)
  `anchor`    varchar(16)         NOT NULL DEFAULT 'bank',  -- newbie, peer, rival, veteran, settled, bank
  `cohort`    int(10) unsigned    NOT NULL DEFAULT '0',     -- a peer or rival's cohort: the job she follows, as charid * 100 + job; 0 = none
  `seed`      int(10) unsigned    NOT NULL DEFAULT '0',     -- her private variance
  `wealth`    tinyint(3) unsigned NOT NULL DEFAULT '1',     -- 0 poor, 1 middling, 2 rich
  `trade`     varchar(16)         NOT NULL DEFAULT '',      -- the settled: craft, gathering kind, hunt, merchant
  `charid`    int(10) unsigned    NOT NULL DEFAULT '0',     -- once minted
  `recruited` tinyint(1) unsigned NOT NULL DEFAULT '0',
  `cp_level`  tinyint(3) unsigned NOT NULL DEFAULT '0',     -- the level up to which her conquest points have been granted (census.py grant_points)
  `job2`      tinyint(3) unsigned NOT NULL DEFAULT '0',     -- her career's second job (RESEARCH §11.12), 0 none yet: taken up once and for good
  `cohort2`   int(10) unsigned    NOT NULL DEFAULT '0',     --   the cohort she follows on it, 0 none (its pace and target are then a share of her first's character's)
  `target2`   tinyint(3) unsigned NOT NULL DEFAULT '0',     --   what its ladder says she should be on it now
  `later_jobs` varchar(64)        NOT NULL DEFAULT '',      -- the jobs her support windows had her take up after it, "job:target" joined by commas (census.py take_up)
  PRIMARY KEY (`name`),
  KEY `idx_cardian_census_charid` (`charid`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_general_ci;

-- CARDIAN: the wardrobe -- what each adventurer wears, one row per slot,
-- picked by the census tool from what the price book has opened to the
-- player (RESEARCH.md §11.4). Slots are the client's equip slots, 0-15.
CREATE TABLE IF NOT EXISTS `cardian_wardrobe` (
  `name`   varchar(15)          NOT NULL,
  `slot`   tinyint(3) unsigned  NOT NULL,
  `itemid` smallint(5) unsigned NOT NULL,
  PRIMARY KEY (`name`, `slot`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_general_ci;

-- CARDIAN: the spellbook -- what each adventurer has learned, one row per
-- spell, picked by the census tool by the wardrobe's rule: every spell her
-- job casts at her level whose scroll the price book has opened to the
-- player. A body learns them as she stands.
CREATE TABLE IF NOT EXISTS `cardian_spells` (
  `name`    varchar(15)          NOT NULL,
  `spellid` smallint(5) unsigned NOT NULL,
  PRIMARY KEY (`name`, `spellid`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_general_ci;

-- CARDIAN: her food (RESEARCH.md §19) -- one food for each party role she
-- might hold, picked by the census tool with her wardrobe by her wealth and
-- a roll of her seed, and laid in her bag at every dress; the map tops it up
-- while she is in a player's party and eats it with him. `role` is the
-- party role as the Cardian Link numbers it (1 Tank, 2 Healer, 3 Damage; a
-- Puller and no role eat as Damage). `cookie`: a Healer's short MP food,
-- eaten as she kneels rather than with the player. The census tool and the
-- map both make the table where it is missing.
CREATE TABLE IF NOT EXISTS `cardian_food` (
  `name`   varchar(15)          NOT NULL,
  `role`   tinyint(3) unsigned  NOT NULL,
  `itemid` smallint(5) unsigned NOT NULL,
  `cookie` tinyint(1) unsigned  NOT NULL DEFAULT '0',
  PRIMARY KEY (`name`, `role`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_general_ci;

-- CARDIAN: the live census's pace (RESEARCH.md §11.11, tools/world/pace.py). Out of
-- sight a body of the world levels at the pace of the real character's job her
-- cohort follows: these keep what the census watcher needs across a restart. The
-- watcher makes them where they are missing.
-- Each real character's jobs as sampled every 20 minutes he is online: the seconds
-- online since the last sample, his progress on the job then (level and share),
-- and his samples (shares of a level, joined by commas; `|` where he signed out).
CREATE TABLE IF NOT EXISTS `cardian_pace` (
  `charid`   int(10) unsigned    NOT NULL,
  `job`      tinyint(3) unsigned NOT NULL,
  `online`   double              NOT NULL DEFAULT '0',
  `progress` double              DEFAULT NULL,
  `samples`  text                NOT NULL DEFAULT '',
  PRIMARY KEY (`charid`, `job`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_general_ci;

-- Each body's clock on the census clock: in a session or a break, until when, the
-- share of a level she has earned toward her next ding, the phases drawn for her so
-- far (the next draw's seed), the census time she was last run to, and the job her
-- session goes to (0 none: the one of her career's jobs further below its target).
CREATE TABLE IF NOT EXISTS `cardian_pace_body` (
  `charid`   int(10) unsigned    NOT NULL,
  `session`  tinyint(1) unsigned NOT NULL DEFAULT '0',
  `ends`     double              NOT NULL DEFAULT '0',
  `progress` double              NOT NULL DEFAULT '0',
  `drawn`    int(10) unsigned    NOT NULL DEFAULT '0',
  `ran_to`   double              NOT NULL DEFAULT '0',
  `job`      tinyint(3) unsigned NOT NULL DEFAULT '0',
  PRIMARY KEY (`charid`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_general_ci;

-- The census clock: the seconds the watcher has counted while the map runs (nothing
-- moves while the server is down), and its speed against real time -- 1 in play; a
-- test world may raise it to see days of pace in minutes. One row, id 1.
CREATE TABLE IF NOT EXISTS `cardian_pace_clock` (
  `id`      tinyint(3) unsigned NOT NULL,
  `seconds` double              NOT NULL DEFAULT '0',
  `speed`   double              NOT NULL DEFAULT '1',
  PRIMARY KEY (`id`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_general_ci;
