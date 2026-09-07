-- CARDIAN: the census -- the adventurers of the world (RESEARCH.md §11.2).
-- One row per name, written by tools/world/census.py; the map server mints
-- a body from a row on first need and the auction house crowd reads levels.
CREATE TABLE IF NOT EXISTS `cardian_census` (
  `name`      varchar(15)         NOT NULL,
  `race`      tinyint(3) unsigned NOT NULL,                 -- the client's race enum, 1-8: race and sex in one
  `sex`       tinyint(1) unsigned NOT NULL DEFAULT '0',     -- 0 male, 1 female (implied by race; kept for queries)
  `face`      tinyint(3) unsigned NOT NULL DEFAULT '0',     -- 0-15: eight faces by two hairstyles
  `size`      tinyint(3) unsigned NOT NULL DEFAULT '1',     -- 0 small, 1 medium, 2 large
  `nation`    tinyint(3) unsigned NOT NULL DEFAULT '0',     -- 0 San d'Oria, 1 Bastok, 2 Windurst
  `job`       tinyint(3) unsigned NOT NULL DEFAULT '1',     -- main job id
  `level`     tinyint(3) unsigned NOT NULL DEFAULT '1',     -- the computed level, cached
  `anchor`    varchar(16)         NOT NULL DEFAULT 'fixed', -- newbie, peer, rival, veteran, settled, fixed
  `cohort`    int(10) unsigned    NOT NULL DEFAULT '0',     -- the charid a relative anchor follows; 0 = none
  `seed`      int(10) unsigned    NOT NULL DEFAULT '0',     -- her private variance
  `wealth`    tinyint(3) unsigned NOT NULL DEFAULT '1',     -- 0 poor, 1 middling, 2 rich
  `trade`     varchar(16)         NOT NULL DEFAULT '',      -- the settled: craft, gathering kind, hunt, merchant
  `charid`    int(10) unsigned    NOT NULL DEFAULT '0',     -- once minted
  `recruited` tinyint(1) unsigned NOT NULL DEFAULT '0',
  PRIMARY KEY (`name`),
  KEY `idx_cardian_census_charid` (`charid`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_general_ci;
