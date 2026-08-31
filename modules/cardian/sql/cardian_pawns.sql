-- Cardian pawn registry: generated characters owned by a player's account.
-- Runs at the end of every dbtool update; must stay idempotent.

CREATE TABLE IF NOT EXISTS `cardian_pawns` (
  `pawn_charid` int(10) unsigned NOT NULL,
  `owner_accid` int(10) unsigned NOT NULL,
  `kitted` tinyint(1) unsigned NOT NULL DEFAULT '0',
  `created` datetime NOT NULL DEFAULT CURRENT_TIMESTAMP,
  PRIMARY KEY (`pawn_charid`),
  KEY `idx_cardian_pawns_owner` (`owner_accid`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_general_ci;
