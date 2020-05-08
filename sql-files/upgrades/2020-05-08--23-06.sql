#1588971960

-- This file is part of Hercules.
-- http://herc.ws - http://github.com/HerculesWS/Hercules
--
-- Copyright (C) 2019-2020 Hercules Dev Team
--
-- Hercules is free software: you can redistribute it and/or modify
-- it under the terms of the GNU General Public License as published by
-- the Free Software Foundation, either version 3 of the License, or
-- (at your option) any later version.
--
-- This program is distributed in the hope that it will be useful,
-- but WITHOUT ANY WARRANTY; without even the implied warranty of
-- MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
-- GNU General Public License for more details.
--
-- You should have received a copy of the GNU General Public License
-- along with this program.  If not, see <http://www.gnu.org/licenses/>.

-- Unify size of value column in *_reg_str_db tables to 255.
ALTER TABLE `acc_reg_str_db` MODIFY `value` VARCHAR(255) NOT NULL DEFAULT '0';
ALTER TABLE `char_reg_str_db` MODIFY `value` VARCHAR(255) NOT NULL DEFAULT '0';
ALTER TABLE `global_acc_reg_str_db` MODIFY `value` VARCHAR(255) NOT NULL DEFAULT '0';

-- Remove affixes from variable names in *_reg_*_db tables.
UPDATE `acc_reg_num_db` SET `key`=REPLACE(`key`, '#', '');
UPDATE `acc_reg_str_db` SET `key`=REPLACE(REPLACE(`key`, '#', ''), '$', '');
UPDATE `char_reg_str_db` SET `key`=REPLACE(`key`, '$', '');
UPDATE `global_acc_reg_num_db` SET `key`=REPLACE(`key`, '#', '');
UPDATE `global_acc_reg_str_db` SET `key`=REPLACE(REPLACE(`key`, '#', ''), '$', '');

-- Add separate tables for global integer and string variables.
CREATE TABLE IF NOT EXISTS `map_reg_num_db` (
  `key` VARCHAR(32) BINARY NOT NULL DEFAULT '',
  `index` INT UNSIGNED NOT NULL DEFAULT '0',
  `value` INT NOT NULL DEFAULT '0',
  PRIMARY KEY (`key`, `index`)
) ENGINE=MyISAM;
CREATE TABLE IF NOT EXISTS `map_reg_str_db` (
  `key` VARCHAR(32) BINARY NOT NULL DEFAULT '',
  `index` INT UNSIGNED NOT NULL DEFAULT '0',
  `value` VARCHAR(255) NOT NULL DEFAULT '0',
  PRIMARY KEY (`key`, `index`)
) ENGINE=MyISAM;

-- Copy data without variable name affixes from mapreg table to new map_reg_*_db tables.
INSERT INTO `map_reg_num_db` (`key`, `index`, `value`) SELECT REPLACE(`varname`, '$', ''), `index`, CAST(`value` AS SIGNED) FROM `mapreg` WHERE NOT RIGHT(`varname`, 1)='$';
INSERT INTO `map_reg_str_db` (`key`, `index`, `value`) SELECT REPLACE(`varname`, '$', ''), `index`, `value` FROM `mapreg` WHERE RIGHT(`varname`, 1)='$';

-- Remove mapreg table.
DROP TABLE IF EXISTS `mapreg`;

-- Add update timestamp
INSERT INTO `sql_updates` (`timestamp`) VALUES (1588971960);
