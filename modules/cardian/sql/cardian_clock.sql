-- CARDIAN: the game clock, kept across a restart -- one row, what the real
-- clock and the game clock read at the same moment (src/map/pause/calendar_store.h).
-- The game clock is real time less every pause so far; lockouts and NM
-- windows are saved in its seconds. Written by xi_map alone.
CREATE TABLE IF NOT EXISTS `cardian_clock` (
  `id`      tinyint(3) unsigned NOT NULL,  -- always 1
  `real_ms` bigint(20) NOT NULL,           -- real Unix time of the write, in milliseconds
  `game_ms` bigint(20) NOT NULL,           -- what the game clock read at that moment
  PRIMARY KEY (`id`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_general_ci;
