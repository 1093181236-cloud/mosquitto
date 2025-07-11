/*
 * tag.c
 *
 *  Created on: 2025Äê7ÔÂ3ÈÕ
 *      Author: a
 */

#include "config.h"
#include "util.h"

#include <cjson/cJSON.h>
#include <stdio.h>
#include <sqlite3.h>
#include <string.h>

#include "mosquitto.h"
#include "mosquitto_broker.h"
#include "json_help.h"
#include "rax.h"
#include "device.h"

extern sqlite3 *sqlite3_db;
extern rax* devices;
int db_tag_init(void){
	char *zErrMsg = 0;
	int  rc;
	char* sql = "CREATE TABLE IF NOT EXISTS tags("  \
	         "did 			INTEGER  NOT NULL," \
			 "name 			TEXT NOT NULL," \
			 "value 		TEXT," \
			 "PRIMARY KEY (did,name))WITHOUT ROWID;";

	rc = sqlite3_exec(sqlite3_db, sql, NULL, 0, &zErrMsg);
	if( rc != SQLITE_OK ){
		mosquitto_log_printf(MOSQ_LOG_ERR, "CREATE TABLE tags,SQL error: %s\n", zErrMsg);
		sqlite3_free(zErrMsg);
		return rc;
	}

	return rc;
}

int db_set_device_tag(int did,char* tname,char* tvalue){
    sqlite3_stmt *stmt;
    int rc = sqlite3_prepare_v2(sqlite3_db, "INSERT INTO tags (did,name,value) VALUES (?1,?2,?3) ON CONFLICT(did,name) DO UPDATE SET value=?3", -1, &stmt, NULL);
    if(rc == SQLITE_OK) {
        sqlite3_bind_int64(stmt, 1, did);
        sqlite3_bind_text(stmt, 2, tname, -1, SQLITE_STATIC);
        sqlite3_bind_text(stmt, 3, tvalue, -1, SQLITE_STATIC);
        rc = sqlite3_step(stmt);
        if(rc != SQLITE_DONE) {
        	mosquitto_log_printf(MOSQ_LOG_ERR, "db_set_device_tag SQL error: %s\n", sqlite3_errmsg(sqlite3_db));
        }
        sqlite3_finalize(stmt);
    }

	return rc;
}

int db_list_devices(char* tname,char* tvalue,cJSON* j_tree){
	cJSON *list = cJSON_CreateArray();
	cJSON_AddItemToObject(j_tree,"devices",list);

	sqlite3_stmt *pSelectStmt;
	int rc = sqlite3_prepare_v2(sqlite3_db,"SELECT device.name FROM device INNER JOIN tags ON tags.did=device.id WHERE tags.name=?1 AND tags.value=?2",-1, &pSelectStmt, 0);
	if( rc != SQLITE_OK || pSelectStmt == NULL){
		cJSON_AddStringToObject(j_tree,"error","sqlite error");
		mosquitto_log_printf(MOSQ_LOG_ERR, "sqlite3_prepare_v2 error: %s\n", sqlite3_errmsg(sqlite3_db));
    	return -1;
	}

    sqlite3_bind_text(pSelectStmt, 1, tname, -1, SQLITE_STATIC);
    sqlite3_bind_text(pSelectStmt, 2, tvalue, -1, SQLITE_STATIC);
	while( sqlite3_step(pSelectStmt)==SQLITE_ROW ){
		const char* dname = sqlite3_column_text(pSelectStmt,0);
	    cJSON* j_row = cJSON_CreateString(dname);
	    cJSON_AddItemToArray(list,j_row);
	}
	sqlite3_finalize(pSelectStmt);
	return 0;
}

int db_list_tags(int did,cJSON* j_tree){
	sqlite3_stmt *pSelectStmt;
	int rc = sqlite3_prepare_v2(sqlite3_db,"SELECT name,value FROM tags WHERE did=?1",-1, &pSelectStmt, 0);
	if( rc != SQLITE_OK || pSelectStmt == NULL){
		cJSON_AddStringToObject(j_tree,"error","sqlite error");
		mosquitto_log_printf(MOSQ_LOG_ERR, "sqlite3_prepare_v2 error: %s\n", sqlite3_errmsg(sqlite3_db));
    	return -1;
	}

	sqlite3_bind_int64(pSelectStmt, 1, did);
	while( sqlite3_step(pSelectStmt)==SQLITE_ROW ){
		const char* name = sqlite3_column_text(pSelectStmt,0);
		const char* value = sqlite3_column_text(pSelectStmt,1);
		cJSON_AddStringToObject(j_tree,name,value);
	}
	sqlite3_finalize(pSelectStmt);
	return 0;
}

int db_get_device_tag(int did,char* tname,cJSON* j_tree){
	sqlite3_stmt *pSelectStmt;
	int rc = sqlite3_prepare_v2(sqlite3_db,"SELECT value FROM tags WHERE did=?1 AND name=?2",-1, &pSelectStmt, 0);
	if( rc != SQLITE_OK || pSelectStmt == NULL){
		cJSON_AddStringToObject(j_tree,"error","sqlite error");
		mosquitto_log_printf(MOSQ_LOG_ERR, "sqlite3_prepare_v2 error: %s\n", sqlite3_errmsg(sqlite3_db));
    	return -1;
	}

    sqlite3_bind_int64(pSelectStmt, 1, did);
    sqlite3_bind_text(pSelectStmt, 2, tname, -1, SQLITE_STATIC);
	if( sqlite3_step(pSelectStmt)==SQLITE_ROW ){
		const char* value = sqlite3_column_text(pSelectStmt,0);
		cJSON_AddStringToObject(j_tree,"value",value);
	}else{
		cJSON_AddNullToObject(j_tree,"value");
	}
	sqlite3_finalize(pSelectStmt);
	return 0;
}

int db_remove_device_tag(int did,char* tname,cJSON* j_tree){
	sqlite3_stmt *pSelectStmt;
	int rc = sqlite3_prepare_v2(sqlite3_db,"DELETE FROM tags WHERE did=?1 AND name=?2",-1, &pSelectStmt, 0);
	if( rc != SQLITE_OK || pSelectStmt == NULL){
		cJSON_AddStringToObject(j_tree,"error","sqlite error");
		mosquitto_log_printf(MOSQ_LOG_ERR, "sqlite3_prepare_v2 error: %s\n", sqlite3_errmsg(sqlite3_db));
    	return -1;
	}

    sqlite3_bind_int64(pSelectStmt, 1, did);
    sqlite3_bind_text(pSelectStmt, 2, tname, -1, SQLITE_STATIC);
	if(sqlite3_step(pSelectStmt) != SQLITE_DONE){
		mosquitto_log_printf(MOSQ_LOG_ERR, "SQL error: %s\n", sqlite3_errmsg(sqlite3_db));
	}
	sqlite3_finalize(pSelectStmt);
	return 0;
}

int tagCommand(int argc,char** argv,cJSON** j_responses){
    cJSON* j_tree = cJSON_CreateObject();
    if (argc < 1) {
    	cJSON_AddStringToObject(j_tree,"info","Unknown sub command");
    	*j_responses = j_tree;
        return 0;
    }

    char* command = argv[0];
    if(!strcasecmp(command, "listDevices")){
	    if (argc < 2) {
	    	cJSON_AddStringToObject(j_tree,"info","missing tag name");
	    }else if (argc < 3) {
	    	cJSON_AddStringToObject(j_tree,"info","missing tag value");
	    }else{
		    db_list_devices(argv[1],argv[2],j_tree);
	    }
	}else if(!strcasecmp(command, "listTags")){
	    if (argc < 2) {
	    	cJSON_AddStringToObject(j_tree,"info","missing device name");
	    }else{
	        device *d = raxFind(devices,(unsigned char*)argv[1],strlen(argv[1]));
	        if (d == raxNotFound){
	        	cJSON_AddStringToObject(j_tree,"info","device not found");
	        }else{
	        	db_list_tags(d->id,j_tree);
	        }
	    }
	}else if(!strcasecmp(command, "getTag")){
	    if (argc < 2) {
	    	cJSON_AddStringToObject(j_tree,"info","missing device name");
	    }else if (argc < 3) {
	    	cJSON_AddStringToObject(j_tree,"info","missing tag name");
	    }else{
	        device *d = raxFind(devices,(unsigned char*)argv[1],strlen(argv[1]));
	        if (d == raxNotFound){
	        	cJSON_AddStringToObject(j_tree,"info","device not found");
	        }else{
	        	db_get_device_tag(d->id,argv[2],j_tree);
	        }
	    }
	}else if(!strcasecmp(command, "removeTag")){
	    if (argc < 2) {
	    	cJSON_AddStringToObject(j_tree,"info","missing device name");
	    }else if (argc < 3) {
	    	cJSON_AddStringToObject(j_tree,"info","missing tag name");
	    }else{
	        device *d = raxFind(devices,(unsigned char*)argv[1],strlen(argv[1]));
	        if (d == raxNotFound){
	        	cJSON_AddStringToObject(j_tree,"info","device not found");
	        }else{
	        	db_remove_device_tag(d->id,argv[2],j_tree);
	        	cJSON_AddStringToObject(j_tree,"info","OK");
	        }
	    }
	}else if(!strcasecmp(command, "addTag")){
	    if (argc < 2) {
	    	cJSON_AddStringToObject(j_tree,"info","missing device name");
	    }else if (argc < 3) {
	    	cJSON_AddStringToObject(j_tree,"info","missing tag name");
	    }else if (argc < 4) {
	    	cJSON_AddStringToObject(j_tree,"info","missing tag value");
	    }else{
	        device *d = raxFind(devices,(unsigned char*)argv[1],strlen(argv[1]));
	        if (d == raxNotFound){
	        	cJSON_AddStringToObject(j_tree,"info","device not found");
	        }else{
	        	db_set_device_tag(d->id,argv[2],argv[3]);
			    cJSON_AddStringToObject(j_tree,"info","OK");
	        }
	    }
	}else{
    	cJSON_AddStringToObject(j_tree,"info","Unknown command");
    }

    *j_responses = j_tree;
    return 0;
}
