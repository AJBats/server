-- Cardian party orders (M3.9): what a player's hunters pull -- the check
-- band, which end of it first, and whether aggressive or linking company
-- near the target is fair -- which of Dia and Bio the tactician
-- prefers (0 Dia, 1 Bio), and whether his cardians' Avoid aggro rows
-- speak (the Shortcuts menu's switch, 1 on), and the Pull page's list (the kinds
-- of mob his puller brings in, in his order, each on or off: 'Forest_Hare:1|Tunnel_Worm:0').
-- The strategy and retreat themselves are not
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
  `avoid_aggro` tinyint(1) unsigned NOT NULL DEFAULT '1',
  `pull_list` varchar(1024) NOT NULL DEFAULT '',
  `updated` timestamp NOT NULL DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP,
  PRIMARY KEY (`charid`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_general_ci;

-- A table made before the Dia or Bio order
ALTER TABLE `cardian_orders` ADD COLUMN IF NOT EXISTS `prefer_bio` tinyint(1) unsigned NOT NULL DEFAULT '0' AFTER `links`;

-- A table made before the Avoid aggro switch
ALTER TABLE `cardian_orders` ADD COLUMN IF NOT EXISTS `avoid_aggro` tinyint(1) unsigned NOT NULL DEFAULT '1' AFTER `prefer_bio`;

-- A table made before the Pull page
ALTER TABLE `cardian_orders` ADD COLUMN IF NOT EXISTS `pull_list` varchar(1024) NOT NULL DEFAULT '' AFTER `avoid_aggro`;
