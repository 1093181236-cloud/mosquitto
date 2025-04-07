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
#include <string.h>
#include <uthash.h>
#include <utlist.h>
#include <libwebsockets.h>
#ifndef WIN32
#  include <strings.h>
#endif
#include <sqlite3.h>

#include "dynamic_security.h"
#include "json_help.h"
#include "mosquitto.h"
#include "mosquitto_broker.h"

static cJSON *add_role_to_json(struct dynsec__role *role, bool verbose);
static void role__remove_all_clients(struct dynsec__role *role);
int db_delete_group_role(unsigned long long gid,unsigned long long rid);
int db_delete_group_role_by_role(unsigned long long rid);
int db_delete_user_role_by_role(unsigned long long rid);
/* ################################################################
 * #
 * # Local variables
 * #
 * ################################################################ */

struct dynsec__role *local_roles = NULL;
extern sqlite3 *db;
unsigned long long max_role_id = 0;

/* ################################################################
 * #
 * # Utility functions
 * #
 * ################################################################ */
int role_cmp(void *a, void *b)
{
	struct dynsec__role *role_a = a;
	struct dynsec__role *role_b = b;

	return strcmp(role_a->rolename, role_b->rolename);
}


static void role__free_acl(struct dynsec__acl **acl, struct dynsec__acl *item)
{
	HASH_DELETE(hh, *acl, item);
	mosquitto_free(item->topic);
	mosquitto_free(item);
}

static void role__free_all_acls(struct dynsec__acl **acl)
{
	struct dynsec__acl *iter, *tmp = NULL;

	HASH_ITER(hh, *acl, iter, tmp){
		role__free_acl(acl, iter);
	}
}

static void role__free_item(struct dynsec__role *role, bool remove_from_hash)
{
	if(remove_from_hash){
		HASH_DEL(local_roles, role);
	}
	dynsec_clientlist__cleanup(&role->clientlist);
	dynsec_grouplist__cleanup(&role->grouplist);
	mosquitto_free(role->text_name);
	mosquitto_free(role->text_description);
	mosquitto_free(role->rolename);
	role__free_all_acls(&role->acls.publish_c_send);
	role__free_all_acls(&role->acls.publish_c_recv);
	role__free_all_acls(&role->acls.subscribe_literal);
	role__free_all_acls(&role->acls.subscribe_pattern);
	role__free_all_acls(&role->acls.unsubscribe_literal);
	role__free_all_acls(&role->acls.unsubscribe_pattern);
	mosquitto_free(role);
}

struct dynsec__role *dynsec_roles__find(const char *rolename)
{
	struct dynsec__role *role = NULL;

	if(rolename){
		HASH_FIND(hh, local_roles, rolename, strlen(rolename), role);
	}
	return role;
}


void dynsec_roles__cleanup(void)
{
	struct dynsec__role *role, *role_tmp = NULL;

	HASH_ITER(hh, local_roles, role, role_tmp){
		role__free_item(role, true);
	}
}


static void role__kick_all(struct dynsec__role *role)
{
	struct dynsec__grouplist *grouplist, *grouplist_tmp = NULL;

	dynsec_clientlist__kick_all(role->clientlist);

	HASH_ITER(hh, role->grouplist, grouplist, grouplist_tmp){
		if(grouplist->group == dynsec_anonymous_group){
			mosquitto_kick_client_by_username(NULL, false);
		}
		dynsec_clientlist__kick_all(grouplist->group->clientlist);
	}
}

static int insert_acl_cmp(struct dynsec__acl *a, struct dynsec__acl *b)
{
	return b->priority - a->priority;
}
/* ################################################################
 * #
 * # db function
 * #
 * ################################################################ */
int select_role_callback(void *data, int argc, char **argv, char **azColName){
	UNUSED(data);
	UNUSED(argc);
	UNUSED(azColName);
	unsigned long long id = 0;
	if(string2ull(argv[0],&id) == 0)
		return MOSQ_ERR_INVAL;

	char* rolename = argv[1];
	char* text_name = argv[2];
	char* text_description = argv[2];

	struct dynsec__role *role = dynsec_roles__find(rolename);
	if(role){
		return 0;
	}

	role = mosquitto_calloc(1, sizeof(struct dynsec__role));
	if(role == NULL){
		return MOSQ_ERR_NOMEM;
	}
	role->id = id;
	if(id > max_role_id)
		max_role_id = id;
	role->rolename = mosquitto_strdup(rolename);
	if(role->rolename == NULL){
		mosquitto_free(role);
		return 0;
	}

	if(text_name){
		role->text_name = mosquitto_strdup(text_name);
		if(role->text_name == NULL){
			mosquitto_free(role->rolename);
			mosquitto_free(role);
			return 0;
		}
	}
	if(text_description){
		role->text_description = mosquitto_strdup(text_description);
		if(role->text_description == NULL){
			mosquitto_free(role->text_name);
			mosquitto_free(role->rolename);
			mosquitto_free(role);
			return 0;
		}
	}
	HASH_ADD_KEYPTR(hh, local_roles, role->rolename, strlen(role->rolename), role);

	return 0;
}

int select_acl_callback(void *data, int argc, char **argv, char **azColName){
	UNUSED(argc);
	UNUSED(azColName);
	struct dynsec__role *role = data;

	char* topic = argv[0];
	char* acltype = argv[1];
	char* priority = argv[2];
	char* allow = argv[3];

	if(!acltype || !topic){
		return 0;
	}

	struct dynsec__acl **acllist, *acl;
	if(!strcasecmp(acltype, ACL_TYPE_PUB_C_SEND)){
		acllist = &role->acls.publish_c_send;
	}else if(!strcasecmp(acltype, ACL_TYPE_PUB_C_RECV)){
		acllist = &role->acls.publish_c_recv;
	}else if(!strcasecmp(acltype, ACL_TYPE_SUB_LITERAL)){
		acllist = &role->acls.subscribe_literal;
	}else if(!strcasecmp(acltype, ACL_TYPE_SUB_PATTERN)){
		acllist = &role->acls.subscribe_pattern;
	}else if(!strcasecmp(acltype, ACL_TYPE_UNSUB_LITERAL)){
		acllist = &role->acls.unsubscribe_literal;
	}else if(!strcasecmp(acltype, ACL_TYPE_UNSUB_PATTERN)){
		acllist = &role->acls.unsubscribe_pattern;
	}else{
		return 0;
	}

	HASH_FIND(hh, *acllist, topic, strlen(topic), acl);
	if(acl){
		return 0;
	}

	acl = mosquitto_calloc(1, sizeof(struct dynsec__acl));
	if(acl == NULL){
		return -1;
	}
	if(priority){
		unsigned long long id = 0;
		string2ull(argv[0],&id);
		acl->priority = (int)id;
	}else{
		acl->priority = 0;
	}
	if(allow){
		unsigned long long id = 0;
		string2ull(argv[0],&id);
		acl->allow = (bool)id;
	}else{
		acl->allow = false;
	}

	acl->topic = mosquitto_strdup(topic);
	if(acl->topic == NULL){
		mosquitto_free(acl);
		return -1;
	}
	HASH_ADD_KEYPTR_INORDER(hh, *acllist, acl->topic, strlen(acl->topic), acl, insert_acl_cmp);

	return 0;
}

int db_role_init(void){
	char *zErrMsg = 0;
	int  rc;
	char* sql = "CREATE TABLE IF NOT EXISTS  roles("  \
	         "id INTEGER PRIMARY KEY," \
			 "text_name TEXT," \
			 "text_description TEXT," \
	         "rolename TEXT NOT NULL UNIQUE);";

	rc = sqlite3_exec(db, sql, NULL, 0, &zErrMsg);
	if( rc != SQLITE_OK ){
		mosquitto_log_printf(MOSQ_LOG_ERR, "CREATE TABLE role,SQL error: %s\n", zErrMsg);
		sqlite3_free(zErrMsg);
		return rc;
	}

	sql = "CREATE TABLE IF NOT EXISTS  acls("  \
			 "rid INTEGER      NOT NULL," \
			 "topic TEXT      NOT NULL," \
			 "acltype TEXT      NOT NULL," \
			 "priority TINYINT  ," \
			 "allow BOOLEAN  ," \
			 "PRIMARY KEY (rid,topic,acltype)," \
			 "FOREIGN KEY(rid) REFERENCES role(id))WITHOUT ROWID;";

	rc = sqlite3_exec(db, sql, NULL, 0, &zErrMsg);
	if( rc != SQLITE_OK ){
		mosquitto_log_printf(MOSQ_LOG_ERR, "CREATE TABLE acl,SQL error: %s\n", zErrMsg);
		sqlite3_free(zErrMsg);
		return rc;
	}

	rc = sqlite3_exec(db, "SELECT id,rolename,text_name,text_description FROM roles", select_role_callback, NULL, &zErrMsg);
	if( rc != SQLITE_OK ){
		mosquitto_log_printf(MOSQ_LOG_ERR, "SELECT role,SQL error: %s\n", zErrMsg);
		sqlite3_free(zErrMsg);
		return rc;
	}

	struct dynsec__role *role, *role_tmp = NULL;
	HASH_ITER(hh, local_roles, role, role_tmp){
		char sql[128];
		snprintf(sql,128,"SELECT topic,acltype,priority,allow FROM acls WHERE rid=%lld",role->id);
		rc = sqlite3_exec(db, sql, select_acl_callback, role, &zErrMsg);
		if( rc != SQLITE_OK ){
			mosquitto_log_printf(MOSQ_LOG_ERR, "SELECT acl,SQL error: %s\n", zErrMsg);
			sqlite3_free(zErrMsg);
			return rc;
		}
	}
	HASH_SORT(local_roles, role_cmp);

	return rc;
}

int db_insert_acl(unsigned long long rid,char* topic,char* acltype,int priority, int allow){
	char *zErrMsg = 0;
	int  rc;
	char sql[256];
	snprintf(sql,255,"INSERT INTO acls (rid,topic,acltype,priority,allow) VALUES (%lld,%s,%s,%d,%d)",rid,topic,acltype,priority,allow);

	rc = sqlite3_exec(db, sql, NULL, NULL, &zErrMsg);
	if( rc != SQLITE_OK ){
		mosquitto_log_printf(MOSQ_LOG_ERR, "db_insert_acl SQL error: %s\n", zErrMsg);
		sqlite3_free(zErrMsg);
	}
	return rc;
}

int db_insert_role_acls(struct dynsec__role *role){
	struct dynsec__acl *acl,*acl_tmp;
	HASH_ITER(hh, role->acls.publish_c_send, acl, acl_tmp){
		db_insert_acl(role->id,acl->topic,ACL_TYPE_PUB_C_SEND,acl->priority,acl->allow);
	}
	HASH_ITER(hh, role->acls.publish_c_recv, acl, acl_tmp){
		db_insert_acl(role->id,acl->topic,ACL_TYPE_PUB_C_RECV,acl->priority,acl->allow);
	}
	HASH_ITER(hh, role->acls.subscribe_literal, acl, acl_tmp){
		db_insert_acl(role->id,acl->topic,ACL_TYPE_SUB_LITERAL,acl->priority,acl->allow);
	}
	HASH_ITER(hh, role->acls.subscribe_pattern, acl, acl_tmp){
		db_insert_acl(role->id,acl->topic,ACL_TYPE_SUB_PATTERN,acl->priority,acl->allow);
	}
	HASH_ITER(hh, role->acls.unsubscribe_literal, acl, acl_tmp){
		db_insert_acl(role->id,acl->topic,ACL_TYPE_UNSUB_LITERAL,acl->priority,acl->allow);
	}
	HASH_ITER(hh, role->acls.unsubscribe_pattern, acl, acl_tmp){
		db_insert_acl(role->id,acl->topic,ACL_TYPE_UNSUB_PATTERN,acl->priority,acl->allow);
	}
	return 0;
}

int db_insert_role(struct dynsec__role* role){
	char *zErrMsg = 0;
	int  rc;
	char sql[256];
	if(role->text_name == NULL && role->text_description == NULL)
		snprintf(sql,255,"INSERT INTO roles (id,rolename) VALUES (%lld,%s)",role->id,role->rolename);
	else if(role->text_name == NULL)
		snprintf(sql,255,"INSERT INTO roles (id,rolename,text_description) VALUES (%lld,%s,%s)",role->id,role->rolename,role->text_description);
	else if(role->text_description == NULL)
		snprintf(sql,255,"INSERT INTO roles (id,rolename,text_name) VALUES (%lld,%s,%s)",role->id,role->rolename,role->text_name);
	else
		snprintf(sql,255,"INSERT INTO roles (id,rolename,text_name,text_description) VALUES (%lld,%s,%s,%s)",role->id,role->rolename,role->text_name,role->text_description);

	rc = sqlite3_exec(db, sql, NULL, NULL, &zErrMsg);
	if( rc != SQLITE_OK ){
		mosquitto_log_printf(MOSQ_LOG_ERR, "db_insert_role,SQL error: %s\n", zErrMsg);
		sqlite3_free(zErrMsg);
	}

	return rc;
}

int db_set_role(unsigned long long id, char* text_name, char* text_description){
	char *zErrMsg = 0;
	int  rc;
	char sql[256];
	snprintf(sql,255,"UPDATE roles SET text_name = %s,text_description = %s WHERE id = %lld",text_name,text_description,id);

	rc = sqlite3_exec(db, sql, NULL, NULL, &zErrMsg);
	if( rc != SQLITE_OK ){
		mosquitto_log_printf(MOSQ_LOG_ERR, "db_set_role,SQL error: %s\n", zErrMsg);
		sqlite3_free(zErrMsg);
	}
	return rc;
}

int db_delete_role(unsigned long long id){
	char *zErrMsg = 0;
	int  rc;
	char sql[256];

	snprintf(sql,255,"DELETE FROM user_role WHERE rid=%lld",id);
	rc = sqlite3_exec(db, sql, NULL, NULL, &zErrMsg);
	if( rc != SQLITE_OK ){
		mosquitto_log_printf(MOSQ_LOG_ERR, "DELETE user_role,SQL error: %s\n", zErrMsg);
		sqlite3_free(zErrMsg);
		return rc;
	}

	snprintf(sql,255,"DELETE FROM group_role WHERE rid=%lld",id);
	rc = sqlite3_exec(db, sql, NULL, NULL, &zErrMsg);
	if( rc != SQLITE_OK ){
		mosquitto_log_printf(MOSQ_LOG_ERR, "DELETE group_role,SQL error: %s\n", zErrMsg);
		sqlite3_free(zErrMsg);
		return rc;
	}

	snprintf(sql,255,"DELETE FROM acls WHERE rid=%lld",id);
	rc = sqlite3_exec(db, sql, NULL, NULL, &zErrMsg);
	if( rc != SQLITE_OK ){
		mosquitto_log_printf(MOSQ_LOG_ERR, "DELETE acls,SQL error: %s\n", zErrMsg);
		sqlite3_free(zErrMsg);
		return rc;
	}

	snprintf(sql,255,"DELETE FROM roles WHERE id=%lld",id);
	rc = sqlite3_exec(db, sql, NULL, NULL, &zErrMsg);
	if( rc != SQLITE_OK ){
		mosquitto_log_printf(MOSQ_LOG_ERR, "DELETE role,SQL error: %s\n", zErrMsg);
		sqlite3_free(zErrMsg);
	}
	return rc;
}

int db_delete_acl(unsigned long long rid,char* topic,char* acltype){
	char *zErrMsg = 0;
	int  rc;
	char sql[256];
	if(topic == NULL && acltype == NULL)
		snprintf(sql,255,"DELETE FROM acls WHERE rid=%lld",rid);
	else if(topic != NULL)
		snprintf(sql,255,"DELETE FROM acls WHERE rid=%lld AND topic=%s",rid,topic);
	else if(acltype != NULL)
		snprintf(sql,255,"DELETE FROM acls WHERE rid=%lld AND acltype=%s",rid,acltype);
	else
		snprintf(sql,255,"DELETE FROM acls WHERE rid=%lld AND topic=%s and acltype=%s",rid,topic,acltype);
	rc = sqlite3_exec(db, sql, NULL, NULL, &zErrMsg);
	if( rc != SQLITE_OK ){
		mosquitto_log_printf(MOSQ_LOG_ERR, "db_delete_acl,SQL error: %s\n", zErrMsg);
		sqlite3_free(zErrMsg);
	}
	return rc;
}

/******************************************************************************************/
static int dynsec_roles__acl_load(cJSON *j_acls, const char *key, struct dynsec__acl **acllist)
{
	cJSON *j_acl;
	struct dynsec__acl *acl;

	cJSON_ArrayForEach(j_acl, j_acls){
		char *acltype;
		char *topic;

		json_get_string(j_acl, "acltype", &acltype, false);
		json_get_string(j_acl, "topic", &topic, false);

		if(!acltype || strcasecmp(acltype, key) != 0 || !topic){
			continue;
		}
		HASH_FIND(hh, *acllist, topic, strlen(topic), acl);
		if(acl){
			continue;
		}

		acl = mosquitto_calloc(1, sizeof(struct dynsec__acl));
		if(acl == NULL){
			return 1;
		}

		json_get_int(j_acl, "priority", &acl->priority, true, 0);
		json_get_bool(j_acl, "allow", &acl->allow, true, false);

		acl->topic = mosquitto_strdup(topic);
		if(acl->topic == NULL){
			mosquitto_free(acl);
			continue;
		}

		HASH_ADD_KEYPTR_INORDER(hh, *acllist, acl->topic, strlen(acl->topic), acl, insert_acl_cmp);
	}

	return 0;
}

int dynsec_roles__process_create(cJSON *j_responses, cJSON *command)
{
	char *rolename;
	char *text_name, *text_description;
	struct dynsec__role *role;
	int rc = MOSQ_ERR_SUCCESS;
	cJSON *j_acls;

	if(json_get_string(command, "rolename", &rolename, false) != MOSQ_ERR_SUCCESS){
		cJSON_AddStringToObject(j_responses,"info","Invalid/missing rolename");
		return MOSQ_ERR_INVAL;
	}
	if(mosquitto_validate_utf8(rolename, (int)strlen(rolename)) != MOSQ_ERR_SUCCESS){
		cJSON_AddStringToObject(j_responses,"info","Role name not valid UTF-8");
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

	role = dynsec_roles__find(rolename);
	if(role){
		cJSON_AddStringToObject(j_responses,"info","Role already exists");
		return MOSQ_ERR_SUCCESS;
	}

	role = mosquitto_calloc(1, sizeof(struct dynsec__role));
	if(role == NULL){
		cJSON_AddStringToObject(j_responses,"info","Internal error");
		return MOSQ_ERR_NOMEM;
	}
	role->rolename = mosquitto_strdup(rolename);
	if(role->rolename == NULL){
		cJSON_AddStringToObject(j_responses,"info","Internal error");
		rc = MOSQ_ERR_NOMEM;
		goto error;
	}
	if(text_name){
		role->text_name = mosquitto_strdup(text_name);
		if(role->text_name == NULL){
			cJSON_AddStringToObject(j_responses,"info","Internal error");
			rc = MOSQ_ERR_NOMEM;
			goto error;
		}
	}
	if(text_description){
		role->text_description = mosquitto_strdup(text_description);
		if(role->text_description == NULL){
			cJSON_AddStringToObject(j_responses,"info","Internal error");
			rc = MOSQ_ERR_NOMEM;
			goto error;
		}
	}

	/* ACLs */
	j_acls = cJSON_GetObjectItem(command, "acls");
	if(j_acls && cJSON_IsArray(j_acls)){
		if(dynsec_roles__acl_load(j_acls, ACL_TYPE_PUB_C_SEND, &role->acls.publish_c_send) != 0
				|| dynsec_roles__acl_load(j_acls, ACL_TYPE_PUB_C_RECV, &role->acls.publish_c_recv) != 0
				|| dynsec_roles__acl_load(j_acls, ACL_TYPE_SUB_LITERAL, &role->acls.subscribe_literal) != 0
				|| dynsec_roles__acl_load(j_acls, ACL_TYPE_SUB_PATTERN, &role->acls.subscribe_pattern) != 0
				|| dynsec_roles__acl_load(j_acls, ACL_TYPE_UNSUB_LITERAL, &role->acls.unsubscribe_literal) != 0
				|| dynsec_roles__acl_load(j_acls, ACL_TYPE_UNSUB_PATTERN, &role->acls.unsubscribe_pattern) != 0
				){

			cJSON_AddStringToObject(j_responses,"info","Internal error");
			rc = MOSQ_ERR_NOMEM;
			goto error;
		}
	}

	role->id = ++max_role_id;
	if(db_insert_role(role) == SQLITE_OK){
		db_insert_role_acls(role);
		HASH_ADD_KEYPTR_INORDER(hh, local_roles, role->rolename, strlen(role->rolename), role, role_cmp);
		cJSON_AddStringToObject(j_responses,"info","SUCCESS");
		return MOSQ_ERR_SUCCESS;
	}

error:
	if(role){
		role__free_item(role, false);
	}
	return rc;
}

static void role__remove_all_clients(struct dynsec__role *role)
{
	struct dynsec__clientlist *clientlist, *clientlist_tmp = NULL;

	HASH_ITER(hh, role->clientlist, clientlist, clientlist_tmp){
		mosquitto_kick_client_by_username(clientlist->client->username, false);

		dynsec_rolelist__client_remove(clientlist->client, role);
	}
}

static void role__remove_all_groups(struct dynsec__role *role)
{
	struct dynsec__grouplist *grouplist, *grouplist_tmp = NULL;

	HASH_ITER(hh, role->grouplist, grouplist, grouplist_tmp){
		if(grouplist->group == dynsec_anonymous_group){
			mosquitto_kick_client_by_username(NULL, false);
		}
		dynsec_clientlist__kick_all(grouplist->group->clientlist);

		dynsec_rolelist__group_remove(grouplist->group, role);
	}
}

int dynsec_roles__process_delete(cJSON *j_responses, cJSON *command)
{
	char *rolename;
	struct dynsec__role *role;

	if(json_get_string(command, "rolename", &rolename, false) != MOSQ_ERR_SUCCESS){
		cJSON_AddStringToObject(j_responses,"info","Invalid/missing rolename");
		return MOSQ_ERR_INVAL;
	}
	if(mosquitto_validate_utf8(rolename, (int)strlen(rolename)) != MOSQ_ERR_SUCCESS){
		cJSON_AddStringToObject(j_responses,"info","Role name not valid UTF-8");
		return MOSQ_ERR_INVAL;
	}

	role = dynsec_roles__find(rolename);
	if(role){
		role__remove_all_clients(role);
		role__remove_all_groups(role);

		db_delete_role(role->id);
		role__free_item(role, true);

		cJSON_AddStringToObject(j_responses,"info","SUCCESS");
		mosquitto_log_printf(MOSQ_LOG_INFO, "dynsec: deleteRole | rolename=%s", rolename);

		return MOSQ_ERR_SUCCESS;
	}else{
		cJSON_AddStringToObject(j_responses,"info","Role not found");
		return MOSQ_ERR_SUCCESS;
	}
}
static int add_single_acl_to_json(cJSON *j_array, const char *acl_type, struct dynsec__acl *acl)
{
	struct dynsec__acl *iter, *tmp = NULL;
	cJSON *j_acl;

	HASH_ITER(hh, acl, iter, tmp){
		j_acl = cJSON_CreateObject();
		if(j_acl == NULL){
			return 1;
		}
		cJSON_AddItemToArray(j_array, j_acl);

		if(cJSON_AddStringToObject(j_acl, "acltype", acl_type) == NULL
				|| cJSON_AddStringToObject(j_acl, "topic", iter->topic) == NULL
				|| cJSON_AddIntToObject(j_acl, "priority", iter->priority) == NULL
				|| cJSON_AddBoolToObject(j_acl, "allow", iter->allow) == NULL
				){

			return 1;
		}
	}


	return 0;
}

static int add_acls_to_json(cJSON *j_role, struct dynsec__role *role)
{
	cJSON *j_acls;

	if((j_acls = cJSON_AddArrayToObject(j_role, "acls")) == NULL){
		return 1;
	}

	if(add_single_acl_to_json(j_acls, ACL_TYPE_PUB_C_SEND, role->acls.publish_c_send) != MOSQ_ERR_SUCCESS
			|| add_single_acl_to_json(j_acls, ACL_TYPE_PUB_C_RECV, role->acls.publish_c_recv) != MOSQ_ERR_SUCCESS
			|| add_single_acl_to_json(j_acls, ACL_TYPE_SUB_LITERAL, role->acls.subscribe_literal) != MOSQ_ERR_SUCCESS
			|| add_single_acl_to_json(j_acls, ACL_TYPE_SUB_PATTERN, role->acls.subscribe_pattern) != MOSQ_ERR_SUCCESS
			|| add_single_acl_to_json(j_acls, ACL_TYPE_UNSUB_LITERAL, role->acls.unsubscribe_literal) != MOSQ_ERR_SUCCESS
			|| add_single_acl_to_json(j_acls, ACL_TYPE_UNSUB_PATTERN, role->acls.unsubscribe_pattern) != MOSQ_ERR_SUCCESS
			){

		return 1;
	}
	return 0;
}

static cJSON *add_role_to_json(struct dynsec__role *role, bool verbose)
{
	cJSON *j_role = NULL;

	if(verbose){
		j_role = cJSON_CreateObject();
		if(j_role == NULL){
			return NULL;
		}

		if(cJSON_AddStringToObject(j_role, "rolename", role->rolename) == NULL
				|| (role->text_name && cJSON_AddStringToObject(j_role, "textname", role->text_name) == NULL)
				|| (role->text_description && cJSON_AddStringToObject(j_role, "textdescription", role->text_description) == NULL)
				){

			cJSON_Delete(j_role);
			return NULL;
		}
		if(add_acls_to_json(j_role, role)){
			cJSON_Delete(j_role);
			return NULL;
		}
	}else{
		j_role = cJSON_CreateString(role->rolename);
		if(j_role == NULL){
			return NULL;
		}
	}
	return j_role;
}

int dynsec_roles__process_list(cJSON *j_responses, cJSON *command)
{
	bool verbose;
	struct dynsec__role *role, *role_tmp = NULL;
	cJSON *tree, *j_roles, *j_role, *j_data;
	int i, count, offset;

	json_get_bool(command, "verbose", &verbose, true, false);
	json_get_int(command, "count", &count, true, -1);
	json_get_int(command, "offset", &offset, true, 0);

	tree = cJSON_CreateObject();
	if(tree == NULL){
		cJSON_AddStringToObject(j_responses,"info","Internal error");
		return MOSQ_ERR_NOMEM;
	}

	if(cJSON_AddStringToObject(tree, "command", "listRoles") == NULL
			|| (j_data = cJSON_AddObjectToObject(tree, "data")) == NULL
			|| cJSON_AddIntToObject(j_data, "totalCount", (int)HASH_CNT(hh, local_roles)) == NULL
			|| (j_roles = cJSON_AddArrayToObject(j_data, "roles")) == NULL
			){

		cJSON_Delete(tree);
		cJSON_AddStringToObject(j_responses,"info","Internal error");
		return MOSQ_ERR_NOMEM;
	}

	i = 0;
	HASH_ITER(hh, local_roles, role, role_tmp){
		if(i>=offset){
			j_role = add_role_to_json(role, verbose);
			if(j_role == NULL){
				cJSON_Delete(tree);
				cJSON_AddStringToObject(j_responses,"info","Internal error");
				return MOSQ_ERR_NOMEM;
			}
			cJSON_AddItemToArray(j_roles, j_role);

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
	mosquitto_log_printf(MOSQ_LOG_INFO, "dynsec: listRoles | verbose=%s | count=%d | offset=%d",
			verbose?"true":"false", count, offset);

	return MOSQ_ERR_SUCCESS;
}


int dynsec_roles__process_add_acl(cJSON *j_responses, cJSON *command)
{
	char *rolename;
	char *topic;
	struct dynsec__role *role;
	struct dynsec__acl **acllist, *acl;
	int rc;
	char *acltype;

	if(json_get_string(command, "rolename", &rolename, false) != MOSQ_ERR_SUCCESS){
		cJSON_AddStringToObject(j_responses,"info","Invalid/missing rolename");
		return MOSQ_ERR_INVAL;
	}
	if(mosquitto_validate_utf8(rolename, (int)strlen(rolename)) != MOSQ_ERR_SUCCESS){
		cJSON_AddStringToObject(j_responses,"info","Role name not valid UTF-8");
		return MOSQ_ERR_INVAL;
	}

	role = dynsec_roles__find(rolename);
	if(role == NULL){
		cJSON_AddStringToObject(j_responses,"info","Role not found");
		return MOSQ_ERR_SUCCESS;
	}

	if(json_get_string(command, "acltype", &acltype, false) != MOSQ_ERR_SUCCESS){
		cJSON_AddStringToObject(j_responses,"info","Invalid/missing acltype");
		return MOSQ_ERR_SUCCESS;
	}
	if(!strcasecmp(acltype, ACL_TYPE_PUB_C_SEND)){
		acllist = &role->acls.publish_c_send;
	}else if(!strcasecmp(acltype, ACL_TYPE_PUB_C_RECV)){
		acllist = &role->acls.publish_c_recv;
	}else if(!strcasecmp(acltype, ACL_TYPE_SUB_LITERAL)){
		acllist = &role->acls.subscribe_literal;
	}else if(!strcasecmp(acltype, ACL_TYPE_SUB_PATTERN)){
		acllist = &role->acls.subscribe_pattern;
	}else if(!strcasecmp(acltype, ACL_TYPE_UNSUB_LITERAL)){
		acllist = &role->acls.unsubscribe_literal;
	}else if(!strcasecmp(acltype, ACL_TYPE_UNSUB_PATTERN)){
		acllist = &role->acls.unsubscribe_pattern;
	}else{
		cJSON_AddStringToObject(j_responses,"info","Unknown acltype");
		return MOSQ_ERR_SUCCESS;
	}

	if(json_get_string(command, "topic", &topic, false) == MOSQ_ERR_SUCCESS){
		if(mosquitto_validate_utf8(topic, (int)strlen(topic)) != MOSQ_ERR_SUCCESS){
			cJSON_AddStringToObject(j_responses,"info","Topic not valid UTF-8");
			return MOSQ_ERR_INVAL;
		}
		rc = mosquitto_sub_topic_check(topic);
		if(rc != MOSQ_ERR_SUCCESS){
			cJSON_AddStringToObject(j_responses,"info","Invalid ACL topic");
			return MOSQ_ERR_INVAL;
		}
	}else{
		cJSON_AddStringToObject(j_responses,"info","Invalid/missing topic");
		return MOSQ_ERR_SUCCESS;
	}

	HASH_FIND(hh, *acllist, topic, strlen(topic), acl);
	if(acl){
		cJSON_AddStringToObject(j_responses,"info","ACL with this topic already exists");
		return MOSQ_ERR_SUCCESS;
	}

	acl = mosquitto_calloc(1, sizeof(struct dynsec__acl));
	if(acl == NULL){
		cJSON_AddStringToObject(j_responses,"info","Internal error");
		return MOSQ_ERR_SUCCESS;
	}
	acl->topic = mosquitto_strdup(topic);
	if(acl->topic == NULL){
		mosquitto_free(acl);
		cJSON_AddStringToObject(j_responses,"info","Internal error");
		return MOSQ_ERR_SUCCESS;
	}

	json_get_int(command, "priority", &acl->priority, true, 0);
	json_get_bool(command, "allow", &acl->allow, true, false);

	HASH_ADD_KEYPTR_INORDER(hh, *acllist, acl->topic, strlen(acl->topic), acl, insert_acl_cmp);
	db_insert_acl(role->id,acl->topic,acltype,acl->priority,acl->allow);
	cJSON_AddStringToObject(j_responses,"info","SUCCESS");

	role__kick_all(role);
	mosquitto_log_printf(MOSQ_LOG_INFO, "dynsec: addRoleACL | rolename=%s | acltype=%s | topic=%s | priority=%d | allow=%s",
			rolename, acltype, topic, acl->priority, acl->allow?"true":"false");

	return MOSQ_ERR_SUCCESS;
}


int dynsec_roles__process_remove_acl(cJSON *j_responses, cJSON *command)
{
	char *rolename;
	struct dynsec__role *role;
	struct dynsec__acl **acllist, *acl;
	char *topic;
	char *acltype;
	int rc;

	if(json_get_string(command, "rolename", &rolename, false) != MOSQ_ERR_SUCCESS){
		cJSON_AddStringToObject(j_responses,"info","Invalid/missing rolename");
		return MOSQ_ERR_INVAL;
	}
	if(mosquitto_validate_utf8(rolename, (int)strlen(rolename)) != MOSQ_ERR_SUCCESS){
		cJSON_AddStringToObject(j_responses,"info","Role name not valid UTF-8");
		return MOSQ_ERR_INVAL;
	}

	role = dynsec_roles__find(rolename);
	if(role == NULL){
		cJSON_AddStringToObject(j_responses,"info","Role not found");
		return MOSQ_ERR_SUCCESS;
	}

	if(json_get_string(command, "acltype", &acltype, false) != MOSQ_ERR_SUCCESS){
		cJSON_AddStringToObject(j_responses,"info","Invalid/missing acltype");
		return MOSQ_ERR_SUCCESS;
	}
	if(!strcasecmp(acltype, ACL_TYPE_PUB_C_SEND)){
		acllist = &role->acls.publish_c_send;
	}else if(!strcasecmp(acltype, ACL_TYPE_PUB_C_RECV)){
		acllist = &role->acls.publish_c_recv;
	}else if(!strcasecmp(acltype, ACL_TYPE_SUB_LITERAL)){
		acllist = &role->acls.subscribe_literal;
	}else if(!strcasecmp(acltype, ACL_TYPE_SUB_PATTERN)){
		acllist = &role->acls.subscribe_pattern;
	}else if(!strcasecmp(acltype, ACL_TYPE_UNSUB_LITERAL)){
		acllist = &role->acls.unsubscribe_literal;
	}else if(!strcasecmp(acltype, ACL_TYPE_UNSUB_PATTERN)){
		acllist = &role->acls.unsubscribe_pattern;
	}else{
		cJSON_AddStringToObject(j_responses,"info","Unknown acltype");
		return MOSQ_ERR_SUCCESS;
	}

	if(json_get_string(command, "topic", &topic, false)){
		cJSON_AddStringToObject(j_responses,"info","Invalid/missing topic");
		return MOSQ_ERR_SUCCESS;
	}
	if(mosquitto_validate_utf8(topic, (int)strlen(topic)) != MOSQ_ERR_SUCCESS){
		cJSON_AddStringToObject(j_responses,"info","Topic not valid UTF-8");
		return MOSQ_ERR_INVAL;
	}
	rc = mosquitto_sub_topic_check(topic);
	if(rc != MOSQ_ERR_SUCCESS){
		cJSON_AddStringToObject(j_responses,"info","Invalid ACL topic");
		return MOSQ_ERR_INVAL;
	}

	HASH_FIND(hh, *acllist, topic, strlen(topic), acl);
	if(acl){
		role__free_acl(acllist, acl);
		db_delete_acl(role->id,topic,acltype);
		cJSON_AddStringToObject(j_responses,"info","SUCCESS");
		role__kick_all(role);

		mosquitto_log_printf(MOSQ_LOG_INFO, "dynsec: removeRoleACL | rolename=%s | acltype=%s | topic=%s",
				rolename, acltype, topic);

	}else{
		cJSON_AddStringToObject(j_responses,"info","ACL not found");
	}

	return MOSQ_ERR_SUCCESS;
}


int dynsec_roles__process_get(cJSON *j_responses, cJSON *command)
{
	char *rolename;
	struct dynsec__role *role;
	cJSON *tree, *j_role, *j_data;

	if(json_get_string(command, "rolename", &rolename, false) != MOSQ_ERR_SUCCESS){
		cJSON_AddStringToObject(j_responses,"info","Invalid/missing rolename");
		return MOSQ_ERR_INVAL;
	}
	if(mosquitto_validate_utf8(rolename, (int)strlen(rolename)) != MOSQ_ERR_SUCCESS){
		cJSON_AddStringToObject(j_responses,"info","Role name not valid UTF-8");
		return MOSQ_ERR_INVAL;
	}

	role = dynsec_roles__find(rolename);
	if(role == NULL){
		cJSON_AddStringToObject(j_responses,"info","Role not found");
		return MOSQ_ERR_SUCCESS;
	}

	tree = cJSON_CreateObject();
	if(tree == NULL){
		cJSON_AddStringToObject(j_responses,"info","Internal error");
		return MOSQ_ERR_NOMEM;
	}

	if(cJSON_AddStringToObject(tree, "command", "getRole") == NULL
			|| (j_data = cJSON_AddObjectToObject(tree, "data")) == NULL
			){

		cJSON_Delete(tree);
		cJSON_AddStringToObject(j_responses,"info","Internal error");
		return MOSQ_ERR_NOMEM;
	}

	j_role = add_role_to_json(role, true);
	if(j_role == NULL){
		cJSON_Delete(tree);
		cJSON_AddStringToObject(j_responses,"info","Internal error");
		return MOSQ_ERR_NOMEM;
	}
	cJSON_AddItemToObject(j_data, "role", j_role);
	cJSON_AddItemToArray(j_responses, tree);

	return MOSQ_ERR_SUCCESS;
}


int dynsec_roles__process_modify(cJSON *j_responses, cJSON *command)
{
	char *rolename;
	char *text_name, *text_description;
	struct dynsec__role *role;
	char *str;
	cJSON *j_acls;
	struct dynsec__acl *tmp_publish_c_send = NULL, *tmp_publish_c_recv = NULL;
	struct dynsec__acl *tmp_subscribe_literal = NULL, *tmp_subscribe_pattern = NULL;
	struct dynsec__acl *tmp_unsubscribe_literal = NULL, *tmp_unsubscribe_pattern = NULL;

	if(json_get_string(command, "rolename", &rolename, false) != MOSQ_ERR_SUCCESS){
		cJSON_AddStringToObject(j_responses,"info","Invalid/missing rolename");
		return MOSQ_ERR_INVAL;
	}
	if(mosquitto_validate_utf8(rolename, (int)strlen(rolename)) != MOSQ_ERR_SUCCESS){
		cJSON_AddStringToObject(j_responses,"info","Role name not valid UTF-8");
		return MOSQ_ERR_INVAL;
	}

	role = dynsec_roles__find(rolename);
	if(role == NULL){
		cJSON_AddStringToObject(j_responses,"info","Role does not exist");
		return MOSQ_ERR_INVAL;
	}

	if(json_get_string(command, "textname", &text_name, false) == MOSQ_ERR_SUCCESS){
		str = mosquitto_strdup(text_name);
		if(str == NULL){
			cJSON_AddStringToObject(j_responses,"info","Internal error");
			return MOSQ_ERR_NOMEM;
		}
		mosquitto_free(role->text_name);
		role->text_name = str;
	}

	if(json_get_string(command, "textdescription", &text_description, false) == MOSQ_ERR_SUCCESS){
		str = mosquitto_strdup(text_description);
		if(str == NULL){
			cJSON_AddStringToObject(j_responses,"info","Internal error");
			return MOSQ_ERR_NOMEM;
		}
		mosquitto_free(role->text_description);
		role->text_description = str;
	}

	j_acls = cJSON_GetObjectItem(command, "acls");
	if(j_acls && cJSON_IsArray(j_acls)){
		if(dynsec_roles__acl_load(j_acls, ACL_TYPE_PUB_C_SEND, &tmp_publish_c_send) != 0
				|| dynsec_roles__acl_load(j_acls, ACL_TYPE_PUB_C_RECV, &tmp_publish_c_recv) != 0
				|| dynsec_roles__acl_load(j_acls, ACL_TYPE_SUB_LITERAL, &tmp_subscribe_literal) != 0
				|| dynsec_roles__acl_load(j_acls, ACL_TYPE_SUB_PATTERN, &tmp_subscribe_pattern) != 0
				|| dynsec_roles__acl_load(j_acls, ACL_TYPE_UNSUB_LITERAL, &tmp_unsubscribe_literal) != 0
				|| dynsec_roles__acl_load(j_acls, ACL_TYPE_UNSUB_PATTERN, &tmp_unsubscribe_pattern) != 0
				){

			/* Free any that were successful */
			role__free_all_acls(&tmp_publish_c_send);
			role__free_all_acls(&tmp_publish_c_recv);
			role__free_all_acls(&tmp_subscribe_literal);
			role__free_all_acls(&tmp_subscribe_pattern);
			role__free_all_acls(&tmp_unsubscribe_literal);
			role__free_all_acls(&tmp_unsubscribe_pattern);

			cJSON_AddStringToObject(j_responses,"info","Internal error");
			return MOSQ_ERR_NOMEM;
		}

		role__free_all_acls(&role->acls.publish_c_send);
		role__free_all_acls(&role->acls.publish_c_recv);
		role__free_all_acls(&role->acls.subscribe_literal);
		role__free_all_acls(&role->acls.subscribe_pattern);
		role__free_all_acls(&role->acls.unsubscribe_literal);
		role__free_all_acls(&role->acls.unsubscribe_pattern);

		role->acls.publish_c_send = tmp_publish_c_send;
		role->acls.publish_c_recv = tmp_publish_c_recv;
		role->acls.subscribe_literal = tmp_subscribe_literal;
		role->acls.subscribe_pattern = tmp_subscribe_pattern;
		role->acls.unsubscribe_literal = tmp_unsubscribe_literal;
		role->acls.unsubscribe_pattern = tmp_unsubscribe_pattern;
	}

	db_set_role(role->id,role->text_name,role->text_description);
	db_delete_acl(role->id,NULL,NULL);
	db_insert_role_acls(role);
	role__kick_all(role);

	cJSON_AddStringToObject(j_responses,"info","SUCCESS");
	mosquitto_log_printf(MOSQ_LOG_INFO, "dynsec: modifyRole | rolename=%s",rolename);

	return MOSQ_ERR_SUCCESS;
}

int roleCommand(int argc,char** argv,cJSON** j_responses){
    cJSON* j_tree;
    j_tree = cJSON_CreateObject();
	if(j_tree == NULL){
		return HTTP_STATUS_INTERNAL_SERVER_ERROR;
	}
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
    if(strcasecmp(subCommand,"list") == 0){
    	dynsec_roles__process_list(j_tree, j_command);
    }else if(strcasecmp(subCommand,"get") == 0){
    	dynsec_roles__process_get(j_tree,j_command);
    }else if(strcasecmp(subCommand,"modify") == 0){
    	dynsec_roles__process_modify(j_tree, j_command);
    }else if(strcasecmp(subCommand,"create") == 0){
    	dynsec_roles__process_create(j_tree,j_command);
    }else if(strcasecmp(subCommand,"delete") == 0){
    	dynsec_roles__process_delete(j_tree, j_command);
    }else if(strcasecmp(subCommand,"aclAdd") == 0){
    	dynsec_roles__process_add_acl(j_tree, j_command);
    }else if(strcasecmp(subCommand,"aclRemove") == 0){
    	dynsec_roles__process_remove_acl(j_tree, j_command);
    }else{
    	cJSON_AddStringToObject(j_tree,"info","Unknown command");
    }
    cJSON_Delete(j_command);

    *j_responses = j_tree;
    return 0;
}
