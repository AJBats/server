-- CARDIAN: names held for players creating a character (src/common/cardian_lobby.h).
-- The lobby holds a name for the account that typed it until the character is saved, or
-- for an hour; the census never mints a held name (tools/world/census.py), and another
-- account is refused it. The lobby and the census each make the table on first use.
CREATE TABLE IF NOT EXISTS `cardian_name_holds` (
  `name`    varchar(15) NOT NULL,          -- compared as the lobby compares names, case aside
  `accid`   int(10) unsigned NOT NULL,     -- the account creating the character
  `held_at` datetime NOT NULL,             -- when the lobby accepted the name
  PRIMARY KEY (`name`),
  KEY `accid` (`accid`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_general_ci;
