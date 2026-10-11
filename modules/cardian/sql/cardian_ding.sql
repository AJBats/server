-- CARDIAN: the live census -- a body of the world levels out of sight (RESEARCH.md §11.11,
-- src/map/pawn/redress.h). One row per world body, the channel between the census watcher
-- (tools/world/census.py watch), which decides her dings at her cohort's pace, and xi_map,
-- which alone knows whether it holds her body and whether a real player can see her:
--   planned  the census: a ding to `level` on `job` (the job she plays after it), or with
--            kind 'dress' her gear at the level she has; her skill values for the level in
--            `skills` ("skillid:value" pairs in the game's tenths, joined by commas), her
--            support job in `sub` with the job's own level in `sublevel`; `gear` when her
--            wardrobe, spellbook and food for it are written (cardian_wardrobe,
--            cardian_spells, cardian_food), which go on only in a city: `dressed` says
--            whether they did, and a body whose planned gear did not go on (she had left
--            the city) is owed a dress by the census at her next stay in one
--   claimed  the map: it holds no body of hers, so the census writes her rows; she stands
--            for nobody until the row is done (a claim not done in two minutes is taken
--            back, and the row is planned again)
--   applied  the map: put on the body it holds, out of sight -- or set aside, `note` saying
--            why. The census pays the level's conquest points (none for a ding set aside)
--            and marks it done
--   done     over: written by the census, applied and paid, or set aside (`note` says why)
-- xi_map makes the table as it boots (pawn::redress::ensureTable), and so does the watcher,
-- since dbtool's update skips module SQL on a database whose upstream SQL has not moved.

CREATE TABLE IF NOT EXISTS `cardian_ding` (
  `charid`     int(10) unsigned NOT NULL,
  `kind`       enum('ding','dress') NOT NULL DEFAULT 'ding',
  `job`        tinyint(3) unsigned NOT NULL DEFAULT '0',
  `level`      tinyint(3) unsigned NOT NULL DEFAULT '0',
  `gear`       tinyint(1) unsigned NOT NULL DEFAULT '0',
  `dressed`    tinyint(1) unsigned NOT NULL DEFAULT '0',
  `skills`     varchar(1024) NOT NULL DEFAULT '',
  `sub`        tinyint(3) unsigned NOT NULL DEFAULT '0',
  `sublevel`   tinyint(3) unsigned NOT NULL DEFAULT '0',
  `state`      enum('planned','claimed','applied','done') NOT NULL DEFAULT 'planned',
  `note`       varchar(128) NOT NULL DEFAULT '',
  `planned_at` datetime DEFAULT NULL,
  `claimed_at` datetime DEFAULT NULL,
  `done_at`    datetime DEFAULT NULL,
  PRIMARY KEY (`charid`),
  KEY `state` (`state`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_general_ci;
