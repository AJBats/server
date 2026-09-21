-- CARDIAN: the game clock, kept across a restart and shared between processes -- one
-- row, what the real clock and the game clock read at the same moment, and whether
-- the game clock was held then (src/common/cardian_clock_row.h).
-- The game clock is real time less every pause so far; lockouts and NM
-- windows are saved in its seconds. Written by xi_map alone; xi_world follows it.
CREATE TABLE IF NOT EXISTS `cardian_clock` (
  `id`      tinyint(3) unsigned NOT NULL,  -- always 1
  `real_ms` bigint(20) NOT NULL,           -- real Unix time of the write, in milliseconds
  `game_ms` bigint(20) NOT NULL,           -- what the game clock read at that moment
  `held`    tinyint(1) NOT NULL DEFAULT 0, -- the game clock stood still at that moment
  PRIMARY KEY (`id`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_general_ci;

-- A table from before the held column
ALTER TABLE `cardian_clock` ADD COLUMN IF NOT EXISTS `held` tinyint(1) NOT NULL DEFAULT 0;
