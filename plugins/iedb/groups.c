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
 * # Plugin global variables
 * #
 * ################################################################ */

struct dynsec__group *dynsec_anonymous_group = NULL;


/* ################################################################
 * #
 * # Function declarations
 * #
 * ################################################################ */

static int dynsec__remove_all_clients_from_group(struct dynsec__group *group);
static int dynsec__remove_all_roles_from_group(struct dynsec__group *group);
static cJSON *add_group_to_json(struct dynsec__group *group);
int db_insert_user_group(unsigned long long uid,unsigned long long gid,int priority);
int db_delete_user_group_by_group(unsigned long long gid);
int db_delete_user_group(unsigned long long uid,unsigned long long gid);
/* ################################################################
 * #
 * # Local variables
 * #
 * ################################################################ */

static struct dynsec__group *local_groups = NULL;
extern sqlite3 *sqlite3_db;
unsigned long long max_group_id;

/* ################################################################
 * #
 * # Utility functions
 * #
 * ################################################################ */

static void group__kick_all(struct dynsec__group *group)
{
	if(group == dynsec_anonymous_group){
		mosquitto_kick_client_by_username(NULL, false);
	}
	dynsec_clientlist__kick_all(group->clientlist);
}


static int group_cmp(void *a, void *b)
{
	struct dynsec__group *group_a = a;
	struct dynsec__group *group_b = b;

	return strcmp(group_a->groupname, group_b->groupname);
}


struct dynsec__group *dynsec_groups__find(const char *groupname)
{
	struct dynsec__group *group = NULL;

	if(groupname){
		HASH_FIND(hh, local_groups, groupname, strlen(groupname), group);
	}
	return group;
}

static void group__free_item(struct dynsec__group *group)
{
	struct dynsec__group *found_group = NULL;

	if(group == NULL) return;

	found_group = dynsec_groups__find(group->groupname);
	if(found_group){
		HASH_DEL(local_groups, found_group);
	}
	dynsec__remove_all_clients_from_group(group);
	mosquitto_free(group->text_name);
	mosquitto_free(group->text_description);
	mosquitto_free(group->groupname);
	dynsec_rolelist__cleanup(&group->rolelist);
	mosquitto_free(group);
}

/* ################################################################
 * #
 * # db function
 * #
 * ################################################################ */
int select_group_callback(void *data, int argc, char **argv, char **azColName){
	UNUSED(data);
	UNUSED(argc);
	UNUSED(azColName);
	unsigned long long id = 0;
	if(string2ull(argv[0],&id) == 0)
		return MOSQ_ERR_INVAL;

	char* groupname = argv[1];
	char* text_name = argv[2];
	char* text_description = argv[2];

	struct dynsec__group *group = dynsec_groups__find(groupname);
	if(group){
		return 0;
	}

	group = mosquitto_calloc(1, sizeof(struct dynsec__group));
	if(group == NULL){
		return MOSQ_ERR_NOMEM;
	}
	group->id = id;
	if(id > max_group_id)
		max_group_id = id;
	group->groupname = mosquitto_strdup(groupname);
	if(group->groupname == NULL){
		mosquitto_free(group);
		return 0;
	}

	if(text_name){
		group->text_name = mosquitto_strdup(text_name);
		if(group->text_name == NULL){
			mosquitto_free(group->groupname);
			mosquitto_free(group);
			return 0;
		}
	}
	if(text_description){
		group->text_description = mosquitto_strdup(text_description);
		if(group->text_description == NULL){
			mosquitto_free(group->text_name);
			mosquitto_free(group->groupname);
			mosquitto_free(group);
			return 0;
		}
	}
	HASH_ADD_KEYPTR(hh, local_groups, group->groupname, strlen(group->groupname), group);

	return 0;
}

int select_group_role_callback(void *data, int argc, char **argv, char **azColName){
	UNUSED(argc);
	UNUSED(azColName);
	long long priority = 0;
	if(string2ll(argv[1],strlen(argv[1]),&priority) == 0)
		return MOSQ_ERR_INVAL;

	struct dynsec__group *group = (struct dynsec__group *)data;
	struct dynsec__role *role;

	role = dynsec_roles__find(argv[0]);
	dynsec_rolelist__group_add(group, role, (int)priority);

	return 0;
}

int db_group_init(void){
	char *zErrMsg = 0;
	int  rc;
	char* sql = "CREATE TABLE IF NOT EXISTS groups("  \
	         "id INTEGER PRIMARY KEY," \
			 "text_name TEXT," \
			 "text_description TEXT," \
	         "groupname TEXT NOT NULL UNIQUE);";

	rc = sqlite3_exec(sqlite3_db, sql, NULL, 0, &zErrMsg);
	if( rc != SQLITE_OK ){
		mosquitto_log_printf(MOSQ_LOG_ERR, "CREATE TABLE groups,SQL error: %s\n", zErrMsg);
		sqlite3_free(zErrMsg);
		return rc;
	}

	sql = "CREATE TABLE IF NOT EXISTS  group_role("  \
	         "gid INTEGER    NOT NULL," \
			 "rid INTEGER    NOT NULL," \
			 "priority INTEGER," \
			 "PRIMARY KEY (gid,rid)," \
			 "FOREIGN KEY(gid) REFERENCES groups(id)," \
			 "FOREIGN KEY(rid) REFERENCES role(id))WITHOUT ROWID;";

	rc = sqlite3_exec(sqlite3_db, sql, NULL, 0, &zErrMsg);
	if( rc != SQLITE_OK ){
		mosquitto_log_printf(MOSQ_LOG_ERR, "CREATE TABLE group_role,SQL error: %s\n", zErrMsg);
		sqlite3_free(zErrMsg);
		return rc;
	}

	rc = sqlite3_exec(sqlite3_db, "SELECT id,groupname,text_name,text_description FROM groups", select_group_callback, NULL, &zErrMsg);
	if( rc != SQLITE_OK ){
		mosquitto_log_printf(MOSQ_LOG_ERR, "SELECT groups,SQL error: %s\n", zErrMsg);
		sqlite3_free(zErrMsg);
		return rc;
	}
	HASH_SORT(local_groups, group_cmp);

	struct dynsec__group *group, *group_tmp = NULL;
	HASH_ITER(hh, local_groups, group, group_tmp){
		char buf[128];
		snprintf(buf,128,"SELECT rolename,priority FROM group_role INNER JOIN role ON role.id=group_role.rid WHERE group_role.gid=%lld",group->id);
		rc = sqlite3_exec(sqlite3_db, buf, select_group_role_callback, group, &zErrMsg);
		if( rc != SQLITE_OK ){
			mosquitto_log_printf(MOSQ_LOG_ERR, "SELECT group_role,SQL error: %s\n", zErrMsg);
			sqlite3_free(zErrMsg);
			return rc;
		}
	}

	dynsec_anonymous_group = dynsec_groups__find("anonymous");

	return rc;
}

int db_insert_group(struct dynsec__group* group){
	char *zErrMsg = 0;
	int  rc;
	char sql[128];
	if(group->text_name == NULL && group->text_description == NULL)
		snprintf(sql,128,"INSERT INTO groups (id,groupname) VALUES (%lld,%s,%s,%s)",group->id,group->groupname);
	else if(group->text_name == NULL)
		snprintf(sql,128,"INSERT INTO groups (id,groupname,text_description) VALUES (%lld,%s,%s,%s)",group->id,group->groupname,group->text_description);
	else if(group->text_description == NULL)
		snprintf(sql,128,"INSERT INTO groups (id,groupname,text_name) VALUES (%lld,%s,%s,%s)",group->id,group->groupname,group->text_name);
	else
		snprintf(sql,128,"INSERT INTO groups (id,groupname,text_name,text_description) VALUES (%lld,%s,%s,%s)",group->id,group->groupname,group->text_name,group->text_description);
	rc = sqlite3_exec(sqlite3_db, sql, NULL, NULL, &zErrMsg);
	if( rc != SQLITE_OK ){
		mosquitto_log_printf(MOSQ_LOG_ERR, "db_insert_group SQL error: %s\n", zErrMsg);
		sqlite3_free(zErrMsg);
	}
	return rc;
}

int db_delete_group(struct dynsec__group* group){
	char *zErrMsg = 0;
	int  rc;
	char sql[128];
	snprintf(sql,128,"DELETE FROM user_group WHERE gid=%lld",group->id);
	rc = sqlite3_exec(sqlite3_db, sql, NULL, NULL, &zErrMsg);
	if( rc != SQLITE_OK ){
		mosquitto_log_printf(MOSQ_LOG_ERR, "delete user_group SQL error: %s\n", zErrMsg);
		sqlite3_free(zErrMsg);
		return rc;
	}

	snprintf(sql,128,"DELETE FROM group_role WHERE gid=%lld",group->id);
	rc = sqlite3_exec(sqlite3_db, sql, NULL, NULL, &zErrMsg);
	if( rc != SQLITE_OK ){
		mosquitto_log_printf(MOSQ_LOG_ERR, "delete group_role SQL error: %s\n", zErrMsg);
		sqlite3_free(zErrMsg);
		return rc;
	}

	snprintf(sql,128,"DELETE FROM groups WHERE id=%lld",group->id);
	rc = sqlite3_exec(sqlite3_db, sql, NULL, NULL, &zErrMsg);
	if( rc != SQLITE_OK ){
		mosquitto_log_printf(MOSQ_LOG_ERR, "db_delete_group SQL error: %s\n", zErrMsg);
		sqlite3_free(zErrMsg);
	}
	return rc;
}

int db_insert_group_role(unsigned long long gid,unsigned long long rid,int priority){
	char *zErrMsg = 0;
	int  rc;
	char sql[128];
	snprintf(sql,128,"INSERT INTO group_role (gid,rid,priority) VALUES (%lld,%lld,%d)",gid,rid,priority);

	rc = sqlite3_exec(sqlite3_db, sql, NULL, NULL, &zErrMsg);
	if( rc != SQLITE_OK ){
		mosquitto_log_printf(MOSQ_LOG_ERR, "db_insert_group_role SQL error: %s\n", zErrMsg);
		sqlite3_free(zErrMsg);
	}
	return rc;
}

int db_delete_group_role(unsigned long long gid,unsigned long long rid){
	char *zErrMsg = 0;
	int  rc;
	char sql[128];
	snprintf(sql,128,"DELETE FROM group_role WHERE gid=%lld AND rid=%lld",gid,rid);

	rc = sqlite3_exec(sqlite3_db, sql, NULL, NULL, &zErrMsg);
	if( rc != SQLITE_OK ){
		mosquitto_log_printf(MOSQ_LOG_ERR, "db_insert_group_role SQL error: %s\n", zErrMsg);
		sqlite3_free(zErrMsg);
	}
	return rc;
}

int db_delete_group_role_by_group(unsigned long long gid){
	char *zErrMsg = 0;
	int  rc;
	char sql[128];
	snprintf(sql,128,"DELETE FROM group_role WHERE gid=%lld",gid);

	rc = sqlite3_exec(sqlite3_db, sql, NULL, NULL, &zErrMsg);
	if( rc != SQLITE_OK ){
		mosquitto_log_printf(MOSQ_LOG_ERR, "db_delete_group_role_by_group SQL error: %s\n", zErrMsg);
		sqlite3_free(zErrMsg);
	}
	return rc;
}

int db_delete_group_role_by_role(unsigned long long rid){
	char *zErrMsg = 0;
	int  rc;
	char sql[128];
	snprintf(sql,128,"DELETE FROM group_role WHERE rid=%lld",rid);

	rc = sqlite3_exec(sqlite3_db, sql, NULL, NULL, &zErrMsg);
	if( rc != SQLITE_OK ){
		mosquitto_log_printf(MOSQ_LOG_ERR, "db_delete_group_role_by_group SQL error: %s\n", zErrMsg);
		sqlite3_free(zErrMsg);
	}
	return rc;
}
int db_set_group(unsigned long long id, char* text_name, char* text_description){
	char *zErrMsg = 0;
	int  rc;
	char sql[256];
	snprintf(sql,255,"UPDATE groups SET text_name = %s,text_description = %s WHERE id = %lld",text_name,text_description,id);

	rc = sqlite3_exec(sqlite3_db, sql, NULL, NULL, &zErrMsg);
	if( rc != SQLITE_OK ){
		mosquitto_log_printf(MOSQ_LOG_ERR, "db_set_groups,SQL error: %s\n", zErrMsg);
		sqlite3_free(zErrMsg);
	}
	return rc;
}

int dynsec_groups__process_add_role(cJSON *j_responses, cJSON *command)
{
	char *groupname, *rolename;
	struct dynsec__group *group;
	struct dynsec__role *role;
	int priority;

	int rc;

	if(json_get_string(command, "groupname", &groupname, false) != MOSQ_ERR_SUCCESS){
		cJSON_AddStringToObject(j_responses,"info","Invalid/missing groupname");
		return MOSQ_ERR_INVAL;
	}
	if(mosquitto_validate_utf8(groupname, (int)strlen(groupname)) != MOSQ_ERR_SUCCESS){
		cJSON_AddStringToObject(j_responses,"info","Group name not valid UTF-8");
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

	group = dynsec_groups__find(groupname);
	if(group == NULL){
		cJSON_AddStringToObject(j_responses,"info","Group not found");
		return MOSQ_ERR_SUCCESS;
	}

	role = dynsec_roles__find(rolename);
	if(role == NULL){
		cJSON_AddStringToObject(j_responses,"info","Role not found");
		return MOSQ_ERR_SUCCESS;
	}

	rc = dynsec_rolelist__group_add(group, role, priority);
	if(rc == MOSQ_ERR_SUCCESS){
		/* Continue */
	}else if(rc == MOSQ_ERR_ALREADY_EXISTS){
		cJSON_AddStringToObject(j_responses,"info","Group is already in this role");
		return MOSQ_ERR_ALREADY_EXISTS;
	}else{
		cJSON_AddStringToObject(j_responses,"info","Internal error");
		return MOSQ_ERR_UNKNOWN;
	}

	mosquitto_log_printf(MOSQ_LOG_INFO, "dynsec:addGroupRole | groupname=%s | rolename=%s | priority=%d",
			groupname, rolename, priority);

	db_insert_group_role(group->id, role->id, priority);
	cJSON_AddStringToObject(j_responses,"info","SUCCESS");

	/* Enforce any changes */
	group__kick_all(group);

	return MOSQ_ERR_SUCCESS;
}


void dynsec_groups__cleanup(void)
{
	struct dynsec__group *group, *group_tmp = NULL;

	HASH_ITER(hh, local_groups, group, group_tmp){
		group__free_item(group);
	}
}

int dynsec_groups__process_create(cJSON *j_responses, cJSON *command)
{
	char *groupname, *text_name, *text_description;
	struct dynsec__group *group = NULL;
	int rc = MOSQ_ERR_SUCCESS;


	if(json_get_string(command, "groupname", &groupname, false) != MOSQ_ERR_SUCCESS){
		cJSON_AddStringToObject(j_responses,"info","Invalid/missing groupname");
		return MOSQ_ERR_INVAL;
	}
	if(mosquitto_validate_utf8(groupname, (int)strlen(groupname)) != MOSQ_ERR_SUCCESS){
		cJSON_AddStringToObject(j_responses,"info","Group name not valid UTF-8");
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

	group = dynsec_groups__find(groupname);
	if(group){
		cJSON_AddStringToObject(j_responses,"info","Group already exists");
		return MOSQ_ERR_SUCCESS;
	}

	group = mosquitto_calloc(1, sizeof(struct dynsec__group));
	if(group == NULL){
		cJSON_AddStringToObject(j_responses,"info","Internal error");
		return MOSQ_ERR_NOMEM;
	}
	group->groupname = strdup(groupname);
	if(group->groupname == NULL){
		cJSON_AddStringToObject(j_responses,"info","Internal error");
		group__free_item(group);
		return MOSQ_ERR_NOMEM;
	}
	if(text_name){
		group->text_name = strdup(text_name);
		if(group->text_name == NULL){
			cJSON_AddStringToObject(j_responses,"info","Internal error");
			group__free_item(group);
			return MOSQ_ERR_NOMEM;
		}
	}
	if(text_description){
		group->text_description = strdup(text_description);
		if(group->text_description == NULL){
			cJSON_AddStringToObject(j_responses,"info","Internal error");
			group__free_item(group);
			return MOSQ_ERR_NOMEM;
		}
	}

	rc = dynsec_rolelist__load_from_json(command, &group->rolelist);
	if(rc == MOSQ_ERR_SUCCESS || rc == ERR_LIST_NOT_FOUND){
	}else if(rc == MOSQ_ERR_NOT_FOUND){
		cJSON_AddStringToObject(j_responses,"info","Role not found");
		group__free_item(group);
		return MOSQ_ERR_INVAL;
	}else{
		cJSON_AddStringToObject(j_responses,"info","Internal error");
		group__free_item(group);
		return MOSQ_ERR_INVAL;
	}

	HASH_ADD_KEYPTR_INORDER(hh, local_groups, group->groupname, strlen(group->groupname), group, group_cmp);
	group->id = ++max_group_id;
	db_insert_group(group);

	struct dynsec__rolelist *rolelist, *rolelist_tmp;
	HASH_ITER(hh, group->rolelist, rolelist, rolelist_tmp){
		db_insert_group_role(group->id,rolelist->role->id,rolelist->priority);
	}
	if(strcmp(group->groupname,"anonymous") == 0)
		dynsec_anonymous_group = group;

	cJSON_AddStringToObject(j_responses,"info","SUCCESS");
	mosquitto_log_printf(MOSQ_LOG_INFO, "dynsec: createGroup | groupname=%s",groupname);
	return MOSQ_ERR_SUCCESS;
}


int dynsec_groups__process_delete(cJSON *j_responses, cJSON *command)
{
	char *groupname;
	struct dynsec__group *group;


	if(json_get_string(command, "groupname", &groupname, false) != MOSQ_ERR_SUCCESS){
		cJSON_AddStringToObject(j_responses,"info","Invalid/missing groupname");
		return MOSQ_ERR_INVAL;
	}
	if(mosquitto_validate_utf8(groupname, (int)strlen(groupname)) != MOSQ_ERR_SUCCESS){
		cJSON_AddStringToObject(j_responses,"info","Group name not valid UTF-8");
		return MOSQ_ERR_INVAL;
	}

	group = dynsec_groups__find(groupname);
	if(group){
		if(group == dynsec_anonymous_group){
			cJSON_AddStringToObject(j_responses,"info","Deleting the anonymous group is forbidden");
			return MOSQ_ERR_INVAL;
		}

		db_delete_group(group);

		/* Enforce any changes */
		group__kick_all(group);
		dynsec__remove_all_roles_from_group(group);
		group__free_item(group);

		cJSON_AddStringToObject(j_responses,"info","SUCCESS");

		mosquitto_log_printf(MOSQ_LOG_INFO, "dynsec: deleteGroup | groupname=%s", groupname);
		return MOSQ_ERR_SUCCESS;
	}else{
		cJSON_AddStringToObject(j_responses,"info","Group not found");
		return MOSQ_ERR_SUCCESS;
	}
}


int dynsec_groups__add_client(const char *username, const char *groupname, int priority, bool update_config)
{
	struct dynsec__client *client;
	struct dynsec__clientlist *clientlist;
	struct dynsec__group *group;
	int rc;

	client = dynsec_clients__find(username);
	if(client == NULL){
		return ERR_USER_NOT_FOUND;
	}

	group = dynsec_groups__find(groupname);
	if(group == NULL){
		return ERR_GROUP_NOT_FOUND;
	}

	HASH_FIND(hh, group->clientlist, username, strlen(username), clientlist);
	if(clientlist != NULL){
		/* Client is already in the group */
		return MOSQ_ERR_ALREADY_EXISTS;
	}

	rc = dynsec_clientlist__add(&group->clientlist, client, priority);
	if(rc){
		return rc;
	}
	rc = dynsec_grouplist__add(&client->grouplist, group, priority);
	if(rc){
		dynsec_clientlist__remove(&group->clientlist, client);
		return rc;
	}

	if(update_config){
		db_insert_user_group(client->id,group->id,priority);
	}

	return MOSQ_ERR_SUCCESS;
}


int dynsec_groups__process_add_client(cJSON *j_responses, cJSON *command)
{
	char *username, *groupname;
	int rc;
	int priority;


	if(json_get_string(command, "username", &username, false) != MOSQ_ERR_SUCCESS){
		cJSON_AddStringToObject(j_responses,"info","Invalid/missing username");
		return MOSQ_ERR_INVAL;
	}
	if(mosquitto_validate_utf8(username, (int)strlen(username)) != MOSQ_ERR_SUCCESS){
		cJSON_AddStringToObject(j_responses,"info","Username not valid UTF-8");
		return MOSQ_ERR_INVAL;
	}

	if(json_get_string(command, "groupname", &groupname, false) != MOSQ_ERR_SUCCESS){
		cJSON_AddStringToObject(j_responses,"info","Invalid/missing groupname");
		return MOSQ_ERR_INVAL;
	}
	if(mosquitto_validate_utf8(groupname, (int)strlen(groupname)) != MOSQ_ERR_SUCCESS){
		cJSON_AddStringToObject(j_responses,"info","Group name not valid UTF-8");
		return MOSQ_ERR_INVAL;
	}

	json_get_int(command, "priority", &priority, true, -1);

	rc = dynsec_groups__add_client(username, groupname, priority, true);
	if(rc == MOSQ_ERR_SUCCESS){
		mosquitto_log_printf(MOSQ_LOG_INFO, "dynsec: addGroupClient | groupname=%s | username=%s | priority=%d",
				 groupname, username, priority);

		cJSON_AddStringToObject(j_responses,"info","SUCCESS");
	}else if(rc == ERR_USER_NOT_FOUND){
		cJSON_AddStringToObject(j_responses,"info","Client not found");
	}else if(rc == ERR_GROUP_NOT_FOUND){
		cJSON_AddStringToObject(j_responses,"info","Group not found");
	}else if(rc == MOSQ_ERR_ALREADY_EXISTS){
		cJSON_AddStringToObject(j_responses,"info","Client is already in this group");
	}else{
		cJSON_AddStringToObject(j_responses,"info","Internal error");
	}

	/* Enforce any changes */
	mosquitto_kick_client_by_username(username, false);

	return rc;
}


static int dynsec__remove_all_clients_from_group(struct dynsec__group *group)
{
	struct dynsec__clientlist *clientlist, *clientlist_tmp = NULL;

	HASH_ITER(hh, group->clientlist, clientlist, clientlist_tmp){
		/* Remove client stored group reference */
		dynsec_grouplist__remove(&clientlist->client->grouplist, group);

		HASH_DELETE(hh, group->clientlist, clientlist);
		mosquitto_free(clientlist);
	}

	return MOSQ_ERR_SUCCESS;
}

static int dynsec__remove_all_roles_from_group(struct dynsec__group *group)
{
	struct dynsec__rolelist *rolelist, *rolelist_tmp = NULL;

	HASH_ITER(hh, group->rolelist, rolelist, rolelist_tmp){
		dynsec_rolelist__group_remove(group, rolelist->role);
	}

	return MOSQ_ERR_SUCCESS;
}

int dynsec_groups__remove_client(const char *username, const char *groupname, bool update_config)
{
	struct dynsec__client *client;
	struct dynsec__group *group;

	client = dynsec_clients__find(username);
	if(client == NULL){
		return ERR_USER_NOT_FOUND;
	}

	group = dynsec_groups__find(groupname);
	if(group == NULL){
		return ERR_GROUP_NOT_FOUND;
	}

	dynsec_clientlist__remove(&group->clientlist, client);
	dynsec_grouplist__remove(&client->grouplist, group);

	if(update_config){
		db_delete_user_group(client->id,group->id);
	}
	return MOSQ_ERR_SUCCESS;
}

int dynsec_groups__process_remove_client(cJSON *j_responses, cJSON *command)
{
	char *username, *groupname;
	int rc;


	if(json_get_string(command, "username", &username, false) != MOSQ_ERR_SUCCESS){
		cJSON_AddStringToObject(j_responses,"info","Invalid/missing username");
		return MOSQ_ERR_INVAL;
	}
	if(mosquitto_validate_utf8(username, (int)strlen(username)) != MOSQ_ERR_SUCCESS){
		cJSON_AddStringToObject(j_responses,"info","Username not valid UTF-8");
		return MOSQ_ERR_INVAL;
	}

	if(json_get_string(command, "groupname", &groupname, false) != MOSQ_ERR_SUCCESS){
		cJSON_AddStringToObject(j_responses,"info","Invalid/missing groupname");
		return MOSQ_ERR_INVAL;
	}
	if(mosquitto_validate_utf8(groupname, (int)strlen(groupname)) != MOSQ_ERR_SUCCESS){
		cJSON_AddStringToObject(j_responses,"info","Group name not valid UTF-8");
		return MOSQ_ERR_INVAL;
	}

	rc = dynsec_groups__remove_client(username, groupname, true);
	if(rc == MOSQ_ERR_SUCCESS){
		mosquitto_log_printf(MOSQ_LOG_INFO, "dynsec: removeGroupClient | groupname=%s | username=%s",
				groupname, username);

		cJSON_AddStringToObject(j_responses,"info","SUCCESS");
	}else if(rc == ERR_USER_NOT_FOUND){
		cJSON_AddStringToObject(j_responses,"info","Client not found");
	}else if(rc == ERR_GROUP_NOT_FOUND){
		cJSON_AddStringToObject(j_responses,"info","Group not found");
	}else{
		cJSON_AddStringToObject(j_responses,"info","Internal error");
	}

	/* Enforce any changes */
	mosquitto_kick_client_by_username(username, false);

	return rc;
}


static cJSON *add_group_to_json(struct dynsec__group *group)
{
	cJSON *j_group, *jtmp, *j_clientlist, *j_client, *j_rolelist;
	struct dynsec__clientlist *clientlist, *clientlist_tmp = NULL;

	j_group = cJSON_CreateObject();
	if(j_group == NULL){
		return NULL;
	}

	if(cJSON_AddStringToObject(j_group, "groupname", group->groupname) == NULL
			|| (group->text_name && cJSON_AddStringToObject(j_group, "textname", group->text_name) == NULL)
			|| (group->text_description && cJSON_AddStringToObject(j_group, "textdescription", group->text_description) == NULL)
			|| (j_clientlist = cJSON_AddArrayToObject(j_group, "clients")) == NULL
			){

		cJSON_Delete(j_group);
		return NULL;
	}

	HASH_ITER(hh, group->clientlist, clientlist, clientlist_tmp){
		j_client = cJSON_CreateObject();
		if(j_client == NULL){
			cJSON_Delete(j_group);
			return NULL;
		}
		cJSON_AddItemToArray(j_clientlist, j_client);

		jtmp = cJSON_CreateStringReference(clientlist->client->username);
		if(jtmp == NULL){
			cJSON_Delete(j_group);
			return NULL;
		}
		cJSON_AddItemToObject(j_client, "username", jtmp);
	}

	j_rolelist = dynsec_rolelist__all_to_json(group->rolelist);
	if(j_rolelist == NULL){
		cJSON_Delete(j_group);
		return NULL;
	}
	cJSON_AddItemToObject(j_group, "roles", j_rolelist);

	return j_group;
}

int dynsec_groups__process_list(cJSON *j_responses, cJSON *command)
{
	bool verbose;
	cJSON *tree, *j_groups, *j_group, *j_data;
	struct dynsec__group *group, *group_tmp = NULL;
	int i, count, offset;


	json_get_bool(command, "verbose", &verbose, true, false);
	json_get_int(command, "count", &count, true, -1);
	json_get_int(command, "offset", &offset, true, 0);

	tree = cJSON_CreateObject();
	if(tree == NULL){
		cJSON_AddStringToObject(j_responses,"info","Internal error");
		return MOSQ_ERR_NOMEM;
	}

	if(cJSON_AddStringToObject(tree, "command", "listGroups") == NULL
			|| (j_data = cJSON_AddObjectToObject(tree, "data")) == NULL
			|| cJSON_AddIntToObject(j_data, "totalCount", (int)HASH_CNT(hh, local_groups)) == NULL
			|| (j_groups = cJSON_AddArrayToObject(j_data, "groups")) == NULL
			){

		cJSON_Delete(tree);
		cJSON_AddStringToObject(j_responses,"info","Internal error");
		return MOSQ_ERR_NOMEM;
	}

	i = 0;
	HASH_ITER(hh, local_groups, group, group_tmp){
		if(i>=offset){
			if(verbose){
				j_group = add_group_to_json(group);
				if(j_group == NULL){
					cJSON_Delete(tree);
					cJSON_AddStringToObject(j_responses,"info","Internal error");
					return MOSQ_ERR_NOMEM;
				}
				cJSON_AddItemToArray(j_groups, j_group);

			}else{
				j_group = cJSON_CreateString(group->groupname);
				if(j_group){
					cJSON_AddItemToArray(j_groups, j_group);
				}else{
					cJSON_Delete(tree);
					cJSON_AddStringToObject(j_responses,"info","Internal error");
					return MOSQ_ERR_NOMEM;
				}
			}

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

	mosquitto_log_printf(MOSQ_LOG_INFO, "dynsec: listGroups | verbose=%s | count=%d | offset=%d",
			verbose?"true":"false", count, offset);

	return MOSQ_ERR_SUCCESS;
}

int dynsec_groups__process_get(cJSON *j_responses, cJSON *command)
{
	char *groupname;
	cJSON *tree, *j_group, *j_data;
	struct dynsec__group *group;


	if(json_get_string(command, "groupname", &groupname, false) != MOSQ_ERR_SUCCESS){
		cJSON_AddStringToObject(j_responses,"info","Invalid/missing groupname");
		return MOSQ_ERR_INVAL;
	}
	if(mosquitto_validate_utf8(groupname, (int)strlen(groupname)) != MOSQ_ERR_SUCCESS){
		cJSON_AddStringToObject(j_responses,"info","Group name not valid UTF-8");
		return MOSQ_ERR_INVAL;
	}

	tree = cJSON_CreateObject();
	if(tree == NULL){
		cJSON_AddStringToObject(j_responses,"info","Internal error");
		return MOSQ_ERR_NOMEM;
	}

	if(cJSON_AddStringToObject(tree, "command", "getGroup") == NULL
			|| (j_data = cJSON_AddObjectToObject(tree, "data")) == NULL
			){

		cJSON_Delete(tree);
		cJSON_AddStringToObject(j_responses,"info","Internal error");
		return MOSQ_ERR_NOMEM;
	}

	group = dynsec_groups__find(groupname);
	if(group){
		j_group = add_group_to_json(group);
		if(j_group == NULL){
			cJSON_Delete(tree);
			cJSON_AddStringToObject(j_responses,"info","Internal error");
			return MOSQ_ERR_NOMEM;
		}
		cJSON_AddItemToObject(j_data, "group", j_group);
	}else{
		cJSON_Delete(tree);
		cJSON_AddStringToObject(j_responses,"info","Group not found");
		return MOSQ_ERR_NOMEM;
	}

	cJSON_AddItemToArray(j_responses, tree);
	mosquitto_log_printf(MOSQ_LOG_INFO, "dynsec: getGroup | groupname=%s",groupname);
	return MOSQ_ERR_SUCCESS;
}


int dynsec_groups__process_remove_role(cJSON *j_responses, cJSON *command)
{
	char *groupname, *rolename;
	struct dynsec__group *group;
	struct dynsec__role *role;


	if(json_get_string(command, "groupname", &groupname, false) != MOSQ_ERR_SUCCESS){
		cJSON_AddStringToObject(j_responses,"info","Invalid/missing groupname");
		return MOSQ_ERR_INVAL;
	}
	if(mosquitto_validate_utf8(groupname, (int)strlen(groupname)) != MOSQ_ERR_SUCCESS){
		cJSON_AddStringToObject(j_responses,"info","Group name not valid UTF-8");
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

	group = dynsec_groups__find(groupname);
	if(group == NULL){
		cJSON_AddStringToObject(j_responses,"info","Group not found");
		return MOSQ_ERR_SUCCESS;
	}

	role = dynsec_roles__find(rolename);
	if(role == NULL){
		cJSON_AddStringToObject(j_responses,"info","Role not found");
		return MOSQ_ERR_SUCCESS;
	}

	dynsec_rolelist__group_remove(group, role);
	db_delete_group_role(group->id, role->id);
	cJSON_AddStringToObject(j_responses,"info","SUCCESS");

	/* Enforce any changes */
	group__kick_all(group);

	mosquitto_log_printf(MOSQ_LOG_INFO, "dynsec: removeGroupRole | groupname=%s | rolename=%s",groupname, rolename);

	return MOSQ_ERR_SUCCESS;
}


int dynsec_groups__process_modify(cJSON *j_responses, cJSON *command)
{
	char *groupname = NULL;
	char *text_name = NULL, *text_description = NULL;
	struct dynsec__client *client = NULL;
	struct dynsec__group *group = NULL;
	struct dynsec__rolelist *rolelist = NULL;
	bool have_text_name = false, have_text_description = false, have_rolelist = false;
	int rc;
	int priority;
	cJSON *j_client, *j_clients;
	char *username;
	char *textname;
	char *textdescription;


	if(json_get_string(command, "groupname", &groupname, false) != MOSQ_ERR_SUCCESS){
		cJSON_AddStringToObject(j_responses,"info","Invalid/missing groupname");
		return MOSQ_ERR_INVAL;
	}
	if(mosquitto_validate_utf8(groupname, (int)strlen(groupname)) != MOSQ_ERR_SUCCESS){
		cJSON_AddStringToObject(j_responses,"info","Group name not valid UTF-8");
		return MOSQ_ERR_INVAL;
	}

	group = dynsec_groups__find(groupname);
	if(group == NULL){
		cJSON_AddStringToObject(j_responses,"info","Group not found");
		return MOSQ_ERR_INVAL;
	}

	if(json_get_string(command, "textname", &textname, false) == MOSQ_ERR_SUCCESS){
		have_text_name = true;
		text_name = mosquitto_strdup(textname);
		if(text_name == NULL){
			cJSON_AddStringToObject(j_responses,"info","Internal error");
			rc = MOSQ_ERR_NOMEM;
			goto error;
		}
	}

	if(json_get_string(command, "textdescription", &textdescription, false) == MOSQ_ERR_SUCCESS){
		have_text_description = true;
		text_description = mosquitto_strdup(textdescription);
		if(text_description == NULL){
			cJSON_AddStringToObject(j_responses,"info","Internal error");
			rc = MOSQ_ERR_NOMEM;
			goto error;
		}
	}

	rc = dynsec_rolelist__load_from_json(command, &rolelist);
	if(rc == MOSQ_ERR_SUCCESS){
		/* Apply changes below */
		have_rolelist = true;
	}else if(rc == ERR_LIST_NOT_FOUND){
		/* There was no list in the JSON, so no modification */
		rolelist = NULL;
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

	j_clients = cJSON_GetObjectItem(command, "clients");
	if(j_clients && cJSON_IsArray(j_clients)){
		/* Iterate over array to check clients are valid before proceeding */
		cJSON_ArrayForEach(j_client, j_clients){
			if(cJSON_IsObject(j_client)){
				json_get_string(j_client, "username", &username, false);
				if(username){
					client = dynsec_clients__find(username);
					if(client == NULL){
						cJSON_AddStringToObject(j_responses,"info","'clients' contains an object with a 'username' that does not exist");
						rc = MOSQ_ERR_INVAL;
						goto error;
					}
				}else{
					cJSON_AddStringToObject(j_responses,"info","'clients' contains an object with an invalid 'username'");
					rc = MOSQ_ERR_INVAL;
					goto error;
				}
			}
		}

		/* Kick all clients in the *current* group */
		group__kick_all(group);
		db_delete_user_group_by_group(group->id);
		dynsec__remove_all_clients_from_group(group);

		/* Now we can add the new clients to the group */
		cJSON_ArrayForEach(j_client, j_clients){
			if(cJSON_IsObject(j_client)){
				json_get_string(j_client, "username", &username, false);
				if(username){
					json_get_int(j_client, "priority", &priority, true, -1);
					dynsec_groups__add_client(username, groupname, priority, true);
				}
			}
		}
	}

	/* Apply remaining changes to group, note that user changes are already applied */
	if(have_text_name){
		mosquitto_free(group->text_name);
		group->text_name = text_name;
	}

	if(have_text_description){
		mosquitto_free(group->text_description);
		group->text_description = text_description;
	}

	if(have_rolelist){
		db_delete_group_role_by_group(group->id);
		dynsec_rolelist__cleanup(&group->rolelist);
		group->rolelist = rolelist;

		struct dynsec__rolelist *rolelist, *rolelist_tmp;
		HASH_ITER(hh, group->rolelist, rolelist, rolelist_tmp){
			db_insert_group_role(group->id,rolelist->role->id,rolelist->priority);
		}
	}

	/* And save */
	//dynsec__config_save();
	db_set_group(group->id,group->text_name,group->text_description);
	cJSON_AddStringToObject(j_responses,"info","SUCCESS");

	/* Enforce any changes - kick any clients in the *new* group */
	group__kick_all(group);

	mosquitto_log_printf(MOSQ_LOG_INFO, "dynsec: modifyGroup | groupname=%s",groupname);

	return MOSQ_ERR_SUCCESS;
error:
	mosquitto_free(text_name);
	mosquitto_free(text_description);
	dynsec_rolelist__cleanup(&rolelist);

	mosquitto_log_printf(MOSQ_LOG_INFO, "dynsec: modifyGroup | groupname=%s",groupname);

	return rc;
}

int groupCommand(int argc,char** argv,cJSON** j_responses){
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

    char* subCommand = argv[0];
    if(!strcasecmp(subCommand, "addGroupClient")){
		dynsec_groups__process_add_client(j_tree, j_command);
	}else if(!strcasecmp(subCommand, "createGroup")){
		dynsec_groups__process_create(j_tree, j_command);
	}else if(!strcasecmp(subCommand, "deleteGroup")){
		dynsec_groups__process_delete(j_tree, j_command);
	}else if(!strcasecmp(subCommand, "getGroup")){
		dynsec_groups__process_get(j_tree, j_command);
	}else if(!strcasecmp(subCommand, "listGroups")){
		dynsec_groups__process_list(j_tree, j_command);
	}else if(!strcasecmp(subCommand, "modifyGroup")){
		dynsec_groups__process_modify(j_tree, j_command);
	}else if(!strcasecmp(subCommand, "removeGroupClient")){
		dynsec_groups__process_remove_client(j_tree, j_command);
	}else if(!strcasecmp(subCommand, "addGroupRole")){
		dynsec_groups__process_add_role(j_tree, j_command);
	}else if(!strcasecmp(subCommand, "removeGroupRole")){
		dynsec_groups__process_remove_role(j_tree, j_command);
	}else{
    	cJSON_AddStringToObject(j_tree,"info","Unknown command");
    }
    cJSON_Delete(j_command);

    *j_responses = j_tree;
    return 0;
}
