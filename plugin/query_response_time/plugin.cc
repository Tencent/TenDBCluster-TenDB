/* Copyright (C) 2014 Percona and Sergey Vojtovich

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301,  USA */

#ifndef MYSQL_SERVER
#define MYSQL_SERVER
#endif
#include <sql_class.h>
#include <table.h>
#include <sql_show.h>
#include <mysql/plugin_audit.h>
#include <sp_instr.h>
#include <sql_parse.h>
#include <sql_prepare.h>
#include "query_response_time.h"

#ifdef _WIN32
//static uint sql_command_flags[];

static void init_update_queries(void);

#endif


ulong opt_query_response_time_range_base= QRT_DEFAULT_BASE;
my_bool opt_query_response_time_stats= FALSE;
static my_bool opt_query_response_time_flush= FALSE;


static void query_response_time_flush_update(
              MYSQL_THD thd ,
              struct st_mysql_sys_var *var ,
              void *tgt ,
              const void *save )
{
  query_response_time_flush();
}


static MYSQL_SYSVAR_ULONG(range_base, opt_query_response_time_range_base,
       PLUGIN_VAR_RQCMDARG,
       "Select base of log for query_response_time ranges."
       "WARNING: change of this variable take effect only after next "
       "FLUSH QUERY_RESPONSE_TIME execution.",
       NULL, NULL, QRT_DEFAULT_BASE, 2, QRT_MAXIMUM_BASE, 1);
static MYSQL_SYSVAR_BOOL(stats, opt_query_response_time_stats,
       PLUGIN_VAR_OPCMDARG,
       "Enable and disable collection of query times.",
       NULL, NULL, FALSE);
static MYSQL_SYSVAR_BOOL(flush, opt_query_response_time_flush,
       PLUGIN_VAR_NOCMDOPT,
       "Update of this variable flushes statistics and re-reads "
       "query_response_time_range_base.",
       NULL, query_response_time_flush_update, FALSE);
#ifndef DBUG_OFF
static MYSQL_THDVAR_ULONGLONG(exec_time_debug, PLUGIN_VAR_NOCMDOPT,
       "Pretend queries take this many microseconds. When 0 (the default) use "
       "the actual execution time. Used only for debugging.",
       NULL, NULL, 0, 0, LONG_TIMEOUT, 1);
#endif


static struct st_mysql_sys_var *query_response_time_info_vars[]=
{
  MYSQL_SYSVAR(range_base),
  MYSQL_SYSVAR(stats),
  MYSQL_SYSVAR(flush),
#ifndef DBUG_OFF
  MYSQL_SYSVAR(exec_time_debug),
#endif
  NULL
};


ST_FIELD_INFO query_response_time_fields_info[] =
{
  { "TIME",
    QRT_TIME_STRING_LENGTH,
    MYSQL_TYPE_STRING,
    0,
    0,
    "Time",
    SKIP_OPEN_TABLE },
  { "COUNT",
    MY_INT32_NUM_DECIMAL_DIGITS,
    MYSQL_TYPE_LONG,
    0,
    MY_I_S_UNSIGNED,
    "Count",
    SKIP_OPEN_TABLE },
  { "TOTAL",
    QRT_TOTAL_STRING_LENGTH,
    MYSQL_TYPE_STRING,
    0,
    0,
    "Total",
    SKIP_OPEN_TABLE },
  { 0, 0, MYSQL_TYPE_NULL, 0, 0, 0, 0 }
};


static int query_response_time_info_init(void *p)
{
  ST_SCHEMA_TABLE *i_s_query_response_time= (ST_SCHEMA_TABLE *) p;
  i_s_query_response_time->fields_info= query_response_time_fields_info;
  if (!my_strcasecmp(system_charset_info, i_s_query_response_time->table_name,
                     "QUERY_RESPONSE_TIME"))
    i_s_query_response_time->fill_table= query_response_time_fill;
  else if (!my_strcasecmp(system_charset_info,
                          i_s_query_response_time->table_name,
                          "QUERY_RESPONSE_TIME_READ"))
    i_s_query_response_time->fill_table= query_response_time_fill_ro;
  else if (!my_strcasecmp(system_charset_info,
                          i_s_query_response_time->table_name,
                          "QUERY_RESPONSE_TIME_WRITE"))
    i_s_query_response_time->fill_table= query_response_time_fill_rw;
  else
    DBUG_ASSERT(0);

#ifdef _WIN32
	init_update_queries();
#endif // __WIN__

  query_response_time_init();
  return 0;
}


static int query_response_time_info_deinit(void *arg )
{
  opt_query_response_time_stats= FALSE;
  query_response_time_free();
  return 0;
}


static struct st_mysql_information_schema query_response_time_info_descriptor=
{ MYSQL_INFORMATION_SCHEMA_INTERFACE_VERSION };

static int query_response_time_audit_notify(MYSQL_THD thd,
                                            mysql_event_class_t event_class,
                                            const void *event)
{
  const struct mysql_event_general *event_general=
    (const struct mysql_event_general *) event;
  DBUG_ASSERT(event_class == MYSQL_AUDIT_GENERAL_CLASS);
  if (event_general->event_subclass == MYSQL_AUDIT_GENERAL_STATUS &&
      opt_query_response_time_stats)
  {
    /*
     Get sql command id of currently executed statement
     inside of stored function or procedure. If the command is "PREPARE"
     don't get the statement inside of "PREPARE". If the statement
     is not inside of stored function or procedure get sql command id
     of the statement itself.
    */
    enum_sql_command sql_command=
      (
        thd->lex->sql_command != SQLCOM_PREPARE &&
        thd->sp_runtime_ctx &&
        thd->stmt_arena &&
        ((sp_lex_instr *)thd->stmt_arena)->get_command() >= 0
      ) ?
      (enum_sql_command)((sp_lex_instr *)thd->stmt_arena)->get_command() :
      thd->lex->sql_command;
    if (sql_command == SQLCOM_EXECUTE)
    {
      const LEX_CSTRING *name=
        (
          thd->sp_runtime_ctx &&
          thd->stmt_arena &&
          ((sp_lex_instr *)thd->stmt_arena)->get_prepared_stmt_name()
        )                                                               ?
        /* If we are inside of SP */
        ((sp_lex_instr *)thd->stmt_arena)->get_prepared_stmt_name()     :
        /* otherwise */
        &thd->lex->prepared_stmt_name;
      Prepared_statement *stmt= thd->stmt_map.find_by_name(*name);
      /* In case of EXECUTE <non-existing-PS>, keep SQLCOM_EXECUTE as the
      command. */
      if (likely(stmt && stmt->lex))
        sql_command= stmt->lex->sql_command;
    }
    QUERY_TYPE query_type=
      (sql_command_flags[sql_command] & CF_CHANGES_DATA) ? WRITE : READ;
#ifndef DBUG_OFF
    if (THDVAR(thd, exec_time_debug)) {
      ulonglong t = THDVAR(thd, exec_time_debug);
      if ((thd->lex->sql_command == SQLCOM_SET_OPTION) ||
          (thd->lex->spname && thd->stmt_arena && thd->sp_runtime_ctx &&
              ((sp_lex_instr *)thd->stmt_arena)->get_command() ==
              SQLCOM_SET_OPTION )) {
          t = 0;
      }
      query_response_time_collect(query_type, t);
    }
    else
#endif
      query_response_time_collect(query_type,
                                  thd->utime_after_query -
                                  thd->utime_after_lock);
  }
  return 0;
}


static struct st_mysql_audit query_response_time_audit_descriptor=
{
  MYSQL_AUDIT_INTERFACE_VERSION, NULL, query_response_time_audit_notify,
  { MYSQL_AUDIT_GENERAL_ALL, 0, 0, 0, 0, 0, 0, 0, 0, 0 }
};


mysql_declare_plugin(query_response_time)
{
  MYSQL_INFORMATION_SCHEMA_PLUGIN,
  &query_response_time_info_descriptor,
  "QUERY_RESPONSE_TIME",
  "Percona and Sergey Vojtovich",
  "Query Response Time Distribution INFORMATION_SCHEMA Plugin",
  PLUGIN_LICENSE_GPL,
  query_response_time_info_init,
  query_response_time_info_deinit,
  0x0100,
  NULL,
  query_response_time_info_vars,
  (void *)"1.0",
  0,
},
{
  MYSQL_INFORMATION_SCHEMA_PLUGIN,
  &query_response_time_info_descriptor,
  "QUERY_RESPONSE_TIME_READ",
  "Percona and Sergey Vojtovich",
  "Query Response Time Distribution INFORMATION_SCHEMA Plugin",
  PLUGIN_LICENSE_GPL,
  query_response_time_info_init,
  query_response_time_info_deinit,
  0x0100,
  NULL,
  NULL,
  (void *)"1.0",
  0,
},
{
  MYSQL_INFORMATION_SCHEMA_PLUGIN,
  &query_response_time_info_descriptor,
  "QUERY_RESPONSE_TIME_WRITE",
  "Percona and Sergey Vojtovich",
  "Query Response Time Distribution INFORMATION_SCHEMA Plugin",
  PLUGIN_LICENSE_GPL,
  query_response_time_info_init,
  query_response_time_info_deinit,
  0x0100,
  NULL,
  NULL,
  (void *)"1.0",
  0,
},
{
  MYSQL_AUDIT_PLUGIN,
  &query_response_time_audit_descriptor,
  "QUERY_RESPONSE_TIME_AUDIT",
  "Percona and Sergey Vojtovich",
  "Query Response Time Distribution Audit Plugin",
  PLUGIN_LICENSE_GPL,
  NULL,
  NULL,
  0x0100,
  NULL,
  NULL,
  (void *)"1.0",
  0,
}
mysql_declare_plugin_end;

#ifdef _WIN32
static uint sql_command_flags[SQLCOM_END + 1];

static void init_update_queries(void)
{
	/* Initialize the server command flags array. */
	//memset(server_command_flags, 0, sizeof(server_command_flags));

	//server_command_flags[COM_SLEEP] = CF_ALLOW_PROTOCOL_PLUGIN;
	//server_command_flags[COM_INIT_DB] = CF_ALLOW_PROTOCOL_PLUGIN;
	//server_command_flags[COM_QUERY] = CF_ALLOW_PROTOCOL_PLUGIN;
	//server_command_flags[COM_FIELD_LIST] = CF_ALLOW_PROTOCOL_PLUGIN;
	//server_command_flags[COM_REFRESH] = CF_ALLOW_PROTOCOL_PLUGIN;
	//server_command_flags[COM_SHUTDOWN] = CF_ALLOW_PROTOCOL_PLUGIN;
	//server_command_flags[COM_STATISTICS] = CF_SKIP_QUESTIONS;
	//server_command_flags[COM_PROCESS_KILL] = CF_ALLOW_PROTOCOL_PLUGIN;
	//server_command_flags[COM_PING] = CF_SKIP_QUESTIONS;
	//server_command_flags[COM_STMT_PREPARE] = CF_SKIP_QUESTIONS |
	//	CF_ALLOW_PROTOCOL_PLUGIN;
	//server_command_flags[COM_STMT_EXECUTE] = CF_ALLOW_PROTOCOL_PLUGIN;
	//server_command_flags[COM_STMT_SEND_LONG_DATA] = CF_ALLOW_PROTOCOL_PLUGIN;
	//server_command_flags[COM_STMT_CLOSE] = CF_SKIP_QUESTIONS |
	//	CF_ALLOW_PROTOCOL_PLUGIN;
	//server_command_flags[COM_STMT_RESET] = CF_SKIP_QUESTIONS |
	//	CF_ALLOW_PROTOCOL_PLUGIN;
	//server_command_flags[COM_STMT_FETCH] = CF_ALLOW_PROTOCOL_PLUGIN;
	//server_command_flags[COM_END] = CF_ALLOW_PROTOCOL_PLUGIN;

	/* Initialize the sql command flags array. */
	memset(sql_command_flags, 0, sizeof(sql_command_flags));

	/*
	In general, DDL statements do not generate row events and do not go
	through a cache before being written to the binary log. However, the
	CREATE TABLE...SELECT is an exception because it may generate row
	events. For that reason,  the SQLCOM_CREATE_TABLE  which represents
	a CREATE TABLE, including the CREATE TABLE...SELECT, has the
	CF_CAN_GENERATE_ROW_EVENTS flag. The distinction between a regular
	CREATE TABLE and the CREATE TABLE...SELECT is made in other parts of
	the code, in particular in the Query_log_event's constructor.
	*/
	sql_command_flags[SQLCOM_CREATE_TABLE] = CF_CHANGES_DATA | CF_REEXECUTION_FRAGILE |
		CF_AUTO_COMMIT_TRANS |
		CF_CAN_GENERATE_ROW_EVENTS;
	sql_command_flags[SQLCOM_CREATE_INDEX] = CF_CHANGES_DATA | CF_AUTO_COMMIT_TRANS;
	sql_command_flags[SQLCOM_ALTER_TABLE] = CF_CHANGES_DATA | CF_WRITE_LOGS_COMMAND |
		CF_AUTO_COMMIT_TRANS;
	sql_command_flags[SQLCOM_TRUNCATE] = CF_CHANGES_DATA | CF_WRITE_LOGS_COMMAND |
		CF_AUTO_COMMIT_TRANS;
	sql_command_flags[SQLCOM_DROP_TABLE] = CF_CHANGES_DATA | CF_AUTO_COMMIT_TRANS;
	sql_command_flags[SQLCOM_LOAD] = CF_CHANGES_DATA | CF_REEXECUTION_FRAGILE |
		CF_CAN_GENERATE_ROW_EVENTS;
	sql_command_flags[SQLCOM_CREATE_COMPRESSION_DICTIONARY] = CF_CHANGES_DATA |
		CF_AUTO_COMMIT_TRANS;
	sql_command_flags[SQLCOM_DROP_COMPRESSION_DICTIONARY] = CF_CHANGES_DATA |
		CF_AUTO_COMMIT_TRANS;
	sql_command_flags[SQLCOM_CREATE_DB] = CF_CHANGES_DATA | CF_AUTO_COMMIT_TRANS;
	sql_command_flags[SQLCOM_DROP_DB] = CF_CHANGES_DATA | CF_AUTO_COMMIT_TRANS;
	sql_command_flags[SQLCOM_ALTER_DB_UPGRADE] = CF_AUTO_COMMIT_TRANS;
	sql_command_flags[SQLCOM_ALTER_DB] = CF_CHANGES_DATA | CF_AUTO_COMMIT_TRANS;
	sql_command_flags[SQLCOM_RENAME_TABLE] = CF_CHANGES_DATA | CF_AUTO_COMMIT_TRANS;
	sql_command_flags[SQLCOM_DROP_INDEX] = CF_CHANGES_DATA | CF_AUTO_COMMIT_TRANS;
	sql_command_flags[SQLCOM_CREATE_VIEW] = CF_CHANGES_DATA | CF_REEXECUTION_FRAGILE |
		CF_AUTO_COMMIT_TRANS;
	sql_command_flags[SQLCOM_DROP_VIEW] = CF_CHANGES_DATA | CF_AUTO_COMMIT_TRANS;
	sql_command_flags[SQLCOM_CREATE_TRIGGER] = CF_CHANGES_DATA | CF_AUTO_COMMIT_TRANS;
	sql_command_flags[SQLCOM_DROP_TRIGGER] = CF_CHANGES_DATA | CF_AUTO_COMMIT_TRANS;
	sql_command_flags[SQLCOM_CREATE_EVENT] = CF_CHANGES_DATA | CF_AUTO_COMMIT_TRANS;
	sql_command_flags[SQLCOM_ALTER_EVENT] = CF_CHANGES_DATA | CF_AUTO_COMMIT_TRANS;
	sql_command_flags[SQLCOM_DROP_EVENT] = CF_CHANGES_DATA | CF_AUTO_COMMIT_TRANS;

	sql_command_flags[SQLCOM_UPDATE] = CF_CHANGES_DATA | CF_REEXECUTION_FRAGILE |
		CF_CAN_GENERATE_ROW_EVENTS |
		CF_OPTIMIZER_TRACE |
		CF_CAN_BE_EXPLAINED;
	sql_command_flags[SQLCOM_UPDATE_MULTI] = CF_CHANGES_DATA | CF_REEXECUTION_FRAGILE |
		CF_CAN_GENERATE_ROW_EVENTS |
		CF_OPTIMIZER_TRACE |
		CF_CAN_BE_EXPLAINED;
	// This is INSERT VALUES(...), can be VALUES(stored_func()) so we trace it
	sql_command_flags[SQLCOM_INSERT] = CF_CHANGES_DATA | CF_REEXECUTION_FRAGILE |
		CF_CAN_GENERATE_ROW_EVENTS |
		CF_OPTIMIZER_TRACE |
		CF_CAN_BE_EXPLAINED;
	sql_command_flags[SQLCOM_INSERT_SELECT] = CF_CHANGES_DATA | CF_REEXECUTION_FRAGILE |
		CF_CAN_GENERATE_ROW_EVENTS |
		CF_OPTIMIZER_TRACE |
		CF_CAN_BE_EXPLAINED;
	sql_command_flags[SQLCOM_DELETE] = CF_CHANGES_DATA | CF_REEXECUTION_FRAGILE |
		CF_CAN_GENERATE_ROW_EVENTS |
		CF_OPTIMIZER_TRACE |
		CF_CAN_BE_EXPLAINED;
	sql_command_flags[SQLCOM_DELETE_MULTI] = CF_CHANGES_DATA | CF_REEXECUTION_FRAGILE |
		CF_CAN_GENERATE_ROW_EVENTS |
		CF_OPTIMIZER_TRACE |
		CF_CAN_BE_EXPLAINED;
	sql_command_flags[SQLCOM_REPLACE] = CF_CHANGES_DATA | CF_REEXECUTION_FRAGILE |
		CF_CAN_GENERATE_ROW_EVENTS |
		CF_OPTIMIZER_TRACE |
		CF_CAN_BE_EXPLAINED;
	sql_command_flags[SQLCOM_REPLACE_SELECT] = CF_CHANGES_DATA | CF_REEXECUTION_FRAGILE |
		CF_CAN_GENERATE_ROW_EVENTS |
		CF_OPTIMIZER_TRACE |
		CF_CAN_BE_EXPLAINED;
	sql_command_flags[SQLCOM_SELECT] = CF_REEXECUTION_FRAGILE |
		CF_CAN_GENERATE_ROW_EVENTS |
		CF_OPTIMIZER_TRACE |
		CF_CAN_BE_EXPLAINED;
	// (1) so that subquery is traced when doing "SET @var = (subquery)"
	/*
	@todo SQLCOM_SET_OPTION should have CF_CAN_GENERATE_ROW_EVENTS
	set, because it may invoke a stored function that generates row
	events. /Sven
	*/
	sql_command_flags[SQLCOM_SET_OPTION] = CF_REEXECUTION_FRAGILE |
		CF_AUTO_COMMIT_TRANS |
		CF_CAN_GENERATE_ROW_EVENTS |
		CF_OPTIMIZER_TRACE; // (1)
	// (1) so that subquery is traced when doing "DO @var := (subquery)"
	sql_command_flags[SQLCOM_DO] = CF_REEXECUTION_FRAGILE |
		CF_CAN_GENERATE_ROW_EVENTS |
		CF_OPTIMIZER_TRACE; // (1)

	sql_command_flags[SQLCOM_SHOW_STATUS_PROC] = CF_STATUS_COMMAND | CF_REEXECUTION_FRAGILE;
	sql_command_flags[SQLCOM_SHOW_STATUS] = CF_STATUS_COMMAND | CF_REEXECUTION_FRAGILE;
	sql_command_flags[SQLCOM_SHOW_DATABASES] = CF_STATUS_COMMAND | CF_REEXECUTION_FRAGILE;
	sql_command_flags[SQLCOM_SHOW_TRIGGERS] = CF_STATUS_COMMAND | CF_REEXECUTION_FRAGILE;
	sql_command_flags[SQLCOM_SHOW_EVENTS] = CF_STATUS_COMMAND | CF_REEXECUTION_FRAGILE;
	sql_command_flags[SQLCOM_SHOW_OPEN_TABLES] = CF_STATUS_COMMAND | CF_REEXECUTION_FRAGILE;
	sql_command_flags[SQLCOM_SHOW_PLUGINS] = CF_STATUS_COMMAND;
	sql_command_flags[SQLCOM_SHOW_FIELDS] = CF_STATUS_COMMAND | CF_REEXECUTION_FRAGILE;
	sql_command_flags[SQLCOM_SHOW_KEYS] = CF_STATUS_COMMAND | CF_REEXECUTION_FRAGILE;
	sql_command_flags[SQLCOM_SHOW_VARIABLES] = CF_STATUS_COMMAND | CF_REEXECUTION_FRAGILE;
	sql_command_flags[SQLCOM_SHOW_CHARSETS] = CF_STATUS_COMMAND | CF_REEXECUTION_FRAGILE;
	sql_command_flags[SQLCOM_SHOW_COLLATIONS] = CF_STATUS_COMMAND | CF_REEXECUTION_FRAGILE;
	sql_command_flags[SQLCOM_SHOW_BINLOGS] = CF_STATUS_COMMAND;
	sql_command_flags[SQLCOM_SHOW_SLAVE_HOSTS] = CF_STATUS_COMMAND;
	sql_command_flags[SQLCOM_SHOW_BINLOG_EVENTS] = CF_STATUS_COMMAND;
	sql_command_flags[SQLCOM_SHOW_STORAGE_ENGINES] = CF_STATUS_COMMAND;
	sql_command_flags[SQLCOM_SHOW_PRIVILEGES] = CF_STATUS_COMMAND;
	sql_command_flags[SQLCOM_SHOW_WARNS] = CF_STATUS_COMMAND | CF_DIAGNOSTIC_STMT;
	sql_command_flags[SQLCOM_SHOW_ERRORS] = CF_STATUS_COMMAND | CF_DIAGNOSTIC_STMT;
	sql_command_flags[SQLCOM_SHOW_ENGINE_STATUS] = CF_STATUS_COMMAND;
	sql_command_flags[SQLCOM_SHOW_ENGINE_MUTEX] = CF_STATUS_COMMAND;
	sql_command_flags[SQLCOM_SHOW_ENGINE_LOGS] = CF_STATUS_COMMAND;
	sql_command_flags[SQLCOM_SHOW_PROCESSLIST] = CF_STATUS_COMMAND;
	sql_command_flags[SQLCOM_SHOW_GRANTS] = CF_STATUS_COMMAND;
	sql_command_flags[SQLCOM_SHOW_CREATE_DB] = CF_STATUS_COMMAND;
	sql_command_flags[SQLCOM_SHOW_CREATE] = CF_STATUS_COMMAND;
	sql_command_flags[SQLCOM_SHOW_MASTER_STAT] = CF_STATUS_COMMAND;
	sql_command_flags[SQLCOM_SHOW_SLAVE_STAT] = CF_STATUS_COMMAND;
	sql_command_flags[SQLCOM_SHOW_CREATE_PROC] = CF_STATUS_COMMAND;
	sql_command_flags[SQLCOM_SHOW_CREATE_FUNC] = CF_STATUS_COMMAND;
	sql_command_flags[SQLCOM_SHOW_CREATE_TRIGGER] = CF_STATUS_COMMAND;
	sql_command_flags[SQLCOM_SHOW_STATUS_FUNC] = CF_STATUS_COMMAND | CF_REEXECUTION_FRAGILE;
	sql_command_flags[SQLCOM_SHOW_PROC_CODE] = CF_STATUS_COMMAND;
	sql_command_flags[SQLCOM_SHOW_FUNC_CODE] = CF_STATUS_COMMAND;
	sql_command_flags[SQLCOM_SHOW_CREATE_EVENT] = CF_STATUS_COMMAND;
	sql_command_flags[SQLCOM_SHOW_PROFILES] = CF_STATUS_COMMAND;
	sql_command_flags[SQLCOM_SHOW_PROFILE] = CF_STATUS_COMMAND;
	sql_command_flags[SQLCOM_BINLOG_BASE64_EVENT] = CF_STATUS_COMMAND |
		CF_CAN_GENERATE_ROW_EVENTS;

	sql_command_flags[SQLCOM_SHOW_TABLES] = (CF_STATUS_COMMAND |
		CF_SHOW_TABLE_COMMAND |
		CF_REEXECUTION_FRAGILE);
	sql_command_flags[SQLCOM_SHOW_TABLE_STATUS] = (CF_STATUS_COMMAND |
		CF_SHOW_TABLE_COMMAND |
		CF_REEXECUTION_FRAGILE);

	sql_command_flags[SQLCOM_CREATE_USER] = CF_CHANGES_DATA;
	sql_command_flags[SQLCOM_RENAME_USER] = CF_CHANGES_DATA;
	sql_command_flags[SQLCOM_DROP_USER] = CF_CHANGES_DATA;
	sql_command_flags[SQLCOM_ALTER_USER] = CF_CHANGES_DATA;
	sql_command_flags[SQLCOM_GRANT] = CF_CHANGES_DATA;
	sql_command_flags[SQLCOM_REVOKE] = CF_CHANGES_DATA;
	sql_command_flags[SQLCOM_REVOKE_ALL] = CF_CHANGES_DATA;
	sql_command_flags[SQLCOM_OPTIMIZE] = CF_CHANGES_DATA;
	sql_command_flags[SQLCOM_ALTER_INSTANCE] = CF_CHANGES_DATA;
	sql_command_flags[SQLCOM_CREATE_FUNCTION] = CF_CHANGES_DATA | CF_AUTO_COMMIT_TRANS;
	sql_command_flags[SQLCOM_CREATE_PROCEDURE] = CF_CHANGES_DATA | CF_AUTO_COMMIT_TRANS;
	sql_command_flags[SQLCOM_CREATE_SPFUNCTION] = CF_CHANGES_DATA | CF_AUTO_COMMIT_TRANS;
	sql_command_flags[SQLCOM_DROP_PROCEDURE] = CF_CHANGES_DATA | CF_AUTO_COMMIT_TRANS;
	sql_command_flags[SQLCOM_DROP_FUNCTION] = CF_CHANGES_DATA | CF_AUTO_COMMIT_TRANS;
	sql_command_flags[SQLCOM_ALTER_PROCEDURE] = CF_CHANGES_DATA | CF_AUTO_COMMIT_TRANS;
	sql_command_flags[SQLCOM_ALTER_FUNCTION] = CF_CHANGES_DATA | CF_AUTO_COMMIT_TRANS;
	sql_command_flags[SQLCOM_INSTALL_PLUGIN] = CF_CHANGES_DATA | CF_AUTO_COMMIT_TRANS;
	sql_command_flags[SQLCOM_UNINSTALL_PLUGIN] = CF_CHANGES_DATA | CF_AUTO_COMMIT_TRANS;

	/* Does not change the contents of the Diagnostics Area. */
	sql_command_flags[SQLCOM_GET_DIAGNOSTICS] = CF_DIAGNOSTIC_STMT;

	/*
	(1): without it, in "CALL some_proc((subq))", subquery would not be
	traced.
	*/
	sql_command_flags[SQLCOM_CALL] = CF_REEXECUTION_FRAGILE |
		CF_CAN_GENERATE_ROW_EVENTS |
		CF_OPTIMIZER_TRACE; // (1)
	sql_command_flags[SQLCOM_EXECUTE] = CF_CAN_GENERATE_ROW_EVENTS;

	/*
	The following admin table operations are allowed
	on log tables.
	*/
	sql_command_flags[SQLCOM_REPAIR] = CF_WRITE_LOGS_COMMAND | CF_AUTO_COMMIT_TRANS;
	sql_command_flags[SQLCOM_OPTIMIZE] |= CF_WRITE_LOGS_COMMAND | CF_AUTO_COMMIT_TRANS;
	sql_command_flags[SQLCOM_ANALYZE] = CF_WRITE_LOGS_COMMAND | CF_AUTO_COMMIT_TRANS;
	sql_command_flags[SQLCOM_CHECK] = CF_WRITE_LOGS_COMMAND | CF_AUTO_COMMIT_TRANS;

	sql_command_flags[SQLCOM_CREATE_USER] |= CF_AUTO_COMMIT_TRANS;
	sql_command_flags[SQLCOM_DROP_USER] |= CF_AUTO_COMMIT_TRANS;
	sql_command_flags[SQLCOM_RENAME_USER] |= CF_AUTO_COMMIT_TRANS;
	sql_command_flags[SQLCOM_ALTER_USER] |= CF_AUTO_COMMIT_TRANS;
	sql_command_flags[SQLCOM_REVOKE] |= CF_AUTO_COMMIT_TRANS;
	sql_command_flags[SQLCOM_REVOKE_ALL] |= CF_AUTO_COMMIT_TRANS;
	sql_command_flags[SQLCOM_GRANT] |= CF_AUTO_COMMIT_TRANS;

	sql_command_flags[SQLCOM_ASSIGN_TO_KEYCACHE] = CF_AUTO_COMMIT_TRANS;
	sql_command_flags[SQLCOM_PRELOAD_KEYS] = CF_AUTO_COMMIT_TRANS;
	sql_command_flags[SQLCOM_ALTER_INSTANCE] |= CF_AUTO_COMMIT_TRANS;

	sql_command_flags[SQLCOM_FLUSH] = CF_AUTO_COMMIT_TRANS;
	sql_command_flags[SQLCOM_RESET] = CF_AUTO_COMMIT_TRANS;
	sql_command_flags[SQLCOM_CREATE_SERVER] = CF_AUTO_COMMIT_TRANS;
	sql_command_flags[SQLCOM_ALTER_SERVER] = CF_AUTO_COMMIT_TRANS;
	sql_command_flags[SQLCOM_DROP_SERVER] = CF_AUTO_COMMIT_TRANS;
	sql_command_flags[SQLCOM_CHANGE_MASTER] = CF_AUTO_COMMIT_TRANS;
	sql_command_flags[SQLCOM_CHANGE_REPLICATION_FILTER] = CF_AUTO_COMMIT_TRANS;
	sql_command_flags[SQLCOM_SLAVE_START] = CF_AUTO_COMMIT_TRANS;
	sql_command_flags[SQLCOM_SLAVE_STOP] = CF_AUTO_COMMIT_TRANS;
	sql_command_flags[SQLCOM_ALTER_TABLESPACE] |= CF_AUTO_COMMIT_TRANS;

	/*
	The following statements can deal with temporary tables,
	so temporary tables should be pre-opened for those statements to
	simplify privilege checking.

	There are other statements that deal with temporary tables and open
	them, but which are not listed here. The thing is that the order of
	pre-opening temporary tables for those statements is somewhat custom.
	*/
	sql_command_flags[SQLCOM_CREATE_TABLE] |= CF_PREOPEN_TMP_TABLES;
	sql_command_flags[SQLCOM_DROP_TABLE] |= CF_PREOPEN_TMP_TABLES;
	sql_command_flags[SQLCOM_CREATE_INDEX] |= CF_PREOPEN_TMP_TABLES;
	sql_command_flags[SQLCOM_ALTER_TABLE] |= CF_PREOPEN_TMP_TABLES;
	sql_command_flags[SQLCOM_TRUNCATE] |= CF_PREOPEN_TMP_TABLES;
	sql_command_flags[SQLCOM_LOAD] |= CF_PREOPEN_TMP_TABLES;
	sql_command_flags[SQLCOM_DROP_INDEX] |= CF_PREOPEN_TMP_TABLES;
	sql_command_flags[SQLCOM_UPDATE] |= CF_PREOPEN_TMP_TABLES;
	sql_command_flags[SQLCOM_UPDATE_MULTI] |= CF_PREOPEN_TMP_TABLES;
	sql_command_flags[SQLCOM_INSERT_SELECT] |= CF_PREOPEN_TMP_TABLES;
	sql_command_flags[SQLCOM_DELETE] |= CF_PREOPEN_TMP_TABLES;
	sql_command_flags[SQLCOM_DELETE_MULTI] |= CF_PREOPEN_TMP_TABLES;
	sql_command_flags[SQLCOM_REPLACE_SELECT] |= CF_PREOPEN_TMP_TABLES;
	sql_command_flags[SQLCOM_SELECT] |= CF_PREOPEN_TMP_TABLES;
	sql_command_flags[SQLCOM_SET_OPTION] |= CF_PREOPEN_TMP_TABLES;
	sql_command_flags[SQLCOM_DO] |= CF_PREOPEN_TMP_TABLES;
	sql_command_flags[SQLCOM_CALL] |= CF_PREOPEN_TMP_TABLES;
	sql_command_flags[SQLCOM_CHECKSUM] |= CF_PREOPEN_TMP_TABLES;
	sql_command_flags[SQLCOM_ANALYZE] |= CF_PREOPEN_TMP_TABLES;
	sql_command_flags[SQLCOM_CHECK] |= CF_PREOPEN_TMP_TABLES;
	sql_command_flags[SQLCOM_OPTIMIZE] |= CF_PREOPEN_TMP_TABLES;
	sql_command_flags[SQLCOM_REPAIR] |= CF_PREOPEN_TMP_TABLES;
	sql_command_flags[SQLCOM_PRELOAD_KEYS] |= CF_PREOPEN_TMP_TABLES;
	sql_command_flags[SQLCOM_ASSIGN_TO_KEYCACHE] |= CF_PREOPEN_TMP_TABLES;

	/*
	DDL statements that should start with closing opened handlers.

	We use this flag only for statements for which open HANDLERs
	have to be closed before emporary tables are pre-opened.
	*/
	sql_command_flags[SQLCOM_CREATE_TABLE] |= CF_HA_CLOSE;
	sql_command_flags[SQLCOM_DROP_TABLE] |= CF_HA_CLOSE;
	sql_command_flags[SQLCOM_ALTER_TABLE] |= CF_HA_CLOSE;
	sql_command_flags[SQLCOM_TRUNCATE] |= CF_HA_CLOSE;
	sql_command_flags[SQLCOM_REPAIR] |= CF_HA_CLOSE;
	sql_command_flags[SQLCOM_OPTIMIZE] |= CF_HA_CLOSE;
	sql_command_flags[SQLCOM_ANALYZE] |= CF_HA_CLOSE;
	sql_command_flags[SQLCOM_CHECK] |= CF_HA_CLOSE;
	sql_command_flags[SQLCOM_CREATE_INDEX] |= CF_HA_CLOSE;
	sql_command_flags[SQLCOM_DROP_INDEX] |= CF_HA_CLOSE;
	sql_command_flags[SQLCOM_PRELOAD_KEYS] |= CF_HA_CLOSE;
	sql_command_flags[SQLCOM_ASSIGN_TO_KEYCACHE] |= CF_HA_CLOSE;

	/*
	Mark statements that always are disallowed in read-only
	transactions. Note that according to the SQL standard,
	even temporary table DDL should be disallowed.
	*/
	sql_command_flags[SQLCOM_CREATE_TABLE] |= CF_DISALLOW_IN_RO_TRANS;
	sql_command_flags[SQLCOM_ALTER_TABLE] |= CF_DISALLOW_IN_RO_TRANS;
	sql_command_flags[SQLCOM_DROP_TABLE] |= CF_DISALLOW_IN_RO_TRANS;
	sql_command_flags[SQLCOM_RENAME_TABLE] |= CF_DISALLOW_IN_RO_TRANS;
	sql_command_flags[SQLCOM_CREATE_INDEX] |= CF_DISALLOW_IN_RO_TRANS;
	sql_command_flags[SQLCOM_DROP_INDEX] |= CF_DISALLOW_IN_RO_TRANS;
	sql_command_flags[SQLCOM_CREATE_COMPRESSION_DICTIONARY] |=
		CF_DISALLOW_IN_RO_TRANS;
	sql_command_flags[SQLCOM_DROP_COMPRESSION_DICTIONARY] |=
		CF_DISALLOW_IN_RO_TRANS;
	sql_command_flags[SQLCOM_CREATE_DB] |= CF_DISALLOW_IN_RO_TRANS;
	sql_command_flags[SQLCOM_DROP_DB] |= CF_DISALLOW_IN_RO_TRANS;
	sql_command_flags[SQLCOM_ALTER_DB_UPGRADE] |= CF_DISALLOW_IN_RO_TRANS;
	sql_command_flags[SQLCOM_ALTER_DB] |= CF_DISALLOW_IN_RO_TRANS;
	sql_command_flags[SQLCOM_CREATE_VIEW] |= CF_DISALLOW_IN_RO_TRANS;
	sql_command_flags[SQLCOM_DROP_VIEW] |= CF_DISALLOW_IN_RO_TRANS;
	sql_command_flags[SQLCOM_CREATE_TRIGGER] |= CF_DISALLOW_IN_RO_TRANS;
	sql_command_flags[SQLCOM_DROP_TRIGGER] |= CF_DISALLOW_IN_RO_TRANS;
	sql_command_flags[SQLCOM_CREATE_EVENT] |= CF_DISALLOW_IN_RO_TRANS;
	sql_command_flags[SQLCOM_ALTER_EVENT] |= CF_DISALLOW_IN_RO_TRANS;
	sql_command_flags[SQLCOM_DROP_EVENT] |= CF_DISALLOW_IN_RO_TRANS;
	sql_command_flags[SQLCOM_CREATE_USER] |= CF_DISALLOW_IN_RO_TRANS;
	sql_command_flags[SQLCOM_RENAME_USER] |= CF_DISALLOW_IN_RO_TRANS;
	sql_command_flags[SQLCOM_ALTER_USER] |= CF_DISALLOW_IN_RO_TRANS;
	sql_command_flags[SQLCOM_DROP_USER] |= CF_DISALLOW_IN_RO_TRANS;
	sql_command_flags[SQLCOM_CREATE_SERVER] |= CF_DISALLOW_IN_RO_TRANS;
	sql_command_flags[SQLCOM_ALTER_SERVER] |= CF_DISALLOW_IN_RO_TRANS;
	sql_command_flags[SQLCOM_DROP_SERVER] |= CF_DISALLOW_IN_RO_TRANS;
	sql_command_flags[SQLCOM_CREATE_FUNCTION] |= CF_DISALLOW_IN_RO_TRANS;
	sql_command_flags[SQLCOM_CREATE_PROCEDURE] |= CF_DISALLOW_IN_RO_TRANS;
	sql_command_flags[SQLCOM_CREATE_SPFUNCTION] |= CF_DISALLOW_IN_RO_TRANS;
	sql_command_flags[SQLCOM_DROP_PROCEDURE] |= CF_DISALLOW_IN_RO_TRANS;
	sql_command_flags[SQLCOM_DROP_FUNCTION] |= CF_DISALLOW_IN_RO_TRANS;
	sql_command_flags[SQLCOM_ALTER_PROCEDURE] |= CF_DISALLOW_IN_RO_TRANS;
	sql_command_flags[SQLCOM_ALTER_FUNCTION] |= CF_DISALLOW_IN_RO_TRANS;
	sql_command_flags[SQLCOM_TRUNCATE] |= CF_DISALLOW_IN_RO_TRANS;
	sql_command_flags[SQLCOM_ALTER_TABLESPACE] |= CF_DISALLOW_IN_RO_TRANS;
	sql_command_flags[SQLCOM_REPAIR] |= CF_DISALLOW_IN_RO_TRANS;
	sql_command_flags[SQLCOM_OPTIMIZE] |= CF_DISALLOW_IN_RO_TRANS;
	sql_command_flags[SQLCOM_GRANT] |= CF_DISALLOW_IN_RO_TRANS;
	sql_command_flags[SQLCOM_REVOKE] |= CF_DISALLOW_IN_RO_TRANS;
	sql_command_flags[SQLCOM_REVOKE_ALL] |= CF_DISALLOW_IN_RO_TRANS;
	sql_command_flags[SQLCOM_INSTALL_PLUGIN] |= CF_DISALLOW_IN_RO_TRANS;
	sql_command_flags[SQLCOM_UNINSTALL_PLUGIN] |= CF_DISALLOW_IN_RO_TRANS;
	sql_command_flags[SQLCOM_ALTER_INSTANCE] |= CF_DISALLOW_IN_RO_TRANS;

	/*
	Mark statements that are allowed to be executed by the plugins.
	*/
	sql_command_flags[SQLCOM_SELECT] |= CF_ALLOW_PROTOCOL_PLUGIN;
	sql_command_flags[SQLCOM_CREATE_TABLE] |= CF_ALLOW_PROTOCOL_PLUGIN;
	sql_command_flags[SQLCOM_CREATE_INDEX] |= CF_ALLOW_PROTOCOL_PLUGIN;
	sql_command_flags[SQLCOM_ALTER_TABLE] |= CF_ALLOW_PROTOCOL_PLUGIN;
	sql_command_flags[SQLCOM_UPDATE] |= CF_ALLOW_PROTOCOL_PLUGIN;
	sql_command_flags[SQLCOM_INSERT] |= CF_ALLOW_PROTOCOL_PLUGIN;
	sql_command_flags[SQLCOM_INSERT_SELECT] |= CF_ALLOW_PROTOCOL_PLUGIN;
	sql_command_flags[SQLCOM_DELETE] |= CF_ALLOW_PROTOCOL_PLUGIN;
	sql_command_flags[SQLCOM_TRUNCATE] |= CF_ALLOW_PROTOCOL_PLUGIN;
	sql_command_flags[SQLCOM_DROP_TABLE] |= CF_ALLOW_PROTOCOL_PLUGIN;
	sql_command_flags[SQLCOM_DROP_INDEX] |= CF_ALLOW_PROTOCOL_PLUGIN;
	sql_command_flags[SQLCOM_SHOW_DATABASES] |= CF_ALLOW_PROTOCOL_PLUGIN;
	sql_command_flags[SQLCOM_SHOW_TABLES] |= CF_ALLOW_PROTOCOL_PLUGIN;
	sql_command_flags[SQLCOM_SHOW_FIELDS] |= CF_ALLOW_PROTOCOL_PLUGIN;
	sql_command_flags[SQLCOM_SHOW_KEYS] |= CF_ALLOW_PROTOCOL_PLUGIN;
	sql_command_flags[SQLCOM_SHOW_VARIABLES] |= CF_ALLOW_PROTOCOL_PLUGIN;
	sql_command_flags[SQLCOM_SHOW_STATUS] |= CF_ALLOW_PROTOCOL_PLUGIN;
	sql_command_flags[SQLCOM_SHOW_ENGINE_LOGS] |= CF_ALLOW_PROTOCOL_PLUGIN;
	sql_command_flags[SQLCOM_SHOW_ENGINE_STATUS] |= CF_ALLOW_PROTOCOL_PLUGIN;
	sql_command_flags[SQLCOM_SHOW_ENGINE_MUTEX] |= CF_ALLOW_PROTOCOL_PLUGIN;
	sql_command_flags[SQLCOM_SHOW_PROCESSLIST] |= CF_ALLOW_PROTOCOL_PLUGIN;
	sql_command_flags[SQLCOM_SHOW_MASTER_STAT] |= CF_ALLOW_PROTOCOL_PLUGIN;
	sql_command_flags[SQLCOM_SHOW_SLAVE_STAT] |= CF_ALLOW_PROTOCOL_PLUGIN;
	sql_command_flags[SQLCOM_SHOW_GRANTS] |= CF_ALLOW_PROTOCOL_PLUGIN;
	sql_command_flags[SQLCOM_SHOW_CREATE] |= CF_ALLOW_PROTOCOL_PLUGIN;
	sql_command_flags[SQLCOM_SHOW_CHARSETS] |= CF_ALLOW_PROTOCOL_PLUGIN;
	sql_command_flags[SQLCOM_SHOW_COLLATIONS] |= CF_ALLOW_PROTOCOL_PLUGIN;
	sql_command_flags[SQLCOM_SHOW_CREATE_DB] |= CF_ALLOW_PROTOCOL_PLUGIN;
	sql_command_flags[SQLCOM_SHOW_TABLE_STATUS] |= CF_ALLOW_PROTOCOL_PLUGIN;
	sql_command_flags[SQLCOM_SHOW_TRIGGERS] |= CF_ALLOW_PROTOCOL_PLUGIN;
	sql_command_flags[SQLCOM_LOAD] |= CF_ALLOW_PROTOCOL_PLUGIN;
	sql_command_flags[SQLCOM_SET_OPTION] |= CF_ALLOW_PROTOCOL_PLUGIN;
	sql_command_flags[SQLCOM_LOCK_TABLES] |= CF_ALLOW_PROTOCOL_PLUGIN;
	sql_command_flags[SQLCOM_UNLOCK_TABLES] |= CF_ALLOW_PROTOCOL_PLUGIN;
	sql_command_flags[SQLCOM_GRANT] |= CF_ALLOW_PROTOCOL_PLUGIN;
	sql_command_flags[SQLCOM_CHANGE_DB] |= CF_ALLOW_PROTOCOL_PLUGIN;
	sql_command_flags[SQLCOM_CREATE_DB] |= CF_ALLOW_PROTOCOL_PLUGIN;
	sql_command_flags[SQLCOM_DROP_DB] |= CF_ALLOW_PROTOCOL_PLUGIN;
	sql_command_flags[SQLCOM_ALTER_DB] |= CF_ALLOW_PROTOCOL_PLUGIN;
	sql_command_flags[SQLCOM_REPAIR] |= CF_ALLOW_PROTOCOL_PLUGIN;
	sql_command_flags[SQLCOM_REPLACE] |= CF_ALLOW_PROTOCOL_PLUGIN;
	sql_command_flags[SQLCOM_REPLACE_SELECT] |= CF_ALLOW_PROTOCOL_PLUGIN;
	sql_command_flags[SQLCOM_CREATE_FUNCTION] |= CF_ALLOW_PROTOCOL_PLUGIN;
	sql_command_flags[SQLCOM_DROP_FUNCTION] |= CF_ALLOW_PROTOCOL_PLUGIN;
	sql_command_flags[SQLCOM_REVOKE] |= CF_ALLOW_PROTOCOL_PLUGIN;
	sql_command_flags[SQLCOM_OPTIMIZE] |= CF_ALLOW_PROTOCOL_PLUGIN;
	sql_command_flags[SQLCOM_CHECK] |= CF_ALLOW_PROTOCOL_PLUGIN;
	sql_command_flags[SQLCOM_ASSIGN_TO_KEYCACHE] |= CF_ALLOW_PROTOCOL_PLUGIN;
	sql_command_flags[SQLCOM_PRELOAD_KEYS] |= CF_ALLOW_PROTOCOL_PLUGIN;
	sql_command_flags[SQLCOM_FLUSH] |= CF_ALLOW_PROTOCOL_PLUGIN;
	sql_command_flags[SQLCOM_KILL] |= CF_ALLOW_PROTOCOL_PLUGIN;
	sql_command_flags[SQLCOM_ANALYZE] |= CF_ALLOW_PROTOCOL_PLUGIN;
	sql_command_flags[SQLCOM_ROLLBACK] |= CF_ALLOW_PROTOCOL_PLUGIN;
	sql_command_flags[SQLCOM_ROLLBACK_TO_SAVEPOINT] |= CF_ALLOW_PROTOCOL_PLUGIN;
	sql_command_flags[SQLCOM_COMMIT] |= CF_ALLOW_PROTOCOL_PLUGIN;
	sql_command_flags[SQLCOM_SAVEPOINT] |= CF_ALLOW_PROTOCOL_PLUGIN;
	sql_command_flags[SQLCOM_RELEASE_SAVEPOINT] |= CF_ALLOW_PROTOCOL_PLUGIN;
	sql_command_flags[SQLCOM_SLAVE_START] |= CF_ALLOW_PROTOCOL_PLUGIN;
	sql_command_flags[SQLCOM_SLAVE_STOP] |= CF_ALLOW_PROTOCOL_PLUGIN;
	sql_command_flags[SQLCOM_BEGIN] |= CF_ALLOW_PROTOCOL_PLUGIN;
	sql_command_flags[SQLCOM_CHANGE_MASTER] |= CF_ALLOW_PROTOCOL_PLUGIN;
	sql_command_flags[SQLCOM_CHANGE_REPLICATION_FILTER] |= CF_ALLOW_PROTOCOL_PLUGIN;
	sql_command_flags[SQLCOM_RENAME_TABLE] |= CF_ALLOW_PROTOCOL_PLUGIN;
	sql_command_flags[SQLCOM_RESET] |= CF_ALLOW_PROTOCOL_PLUGIN;
	sql_command_flags[SQLCOM_PURGE] |= CF_ALLOW_PROTOCOL_PLUGIN;
	sql_command_flags[SQLCOM_PURGE_BEFORE] |= CF_ALLOW_PROTOCOL_PLUGIN;
	sql_command_flags[SQLCOM_SHOW_BINLOGS] |= CF_ALLOW_PROTOCOL_PLUGIN;
	sql_command_flags[SQLCOM_SHOW_OPEN_TABLES] |= CF_ALLOW_PROTOCOL_PLUGIN;
	sql_command_flags[SQLCOM_HA_OPEN] |= CF_ALLOW_PROTOCOL_PLUGIN;
	sql_command_flags[SQLCOM_HA_CLOSE] |= CF_ALLOW_PROTOCOL_PLUGIN;
	sql_command_flags[SQLCOM_HA_READ] |= CF_ALLOW_PROTOCOL_PLUGIN;
	sql_command_flags[SQLCOM_SHOW_SLAVE_HOSTS] |= CF_ALLOW_PROTOCOL_PLUGIN;
	sql_command_flags[SQLCOM_DELETE_MULTI] |= CF_ALLOW_PROTOCOL_PLUGIN;
	sql_command_flags[SQLCOM_UPDATE_MULTI] |= CF_ALLOW_PROTOCOL_PLUGIN;
	sql_command_flags[SQLCOM_SHOW_BINLOG_EVENTS] |= CF_ALLOW_PROTOCOL_PLUGIN;
	sql_command_flags[SQLCOM_DO] |= CF_ALLOW_PROTOCOL_PLUGIN;
	sql_command_flags[SQLCOM_SHOW_WARNS] |= CF_ALLOW_PROTOCOL_PLUGIN;
	sql_command_flags[SQLCOM_EMPTY_QUERY] |= CF_ALLOW_PROTOCOL_PLUGIN;
	sql_command_flags[SQLCOM_SHOW_ERRORS] |= CF_ALLOW_PROTOCOL_PLUGIN;
	sql_command_flags[SQLCOM_SHOW_STORAGE_ENGINES] |= CF_ALLOW_PROTOCOL_PLUGIN;
	sql_command_flags[SQLCOM_SHOW_PRIVILEGES] |= CF_ALLOW_PROTOCOL_PLUGIN;
	sql_command_flags[SQLCOM_HELP] |= CF_ALLOW_PROTOCOL_PLUGIN;
	sql_command_flags[SQLCOM_CREATE_USER] |= CF_ALLOW_PROTOCOL_PLUGIN;
	sql_command_flags[SQLCOM_DROP_USER] |= CF_ALLOW_PROTOCOL_PLUGIN;
	sql_command_flags[SQLCOM_RENAME_USER] |= CF_ALLOW_PROTOCOL_PLUGIN;
	sql_command_flags[SQLCOM_REVOKE_ALL] |= CF_ALLOW_PROTOCOL_PLUGIN;
	sql_command_flags[SQLCOM_CHECKSUM] |= CF_ALLOW_PROTOCOL_PLUGIN;
	sql_command_flags[SQLCOM_CREATE_PROCEDURE] |= CF_ALLOW_PROTOCOL_PLUGIN;
	sql_command_flags[SQLCOM_CREATE_SPFUNCTION] |= CF_ALLOW_PROTOCOL_PLUGIN;
	sql_command_flags[SQLCOM_CALL] |= CF_ALLOW_PROTOCOL_PLUGIN;
	sql_command_flags[SQLCOM_DROP_PROCEDURE] |= CF_ALLOW_PROTOCOL_PLUGIN;
	sql_command_flags[SQLCOM_ALTER_PROCEDURE] |= CF_ALLOW_PROTOCOL_PLUGIN;
	sql_command_flags[SQLCOM_ALTER_FUNCTION] |= CF_ALLOW_PROTOCOL_PLUGIN;
	sql_command_flags[SQLCOM_SHOW_CREATE_PROC] |= CF_ALLOW_PROTOCOL_PLUGIN;
	sql_command_flags[SQLCOM_SHOW_CREATE_FUNC] |= CF_ALLOW_PROTOCOL_PLUGIN;
	sql_command_flags[SQLCOM_SHOW_STATUS_PROC] |= CF_ALLOW_PROTOCOL_PLUGIN;
	sql_command_flags[SQLCOM_SHOW_STATUS_FUNC] |= CF_ALLOW_PROTOCOL_PLUGIN;
	sql_command_flags[SQLCOM_PREPARE] |= CF_ALLOW_PROTOCOL_PLUGIN;
	sql_command_flags[SQLCOM_EXECUTE] |= CF_ALLOW_PROTOCOL_PLUGIN;
	sql_command_flags[SQLCOM_DEALLOCATE_PREPARE] |= CF_ALLOW_PROTOCOL_PLUGIN;
	sql_command_flags[SQLCOM_CREATE_VIEW] |= CF_ALLOW_PROTOCOL_PLUGIN;
	sql_command_flags[SQLCOM_DROP_VIEW] |= CF_ALLOW_PROTOCOL_PLUGIN;
	sql_command_flags[SQLCOM_CREATE_TRIGGER] |= CF_ALLOW_PROTOCOL_PLUGIN;
	sql_command_flags[SQLCOM_DROP_TRIGGER] |= CF_ALLOW_PROTOCOL_PLUGIN;
	sql_command_flags[SQLCOM_XA_START] |= CF_ALLOW_PROTOCOL_PLUGIN;
	sql_command_flags[SQLCOM_XA_END] |= CF_ALLOW_PROTOCOL_PLUGIN;
	sql_command_flags[SQLCOM_XA_PREPARE] |= CF_ALLOW_PROTOCOL_PLUGIN;
	sql_command_flags[SQLCOM_XA_COMMIT] |= CF_ALLOW_PROTOCOL_PLUGIN;
	sql_command_flags[SQLCOM_XA_ROLLBACK] |= CF_ALLOW_PROTOCOL_PLUGIN;
	sql_command_flags[SQLCOM_XA_RECOVER] |= CF_ALLOW_PROTOCOL_PLUGIN;
	sql_command_flags[SQLCOM_SHOW_PROC_CODE] |= CF_ALLOW_PROTOCOL_PLUGIN;
	sql_command_flags[SQLCOM_SHOW_FUNC_CODE] |= CF_ALLOW_PROTOCOL_PLUGIN;
	sql_command_flags[SQLCOM_ALTER_TABLESPACE] |= CF_ALLOW_PROTOCOL_PLUGIN;
	sql_command_flags[SQLCOM_BINLOG_BASE64_EVENT] |= CF_ALLOW_PROTOCOL_PLUGIN;
	sql_command_flags[SQLCOM_SHOW_PLUGINS] |= CF_ALLOW_PROTOCOL_PLUGIN;
	sql_command_flags[SQLCOM_CREATE_SERVER] |= CF_ALLOW_PROTOCOL_PLUGIN;
	sql_command_flags[SQLCOM_DROP_SERVER] |= CF_ALLOW_PROTOCOL_PLUGIN;
	sql_command_flags[SQLCOM_ALTER_SERVER] |= CF_ALLOW_PROTOCOL_PLUGIN;
	sql_command_flags[SQLCOM_CREATE_EVENT] |= CF_ALLOW_PROTOCOL_PLUGIN;
	sql_command_flags[SQLCOM_ALTER_EVENT] |= CF_ALLOW_PROTOCOL_PLUGIN;
	sql_command_flags[SQLCOM_DROP_EVENT] |= CF_ALLOW_PROTOCOL_PLUGIN;
	sql_command_flags[SQLCOM_SHOW_CREATE_EVENT] |= CF_ALLOW_PROTOCOL_PLUGIN;
	sql_command_flags[SQLCOM_SHOW_EVENTS] |= CF_ALLOW_PROTOCOL_PLUGIN;
	sql_command_flags[SQLCOM_SHOW_CREATE_TRIGGER] |= CF_ALLOW_PROTOCOL_PLUGIN;
	sql_command_flags[SQLCOM_ALTER_DB_UPGRADE] |= CF_ALLOW_PROTOCOL_PLUGIN;
	sql_command_flags[SQLCOM_SHOW_PROFILE] |= CF_ALLOW_PROTOCOL_PLUGIN;
	sql_command_flags[SQLCOM_SHOW_PROFILES] |= CF_ALLOW_PROTOCOL_PLUGIN;
	sql_command_flags[SQLCOM_SIGNAL] |= CF_ALLOW_PROTOCOL_PLUGIN;
	sql_command_flags[SQLCOM_RESIGNAL] |= CF_ALLOW_PROTOCOL_PLUGIN;
	sql_command_flags[SQLCOM_SHOW_RELAYLOG_EVENTS] |= CF_ALLOW_PROTOCOL_PLUGIN;
	sql_command_flags[SQLCOM_GET_DIAGNOSTICS] |= CF_ALLOW_PROTOCOL_PLUGIN;
	sql_command_flags[SQLCOM_ALTER_USER] |= CF_ALLOW_PROTOCOL_PLUGIN;
	sql_command_flags[SQLCOM_EXPLAIN_OTHER] |= CF_ALLOW_PROTOCOL_PLUGIN;
	sql_command_flags[SQLCOM_SHOW_CREATE_USER] |= CF_ALLOW_PROTOCOL_PLUGIN;
	sql_command_flags[SQLCOM_END] |= CF_ALLOW_PROTOCOL_PLUGIN;
}


#endif
