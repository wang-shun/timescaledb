/*
 * Copyright (c) 2018  Timescale, Inc. All Rights Reserved.
 *
 * This file is licensed under the Timescale License,
 * see LICENSE-TIMESCALE at the top of the tsl directory.
 */
#include <postgres.h>
#include <access/xact.h>
#include <access/htup_details.h>
#include <foreign/foreign.h>
#include <nodes/makefuncs.h>
#include <nodes/parsenodes.h>
#include <catalog/pg_foreign_server.h>
#include <commands/dbcommands.h>
#include <commands/defrem.h>
#include <utils/builtins.h>
#include <miscadmin.h>
#include <funcapi.h>

#include "server.h"
#include "compat.h"
#include "catalog.h"

#define TS_FOREIGN_DATA_WRAPPER_NAME "postgres_fdw"
#define TS_DEFAULT_POSTGRES_PORT 5432
#define TS_DEFAULT_POSTGRES_HOST "localhost"

/*
 * Create a user mapping.
 *
 * Returns the OID of the created user mapping.
 */
static Oid
create_user_mapping(const char *username, const char *server_username, const char *servername,
					bool if_not_exists)
{
	ObjectAddress objaddr;
	RoleSpec rolespec = {
		.type = T_RoleSpec,
		.roletype = ROLESPEC_CSTRING,
		.rolename = (char *) username,
		.location = -1,
	};
	CreateUserMappingStmt stmt = {
		.type = T_CreateUserMappingStmt,
#if PG96
		.user = (Node *) &rolespec,
#else
		.user = &rolespec,
		.if_not_exists = if_not_exists,
#endif
		.servername = (char *) servername,
		.options = list_make1(
			makeDefElemCompat("user", (Node *) makeString(pstrdup(server_username)), -1)),
	};

	objaddr = CreateUserMapping(&stmt);

	return objaddr.objectId;
}

/*
 * Create a foreign server.
 *
 * Returns the OID of the created foreign server.
 */
static Oid
create_foreign_server(const char *servername, const char *host, int32 port, const char *dbname,
					  bool if_not_exists)
{
	ObjectAddress objaddr;
	CreateForeignServerStmt stmt = {
		.type = T_CreateForeignServerStmt,
		.servername = (char *) servername,
		.fdwname = TS_FOREIGN_DATA_WRAPPER_NAME,
		.options =
			list_make3(makeDefElemCompat("host", (Node *) makeString(pstrdup(host)), -1),
					   makeDefElemCompat("port", (Node *) makeInteger(port), -1),
					   makeDefElemCompat("dbname", (Node *) makeString(pstrdup(dbname)), -1)),
#if !PG96
		.if_not_exists = if_not_exists,
#endif
	};

	if (NULL == host)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 (errmsg("invalid host"),
				  (errhint("A hostname or IP address must be specified when "
						   "a foreign server does not already exist.")))));

	objaddr = CreateForeignServer(&stmt);

	return objaddr.objectId;
}

/* Attribute numbers for datum returned by create_server() */
enum Anum_create_server
{
	Anum_create_server_name = 1,
	Anum_create_server_host,
	Anum_create_server_port,
	Anum_create_server_dbname,
	Anum_create_server_user,
	Anum_create_server_server_user,
	Anum_create_server_created,
	_Anum_create_server_max,
};

#define Natts_create_server (_Anum_create_server_max - 1)

static Datum
create_server_datum(FunctionCallInfo fcinfo, const char *servername, const char *host, int32 port,
					const char *dbname, const char *username, const char *server_username,
					bool created)
{
	TupleDesc tupdesc;
	Datum values[Natts_create_server];
	bool nulls[Natts_create_server] = { false };
	HeapTuple tuple;

	if (get_call_result_type(fcinfo, NULL, &tupdesc) != TYPEFUNC_COMPOSITE)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("function returning record called in "
						"context that cannot accept type record")));

	tupdesc = BlessTupleDesc(tupdesc);
	values[AttrNumberGetAttrOffset(Anum_create_server_name)] = CStringGetDatum(servername);
	values[AttrNumberGetAttrOffset(Anum_create_server_host)] = CStringGetTextDatum(host);
	values[AttrNumberGetAttrOffset(Anum_create_server_port)] = Int32GetDatum(port);
	values[AttrNumberGetAttrOffset(Anum_create_server_dbname)] = CStringGetDatum(dbname);
	values[AttrNumberGetAttrOffset(Anum_create_server_user)] = CStringGetDatum(username);
	values[AttrNumberGetAttrOffset(Anum_create_server_server_user)] =
		CStringGetDatum(server_username);
	values[AttrNumberGetAttrOffset(Anum_create_server_created)] = BoolGetDatum(created);
	tuple = heap_form_tuple(tupdesc, values, nulls);

	return HeapTupleGetDatum(tuple);
}

static UserMapping *
get_user_mapping(Oid userid, Oid serverid)
{
	UserMapping *um;

	PG_TRY();
	{
		um = GetUserMapping(userid, serverid);
	}
	PG_CATCH();
	{
		um = NULL;
		FlushErrorState();
	}
	PG_END_TRY();

	return um;
}

Datum
server_add(PG_FUNCTION_ARGS)
{
	const char *servername = PG_ARGISNULL(0) ? NULL : PG_GETARG_CSTRING(0);
	const char *host =
		PG_ARGISNULL(1) ? TS_DEFAULT_POSTGRES_HOST : TextDatumGetCString(PG_GETARG_DATUM(1));
	const char *dbname = PG_ARGISNULL(2) ? get_database_name(MyDatabaseId) : PG_GETARG_CSTRING(2);
	int32 port = PG_ARGISNULL(3) ? TS_DEFAULT_POSTGRES_PORT : PG_GETARG_INT32(3);
	Oid userid = PG_ARGISNULL(4) ? GetUserId() : PG_GETARG_OID(4);
	const char *server_username =
		PG_ARGISNULL(5) ? GetUserNameFromId(userid, false) : PG_GETARG_CSTRING(5);
	bool if_not_exists = PG_ARGISNULL(6) ? false : PG_GETARG_BOOL(6);
	ForeignServer *server;
	UserMapping *um;
	const char *username;
	Oid serverid = InvalidOid;
	bool created = false;

	if (NULL == servername)
		ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE), (errmsg("invalid server name"))));

	if (port < 1 || port > PG_UINT16_MAX)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 (errmsg("invalid port"),
				  errhint("The port number must be between 1 and %u", PG_UINT16_MAX))));

	/*
	 * First check for existing foreign server. We could rely on
	 * if_not_exists, but it is not supported in PostgreSQL 9.6 for foreign
	 * servers or user mappings. We still pass use this argument in the create
	 * statement for newer versions in case we drop support 9.6 in the future.
	 */
	server = GetForeignServerByName(servername, true);

	if (NULL == server)
	{
		serverid = create_foreign_server(servername, host, port, dbname, if_not_exists);
		created = true;
	}
	else if (if_not_exists)
		serverid = server->serverid;
	else
		ereport(ERROR,
				(errcode(ERRCODE_DUPLICATE_OBJECT),
				 errmsg("server \"%s\" already exists", servername)));

	/*
	 * Make the foreign server visible in current transaction so that we can
	 * reference it when adding the user mapping
	 */
	CommandCounterIncrement();

	username = GetUserNameFromId(userid, false);

	um = get_user_mapping(userid, serverid);

	if (NULL == um)
	{
		if (!created)
			elog(NOTICE, "adding user mapping for \"%s\" to server \"%s\"", username, servername);

		create_user_mapping(username, server_username, servername, if_not_exists);

		/* Make user mapping visible */
		CommandCounterIncrement();

		um = GetUserMapping(userid, serverid);
	}
	else if (!if_not_exists)
		ereport(ERROR,
				(errcode(ERRCODE_DUPLICATE_OBJECT),
				 errmsg("user mapping for user \"%s\" and server \"%s\" already exists",
						username,
						servername)));

	PG_RETURN_DATUM(create_server_datum(fcinfo,
										servername,
										host,
										port,
										dbname,
										username,
										server_username,
										created));
}

Datum
server_delete(PG_FUNCTION_ARGS)
{
	const char *servername = PG_ARGISNULL(0) ? NULL : PG_GETARG_CSTRING(0);
	bool if_exists = PG_ARGISNULL(1) ? false : PG_GETARG_BOOL(1);
	bool cascade = PG_ARGISNULL(2) ? false : PG_GETARG_BOOL(2);
	ForeignServer *server = GetForeignServerByName(servername, if_exists);
	bool deleted = false;

	if (NULL != server)
	{
		DropStmt stmt = {
			.type = T_DropStmt,
#if PG96
			.objects = list_make1(list_make1(makeString(pstrdup(servername)))),
#else
			.objects = list_make1(makeString(pstrdup(servername))),
#endif
			.removeType = OBJECT_FOREIGN_SERVER,
			.behavior = cascade ? DROP_CASCADE : DROP_RESTRICT,
			.missing_ok = if_exists,
		};

		RemoveObjects(&stmt);

		deleted = true;
	}

	PG_RETURN_BOOL(deleted);
}

List *
server_get_servername_list(void)
{
	HeapTuple tuple;
	ScanKeyData scankey[1];
	SysScanDesc scandesc;
	Relation rel;
	ForeignDataWrapper *fdw = GetForeignDataWrapperByName(TS_FOREIGN_DATA_WRAPPER_NAME, false);
	List *servers = NIL;

	rel = heap_open(ForeignServerRelationId, AccessShareLock);

	ScanKeyInit(&scankey[0],
				Anum_pg_foreign_server_srvfdw,
				BTEqualStrategyNumber,
				F_OIDEQ,
				ObjectIdGetDatum(fdw->fdwid));

	scandesc = systable_beginscan(rel, InvalidOid, false, NULL, 1, scankey);

	while (HeapTupleIsValid(tuple = systable_getnext(scandesc)))
	{
		Form_pg_foreign_server form = (Form_pg_foreign_server) GETSTRUCT(tuple);

		servers = lappend(servers, pstrdup(NameStr(form->srvname)));
	}

	systable_endscan(scandesc);
	heap_close(rel, AccessShareLock);

	return servers;
}
