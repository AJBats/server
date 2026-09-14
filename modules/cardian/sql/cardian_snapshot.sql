-- CARDIAN: the snapshot -- what one of the world's adventurers looked like
-- the last time she stood: her vitals, her seven stats with their gear
-- bonus, her attack and defense, written as her body fades (the ladder's
-- call) so the Party Finder can show a faded responder the page a
-- standing one gets. A body that has never stood has no row.
CREATE TABLE IF NOT EXISTS `cardian_snapshot` (
  `charid`   int(10) unsigned NOT NULL,
  `level`    tinyint(3) unsigned NOT NULL DEFAULT 0,  -- her level as taken: a ding since makes the numbers another woman's
  `hp`       int(10) unsigned NOT NULL DEFAULT 0,
  `maxhp`    int(10) unsigned NOT NULL DEFAULT 0,
  `mp`       int(10) unsigned NOT NULL DEFAULT 0,
  `maxmp`    int(10) unsigned NOT NULL DEFAULT 0,
  `str_t`    smallint(6)      NOT NULL DEFAULT 0,  -- totals
  `dex_t`    smallint(6)      NOT NULL DEFAULT 0,
  `vit_t`    smallint(6)      NOT NULL DEFAULT 0,
  `agi_t`    smallint(6)      NOT NULL DEFAULT 0,
  `int_t`    smallint(6)      NOT NULL DEFAULT 0,
  `mnd_t`    smallint(6)      NOT NULL DEFAULT 0,
  `chr_t`    smallint(6)      NOT NULL DEFAULT 0,
  `str_b`    smallint(6)      NOT NULL DEFAULT 0,  -- the gear's part of each
  `dex_b`    smallint(6)      NOT NULL DEFAULT 0,
  `vit_b`    smallint(6)      NOT NULL DEFAULT 0,
  `agi_b`    smallint(6)      NOT NULL DEFAULT 0,
  `int_b`    smallint(6)      NOT NULL DEFAULT 0,
  `mnd_b`    smallint(6)      NOT NULL DEFAULT 0,
  `chr_b`    smallint(6)      NOT NULL DEFAULT 0,
  `att`      smallint(5) unsigned NOT NULL DEFAULT 0,
  `def`      smallint(5) unsigned NOT NULL DEFAULT 0,
  `taken_at` datetime         NOT NULL DEFAULT current_timestamp(),
  PRIMARY KEY (`charid`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_general_ci;

ALTER TABLE `cardian_snapshot`
  ADD COLUMN IF NOT EXISTS `level` tinyint(3) unsigned NOT NULL DEFAULT 0 AFTER `charid`;
