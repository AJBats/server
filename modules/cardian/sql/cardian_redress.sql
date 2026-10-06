-- CARDIAN: a wild cardian re-dressed at the auction house (src/map/pawn/redress.h).
-- One row per world body, the request channel between xi_map and the census
-- watcher (tools/world/census.py watch), which share nothing else:
--   asked  the map: she stands by an auction counter with the player, risen past
--          the level she was last dressed for; `level` is the level she has now
--   ready  the census: her wardrobe and spellbook for `level` written (cardian_wardrobe,
--          cardian_spells), her skill values for it in `skills` ("skillid:value"
--          pairs in the game's tenths, joined by commas), the pieces it had
--          issued her before this plan in `issued` (item ids, joined by commas),
--          and the support job of that level in `sub` and `sublevel` (0 for none)
--   done   the map: the plan put on her while she stands -- the census never writes
--          the character tables of a body the map holds; `level` is the level she
--          was dressed for
-- A row asked or ready is never asked again until it is done. xi_map also makes the
-- table as it boots (pawn::redress::ensureTable), since dbtool's update skips module
-- SQL on a database whose upstream SQL has not moved; so does the watcher. Must stay
-- idempotent: a table made before the support job's columns gains them below.

CREATE TABLE IF NOT EXISTS `cardian_redress` (
  `charid`   int(10) unsigned NOT NULL,
  `level`    tinyint(3) unsigned NOT NULL,
  `state`    enum('asked','ready','done') NOT NULL DEFAULT 'asked',
  `skills`   varchar(1024) NOT NULL DEFAULT '',
  `issued`   varchar(512) NOT NULL DEFAULT '',
  `sub`      tinyint(3) unsigned NOT NULL DEFAULT '0',
  `sublevel` tinyint(3) unsigned NOT NULL DEFAULT '0',
  `asked_at` datetime DEFAULT NULL,
  `ready_at` datetime DEFAULT NULL,
  `done_at`  datetime DEFAULT NULL,
  PRIMARY KEY (`charid`),
  KEY `state` (`state`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_general_ci;

ALTER TABLE `cardian_redress`
  ADD COLUMN IF NOT EXISTS `sub`      tinyint(3) unsigned NOT NULL DEFAULT '0' AFTER `issued`,
  ADD COLUMN IF NOT EXISTS `sublevel` tinyint(3) unsigned NOT NULL DEFAULT '0' AFTER `sub`;
