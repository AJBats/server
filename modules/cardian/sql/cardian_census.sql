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
  `target`    tinyint(3) unsigned NOT NULL DEFAULT '0',     -- what the ladder says she should be now (D6): her cap while the player is online, her level after the offline
                                                          --   catch-up. Her level itself is her character row's (char_stats.mlvl): the census never copies it (user, 2026-09-08)
  `sub`       tinyint(3) unsigned NOT NULL DEFAULT '0',     -- support job id, 0 for none
  `sublevel`  tinyint(3) unsigned NOT NULL DEFAULT '0',
  `anchor`    varchar(16)         NOT NULL DEFAULT 'bank',  -- newbie, peer, rival, veteran, settled, bank
  `cohort`    int(10) unsigned    NOT NULL DEFAULT '0',     -- the charid a relative anchor follows; 0 = none
  `seed`      int(10) unsigned    NOT NULL DEFAULT '0',     -- her private variance
  `wealth`    tinyint(3) unsigned NOT NULL DEFAULT '1',     -- 0 poor, 1 middling, 2 rich
  `trade`     varchar(16)         NOT NULL DEFAULT '',      -- the settled: craft, gathering kind, hunt, merchant
  `charid`    int(10) unsigned    NOT NULL DEFAULT '0',     -- once minted
  `recruited` tinyint(1) unsigned NOT NULL DEFAULT '0',
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
