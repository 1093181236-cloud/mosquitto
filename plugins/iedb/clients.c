/*
Copyright (c) 2020 Roger Light <roger@atchoo.org>

All rights reserved. This program and the accompanying materials
are made available under the terms of the Eclipse Public License 2.0
and Eclipse Distribution License v1.0 which accompany this distribution.

The Eclipse Public License is available at
   https://www.eclipse.org/legal/epl-2.0/
and the Eclipse Distribution License is available at
  http://www.eclipse.org/org/documents/edl-v10.php.

SPDX-License-Identifier: EPL-2.0 OR BSD-3-Clause

Contributors:
   Roger Light - initial implementation and documentation.
*/
#include "config.h"
#include "util.h"

#include <cjson/cJSON.h>
#include <stdio.h>
#include <uthash.h>
#include <sqlite3.h>
#include "mosquitto.h"
#include "mosquitto_broker.h"
#include "json_help.h"

#include "dynamic_security.h"

/* ################################################################
 * #
 * # Function declarations
 * #
 * ################################################################ */

static int dynsec__remove_client_from_all_groups(const char *username);
static void client__remove_all_roles(struct dynsec__client *client);
int dynsec_groups__remove_client(const char *username, const char *groupname, bool update_config);
/* ################################################################
 * #
 * # Local variables
 * #
 * ################################################################ */

static struct dynsec__client *local_clients = NULL;
extern sqlite3 *db;
unsigned long long max_user_id;

/* ################################################################
 * #
 * # Utility functions
 * #
 * ################################################################ */

static int client_cmp(void *a, void *b)
{
	struct dynsec__client *client_a = a;
	struct dynsec__client *client_b = b;

	return strcmp(client_a->username, client_b->username);
}

struct dynsec__client *dynsec_clients__find(const char *username)
{
	struct dynsec__client *client = NULL;

	if(username){
		HASH_FIND(hh, local_clients, username, strlen(username), client);
	}
	return client;
}


static void client__free_item(struct dynsec__client *client)
{
	struct dynsec__client *client_found;
	if(client == NULL) return;

	client_found = dynsec_clients__find(client->username);
	if(client_found){
		HASH_DEL(local_clients, client_found);
	}
	dynsec_rolelist__cleanup(&client->rolelist);
	dynsec__remove_client_from_all_groups(client->username);
	mosquitto_free(client->text_name);
	mosquitto_free(client->text_description);
	mosquitto_free(client->clientid);
	mosquitto_free(client->username);
	mosquitto_free(client);
}

void dynsec_clients__cleanup(void)
{
	struct dynsec__client *client, *client_tmp;

	HASH_ITER(hh, local_clients, client, client_tmp){
		client__free_item(client);
	}
}

/* ################################################################
 * #
 * # db function
 * #
 * ################################################################ */
int select_user_callback(void *data, int argc, char **argv, char **azColName){
	UNUSED(data);
	UNUSED(argc);
	UNUSED(azColName);
	unsigned long long id = 0;
	if(string2ull(argv[0],&id) == 0)
		return MOSQ_ERR_INVAL;

	char* username = argv[1];
	char* clientid = argv[2];
	char* text_name = argv[3];
	char* text_description = argv[4];
	int disabled = argv[5][0] == '1'?1:0;

	struct dynsec__client *client = dynsec_clients__find(username);
	if(client){
		return 0;
	}

	client = mosquitto_calloc(1, sizeof(struct dynsec__client));
	if(client == NULL){
		return MOSQ_ERR_NOMEM;
	}
	client->id = id;
	client->disabled = disabled;

	if(id > max_user_id)
		max_user_id = id;

	client->username = mosquitto_strdup(username);
	if(client->username == NULL){
		mosquitto_free(client);
		return 0;
	}

	if(text_name){
		client->text_name = mosquitto_strdup(text_name);
		if(client->text_name == NULL){
			mosquitto_free(client->username);
			mosquitto_free(client);
			return 0;
		}
	}
	if(text_description){
		client->text_description = mosquitto_strdup(text_description);
		if(client->text_description == NULL){
			mosquitto_free(client->text_name);
			mosquitto_free(client->username);
			mosquitto_free(client);
			return 0;
		}
	}
	if(clientid){
		client->clientid = mosquitto_strdup(clientid);
		if(client->clientid == NULL){
			mosquitto_free(client->text_description);
			mosquitto_free(client->text_name);
			mosquitto_free(client->username);
			mosquitto_free(client);
			return 0;
		}
	}

	HASH_ADD_KEYPTR(hh, local_clients, client->username, strlen(client->username), client);
	return 0;
}

int select_user_role_callback(void *data, int argc, char **argv, char **azColName){
	UNUSED(argc);
	UNUSED(azColName);
	long long priority = 0;
	if(string2ll(argv[1],strlen(argv[1]),&priority) == 0)
		return MOSQ_ERR_INVAL;

	struct dynsec__client *client = (struct dynsec__client *)data;
	struct dynsec__role *role;

	role = dynsec_roles__find(argv[0]);
	dynsec_rolelist__client_add(client, role, (int)priority);

	return 0;
}

int select_user_group_callback(void *data, int argc, char **argv, char **azColName){
	UNUSED(argc);
	UNUSED(azColName);
	long long priority = 0;
	if(string2ll(argv[1],strlen(argv[1]),&priority) == 0)
		return MOSQ_ERR_INVAL;
	char* groupname = argv[0];

	struct dynsec__client *client = (struct dynsec__client *)data;
	dynsec_groups__add_client(client->username, groupname, (int)priority, false);

	return 0;
}

int db_client_init(void){
	char *zErrMsg = 0;
	int  rc;
	char* sql = "CREATE TABLE IF NOT EXISTS users("  \
	         "id INTEGER PRIMARY KEY," \
			 "text_name TEXT," \
			 "clientid TEXT," \
			 "text_description TEXT," \
			 "disabled BOOLEAN," \
	         "username TEXT NOT NULL UNIQUE);";

	rc = sqlite3_exec(db, sql, NULL, 0, &zErrMsg);
	if( rc != SQLITE_OK ){
		mosquitto_log_printf(MOSQ_LOG_ERR, "CREATE TABLE user,SQL error: %s\n", zErrMsg);
		sqlite3_free(zErrMsg);
		return rc;
	}

	sql = "CREATE TABLE IF NOT EXISTS  user_group("  \
	         "uid INTEGER    NOT NULL," \
			 "gid INTEGER    NOT NULL," \
			 "priority INTEGER," \
			 "PRIMARY KEY (uid,gid)," \
			 "FOREIGN KEY(uid) REFERENCES users(id)," \
			 "FOREIGN KEY(gid) REFERENCES groups(id))WITHOUT ROWID;";

	rc = sqlite3_exec(db, sql, NULL, 0, &zErrMsg);
	if( rc != SQLITE_OK ){
		mosquitto_log_printf(MOSQ_LOG_ERR, "CREATE TABLE user_group,SQL error: %s\n", zErrMsg);
		sqlite3_free(zErrMsg);
		return rc;
	}

	sql = "CREATE TABLE IF NOT EXISTS  user_role("  \
	         "uid INTEGER    NOT NULL," \
			 "rid INTEGER    NOT NULL," \
			 "priority INTEGER," \
			 "PRIMARY KEY (uid,rid)," \
			 "FOREIGN KEY(uid) REFERENCES users(id)," \
			 "FOREIGN KEY(rid) REFERENCES roles(id))WITHOUT ROWID;";

	rc = sqlite3_exec(db, sql, NULL, 0, &zErrMsg);
	if( rc != SQLITE_OK ){
		mosquitto_log_printf(MOSQ_LOG_ERR, "CREATE TABLE user_role,SQL error: %s\n", zErrMsg);
		sqlite3_free(zErrMsg);
		return rc;
	}

	rc = sqlite3_exec(db, "SELECT id,username,clientid,text_name,text_description,disabled FROM users", select_user_callback, NULL, &zErrMsg);
	if( rc != SQLITE_OK ){
		mosquitto_log_printf(MOSQ_LOG_ERR, "SELECT user,SQL error: %s\n", zErrMsg);
		sqlite3_free(zErrMsg);
		return rc;
	}
	HASH_SORT(local_clients, client_cmp);

	struct dynsec__client *client, *client_tmp;
	HASH_ITER(hh, local_clients, client, client_tmp){
		char buf[128];
		snprintf(buf,128,"SELECT rolename,priority FROM user_role INNER JOIN roles ON roles.id=user_role.rid WHERE user_role.uid=%lld",client->id);
		rc = sqlite3_exec(db, buf, select_user_role_callback, client, &zErrMsg);
		if( rc != SQLITE_OK ){
			mosquitto_log_printf(MOSQ_LOG_ERR, "SELECT user_role,SQL error: %s\n", zErrMsg);
			sqlite3_free(zErrMsg);
			return rc;
		}

		snprintf(buf,128,"SELECT groupname,priority FROM user_group INNER JOIN groups ON groups.id=user_group.gid WHERE user_group.uid=%lld",client->id);
		rc = sqlite3_exec(db, buf, select_user_group_callback, client, &zErrMsg);
		if( rc != SQLITE_OK ){
			mosquitto_log_printf(MOSQ_LOG_ERR, "SELECT user_group,SQL error: %s\n", zErrMsg);
			sqlite3_free(zErrMsg);
			return rc;
		}
	}
	return rc;
}

int db_insert_user(struct dynsec__client *client){
	char *zErrMsg = 0;
	int  rc;
	char sql[256];
	if(client->clientid == NULL && client->text_name == NULL&& client->text_description == NULL )
		snprintf(sql,255,"INSERT INTO users (id,username,disabled) VALUES (%lld,%s,%d)",client->id,client->username,client->disabled);
	else if(client->text_name == NULL&& client->text_description == NULL )
		snprintf(sql,255,"INSERT INTO users (id,username,disabled,clientid) VALUES (%lld,%s,%d,%s)",client->id,client->username,client->disabled,client->clientid);
	else if(client->text_name == NULL&& client->clientid == NULL )
		snprintf(sql,255,"INSERT INTO users (id,username,disabled,text_description) VALUES (%lld,%s,%d,%s)",client->id,client->username,client->disabled,client->text_description);
	else if(client->clientid == NULL&& client->text_description == NULL )
		snprintf(sql,255,"INSERT INTO users (id,username,disabled,text_name) VALUES (%lld,%s,%d,%s)",client->id,client->username,client->disabled,client->text_name);
	else if(client->clientid == NULL)
		snprintf(sql,255,"INSERT INTO users (id,username,disabled,text_name,text_description) VALUES (%lld,%s,%d,%s,%s)",client->id,client->username,client->disabled,client->text_name,client->text_description);
	else if(client->text_name == NULL)
		snprintf(sql,255,"INSERT INTO users (id,username,disabled,clientid,text_description) VALUES (%lld,%s,%d,%s,%s)",client->id,client->username,client->disabled,client->clientid,client->text_description);
	else if(client->text_description == NULL )
		snprintf(sql,255,"INSERT INTO users (id,username,disabled,clientid,text_name) VALUES (%lld,%s,%d,%s,%s)",client->id,client->username,client->disabled,client->clientid,client->text_name);
	else
		snprintf(sql,255,"INSERT INTO users (id,username,disabled,clientid,text_name,text_description) VALUES (%lld,%s,%d,%s,%s,%s)",client->id,client->username,client->disabled,client->clientid,client->text_name,client->text_description);
	rc = sqlite3_exec(db, sql, NULL, NULL, &zErrMsg);
	if( rc != SQLITE_OK ){
		mosquitto_log_printf(MOSQ_LOG_ERR, "db_insert_user SQL error: %s\n", zErrMsg);
		sqlite3_free(zErrMsg);
	}
	return rc;
}

int db_delete_user(unsigned long long uid){
	char *zErrMsg = 0;
	int  rc;
	char sql[256];
	snprintf(sql,255,"DELETE FROM users WHERE uid=%lld",uid);

	rc = sqlite3_exec(db, sql, NULL, NULL, &zErrMsg);
	if( rc != SQLITE_OK ){
		mosquitto_log_printf(MOSQ_LOG_ERR, "db_delete_user SQL error: %s\n", zErrMsg);
		sqlite3_free(zErrMsg);
	}
	return rc;
}

int db_disable_user(unsigned long long uid,bool disabled){
	char *zErrMsg = 0;
	int  rc;
	char sql[256];
	snprintf(sql,255,"UPDATE users SET disabled = %d WHERE id = %lld",disabled,uid);

	rc = sqlite3_exec(db, sql, NULL, NULL, &zErrMsg);
	if( rc != SQLITE_OK ){
		mosquitto_log_printf(MOSQ_LOG_ERR, "db_disable_user SQL error: %s\n", zErrMsg);
		sqlite3_free(zErrMsg);
	}
	return rc;
}

int db_set_user(unsigned long long uid,char* clientid,char* text_name,char* text_description){
	char *zErrMsg = 0;
	int  rc;
	char sql[256];
	if(clientid == NULL && text_name == NULL && text_description == NULL)
		return 0;
	else if(text_name == NULL && text_description == NULL)
		snprintf(sql,255,"UPDATE users SET clientid = %s WHERE id = %lld",clientid,uid);
	else if(clientid == NULL  && text_description == NULL)
		snprintf(sql,255,"UPDATE users SET text_name = %s WHERE id = %lld",text_name,uid);
	else if(clientid == NULL && text_name == NULL)
		snprintf(sql,255,"UPDATE users SET text_description = %s WHERE id = %lld",text_description,uid);

	else if(clientid == NULL)
		snprintf(sql,255,"UPDATE users SET text_name = %s, text_description = %s WHERE id = %lld",text_name,text_description,uid);
	else if(text_name == NULL)
		snprintf(sql,255,"UPDATE users SET clientid = %s, text_description = %s WHERE id = %lld",clientid,text_description,uid);
	else if(text_description == NULL)
		snprintf(sql,255,"UPDATE users SET clientid = %s, text_name = %s WHERE id = %lld",clientid,text_name,uid);
	else
		snprintf(sql,255,"UPDATE users SET clientid = %s, text_name = %s text_description = %s, WHERE id = %lld",clientid,text_name,text_description,uid);

	rc = sqlite3_exec(db, sql, NULL, NULL, &zErrMsg);
	if( rc != SQLITE_OK ){
		mosquitto_log_printf(MOSQ_LOG_ERR, "db_disable_user SQL error: %s\n", zErrMsg);
		sqlite3_free(zErrMsg);
	}
	return rc;
}

int db_insert_user_group(unsigned long long uid,unsigned long long gid,int priority){
	char *zErrMsg = 0;
	int  rc;
	char sql[256];
	snprintf(sql,255,"INSERT INTO user_group (uid,gid,priority) VALUES (%lld,%lld,%d)",uid,gid,priority);

	rc = sqlite3_exec(db, sql, NULL, NULL, &zErrMsg);
	if( rc != SQLITE_OK ){
		mosquitto_log_printf(MOSQ_LOG_ERR, "db_insert_user_group SQL error: %s\n", zErrMsg);
		sqlite3_free(zErrMsg);
	}
	return rc;
}

int db_delete_user_group(unsigned long long uid,unsigned long long gid){
	char *zErrMsg = 0;
	int  rc;
	char sql[256];
	snprintf(sql,255,"DELETE FROM user_group WHERE uid=%lld AND gid=%lld",uid,gid);

	rc = sqlite3_exec(db, sql, NULL, NULL, &zErrMsg);
	if( rc != SQLITE_OK ){
		mosquitto_log_printf(MOSQ_LOG_ERR, "db_delete_user_group SQL error: %s\n", zErrMsg);
		sqlite3_free(zErrMsg);
	}
	return rc;
}

int db_delete_user_group_by_group(unsigned long long gid){
	char *zErrMsg = 0;
	int  rc;
	char sql[256];
	snprintf(sql,255,"DELETE FROM user_group WHERE gid=%lld",gid);

	rc = sqlite3_exec(db, sql, NULL, NULL, &zErrMsg);
	if( rc != SQLITE_OK ){
		mosquitto_log_printf(MOSQ_LOG_ERR, "db_delete_user_group SQL error: %s\n", zErrMsg);
		sqlite3_free(zErrMsg);
	}
	return rc;
}

int db_insert_user_role(unsigned long long uid,unsigned long long rid,int priority){
	char *zErrMsg = 0;
	int  rc;
	char sql[256];
	snprintf(sql,255,"INSERT INTO user_role (uid,rid,priority) VALUES (%lld,%lld,%d)",uid,rid,priority);

	rc = sqlite3_exec(db, sql, NULL, NULL, &zErrMsg);
	if( rc != SQLITE_OK ){
		mosquitto_log_printf(MOSQ_LOG_ERR, "db_insert_user_role SQL error: %s\n", zErrMsg);
		sqlite3_free(zErrMsg);
	}
	return rc;
}

int db_delete_user_role(unsigned long long uid,unsigned long long rid){
	char *zErrMsg = 0;
	int  rc;
	char sql[256];
	snprintf(sql,255,"DELETE FROM user_role WHERE uid=%lld AND rid=%lld",uid,rid);

	rc = sqlite3_exec(db, sql, NULL, NULL, &zErrMsg);
	if( rc != SQLITE_OK ){
		mosquitto_log_printf(MOSQ_LOG_ERR, "db_delete_user_role SQL error: %s\n", zErrMsg);
		sqlite3_free(zErrMsg);
	}
	return rc;
}

int db_delete_user_role_by_role(unsigned long long rid){
	char *zErrMsg = 0;
	int  rc;
	char sql[256];
	snprintf(sql,255,"DELETE FROM user_role WHERE rid=%lld",rid);

	rc = sqlite3_exec(db, sql, NULL, NULL, &zErrMsg);
	if( rc != SQLITE_OK ){
		mosquitto_log_printf(MOSQ_LOG_ERR, "db_delete_user_role SQL error: %s\n", zErrMsg);
		sqlite3_free(zErrMsg);
	}
	return rc;
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
int dynsec_clients__process_create(cJSON *j_responses, cJSON *command)
{
	char *username, /**password,*/ *clientid = NULL;
	char *text_name, *text_description;
	struct dynsec__client *client;
	int rc;
	cJSON *j_groups, *j_group;
	int priority;

	if(json_get_string(command, "username", &username, false) != MOSQ_ERR_SUCCESS){
		cJSON_AddStringToObject(j_responses,"info","Invalid/missing username");
		return MOSQ_ERR_INVAL;
	}
	if(mosquitto_validate_utf8(username, (int)strlen(username)) != MOSQ_ERR_SUCCESS){
		cJSON_AddStringToObject(j_responses,"info","Username not valid UTF-8");
		return MOSQ_ERR_INVAL;
	}

/*	if(json_get_string(command, "password", &password, true) != MOSQ_ERR_SUCCESS){
		cJSON_AddStringToObject(j_responses,"info","Invalid/missing password");
		return MOSQ_ERR_INVAL;
	}*/

	if(json_get_string(command, "clientid", &clientid, true) != MOSQ_ERR_SUCCESS){
		cJSON_AddStringToObject(j_responses,"info","Invalid/missing client id");
		return MOSQ_ERR_INVAL;
	}
	if(clientid && mosquitto_validate_utf8(clientid, (int)strlen(clientid)) != MOSQ_ERR_SUCCESS){
		cJSON_AddStringToObject(j_responses,"info","Client ID not valid UTF-8");
		return MOSQ_ERR_INVAL;
	}


	if(json_get_string(command, "textname", &text_name, true) != MOSQ_ERR_SUCCESS){
		cJSON_AddStringToObject(j_responses,"info","Invalid/missing textname");
		return MOSQ_ERR_INVAL;
	}

	if(json_get_string(command, "textdescription", &text_description, true) != MOSQ_ERR_SUCCESS){
		cJSON_AddStringToObject(j_responses,"info","Invalid/missing textdescription");
		return MOSQ_ERR_INVAL;
	}

	client = dynsec_clients__find(username);
	if(client){
		cJSON_AddStringToObject(j_responses,"info","Client already exists");
		return MOSQ_ERR_SUCCESS;
	}

	client = mosquitto_calloc(1, sizeof(struct dynsec__client));
	if(client == NULL){
		cJSON_AddStringToObject(j_responses,"info","Internal error");
		return MOSQ_ERR_NOMEM;
	}
	client->username = mosquitto_strdup(username);
	if(client->username == NULL){
		cJSON_AddStringToObject(j_responses,"info","Internal error");
		client__free_item(client);
		return MOSQ_ERR_NOMEM;
	}
	if(text_name){
		client->text_name = mosquitto_strdup(text_name);
		if(client->text_name == NULL){
			cJSON_AddStringToObject(j_responses,"info","Internal error");
			client__free_item(client);
			return MOSQ_ERR_NOMEM;
		}
	}
	if(text_description){
		client->text_description = mosquitto_strdup(text_description);
		if(client->text_description == NULL){
			cJSON_AddStringToObject(j_responses,"info","Internal error");
			client__free_item(client);
			return MOSQ_ERR_NOMEM;
		}
	}

/*	if(password){
		if(dynsec_auth__pw_hash(client, password, client->pw.password_hash, sizeof(client->pw.password_hash), true)){
			cJSON_AddStringToObject(j_responses,"info","SUCCESS");
			dynsec__command_reply(j_responses, context, "createClient", "Internal error", correlation_data);
			client__free_item(client);
			return MOSQ_ERR_NOMEM;
		}
		client->pw.valid = true;
	}*/
	if(clientid && strlen(clientid) > 0){
		client->clientid = mosquitto_strdup(clientid);
		if(client->clientid == NULL){
			cJSON_AddStringToObject(j_responses,"info","Internal error");
			client__free_item(client);
			return MOSQ_ERR_NOMEM;
		}
	}

	rc = dynsec_rolelist__load_from_json(command, &client->rolelist);
	if(rc == MOSQ_ERR_SUCCESS || rc == ERR_LIST_NOT_FOUND){
	}else if(rc == MOSQ_ERR_NOT_FOUND){
		cJSON_AddStringToObject(j_responses,"info","Role not found");
		client__free_item(client);
		return MOSQ_ERR_INVAL;
	}else{
		if(rc == MOSQ_ERR_INVAL){
			cJSON_AddStringToObject(j_responses,"info","'roles' not an array or missing/invalid rolename");
		}else{
			cJSON_AddStringToObject(j_responses,"info","Internal error");
		}
		client__free_item(client);
		return MOSQ_ERR_INVAL;
	}

	/* Must add user before groups, otherwise adding groups will fail */
	HASH_ADD_KEYPTR_INORDER(hh, local_clients, client->username, strlen(client->username), client, client_cmp);

	j_groups = cJSON_GetObjectItem(command, "groups");
	if(j_groups && cJSON_IsArray(j_groups)){
		cJSON_ArrayForEach(j_group, j_groups){
			if(cJSON_IsObject(j_group)){
				char *groupname;
				json_get_string(j_group, "groupname", &groupname, false);
				if(groupname){
					json_get_int(j_group, "priority", &priority, true, -1);
					rc = dynsec_groups__add_client(username, groupname, priority, false);
					if(rc == ERR_GROUP_NOT_FOUND){
						cJSON_AddStringToObject(j_responses,"info","Group not found");
						client__free_item(client);
						return MOSQ_ERR_INVAL;
					}else if(rc != MOSQ_ERR_SUCCESS){
						cJSON_AddStringToObject(j_responses,"info","Internal error");
						client__free_item(client);
						return MOSQ_ERR_INVAL;
					}
				}
			}
		}
	}
	client->id = ++max_user_id;
	db_insert_user(client);
	struct dynsec__grouplist *grouplist, *grouplist_tmp = NULL;
	HASH_ITER(hh, client->grouplist, grouplist, grouplist_tmp){
		db_insert_user_group(client->id,grouplist->group->id,grouplist->priority);
	}

	struct dynsec__rolelist *rolelist, *rolelist_tmp;
	HASH_ITER(hh, client->rolelist, rolelist, rolelist_tmp){
		db_insert_user_role(client->id,rolelist->role->id,grouplist->priority);
	}

	cJSON_AddStringToObject(j_responses,"info","SUCCESS");
	mosquitto_log_printf(MOSQ_LOG_INFO, "dynsec: createClient | username=%s",username);

	return MOSQ_ERR_SUCCESS;
}


int dynsec_clients__process_delete(cJSON *j_responses, cJSON *command)
{
	char *username;
	struct dynsec__client *client;

	if(json_get_string(command, "username", &username, false) != MOSQ_ERR_SUCCESS){
		cJSON_AddStringToObject(j_responses,"info","Invalid/missing username");
		return MOSQ_ERR_INVAL;
	}

	client = dynsec_clients__find(username);
	if(client){
		struct dynsec__grouplist *grouplist, *grouplist_tmp = NULL;
		HASH_ITER(hh, client->grouplist, grouplist, grouplist_tmp){
			db_delete_user_group(client->id,grouplist->group->id);
		}

		struct dynsec__rolelist *rolelist, *rolelist_tmp;
		HASH_ITER(hh, client->rolelist, rolelist, rolelist_tmp){
			db_delete_user_role(client->id,rolelist->role->id);
		}
		db_delete_user(client->id);
		dynsec__remove_client_from_all_groups(username);
		client__remove_all_roles(client);
		client__free_item(client);

		/* Enforce any changes */
		mosquitto_kick_client_by_username(username, false);

		cJSON_AddStringToObject(j_responses,"info","SUCCESS");
		mosquitto_log_printf(MOSQ_LOG_INFO, "dynsec: deleteClient | username=%s",username);

		return MOSQ_ERR_SUCCESS;
	}else{
		cJSON_AddStringToObject(j_responses,"info","Client not found");
		return MOSQ_ERR_SUCCESS;
	}
}

int dynsec_clients__process_disable(cJSON *j_responses, cJSON *command)
{
	char *username;
	struct dynsec__client *client;

	if(json_get_string(command, "username", &username, false) != MOSQ_ERR_SUCCESS){
		cJSON_AddStringToObject(j_responses,"info","Invalid/missing username");
		return MOSQ_ERR_INVAL;
	}
	if(mosquitto_validate_utf8(username, (int)strlen(username)) != MOSQ_ERR_SUCCESS){
		cJSON_AddStringToObject(j_responses,"info","Username not valid UTF-8");
		return MOSQ_ERR_INVAL;
	}

	client = dynsec_clients__find(username);
	if(client == NULL){
		cJSON_AddStringToObject(j_responses,"info","Client not found");
		return MOSQ_ERR_SUCCESS;
	}

	client->disabled = true;

	mosquitto_kick_client_by_username(username, false);

	db_disable_user(client->id,client->disabled);
	cJSON_AddStringToObject(j_responses,"info","SUCCESS");
	mosquitto_log_printf(MOSQ_LOG_INFO, "dynsec: disableClient | username=%s",username);

	return MOSQ_ERR_SUCCESS;
}


int dynsec_clients__process_enable(cJSON *j_responses, cJSON *command)
{
	char *username;
	struct dynsec__client *client;

	if(json_get_string(command, "username", &username, false) != MOSQ_ERR_SUCCESS){
		cJSON_AddStringToObject(j_responses,"info","Invalid/missing username");
		return MOSQ_ERR_INVAL;
	}
	if(mosquitto_validate_utf8(username, (int)strlen(username)) != MOSQ_ERR_SUCCESS){
		cJSON_AddStringToObject(j_responses,"info","Username not valid UTF-8");
		return MOSQ_ERR_INVAL;
	}

	client = dynsec_clients__find(username);
	if(client == NULL){
		cJSON_AddStringToObject(j_responses,"info","Client not found");
		return MOSQ_ERR_SUCCESS;
	}

	client->disabled = false;

	db_disable_user(client->id,client->disabled);
	cJSON_AddStringToObject(j_responses,"info","SUCCESS");
	mosquitto_log_printf(MOSQ_LOG_INFO, "dynsec: enableClient | username=%s",username);

	return MOSQ_ERR_SUCCESS;
}


int dynsec_clients__process_set_id(cJSON *j_responses, cJSON *command)
{
	char *username, *clientid, *clientid_heap = NULL;
	struct dynsec__client *client;
	size_t slen;

	if(json_get_string(command, "username", &username, false) != MOSQ_ERR_SUCCESS){
		cJSON_AddStringToObject(j_responses,"info","Invalid/missing username");
		return MOSQ_ERR_INVAL;
	}
	if(mosquitto_validate_utf8(username, (int)strlen(username)) != MOSQ_ERR_SUCCESS){
		cJSON_AddStringToObject(j_responses,"info","Username not valid UTF-8");
		return MOSQ_ERR_INVAL;
	}

	if(json_get_string(command, "clientid", &clientid, true) != MOSQ_ERR_SUCCESS){
		cJSON_AddStringToObject(j_responses,"info","Invalid/missing client ID");
		return MOSQ_ERR_INVAL;
	}
	if(clientid){
		slen = strlen(clientid);
		if(mosquitto_validate_utf8(clientid, (int)slen) != MOSQ_ERR_SUCCESS){
			cJSON_AddStringToObject(j_responses,"info","Client ID not valid UTF-8");
			return MOSQ_ERR_INVAL;
		}
		if(slen > 0){
			clientid_heap = mosquitto_strdup(clientid);
			if(clientid_heap == NULL){
				cJSON_AddStringToObject(j_responses,"info","Internal error");
				return MOSQ_ERR_NOMEM;
			}
		}else{
			clientid_heap = NULL;
		}
	}

	client = dynsec_clients__find(username);
	if(client == NULL){
		mosquitto_free(clientid_heap);
		cJSON_AddStringToObject(j_responses,"info","Client not found");
		return MOSQ_ERR_SUCCESS;
	}

	mosquitto_free(client->clientid);
	client->clientid = clientid_heap;

	db_set_user(client->id,client->clientid,NULL,NULL);
	cJSON_AddStringToObject(j_responses,"info","SUCCESS");

	/* Enforce any changes */
	mosquitto_kick_client_by_username(username, false);
	mosquitto_log_printf(MOSQ_LOG_INFO, "dynsec: setClientId | username=%s | clientid=%s",username, client->clientid);

	return MOSQ_ERR_SUCCESS;
}


static int client__set_password(struct dynsec__client *client, const char *password)
{
/*	if(dynsec_auth__pw_hash(client, password, client->pw.password_hash, sizeof(client->pw.password_hash), true) == MOSQ_ERR_SUCCESS){
		client->pw.valid = true;

		return MOSQ_ERR_SUCCESS;
	}else{
		client->pw.valid = false;
		 FIXME - this should fail safe without modifying the existing password
		return MOSQ_ERR_NOMEM;
	}*/
	return MOSQ_ERR_SUCCESS;
}

int dynsec_clients__process_set_password(cJSON *j_responses, cJSON *command)
{
	char *username, *password;
	struct dynsec__client *client;
	int rc;

	if(json_get_string(command, "username", &username, false) != MOSQ_ERR_SUCCESS){
		cJSON_AddStringToObject(j_responses,"info","Invalid/missing username");
		return MOSQ_ERR_INVAL;
	}
	if(mosquitto_validate_utf8(username, (int)strlen(username)) != MOSQ_ERR_SUCCESS){
		cJSON_AddStringToObject(j_responses,"info","Username not valid UTF-8");
		return MOSQ_ERR_INVAL;
	}

	if(json_get_string(command, "password", &password, false) != MOSQ_ERR_SUCCESS){
		cJSON_AddStringToObject(j_responses,"info","Invalid/missing password");
		return MOSQ_ERR_INVAL;
	}
	if(strlen(password) == 0){
		cJSON_AddStringToObject(j_responses,"info","Empty password is not allowed");
		return MOSQ_ERR_INVAL;
	}

	client = dynsec_clients__find(username);
	if(client == NULL){
		cJSON_AddStringToObject(j_responses,"info","Client not found");
		return MOSQ_ERR_SUCCESS;
	}
	rc = client__set_password(client, password);
	if(rc == MOSQ_ERR_SUCCESS){
		//dynsec__config_save();
		cJSON_AddStringToObject(j_responses,"info","SUCCESS");

		/* Enforce any changes */
		mosquitto_kick_client_by_username(username, false);
		mosquitto_log_printf(MOSQ_LOG_INFO, "dynsec: setClientPassword | username=%s | password=******",username);
	}else{
		cJSON_AddStringToObject(j_responses,"info","Internal error");
	}
	return rc;
}


static void client__add_new_roles(struct dynsec__client *client, struct dynsec__rolelist *base_rolelist)
{
	struct dynsec__rolelist *rolelist, *rolelist_tmp;

	HASH_ITER(hh, base_rolelist, rolelist, rolelist_tmp){
		dynsec_rolelist__client_add(client, rolelist->role, rolelist->priority);
	}
}

static void client__remove_all_roles(struct dynsec__client *client)
{
	struct dynsec__rolelist *rolelist, *rolelist_tmp;

	HASH_ITER(hh, client->rolelist, rolelist, rolelist_tmp){
		dynsec_rolelist__client_remove(client, rolelist->role);
	}
}

int dynsec_clients__process_modify(cJSON *j_responses, cJSON *command)
{
	char *username;
	char *clientid = NULL;
	char *password = NULL;
	char *text_name = NULL, *text_description = NULL;
	bool have_clientid = false, have_text_name = false, have_text_description = false, have_rolelist = false, have_password = false;
	struct dynsec__client *client;
	struct dynsec__group *group;
	struct dynsec__rolelist *rolelist = NULL;
	char *str;
	int rc;
	int priority;
	cJSON *j_group, *j_groups;

	if(json_get_string(command, "username", &username, false) != MOSQ_ERR_SUCCESS){
		cJSON_AddStringToObject(j_responses,"info","Invalid/missing username");
		return MOSQ_ERR_INVAL;
	}
	if(mosquitto_validate_utf8(username, (int)strlen(username)) != MOSQ_ERR_SUCCESS){
		cJSON_AddStringToObject(j_responses,"info","Username not valid UTF-8");
		return MOSQ_ERR_INVAL;
	}

	client = dynsec_clients__find(username);
	if(client == NULL){
		cJSON_AddStringToObject(j_responses,"info","Client not found");
		return MOSQ_ERR_INVAL;
	}

	if(json_get_string(command, "clientid", &str, false) == MOSQ_ERR_SUCCESS){
		have_clientid = true;
		if(str && strlen(str) > 0){
			clientid = mosquitto_strdup(str);
			if(clientid == NULL){
				cJSON_AddStringToObject(j_responses,"info","Internal error");
				rc = MOSQ_ERR_NOMEM;
				goto error;
			}
		}else{
			clientid = NULL;
		}
	}

	if(json_get_string(command, "password", &password, false) == MOSQ_ERR_SUCCESS){
		if(strlen(password) > 0){
			have_password = true;
		}
	}

	if(json_get_string(command, "textname", &str, false) == MOSQ_ERR_SUCCESS){
		have_text_name = true;
		text_name = mosquitto_strdup(str);
		if(text_name == NULL){
			cJSON_AddStringToObject(j_responses,"info","Internal error");
			rc = MOSQ_ERR_NOMEM;
			goto error;
		}
	}

	if(json_get_string(command, "textdescription", &str, false) == MOSQ_ERR_SUCCESS){
		have_text_description = true;
		text_description = mosquitto_strdup(str);
		if(text_description == NULL){
			cJSON_AddStringToObject(j_responses,"info","Internal error");
			rc = MOSQ_ERR_NOMEM;
			goto error;
		}
	}

	rc = dynsec_rolelist__load_from_json(command, &rolelist);
	if(rc == MOSQ_ERR_SUCCESS){
		have_rolelist = true;
	}else if(rc == ERR_LIST_NOT_FOUND){
		/* There was no list in the JSON, so no modification */
	}else if(rc == MOSQ_ERR_NOT_FOUND){
		cJSON_AddStringToObject(j_responses,"info","Role not found");
		rc = MOSQ_ERR_INVAL;
		goto error;
	}else{
		if(rc == MOSQ_ERR_INVAL){
			cJSON_AddStringToObject(j_responses,"info","'roles' not an array or missing/invalid rolename");
		}else{
			cJSON_AddStringToObject(j_responses,"info","Internal error");
		}
		rc = MOSQ_ERR_INVAL;
		goto error;
	}

	j_groups = cJSON_GetObjectItem(command, "groups");
	if(j_groups && cJSON_IsArray(j_groups)){
		/* Iterate through list to check all groups are valid */
		cJSON_ArrayForEach(j_group, j_groups){
			if(cJSON_IsObject(j_group)){
				char *groupname;
				json_get_string(j_group, "groupname", &groupname, false);
				if(groupname){
					group = dynsec_groups__find(groupname);
					if(group == NULL){
						cJSON_AddStringToObject(j_responses,"info","'groups' contains an object with a 'groupname' that does not exist");
						rc = MOSQ_ERR_INVAL;
						goto error;
					}
				}else{
					cJSON_AddStringToObject(j_responses,"info","'groups' contains an object with an invalid 'groupname'");
					rc = MOSQ_ERR_INVAL;
					goto error;
				}
			}
		}

		struct dynsec__grouplist *grouplist2, *grouplist_tmp2 = NULL;
		HASH_ITER(hh, client->grouplist, grouplist2, grouplist_tmp2){
			db_delete_user_group(client->id,grouplist2->group->id);
		}
		dynsec__remove_client_from_all_groups(username);
		cJSON_ArrayForEach(j_group, j_groups){
			if(cJSON_IsObject(j_group)){
				char *groupname;
				json_get_string(j_group, "groupname", &groupname, false);
				if(groupname){
					json_get_int(j_group, "priority", &priority, true, -1);
					dynsec_groups__add_client(username, groupname, priority, false);
				}
			}
		}
	}

	if(have_password){
		/* FIXME - This is the one call that will result in modification on internal error - note that groups have already been modified */
		rc = client__set_password(client, password);
		if(rc != MOSQ_ERR_SUCCESS){
			cJSON_AddStringToObject(j_responses,"info","Internal error");
			mosquitto_kick_client_by_username(username, false);
			/* If this fails we have the situation that the password is set as
			 * invalid, but the config isn't saved, so restarting the broker
			 * *now* will mean the client can log in again. This might be
			 * "good", but is inconsistent, so save the config to be
			 * consistent. */
			//dynsec__config_save();
			rc = MOSQ_ERR_NOMEM;
			goto error;
		}
	}

	if(have_clientid){
		mosquitto_free(client->clientid);
		client->clientid = clientid;
	}

	if(have_text_name){
		mosquitto_free(client->text_name);
		client->text_name = text_name;
	}

	if(have_text_description){
		mosquitto_free(client->text_description);
		client->text_description = text_description;
	}

	if(have_rolelist){
		struct dynsec__rolelist *rolelist2, *rolelist_tmp2;
		HASH_ITER(hh, client->rolelist, rolelist2, rolelist_tmp2){
			db_delete_user_role(client->id,rolelist2->role->id);
		}
		client__remove_all_roles(client);
		client__add_new_roles(client, rolelist);
		dynsec_rolelist__cleanup(&rolelist);
	}

	db_set_user(client->id,clientid,text_name,text_description);
	struct dynsec__rolelist *rolelist2, *rolelist_tmp2;
	HASH_ITER(hh, client->rolelist, rolelist2, rolelist_tmp2){
		db_insert_user_role(client->id,rolelist2->role->id,rolelist2->priority);
	}
	struct dynsec__grouplist *grouplist2, *grouplist_tmp2 = NULL;
	HASH_ITER(hh, client->grouplist, grouplist2, grouplist_tmp2){
		db_insert_user_group(client->id,grouplist2->group->id,grouplist2->priority);
	}
	cJSON_AddStringToObject(j_responses,"info","SUCCESS");

	/* Enforce any changes */
	mosquitto_kick_client_by_username(username, false);
	mosquitto_log_printf(MOSQ_LOG_INFO, "dynsec: modifyClient | username=%s",username);
	return MOSQ_ERR_SUCCESS;
error:
	mosquitto_free(clientid);
	mosquitto_free(text_name);
	mosquitto_free(text_description);
	dynsec_rolelist__cleanup(&rolelist);
	return rc;
}


static int dynsec__remove_client_from_all_groups(const char *username)
{
	struct dynsec__grouplist *grouplist, *grouplist_tmp;
	struct dynsec__client *client;

	client = dynsec_clients__find(username);
	if(client){
		HASH_ITER(hh, client->grouplist, grouplist, grouplist_tmp){
			dynsec_groups__remove_client(username, grouplist->group->groupname, false);
		}
	}

	return MOSQ_ERR_SUCCESS;
}


static cJSON *add_client_to_json(struct dynsec__client *client, bool verbose)
{
	cJSON *j_client = NULL, *j_groups, *j_roles;

	if(verbose){
		j_client = cJSON_CreateObject();
		if(j_client == NULL){
			return NULL;
		}

		if(cJSON_AddStringToObject(j_client, "username", client->username) == NULL
				|| (client->clientid && cJSON_AddStringToObject(j_client, "clientid", client->clientid) == NULL)
				|| (client->text_name && cJSON_AddStringToObject(j_client, "textname", client->text_name) == NULL)
				|| (client->text_description && cJSON_AddStringToObject(j_client, "textdescription", client->text_description) == NULL)
				|| (client->disabled && cJSON_AddBoolToObject(j_client, "disabled", client->disabled) == NULL)
				){

			cJSON_Delete(j_client);
			return NULL;
		}

		j_roles = dynsec_rolelist__all_to_json(client->rolelist);
		if(j_roles == NULL){
			cJSON_Delete(j_client);
			return NULL;
		}
		cJSON_AddItemToObject(j_client, "roles", j_roles);

		j_groups = dynsec_grouplist__all_to_json(client->grouplist);
		if(j_groups == NULL){
			cJSON_Delete(j_client);
			return NULL;
		}
		cJSON_AddItemToObject(j_client, "groups", j_groups);
	}else{
		j_client = cJSON_CreateString(client->username);
		if(j_client == NULL){
			return NULL;
		}
	}
	return j_client;
}


int dynsec_clients__process_get(cJSON *j_responses, cJSON *command)
{
	char *username;
	struct dynsec__client *client;
	cJSON *tree, *j_client, *j_data;

	if(json_get_string(command, "username", &username, false) != MOSQ_ERR_SUCCESS){
		cJSON_AddStringToObject(j_responses,"info","Invalid/missing username");
		return MOSQ_ERR_INVAL;
	}
	if(mosquitto_validate_utf8(username, (int)strlen(username)) != MOSQ_ERR_SUCCESS){
		cJSON_AddStringToObject(j_responses,"info","Username not valid UTF-8");
		return MOSQ_ERR_INVAL;
	}

	client = dynsec_clients__find(username);
	if(client == NULL){
		cJSON_AddStringToObject(j_responses,"info","Client not found");
		return MOSQ_ERR_SUCCESS;
	}

	tree = cJSON_CreateObject();
	if(tree == NULL){
		cJSON_AddStringToObject(j_responses,"info","Internal error");
		return MOSQ_ERR_NOMEM;
	}

	if(cJSON_AddStringToObject(tree, "command", "getClient") == NULL
			|| (j_data = cJSON_AddObjectToObject(tree, "data")) == NULL
			){

		cJSON_Delete(tree);
		cJSON_AddStringToObject(j_responses,"info","Internal error");
		return MOSQ_ERR_NOMEM;
	}

	j_client = add_client_to_json(client, true);
	if(j_client == NULL){
		cJSON_Delete(tree);
		cJSON_AddStringToObject(j_responses,"info","Internal error");
		return MOSQ_ERR_NOMEM;
	}
	cJSON_AddItemToObject(j_data, "client", j_client);
	cJSON_AddItemToArray(j_responses, tree);

	mosquitto_log_printf(MOSQ_LOG_INFO, "dynsec: getClient | username=%s",username);

	return MOSQ_ERR_SUCCESS;
}


int dynsec_clients__process_list(cJSON *j_responses, cJSON *command)
{
	bool verbose;
	struct dynsec__client *client, *client_tmp;
	cJSON *tree, *j_clients, *j_client, *j_data;
	int i, count, offset;

	json_get_bool(command, "verbose", &verbose, true, false);
	json_get_int(command, "count", &count, true, -1);
	json_get_int(command, "offset", &offset, true, 0);

	tree = cJSON_CreateObject();
	if(tree == NULL){
		cJSON_AddStringToObject(j_responses,"info","Internal error");
		return MOSQ_ERR_NOMEM;
	}

	if(cJSON_AddStringToObject(tree, "command", "listClients") == NULL
			|| (j_data = cJSON_AddObjectToObject(tree, "data")) == NULL
			|| cJSON_AddIntToObject(j_data, "totalCount", (int)HASH_CNT(hh, local_clients)) == NULL
			|| (j_clients = cJSON_AddArrayToObject(j_data, "clients")) == NULL
			){

		cJSON_Delete(tree);
		cJSON_AddStringToObject(j_responses,"info","Internal error");
		return MOSQ_ERR_NOMEM;
	}

	i = 0;
	HASH_ITER(hh, local_clients, client, client_tmp){
		if(i>=offset){
			j_client = add_client_to_json(client, verbose);
			if(j_client == NULL){
				cJSON_Delete(tree);
				cJSON_AddStringToObject(j_responses,"info","Internal error");
				return MOSQ_ERR_NOMEM;
			}
			cJSON_AddItemToArray(j_clients, j_client);

			if(count >= 0){
				count--;
				if(count <= 0){
					break;
				}
			}
		}
		i++;
	}
	cJSON_AddItemToArray(j_responses, tree);

	mosquitto_log_printf(MOSQ_LOG_INFO, "dynsec: listClients | verbose=%s | count=%d | offset=%d",verbose?"true":"false", count, offset);

	return MOSQ_ERR_SUCCESS;
}


int dynsec_clients__process_add_role(cJSON *j_responses, cJSON *command)
{
	char *username, *rolename;
	struct dynsec__client *client;
	struct dynsec__role *role;
	int priority;

	if(json_get_string(command, "username", &username, false) != MOSQ_ERR_SUCCESS){
		cJSON_AddStringToObject(j_responses,"info","Invalid/missing username");
		return MOSQ_ERR_INVAL;
	}
	if(mosquitto_validate_utf8(username, (int)strlen(username)) != MOSQ_ERR_SUCCESS){
		cJSON_AddStringToObject(j_responses,"info","Username not valid UTF-8");
		return MOSQ_ERR_INVAL;
	}

	if(json_get_string(command, "rolename", &rolename, false) != MOSQ_ERR_SUCCESS){
		cJSON_AddStringToObject(j_responses,"info","Invalid/missing rolename");
		return MOSQ_ERR_INVAL;
	}
	if(mosquitto_validate_utf8(rolename, (int)strlen(rolename)) != MOSQ_ERR_SUCCESS){
		cJSON_AddStringToObject(j_responses,"info","Role name not valid UTF-8");
		return MOSQ_ERR_INVAL;
	}
	json_get_int(command, "priority", &priority, true, -1);

	client = dynsec_clients__find(username);
	if(client == NULL){
		cJSON_AddStringToObject(j_responses,"info","Client not found");
		return MOSQ_ERR_SUCCESS;
	}

	role = dynsec_roles__find(rolename);
	if(role == NULL){
		cJSON_AddStringToObject(j_responses,"info","Role not found");
		return MOSQ_ERR_SUCCESS;
	}

	if(dynsec_rolelist__client_add(client, role, priority) != MOSQ_ERR_SUCCESS){
		cJSON_AddStringToObject(j_responses,"info","Internal error");
		return MOSQ_ERR_UNKNOWN;
	}
	db_insert_user_role(client->id,role->id,priority);
	cJSON_AddStringToObject(j_responses,"info","SUCCESS");

	/* Enforce any changes */
	mosquitto_kick_client_by_username(username, false);

	mosquitto_log_printf(MOSQ_LOG_INFO, "dynsec: addClientRole | username=%s | rolename=%s | priority=%d",username, rolename, priority);

	return MOSQ_ERR_SUCCESS;
}


int dynsec_clients__process_remove_role(cJSON *j_responses,cJSON *command)
{
	char *username, *rolename;
	struct dynsec__client *client;
	struct dynsec__role *role;

	if(json_get_string(command, "username", &username, false) != MOSQ_ERR_SUCCESS){
		cJSON_AddStringToObject(j_responses,"info","Invalid/missing username");
		return MOSQ_ERR_INVAL;
	}
	if(mosquitto_validate_utf8(username, (int)strlen(username)) != MOSQ_ERR_SUCCESS){
		cJSON_AddStringToObject(j_responses,"info","Username not valid UTF-8");
		return MOSQ_ERR_INVAL;
	}

	if(json_get_string(command, "rolename", &rolename, false) != MOSQ_ERR_SUCCESS){
		cJSON_AddStringToObject(j_responses,"info","Invalid/missing rolename");
		return MOSQ_ERR_INVAL;
	}
	if(mosquitto_validate_utf8(rolename, (int)strlen(rolename)) != MOSQ_ERR_SUCCESS){
		cJSON_AddStringToObject(j_responses,"info","Role name not valid UTF-8");
		return MOSQ_ERR_INVAL;
	}


	client = dynsec_clients__find(username);
	if(client == NULL){
		cJSON_AddStringToObject(j_responses,"info","Client not found");
		return MOSQ_ERR_SUCCESS;
	}

	role = dynsec_roles__find(rolename);
	if(role == NULL){
		cJSON_AddStringToObject(j_responses,"info","Role not found");
		return MOSQ_ERR_SUCCESS;
	}

	dynsec_rolelist__client_remove(client, role);
	db_delete_user_role(client->id,role->id);
	cJSON_AddStringToObject(j_responses,"info","SUCCESS");

	/* Enforce any changes */
	mosquitto_kick_client_by_username(username, false);

	mosquitto_log_printf(MOSQ_LOG_INFO, "dynsec: removeClientRole | username=%s | rolename=%s", username, rolename);

	return MOSQ_ERR_SUCCESS;
}

int clientCommand(int argc,char** argv,cJSON** j_responses){
    cJSON* j_tree;
    j_tree = cJSON_CreateObject();
    if (argc < 1) {
    	cJSON_AddStringToObject(j_tree,"error","Unknown sub command");
        return 0;
    }

    if (argc < 2) {
    	cJSON_AddStringToObject(j_tree,"info","Unknown command content");
        return 0;
    }

	cJSON *j_command = cJSON_ParseWithLength(argv[1], strlen(argv[1]));
	if(!cJSON_IsObject(j_command)){
		cJSON_AddStringToObject(j_tree,"info","Invalid json format");
	}

    char* command = argv[0];
    if(!strcasecmp(command, "createClient")){
		dynsec_clients__process_create(j_tree, j_command);
	}else if(!strcasecmp(command, "deleteClient")){
		dynsec_clients__process_delete(j_tree, j_command);
	}else if(!strcasecmp(command, "getClient")){
		dynsec_clients__process_get(j_tree, j_command);
	}else if(!strcasecmp(command, "listClients")){
		dynsec_clients__process_list(j_tree, j_command);
	}else if(!strcasecmp(command, "modifyClient")){
		dynsec_clients__process_modify(j_tree, j_command);
	}else if(!strcasecmp(command, "setClientPassword")){
		dynsec_clients__process_set_password(j_tree, j_command);
	}else if(!strcasecmp(command, "setClientId")){
		dynsec_clients__process_set_id(j_tree, j_command);
	}else if(!strcasecmp(command, "addClientRole")){
		dynsec_clients__process_add_role(j_tree, j_command);
	}else if(!strcasecmp(command, "removeClientRole")){
		dynsec_clients__process_remove_role(j_tree, j_command);
	}else if(!strcasecmp(command, "enableClient")){
		dynsec_clients__process_enable(j_tree, j_command);
	}else if(!strcasecmp(command, "disableClient")){
		dynsec_clients__process_disable(j_tree, j_command);
	}else{
    	cJSON_AddStringToObject(j_tree,"info","Unknown command");
    }
    cJSON_Delete(j_command);

    *j_responses = j_tree;
    return 0;
}

