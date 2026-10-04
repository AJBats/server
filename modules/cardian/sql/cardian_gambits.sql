-- Cardian gambit sets (M3.85): a cardian's rows in the row grammar
-- (gambit_text.h), one "on spec" line per row, and the master switch.
-- set_id 0 is a character's set while a cardian's controller runs her;
-- set_id 100 the player's own, run by his hands while he plays
-- (pawn::kOwnClientSet). TZA-style sets are a set_id away.
-- Runs at the end of every dbtool update; must stay idempotent.

CREATE TABLE IF NOT EXISTS `cardian_gambits` (
  `pawn_charid` int(10) unsigned NOT NULL,
  `set_id` tinyint(3) unsigned NOT NULL DEFAULT '0',
  `master_on` tinyint(1) unsigned NOT NULL DEFAULT '1',
  `set_rows` text NOT NULL,
  `updated` timestamp NOT NULL DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP,
  PRIMARY KEY (`pawn_charid`, `set_id`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_general_ci;
