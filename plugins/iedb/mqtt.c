#include "config.h"
#include "util.h"

#include <cjson/cJSON.h>
#include <stdio.h>
#include <uthash.h>
#include <sqlite3.h>
#include "mosquitto.h"
#include "mosquitto_broker.h"
#include "json_help.h"

extern sqlite3 *db;
long long start_time;

int db_mqtt_client_init(void){
	start_time = mstime();

	char *zErrMsg = 0;
	int  rc;
	char* sql = "CREATE TABLE IF NOT EXISTS mqtt_client("  \
	         "sn				TEXT PRIMARY KEY," \
			 "client_id 		TEXT NOT NULL," \
			 "text_name 		TEXT," \
			 "disabled 			BOOLEAN," \
			 "disconnected_time INTEGER," \
	         "connected_time 	INTEGER NOT NULL)WITHOUT ROWID;";

	rc = sqlite3_exec(db, sql, NULL, 0, &zErrMsg);
	if( rc != SQLITE_OK ){
		mosquitto_log_printf(MOSQ_LOG_ERR, "CREATE TABLE mqtt_client,SQL error: %s\n", zErrMsg);
		sqlite3_free(zErrMsg);
		return rc;
	}

	return rc;
}

int db_set_mqtt_client_connected(char* sn,char* client_id,long long ts){
    sqlite3_stmt *stmt;
    int rc = sqlite3_prepare_v2(db, "INSERT INTO mqtt_client (sn,client_id,connected_time) VALUES (?1,?2,?3) ON CONFLICT(sn) DO UPDATE SET connected_time=?3,client_id=?2", -1, &stmt, NULL);
    if(rc == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, sn, -1, SQLITE_STATIC);
        sqlite3_bind_text(stmt, 2, client_id, -1, SQLITE_STATIC);
        sqlite3_bind_int64(stmt, 3, ts);
        rc = sqlite3_step(stmt);
        if(rc != SQLITE_DONE) {
        	mosquitto_log_printf(MOSQ_LOG_ERR, "db_set_mqtt_client_connected SQL error: %s\n", sqlite3_errmsg(db));
        }
        sqlite3_finalize(stmt);
    }

	return rc;
}

int db_set_mqtt_client_disconnected(char* sn,long long ts){
    sqlite3_stmt *stmt;
    int rc = sqlite3_prepare_v2(db, "UPDATE mqtt_client SET disconnected_time = ?2 WHERE sn = ?1", -1, &stmt, NULL);
    if(rc == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, sn, -1, SQLITE_STATIC);
        sqlite3_bind_int64(stmt, 2, ts);
        rc = sqlite3_step(stmt);
        if(rc != SQLITE_DONE) {
        	mosquitto_log_printf(MOSQ_LOG_ERR, "db_set_mqtt_client_disconnected SQL error: %s\n", sqlite3_errmsg(db));
        }
        sqlite3_finalize(stmt);
    }

	return rc;
}

int db_set_mqtt_client_name(char* sn,char* text_name){
    sqlite3_stmt *stmt;
    int rc = sqlite3_prepare_v2(db, "UPDATE mqtt_client SET text_name = ?2 WHERE sn = ?1", -1, &stmt, NULL);
    if(rc == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, sn, -1, SQLITE_STATIC);
        sqlite3_bind_text(stmt, 2, text_name, -1, SQLITE_STATIC);
        rc = sqlite3_step(stmt);
        if(rc != SQLITE_DONE) {
        	mosquitto_log_printf(MOSQ_LOG_ERR, "db_set_mqtt_client_disconnected SQL error: %s\n", sqlite3_errmsg(db));
        }
        sqlite3_finalize(stmt);
    }

	return rc;
}

int db_select_mqtt_client(cJSON* j_tree){
	sqlite3_stmt *pSelectStmt;
	int rc = sqlite3_prepare_v2(db,"SELECT sn,client_id,text_name,connected_time,disconnected_time  FROM mqtt_client",-1, &pSelectStmt, 0);
	if( rc != SQLITE_OK || pSelectStmt == NULL){
		cJSON_AddStringToObject(j_tree,"error","sqlite error");
		mosquitto_log_printf(MOSQ_LOG_ERR, "sqlite3_prepare_v2 error: %s\n", sqlite3_errmsg(db));
    	return -1;
	}

	while( sqlite3_step(pSelectStmt)==SQLITE_ROW ){
		char* sn = sqlite3_column_text(pSelectStmt,0);
		char* client_id = sqlite3_column_text(pSelectStmt,1);
		char* text_name = sqlite3_column_text(pSelectStmt,2);
		long long connected_time = sqlite3_column_int64(pSelectStmt,3);
		long long disconnected_time = sqlite3_column_int64(pSelectStmt,4);

	    cJSON* j_row = cJSON_CreateObject();
	    cJSON_AddStringToObject(j_row,"sn",sn);
	    cJSON_AddStringToObject(j_row,"client_id",client_id);
	    if(text_name)
	    	cJSON_AddStringToObject(j_row,"name",text_name);
	    if(connected_time >= start_time){
	    	if(disconnected_time < connected_time)
	    		cJSON_AddTrueToObject(j_row,"online");
	    	else
	    		cJSON_AddFalseToObject(j_row,"online");
	    }else{
	    	cJSON_AddFalseToObject(j_row,"online");
	    }
	    cJSON_AddNumberToObject(j_row,"online_timestamp",connected_time);
	    cJSON_AddItemToArray(j_tree,j_row);
	}
	sqlite3_finalize(pSelectStmt);
	return 0;
}

int mqttCommand(int argc,char** argv,cJSON** j_responses){
    cJSON* j_tree;
    if (argc < 1) {
    	j_tree = cJSON_CreateObject();
    	cJSON_AddStringToObject(j_tree,"info","Unknown sub command");
    	*j_responses = j_tree;
        return 0;
    }

    char* command = argv[0];
    if(!strcasecmp(command, "listClients")){
    	j_tree = cJSON_CreateArray();
    	db_select_mqtt_client(j_tree);
	}else if(!strcasecmp(command, "setClientName")){
		j_tree = cJSON_CreateObject();
	    if (argc < 2) {
	    	cJSON_AddStringToObject(j_tree,"info","missing sn");
	    }else if (argc < 3) {
	    	cJSON_AddStringToObject(j_tree,"info","missing name");
	    }else{
		    db_set_mqtt_client_name(argv[1],argv[2]);
		    cJSON_AddStringToObject(j_tree,"info","OK");
	    }
	}else if(!strcasecmp(command, "publishMessage")){
		j_tree = cJSON_CreateObject();
	    if (argc < 2) {
	    	cJSON_AddStringToObject(j_tree,"info","missing content");
	    }

		cJSON *j_content = cJSON_ParseWithLength(argv[1], strlen(argv[1]));
		if(!cJSON_IsObject(j_content)){
			cJSON_AddStringToObject(j_tree,"info","Invalid json format");
		}else{
			char* topic;
			char* payload;
			if(json_get_string(j_content, "topic", &topic, false) != MOSQ_ERR_SUCCESS){
				cJSON_AddStringToObject(j_tree,"info","Invalid/missing topic");
			}else if(json_get_string(j_content, "payload", &payload, false) != MOSQ_ERR_SUCCESS){
				cJSON_AddStringToObject(j_tree,"info","Invalid/missing message");
			}else if(strncmp(topic,"/edge/property/",15)){
				cJSON_AddStringToObject(j_tree,"info","forbidden topic");
			}else{
				size_t len = strlen(topic);
				if(strcmp(topic + len - 4,"/set")){
					cJSON_AddStringToObject(j_tree,"info","forbidden topic");
				}else{
					size_t sn_len = len-15-4;
					char* sn = mosquitto_malloc(sn_len+1);
					memcpy(sn,topic+15,sn_len);
					sn[sn_len] = 0;

				    sqlite3_stmt *stmt;
				    if(sqlite3_prepare_v2(db, "SELECT client_id,connected_time,disconnected_time FROM mqtt_client WHERE sn = ? LIMIT 1", -1, &stmt, NULL) == SQLITE_OK) {
				        sqlite3_bind_text(stmt, 1, sn, -1, SQLITE_STATIC);
				        if(sqlite3_step(stmt) == SQLITE_ROW) {
							char* client_id = sqlite3_column_text(stmt,0);
							long long connected_time = sqlite3_column_int64(stmt,1);
							long long disconnected_time = sqlite3_column_int64(stmt,2);
							if(connected_time >= start_time && connected_time > disconnected_time){
								int qos;
								json_get_int(j_content, "qos", &qos, true, 0);
								int res = mosquitto_broker_publish_copy(client_id,topic,strlen(payload),payload,qos,0,NULL);
								if(res == MOSQ_ERR_SUCCESS)
									cJSON_AddStringToObject(j_tree,"info","OK");
								else if(res == MOSQ_ERR_INVAL)
									cJSON_AddStringToObject(j_tree,"info","payloadlen < 0 or qos is not 0, 1, or 2");
								else
									cJSON_AddStringToObject(j_tree,"info","out of memory");
							}else{
								cJSON_AddStringToObject(j_tree,"info","client offline");
							}
				        }else{
				        	cJSON_AddStringToObject(j_tree,"info","Invalid SN");
				        }
				        sqlite3_finalize(stmt);
				    }else{
				    	cJSON_AddStringToObject(j_tree,"info","db error");
				    }

				    mosquitto_free(sn);
				}
			}
		}
		cJSON_Delete(j_content);
	}else{
		j_tree = cJSON_CreateObject();
    	cJSON_AddStringToObject(j_tree,"info","Unknown command");
    }

    *j_responses = j_tree;
    return 0;
}

void handle_client_auth(char* sn,char* client_id){
	long long ts = mstime();
	db_set_mqtt_client_connected(sn,client_id,ts);
}

int mqtt_disconnect_callback(int event, void *event_data, void *userdata){
	struct mosquitto_evt_disconnect *event_disconnect = event_data;
	if(event_disconnect->client){
		char* username = mosquitto_client_username(event_disconnect->client);
		if(username){
		    char* sn = strchr(username, '|');
		    if(sn != NULL){
		        sn += 1;
		        long long ts = mstime();
		    	db_set_mqtt_client_disconnected(sn,ts);
		    }
		}
	}


	return MOSQ_ERR_SUCCESS;
}
