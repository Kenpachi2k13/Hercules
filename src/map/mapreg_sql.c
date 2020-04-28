/**
 * This file is part of Hercules.
 * http://herc.ws - http://github.com/HerculesWS/Hercules
 *
 * Copyright (C) 2012-2020 Hercules Dev Team
 * Copyright (C) Athena Dev Teams
 *
 * Hercules is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */
#define HERCULES_CORE

#include "mapreg.h"

#include "map/map.h" // map-"mysql_handle
#include "map/script.h"
#include "common/cbasetypes.h"
#include "common/conf.h"
#include "common/db.h"
#include "common/ers.h"
#include "common/memmgr.h"
#include "common/nullpo.h"
#include "common/showmsg.h"
#include "common/sql.h"
#include "common/strlib.h"
#include "common/timer.h"

#include <stdlib.h>
#include <string.h>

static struct mapreg_interface mapreg_s;
struct mapreg_interface *mapreg;

#define MAPREG_AUTOSAVE_INTERVAL (300*1000)

/**
 * Looks up the value of an integer variable using its uid.
 *
 * @param uid variable's unique identifier.
 * @return variable's integer value
 */
static int mapreg_readreg(int64 uid)
{
	struct mapreg_save *m = i64db_get(mapreg->regs.vars, uid);
	return m?m->u.i:0;
}

/**
 * Looks up the value of a string variable using its uid.
 *
 * @param uid variable's unique identifier
 * @return variable's string value
 */
static char *mapreg_readregstr(int64 uid)
{
	struct mapreg_save *m = i64db_get(mapreg->regs.vars, uid);
	return m?m->u.str:NULL;
}

/**
 * Modifies the value of an integer variable.
 *
 * @param uid variable's unique identifier
 * @param val new value
 * @retval true value was successfully set
 */
static bool mapreg_setreg(int64 uid, int val)
{
	struct mapreg_save *m;
	int num = script_getvarid(uid);
	unsigned int i = script_getvaridx(uid);
	const char* name = script->get_str(num);

	nullpo_retr(true, name);
	if( val != 0 ) {
		if( (m = i64db_get(mapreg->regs.vars, uid)) ) {
			m->u.i = val;
			if(name[1] != '@') {
				m->save = true;
				mapreg->dirty = true;
			}
		} else {
			if( i )
				script->array_update(&mapreg->regs, uid, false);

			m = ers_alloc(mapreg->ers, struct mapreg_save);

			m->u.i = val;
			m->uid = uid;
			m->save = false;
			m->is_string = false;

			if (name[1] != '@' && !mapreg->skip_insert) {// write new variable to database
				struct SqlStmt *stmt = SQL->StmtMalloc(map->mysql_handle);

				if (stmt == NULL) {
					SqlStmt_ShowDebug(stmt);
				} else {
					const char *query = "INSERT INTO `%s` (`key`, `index`, `value`) VALUES (?, ?, ?)";
					char name_plain[SCRIPT_VARNAME_LENGTH + 1];
					safestrncpy(name_plain, name + 1, strnlen(name, SCRIPT_VARNAME_LENGTH + 1));
					size_t len = strnlen(name_plain, sizeof(name_plain));

					if (SQL_ERROR == SQL->StmtPrepare(stmt, query, mapreg->num_db)
					    || SQL_ERROR == SQL->StmtBindParam(stmt, 0, SQLDT_STRING, &name_plain, len)
					    || SQL_ERROR == SQL->StmtBindParam(stmt, 1, SQLDT_UINT32, &i, sizeof(i))
					    || SQL_ERROR == SQL->StmtBindParam(stmt, 2, SQLDT_INT32, &val, sizeof(val))
					    || SQL_ERROR == SQL->StmtExecute(stmt)) {
						SqlStmt_ShowDebug(stmt);
						SQL->StmtFree(stmt);
					}

					SQL->StmtFree(stmt);
				}
			}
			i64db_put(mapreg->regs.vars, uid, m);
		}
	} else { // val == 0
		if( i )
			script->array_update(&mapreg->regs, uid, true);
		if( (m = i64db_get(mapreg->regs.vars, uid)) ) {
			ers_free(mapreg->ers, m);
		}
		i64db_remove(mapreg->regs.vars, uid);

		if( name[1] != '@' ) {// Remove from database because it is unused.
			struct SqlStmt *stmt = SQL->StmtMalloc(map->mysql_handle);

			if (stmt == NULL) {
				SqlStmt_ShowDebug(stmt);
			} else {
				const char *query = "DELETE FROM `%s` WHERE `key`=? AND `index`=?";
				char name_plain[SCRIPT_VARNAME_LENGTH + 1];
				safestrncpy(name_plain, name + 1, strnlen(name, SCRIPT_VARNAME_LENGTH + 1));
				size_t len = strnlen(name_plain, sizeof(name_plain));

				if (SQL_ERROR == SQL->StmtPrepare(stmt, query, mapreg->num_db)
				    || SQL_ERROR == SQL->StmtBindParam(stmt, 0, SQLDT_STRING, &name_plain, len)
				    || SQL_ERROR == SQL->StmtBindParam(stmt, 1, SQLDT_UINT32, &i, sizeof(i))
				    || SQL_ERROR == SQL->StmtExecute(stmt)) {
					SqlStmt_ShowDebug(stmt);
					SQL->StmtFree(stmt);
				}

				SQL->StmtFree(stmt);
			}
		}
	}

	return true;
}

/**
 * Modifies the value of a string variable.
 *
 * @param uid variable's unique identifier
 * @param str new value
 * @retval true value was successfully set
 */
static bool mapreg_setregstr(int64 uid, const char *str)
{
	struct mapreg_save *m;
	int num = script_getvarid(uid);
	unsigned int i   = script_getvaridx(uid);
	const char* name = script->get_str(num);

	nullpo_retr(true, name);

	if( str == NULL || *str == 0 ) {
		if( i )
			script->array_update(&mapreg->regs, uid, true);
		if(name[1] != '@') {
			struct SqlStmt *stmt = SQL->StmtMalloc(map->mysql_handle);

			if (stmt == NULL) {
				SqlStmt_ShowDebug(stmt);
			} else {
				const char *query = "DELETE FROM `%s` WHERE `key`=? AND `index`=?";
				char name_plain[SCRIPT_VARNAME_LENGTH + 1];
				safestrncpy(name_plain, name + 1, strnlen(name, SCRIPT_VARNAME_LENGTH + 1));
				size_t len = strnlen(name_plain, sizeof(name_plain));

				if (SQL_ERROR == SQL->StmtPrepare(stmt, query, mapreg->str_db)
				    || SQL_ERROR == SQL->StmtBindParam(stmt, 0, SQLDT_STRING, &name_plain, len)
				    || SQL_ERROR == SQL->StmtBindParam(stmt, 1, SQLDT_UINT32, &i, sizeof(i))
				    || SQL_ERROR == SQL->StmtExecute(stmt)) {
					SqlStmt_ShowDebug(stmt);
					SQL->StmtFree(stmt);
				}

				SQL->StmtFree(stmt);
			}
		}
		if( (m = i64db_get(mapreg->regs.vars, uid)) ) {
			if( m->u.str != NULL )
				aFree(m->u.str);
			ers_free(mapreg->ers, m);
		}
		i64db_remove(mapreg->regs.vars, uid);
	} else {
		if( (m = i64db_get(mapreg->regs.vars, uid)) ) {
			if( m->u.str != NULL )
				aFree(m->u.str);
			m->u.str = aStrdup(str);
			if(name[1] != '@') {
				mapreg->dirty = true;
				m->save = true;
			}
		} else {
			if( i )
				script->array_update(&mapreg->regs, uid, false);

			m = ers_alloc(mapreg->ers, struct mapreg_save);

			m->uid = uid;
			m->u.str = aStrdup(str);
			m->save = false;
			m->is_string = true;

			if(name[1] != '@' && !mapreg->skip_insert) { //put returned null, so we must insert.
				struct SqlStmt *stmt = SQL->StmtMalloc(map->mysql_handle);

				if (stmt == NULL) {
					SqlStmt_ShowDebug(stmt);
				} else {
					const char *query = "INSERT INTO `%s` (`key`, `index`, `value`) VALUES (?, ?, ?)";
					char name_plain[SCRIPT_VARNAME_LENGTH + 1];
					char value[256];
					safestrncpy(name_plain, name + 1, strnlen(name, SCRIPT_VARNAME_LENGTH + 1) - 1);
					safestrncpy(value, str, strnlen(str, 255) + 1);
					size_t len_n = strnlen(name_plain, sizeof(name_plain));
					size_t len_v = strnlen(value, sizeof(value));

					if (SQL_ERROR == SQL->StmtPrepare(stmt, query, mapreg->str_db)
					    || SQL_ERROR == SQL->StmtBindParam(stmt, 0, SQLDT_STRING, &name_plain, len_n)
					    || SQL_ERROR == SQL->StmtBindParam(stmt, 1, SQLDT_UINT32, &i, sizeof(i))
					    || SQL_ERROR == SQL->StmtBindParam(stmt, 2, SQLDT_STRING, &value, len_v)
					    || SQL_ERROR == SQL->StmtExecute(stmt)) {
						SqlStmt_ShowDebug(stmt);
						SQL->StmtFree(stmt);
					}

					SQL->StmtFree(stmt);
				}
			}
			i64db_put(mapreg->regs.vars, uid, m);
		}
	}

	return true;
}

/**
 * Loads permanent interger variables from the database.
 *
 **/
static void mapreg_load_num_db(void)
{
	struct SqlStmt *stmt = SQL->StmtMalloc(map->mysql_handle);

	if (stmt == NULL) {
		SqlStmt_ShowDebug(stmt);
		return;
	}

	const char *query = "SELECT CONCAT('$', `key`), `index`, `value` FROM `%s`";
	char name[SCRIPT_VARNAME_LENGTH + 1];
	unsigned int index;
	int value;

	if (SQL_ERROR == SQL->StmtPrepare(stmt, query, mapreg->num_db)
	    || SQL_ERROR == SQL->StmtExecute(stmt)
	    || SQL_ERROR == SQL->StmtBindColumn(stmt, 0, SQLDT_STRING, &name, sizeof(name), NULL, NULL)
	    || SQL_ERROR == SQL->StmtBindColumn(stmt, 1, SQLDT_UINT32, &index, sizeof(index), NULL, NULL)
	    || SQL_ERROR == SQL->StmtBindColumn(stmt, 2, SQLDT_INT32, &value, sizeof(value), NULL, NULL)) {
		SqlStmt_ShowDebug(stmt);
		SQL->StmtFree(stmt);
		return;
	}

	if (SQL->StmtNumRows(stmt) < 1) {
		SQL->StmtFree(stmt);
		return;
	}

	while (SQL_SUCCESS == SQL->StmtNextRow(stmt)) {
		int var_key = script->add_variable(name);
		int64 uid = reference_uid(var_key, index);

		if (i64db_exists(mapreg->regs.vars, uid)) {
			ShowWarning("mapreg_load_num_db: Duplicate! '%s' => '%d' Skipping...\n", name, value);
			continue;
		}

		mapreg->setreg(uid, value);
	}

	SQL->StmtFree(stmt);
}

/**
 * Loads permanent string variables from the database.
 *
 **/
static void mapreg_load_str_db(void)
{
	struct SqlStmt *stmt = SQL->StmtMalloc(map->mysql_handle);

	if (stmt == NULL) {
		SqlStmt_ShowDebug(stmt);
		return;
	}

	const char *query = "SELECT CONCAT('$', `key`, '$'), `index`, `value` FROM `%s`";
	char name[SCRIPT_VARNAME_LENGTH + 1];
	unsigned int index;
	char value[256];

	if (SQL_ERROR == SQL->StmtPrepare(stmt, query, mapreg->str_db)
	    || SQL_ERROR == SQL->StmtExecute(stmt)
	    || SQL_ERROR == SQL->StmtBindColumn(stmt, 0, SQLDT_STRING, &name, sizeof(name), NULL, NULL)
	    || SQL_ERROR == SQL->StmtBindColumn(stmt, 1, SQLDT_UINT32, &index, sizeof(index), NULL, NULL)
	    || SQL_ERROR == SQL->StmtBindColumn(stmt, 2, SQLDT_STRING, &value, sizeof(value), NULL, NULL)) {
		SqlStmt_ShowDebug(stmt);
		SQL->StmtFree(stmt);
		return;
	}

	if (SQL->StmtNumRows(stmt) < 1) {
		SQL->StmtFree(stmt);
		return;
	}

	while (SQL_SUCCESS == SQL->StmtNextRow(stmt)) {
		int var_key = script->add_variable(name);
		int64 uid = reference_uid(var_key, index);

		if (i64db_exists(mapreg->regs.vars, uid)) {
			ShowWarning("mapreg_load_str_db: Duplicate! '%s' => '%s' Skipping...\n", name, value);
			continue;
		}

		mapreg->setregstr(uid, value);
	}

	SQL->StmtFree(stmt);
}

/**
 * Loads permanent variables from database.
 */
static void script_load_mapreg(void)
{
	mapreg->skip_insert = true;
	mapreg->load_num_db();
	mapreg->load_str_db();
	mapreg->skip_insert = false;
	mapreg->dirty = false;
}

/**
 * Saves a permanent integer variable to the database.
 *
 * @param name The variable's name.
 * @param idx The variable's array index.
 * @param val The variable's value.
 *
 **/
static void mapreg_save_num_db(const char *name, unsigned int idx, int val)
{
	nullpo_retv(name);

	struct SqlStmt *stmt = SQL->StmtMalloc(map->mysql_handle);

	if (stmt == NULL) {
		SqlStmt_ShowDebug(stmt);
		return;
	}

	const char *query = "UPDATE `%s` SET `value`=? WHERE `key`=? AND `index`=? LIMIT 1";
	char name_plain[SCRIPT_VARNAME_LENGTH + 1];
	safestrncpy(name_plain, name + 1, strnlen(name, SCRIPT_VARNAME_LENGTH + 1));
	size_t len = strnlen(name_plain, sizeof(name_plain));

	if (SQL_ERROR == SQL->StmtPrepare(stmt, query, mapreg->num_db)
	    || SQL_ERROR == SQL->StmtBindParam(stmt, 0, SQLDT_INT32, &val, sizeof(val))
	    || SQL_ERROR == SQL->StmtBindParam(stmt, 1, SQLDT_STRING, &name_plain, len)
	    || SQL_ERROR == SQL->StmtBindParam(stmt, 2, SQLDT_UINT32, &idx, sizeof(idx))
	    || SQL_ERROR == SQL->StmtExecute(stmt)) {
		SqlStmt_ShowDebug(stmt);
		SQL->StmtFree(stmt);
	}

	SQL->StmtFree(stmt);
}

/**
 * Saves a permanent string variable to the database.
 *
 * @param name The variable's name.
 * @param idx The variable's array index.
 * @param val The variable's value.
 *
 **/
static void mapreg_save_str_db(const char *name, unsigned int idx, char *val)
{
	nullpo_retv(name);
	nullpo_retv(val);

	struct SqlStmt *stmt = SQL->StmtMalloc(map->mysql_handle);

	if (stmt == NULL) {
		SqlStmt_ShowDebug(stmt);
		return;
	}

	const char *query = "UPDATE `%s` SET `value`=? WHERE `key`=? AND `index`=? LIMIT 1";
	char name_plain[SCRIPT_VARNAME_LENGTH + 1];
	char value[256];
	safestrncpy(name_plain, name + 1, strnlen(name, SCRIPT_VARNAME_LENGTH + 1) - 1);
	safestrncpy(value, val, strnlen(val, 255) + 1);
	size_t len_n = strnlen(name_plain, sizeof(name_plain));
	size_t len_v = strnlen(value, sizeof(value));

	if (SQL_ERROR == SQL->StmtPrepare(stmt, query, mapreg->str_db)
	    || SQL_ERROR == SQL->StmtBindParam(stmt, 0, SQLDT_STRING, &value, len_v)
	    || SQL_ERROR == SQL->StmtBindParam(stmt, 1, SQLDT_STRING, &name_plain, len_n)
	    || SQL_ERROR == SQL->StmtBindParam(stmt, 2, SQLDT_UINT32, &idx, sizeof(idx))
	    || SQL_ERROR == SQL->StmtExecute(stmt)) {
		SqlStmt_ShowDebug(stmt);
		SQL->StmtFree(stmt);
	}

	SQL->StmtFree(stmt);
}

/**
 * Saves permanent variables to database.
 */
static void script_save_mapreg(void)
{
	if (mapreg->dirty) {
		struct DBIterator *iter = db_iterator(mapreg->regs.vars);
		struct mapreg_save *m = NULL;
		for (m = dbi_first(iter); dbi_exists(iter); m = dbi_next(iter)) {
			if (m->save) {
				int num = script_getvarid(m->uid);
				int i   = script_getvaridx(m->uid);
				const char* name = script->get_str(num);
				nullpo_retv(name);

				if (!m->is_string)
					mapreg->save_num_db(name, i, m->u.i);
				else
					mapreg->save_str_db(name, i, m->u.str);

				m->save = false;
			}
		}
		dbi_destroy(iter);
		mapreg->dirty = false;
	}
}

/**
 * Timer event to auto-save permanent variables.
 *
 * @see timer->do_timer
 */
static int script_autosave_mapreg(int tid, int64 tick, int id, intptr_t data)
{
	mapreg->save();
	return 0;
}

/**
 * Destroys a mapreg_save structure, freeing the contained string, if any.
 *
 * @see DBApply
 */
static int mapreg_destroyreg(union DBKey key, struct DBData *data, va_list ap)
{
	struct mapreg_save *m = NULL;

	if (data->type != DB_DATA_PTR) // Sanity check
		return 0;

	m = DB->data2ptr(data);

	if (m->is_string) {
		if (m->u.str)
			aFree(m->u.str);
	}
	ers_free(mapreg->ers, m);

	return 0;
}

/**
 * Reloads mapregs, saving to database beforehand.
 *
 * This has the effect of clearing the temporary variables, and
 * reloading the permanent ones.
 */
static void mapreg_reload(void)
{
	mapreg->save();

	mapreg->regs.vars->clear(mapreg->regs.vars, mapreg->destroyreg);

	if( mapreg->regs.arrays ) {
		mapreg->regs.arrays->destroy(mapreg->regs.arrays, script->array_free_db);
		mapreg->regs.arrays = NULL;
	}

	mapreg->load();
}

/**
 * Finalizer.
 */
static void mapreg_final(void)
{
	mapreg->save();

	mapreg->regs.vars->destroy(mapreg->regs.vars, mapreg->destroyreg);

	ers_destroy(mapreg->ers);

	if( mapreg->regs.arrays )
		mapreg->regs.arrays->destroy(mapreg->regs.arrays, script->array_free_db);
}

/**
 * Initializer.
 */
static void mapreg_init(void)
{
	mapreg->regs.vars = i64db_alloc(DB_OPT_BASE);
	mapreg->ers = ers_new(sizeof(struct mapreg_save), "mapreg_sql.c::mapreg_ers", ERS_OPT_CLEAN);

	mapreg->load();

	timer->add_func_list(mapreg->save_timer, "mapreg_script_autosave_mapreg");
	timer->add_interval(timer->gettick() + MAPREG_AUTOSAVE_INTERVAL, mapreg->save_timer, 0, 0, MAPREG_AUTOSAVE_INTERVAL);
}

/**
 * Loads the mapreg database table names from configuration file.
 *
 * @param filename Path to configuration file. (Used in error and warning messages).
 * @param config The current config being parsed.
 * @param imported Whether the current config is imported from another file.
 * @return false on failure, true on success.
 *
 **/
static bool mapreg_config_read_registry(const char *filename, const struct config_setting_t *config, bool imported)
{
	nullpo_retr(false, filename);
	nullpo_retr(false, config);

	size_t sz = sizeof(mapreg->num_db);
	int result = libconfig->setting_lookup_mutable_string(config, "map_reg_num_db", mapreg->num_db, sz);
	bool ret_val = true;

	if (result != CONFIG_TRUE && !imported) {
		ShowError("mapreg_config_read_registry: inter_configuration/database_names/registry/map_reg_num_db was not found in %s!\n",
			  filename);
		ret_val = false;
	}

	sz = sizeof(mapreg->str_db);
	result = libconfig->setting_lookup_mutable_string(config, "map_reg_str_db", mapreg->str_db, sz);

	if (result != CONFIG_TRUE && !imported) {
		ShowError("mapreg_config_read_registry: inter_configuration/database_names/registry/map_reg_str_db was not found in %s!\n",
			  filename);
		ret_val = false;
	}

	return ret_val;
}

/**
 * Interface defaults initializer.
 */
void mapreg_defaults(void)
{
	mapreg = &mapreg_s;

	/* */
	mapreg->regs.vars = NULL;
	mapreg->ers = NULL;
	mapreg->skip_insert = false;

	safestrncpy(mapreg->num_db, "map_reg_num_db", sizeof(mapreg->num_db));
	safestrncpy(mapreg->str_db, "map_reg_str_db", sizeof(mapreg->str_db));
	mapreg->dirty = false;

	/* */
	mapreg->regs.arrays = NULL;

	/* */
	mapreg->init = mapreg_init;
	mapreg->final = mapreg_final;

	/* */
	mapreg->readreg = mapreg_readreg;
	mapreg->readregstr = mapreg_readregstr;
	mapreg->setreg = mapreg_setreg;
	mapreg->setregstr = mapreg_setregstr;
	mapreg->load_num_db = mapreg_load_num_db;
	mapreg->load_str_db = mapreg_load_str_db;
	mapreg->load = script_load_mapreg;
	mapreg->save_num_db = mapreg_save_num_db;
	mapreg->save_str_db = mapreg_save_str_db;
	mapreg->save = script_save_mapreg;
	mapreg->save_timer = script_autosave_mapreg;
	mapreg->destroyreg = mapreg_destroyreg;
	mapreg->reload = mapreg_reload;
	mapreg->config_read_registry = mapreg_config_read_registry;

}
