-- Cardian party orders (M3.9): what a player's hunters pull -- the check
-- band, which end of it first, and whether aggressive or linking company
-- near the target is fair -- and which of Dia and Bio the tactician
-- prefers (0 Dia, 1 Bio). The strategy and retreat themselves are not
-- saved: a login starts at Off. Runs at the end of a dbtool update that
-- imports; xi_map also makes the table, or adds a missing column, as it
-- boots (pawn::ensureOrdersTable), since an update finding the upstream
-- SQL unchanged imports nothing. Must stay idempotent.

CREATE TABLE IF NOT EXISTS `cardian_orders` (
  `charid` int(10) unsigned NOT NULL,
  `hunt_min` tinyint(3) unsigned NOT NULL DEFAULT '3',
  `hunt_max` tinyint(3) unsigned NOT NULL DEFAULT '5',
  `pull_first` tinyint(3) unsigned NOT NULL DEFAULT '1',
  `aggressive` tinyint(1) unsigned NOT NULL DEFAULT '0',
  `links` tinyint(1) unsigned NOT NULL DEFAULT '0',
  `prefer_bio` tinyint(1) unsigned NOT NULL DEFAULT '0',
  `updated` timestamp NOT NULL DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP,
  PRIMARY KEY (`charid`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_general_ci;

-- A table made before the Dia or Bio order
ALTER TABLE `cardian_orders` ADD COLUMN IF NOT EXISTS `prefer_bio` tinyint(1) unsigned NOT NULL DEFAULT '0' AFTER `links`;
