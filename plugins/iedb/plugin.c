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

#include <cjson/cJSON.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#ifndef WIN32
#  include <strings.h>
#endif

#include "json_help.h"
#include "mosquitto.h"
#include "mosquitto_broker.h"
#include "mosquitto_plugin.h"
#include "mqtt_protocol.h"
#include "util.h"
#include "http.h"

#include "dynamic_security.h"

static mosquitto_plugin_id_t *plg_id = NULL;
char *iedb_dir = NULL;
unsigned long long ts_retention_hours = 24;
unsigned long long ts_retention_mbs = 100;
struct dynsec__acl_default_access default_access = {false, false, false, false};

#ifdef WIN32
#  include <winsock2.h>
#  include <aclapi.h>
#  include <io.h>
#  include <lmcons.h>
#  include <fcntl.h>
#  define PATH_MAX MAX_PATH
#else
#  include <sys/stat.h>
#  include <pwd.h>
#  include <grp.h>
#  include <unistd.h>
#endif

int db_open(void);
int db_close(void);
int mqtt_basic_auth_callback(int event, void *event_data, void *userdata);
int mqtt_message_callback(int event, void *event_data, void *userdata);
int mqtt_tick_callback(int event, void *event_data, void *userdata);
int dynsec__acl_check_callback(int event, void *event_data, void *userdata);
int mqtt_disconnect_callback(int event, void *event_data, void *userdata);

int tsAggRevQueryCommand(int argc,char** argv,cJSON** j_responses);
int tsAggQueryCommand(int argc,char** argv,cJSON** j_responses);
int tsRevQueryCommand(int argc,char** argv,cJSON** j_responses);
int tsQueryCommand(int argc,char** argv,cJSON** j_responses);
int tsFLQueryCommand(int argc,char** argv,cJSON** j_responses);
int tsLastQueryCommand(int argc,char** argv,cJSON** j_responses);
int tsDiffQueryCommand(int argc,char** argv,cJSON** j_responses);
int tsScanCommand(int argc,char** argv,cJSON** tree);
int tsDelRuleCommand(int argc,char** argv,cJSON** j_responses);
int tsAddRuleCommand(int argc,char** argv,cJSON** j_responses);
int tsAllDevicesCommand(int argc,char** argv,cJSON** j_responses);
int tsAllFieldsCommand(int argc,char** argv,cJSON** j_responses);
int tsAllDevicesFiledsCommand(int argc,char** argv,cJSON** j_responses);

int roleCommand(int argc,char** argv,cJSON** j_responses);
int clientCommand(int argc,char** argv,cJSON** j_responses);
int groupCommand(int argc,char** argv,cJSON** j_responses);
void dynsec_groups__cleanup(void);
void dynsec_clients__cleanup(void);
void dynsec_roles__cleanup(void);

int uploadCommand(int argc,char** argv,cJSON** j_responses);
int newdeviceCommand(int argc,char** argv,cJSON** j_responses);
int deldeviceCommand(int argc,char** argv,cJSON** j_responses);
int tsBoolQueryCommand(int argc,char** argv,cJSON** j_responses);
int tsRangeQueryCommand(int argc,char** argv,cJSON** j_responses);

int mqttCommand(int argc,char** argv,cJSON** j_responses);
int tagCommand(int argc,char** argv,cJSON** j_responses);

int mosquitto_plugin_version(int supported_version_count, const int *supported_versions)
{
	int i;

	for(i=0; i<supported_version_count; i++){
		if(supported_versions[i] == 5){
			return 5;
		}
	}
	return -1;
}

int mosquitto_plugin_init(mosquitto_plugin_id_t *identifier, void **user_data, struct mosquitto_opt *options, int option_count)
{
	int i;
	int rc;

	UNUSED(user_data);

	for(i=0; i<option_count; i++){
		if(!strcasecmp(options[i].key, "iedb_dir")){
#ifdef WIN32
			iedb_dir = _fullpath(NULL, options[i].value, 0);
#else
			iedb_dir = realpath(options[i].value, NULL);
#endif
			if(iedb_dir == NULL){
				mosquitto_log_printf(MOSQ_LOG_ERR, "Error: iedb_dir[%s] does not exist.",options[i].value);
				return MOSQ_ERR_NOMEM;
			}

		}else if(!strcasecmp(options[i].key, "iedb_retention_hours")){
			string2ull(options[i].value,&ts_retention_hours);
		}else if(!strcasecmp(options[i].key, "iedb_retention_mbs")){
			string2ull(options[i].value,&ts_retention_mbs);
		}
	}
	if(iedb_dir == NULL){
		mosquitto_log_printf(MOSQ_LOG_WARNING, "Warning: iedb plugin has no plugin_opt_iedb_dir defined. The plugin will not be activated.");
		return MOSQ_ERR_SUCCESS;
	}

	plg_id = identifier;

	rc = db_open();
	if(rc != MOSQ_ERR_SUCCESS){
		goto error;
	}
	rc = mosquitto_callback_register(plg_id, MOSQ_EVT_BASIC_AUTH, mqtt_basic_auth_callback, NULL, NULL);
	if(rc == MOSQ_ERR_ALREADY_EXISTS){
		mosquitto_log_printf(MOSQ_LOG_ERR, "Error: iedb plugin can only be loaded once.");
		goto error;
	}else if(rc == MOSQ_ERR_NOMEM){
		mosquitto_log_printf(MOSQ_LOG_ERR, "Error: Out of memory.");
		goto error;
	}else if(rc != MOSQ_ERR_SUCCESS){
		goto error;
	}

	rc = mosquitto_callback_register(plg_id, MOSQ_EVT_MESSAGE, mqtt_message_callback, NULL, NULL);
	if(rc == MOSQ_ERR_ALREADY_EXISTS){
		mosquitto_log_printf(MOSQ_LOG_ERR, "Error: iedb plugin can only be loaded once.");
		goto error;
	}else if(rc == MOSQ_ERR_NOMEM){
		mosquitto_log_printf(MOSQ_LOG_ERR, "Error: Out of memory.");
		goto error;
	}else if(rc != MOSQ_ERR_SUCCESS){
		goto error;
	}

	rc = mosquitto_callback_register(plg_id, MOSQ_EVT_TICK, mqtt_tick_callback, NULL, NULL);
	if(rc == MOSQ_ERR_ALREADY_EXISTS){
		mosquitto_log_printf(MOSQ_LOG_ERR, "Error: iedb plugin can only be loaded once.");
		goto error;
	}else if(rc == MOSQ_ERR_NOMEM){
		mosquitto_log_printf(MOSQ_LOG_ERR, "Error: Out of memory.");
		goto error;
	}else if(rc != MOSQ_ERR_SUCCESS){
		goto error;
	}

	rc = mosquitto_callback_register(plg_id, MOSQ_EVT_DISCONNECT, mqtt_disconnect_callback, NULL, NULL);
	if(rc == MOSQ_ERR_ALREADY_EXISTS){
		mosquitto_log_printf(MOSQ_LOG_ERR, "Error: iedb plugin can only be loaded once.");
		goto error;
	}else if(rc == MOSQ_ERR_NOMEM){
		mosquitto_log_printf(MOSQ_LOG_ERR, "Error: Out of memory.");
		goto error;
	}else if(rc != MOSQ_ERR_SUCCESS){
		goto error;
	}

	url_register("TSQUERY",tsQueryCommand);
	url_register("TSREVQUERY",tsRevQueryCommand);
	url_register("TSAGGQUERY",tsAggQueryCommand);
	url_register("TSREVAGGQUERY",tsAggRevQueryCommand);
	url_register("TSFLQUERY",tsFLQueryCommand);
	url_register("TSLASTQUERY",tsLastQueryCommand);
	url_register("TSDIFFQUERY",tsDiffQueryCommand);

	url_register("TSSCAN",tsScanCommand);
	url_register("TSDELRULE",tsDelRuleCommand);
	url_register("TSADDRULE",tsAddRuleCommand);
	url_register("ALLDEVICES",tsAllDevicesCommand);
	url_register("ALLFIELDS",tsAllFieldsCommand);
	url_register("ALLDEVICESFIELDS",tsAllDevicesFiledsCommand);
	url_register("UPLOAD",uploadCommand);
	url_register("NEWDEVICE",newdeviceCommand);
	url_register("DELDEVICE",deldeviceCommand);
	url_register("TSBOOLQUERY",tsBoolQueryCommand);
	url_register("TSRANGEQUERY",tsRangeQueryCommand);

	url_register("role",roleCommand);
	url_register("user",clientCommand);
	url_register("group",groupCommand);

	url_register("mqtt",mqttCommand);
	url_register("tag",tagCommand);

	return MOSQ_ERR_SUCCESS;
error:
	free(iedb_dir);
	iedb_dir = NULL;
	return rc;
}

int mosquitto_plugin_cleanup(void *user_data, struct mosquitto_opt *options, int option_count)
{
	UNUSED(user_data);
	UNUSED(options);
	UNUSED(option_count);

	url_unregister("TSQUERY");
	url_unregister("TSREVQUERY");
	url_unregister("TSAGGQUERY");
	url_unregister("TSAGGREVQUERY");
	url_unregister("TSFLQUERY");
	url_unregister("TSLASTQUERY");
	url_unregister("TSDIFFQUERY");

	url_unregister("TSSCAN");
	url_unregister("TSDELRULE");
	url_unregister("TSADDRULE");
	url_unregister("ALLDEVICES");
	url_unregister("ALLDEVICESFIELDS");
	url_unregister("UPLOAD");
	url_unregister("NEWDEVICE");
	url_unregister("DELDEVICE");
	url_unregister("TSBOOLQUERY");

	url_unregister("role");
	url_unregister("user");
	url_unregister("group");

	url_unregister("cpuid");
	url_unregister("licence");
	url_unregister("mqtt");
	if(plg_id){
		mosquitto_callback_unregister(plg_id, MOSQ_EVT_BASIC_AUTH, mqtt_basic_auth_callback, NULL);
		mosquitto_callback_unregister(plg_id, MOSQ_EVT_MESSAGE, mqtt_message_callback, NULL);
		mosquitto_callback_unregister(plg_id, MOSQ_EVT_TICK, mqtt_tick_callback, NULL);
		mosquitto_callback_unregister(plg_id, MOSQ_EVT_DISCONNECT, mqtt_disconnect_callback, NULL);
//		mosquitto_callback_unregister(plg_id, MOSQ_EVT_ACL_CHECK, dynsec__acl_check_callback, NULL);
	}
	db_close();

	dynsec_groups__cleanup();
	dynsec_clients__cleanup();
	dynsec_roles__cleanup();

	free(iedb_dir);
	iedb_dir = NULL;
	mosquitto_log_printf(MOSQ_LOG_NOTICE, "iedb: exit.");
	return MOSQ_ERR_SUCCESS;
}

