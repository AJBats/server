-- CARDIAN: the party memory (ROADMAP H, the seat waterfall). One row per
-- player-and-cardian pair: the two of them were in a party, most recently
-- on this date. A memory rather than a tier -- nothing about her changes,
-- she is still wild and the census still owns her -- which is what makes
-- it safe to have the concept without a state that can drift out of sync
-- with the world. It does two jobs: it holds her seat when a zone fills
-- (she sorts above the crowd) and it fills the management page's list of
-- cardians you have played with.
CREATE TABLE IF NOT EXISTS `cardian_party_memory` (
  `player_charid` int(10) unsigned NOT NULL,
  `pawn_charid`   int(10) unsigned NOT NULL,
  `last_partied`  datetime         NOT NULL DEFAULT current_timestamp(),
  PRIMARY KEY (`player_charid`, `pawn_charid`),
  KEY `idx_cardian_party_memory_pawn` (`pawn_charid`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_general_ci;
