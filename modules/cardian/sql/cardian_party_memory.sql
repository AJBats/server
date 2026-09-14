-- CARDIAN: the party memory (ROADMAP H, the seat waterfall). One row per
-- player-and-cardian pair. The two of them were in a party, most recently
-- on last_partied (NULL when the pair has only an ask between them);
-- affinity counts the shared events since -- a mission or quest completed
-- together, exp she earned beside you -- and contract is what she is in
-- this player's party for right now. A memory rather than a tier
-- -- nothing about her changes, she is still wild and the census still
-- owns her -- which is what makes it safe to have the concept without a
-- state that can drift out of sync with the world. It holds her seat when
-- a zone fills (she sorts above the crowd), fills the management page's
-- list of cardians you have played with, and warms her answer in the
-- party finder. Per character, as the game keeps fame and rank: the
-- matrix stays sparse and co-op adds rows, not columns.
CREATE TABLE IF NOT EXISTS `cardian_party_memory` (
  `player_charid` int(10) unsigned    NOT NULL,
  `pawn_charid`   int(10) unsigned    NOT NULL,
  `last_partied`  datetime                     DEFAULT NULL,
  `affinity`      smallint(5) unsigned NOT NULL DEFAULT 0,
  `missions`      smallint(5) unsigned NOT NULL DEFAULT 0,  -- missions completed together under a mission recruitment: the pearl's lock
  `contract`      varchar(8)           NOT NULL DEFAULT '', -- what she is in this player's party for right now: exp, quest, mission; '' out of it
  PRIMARY KEY (`player_charid`, `pawn_charid`),
  KEY `idx_cardian_party_memory_pawn` (`pawn_charid`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_general_ci;

-- An existing table takes the finder's columns and the nullable date
ALTER TABLE `cardian_party_memory`
  MODIFY `last_partied` datetime DEFAULT NULL,
  ADD COLUMN IF NOT EXISTS `affinity`      smallint(5) unsigned NOT NULL DEFAULT 0 AFTER `last_partied`,
  ADD COLUMN IF NOT EXISTS `missions`      smallint(5) unsigned NOT NULL DEFAULT 0 AFTER `affinity`,
  ADD COLUMN IF NOT EXISTS `contract`      varchar(8) NOT NULL DEFAULT '' AFTER `missions`,
  DROP COLUMN IF EXISTS `refused_until`;
