-- Cardian gambit sets (M3.85): a cardian's rows in the row grammar
-- (gambit_text.h), one "on spec" line per row, and the master switch.
-- set_id is 0 today; TZA-style sets are a set_id away.
-- Runs at the end of every dbtool update; must stay idempotent.

CREATE TABLE IF NOT EXISTS `cardian_gambits` (
  `pawn_charid` int(10) unsigned NOT NULL,
  `set_id` tinyint(3) unsigned NOT NULL DEFAULT '0',
  `master_on` tinyint(1) unsigned NOT NULL DEFAULT '1',
  `set_rows` text NOT NULL,
  `updated` timestamp NOT NULL DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP,
  PRIMARY KEY (`pawn_charid`, `set_id`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_general_ci;
