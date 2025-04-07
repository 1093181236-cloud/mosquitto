/*
 * Copyright (c) 2017, aa
 * All rights reserved.
 */
#include "device.h"
#include "lzf.h"
#include "util.h"

#include <ctype.h>
#include <sys/param.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <assert.h>
#include <inttypes.h>
#include <libwebsockets.h>
#include "db.h"
#include "json_help.h"
#include "mosquitto.h"
#include "mosquitto_plugin.h"
#include "mqtt_protocol.h"

extern rax* devices;
static unsigned long long device_max_id = 0;
static int64_t lastDeleteFileId = 0;

extern char *iedb_dir;
extern unsigned long long ts_retention_hours;
extern unsigned long long ts_retention_mbs;

static const char* fieldTypeName[] = {"BOOL","TING_INT","SMALL_INT","INT","BIG_INT","FLOAT","DOUBLE","STRING","Unknown"};
static int fieldTypeBitSize[] = {0,sizeof(char),sizeof(short),sizeof(int),sizeof(int64_t),sizeof(float),sizeof(double),MAX_FIELD_STRING_LEN,0};

static TsdbSaveContext *saveContext = NULL;

int db_insert_tsindex(long long did,timestamp_t start,timestamp_t end,size_t pos,long long field_max_id,long long row_num);
int db_insert_field(char* field_name,long long id,long long did,timestamp_t ctime,int type);
int db_insert_device(char* device_name, long long id);
int db_select_tsindex(queryContext* qc,selectIndexCallback* cb);
int db_delete_rule(char* dname,char* func);
int db_insert_rule(char* dname,char* func);
int db_delete_tsindex(timestamp_t end);
int db_delete_device(long long id);
int db_delete_tsindex_by_device(long long did);
size_t getFieldCacheSize(FIELD_TYPE type){
	if(type == FIELD_TYPE_BOOL)
		return 3*CACHE_BITMAP_SIZE;
	return 2*CACHE_BITMAP_SIZE + fieldTypeBitSize[type] * CACHE_ROW_NUM;
}

/* Create a new device data structure. */
device *deviceNew(long long id) {
	device *d = mosquitto_malloc(sizeof(*d));

	d->id = id;
	d->fields = raxNew();

	d->curTimestampPos = -1;
	d->lastTimestamp = 0;
	d->field_max_id = 0;

    return d;
}

field* fieldNew(long long id,FIELD_TYPE type, timestamp_t ctime) {
	size_t cache_size = getFieldCacheSize(type);
	field* f = mosquitto_calloc(1,sizeof(field));
	f->id = id;
	f->type = type;
	f->ctime = ctime;
	f->pCache = mosquitto_calloc(1,cache_size);

	return f;
}

void fieldFree(void* p){
	field* f = p;
    CompactionRule *rule = f->rules;
	while (rule != NULL) {
		CompactionRule *nextRule = rule->nextRule;
		FreeCompactionRule(rule);
		rule = nextRule;
	}

	if(f->pCache)
		mosquitto_free(f->pCache);
    mosquitto_free(f);
}

/* Free a device, including the fields stored inside the radix tree. */
void freeDevice(void *p) {
	device *d = p;
    raxFreeWithCallback(d->fields,(void(*)(void*))fieldFree);
    mosquitto_free(d);
}

int addSample(device *d,field* f,int rowId,cJSON *item){
	(void)(d);
	bitmapSetBit(f->pCache->haveTimestamp,rowId);
	if(cJSON_IsNull(item) || cJSON_IsInvalid(item)){
		bitmapSetBit(f->pCache->isValueNone,rowId);
		f->flags |= FIELD_FLAG_LAST_NONE;
	}else{
		f->flags &= ~FIELD_FLAG_LAST_NONE;
		if(f->type == FIELD_TYPE_BOOL){
			if(cJSON_IsTrue(item))
				bitmapSetBit(f->pCache->bvalue,rowId);
			else
				bitmapClearBit(f->pCache->bvalue,rowId);
		}else if(f->type == FIELD_TYPE_DOUBLE){
			if (!cJSON_IsNumber(item)) {
				bitmapSetBit(f->pCache->isValueNone,rowId);
			}else if(f->flags & FIELD_FLAG_ACC){
				f->pCache->dvalue[rowId] = item->valuedouble;
				if(f->pCache->dvalue[f->lastTimestampPos] >= item->valuedouble){
					bitmapSetBit(f->pCache->isValueNone,rowId);
				}
			}else{
				f->pCache->dvalue[rowId] = item->valuedouble;
			}
		}else if(f->type == FIELD_TYPE_FLOAT){
			if (!cJSON_IsNumber(item)) {
				bitmapSetBit(f->pCache->isValueNone,rowId);
			}else if(f->flags & FIELD_FLAG_ACC){
				f->pCache->fvalue[rowId] = item->valuedouble;
				if(f->pCache->fvalue[f->lastTimestampPos] >= item->valuedouble){
					bitmapSetBit(f->pCache->isValueNone,rowId);
				}
			}else{
				f->pCache->fvalue[rowId] = item->valuedouble;
			}
		}else if(f->type == FIELD_TYPE_STRING){
			if (!cJSON_IsString(item)) {
				bitmapSetBit(f->pCache->isValueNone,rowId);
			}else{
				size_t len = strlen(item->valuestring);
				if(len >= MAX_FIELD_STRING_LEN)
					len = MAX_FIELD_STRING_LEN-1;
				memcpy(f->pCache->strvalue + MAX_FIELD_STRING_LEN*rowId,item->valuestring,len);
				f->pCache->strvalue[MAX_FIELD_STRING_LEN*rowId+len] = '\0';
			}

		}else if(f->type == FIELD_TYPE_SMALL_INT){
			if (!cJSON_IsNumber(item)) {
				bitmapSetBit(f->pCache->isValueNone,rowId);
			}else if(f->flags & FIELD_FLAG_ACC){
				f->pCache->sivalue[rowId] = item->valueint;
				if(f->pCache->sivalue[f->lastTimestampPos] >= item->valueint){
					bitmapSetBit(f->pCache->isValueNone,rowId);
				}
			}else{
				f->pCache->sivalue[rowId] = item->valueint;
			}
		}else if(f->type == FIELD_TYPE_TING_INT){
			if (!cJSON_IsNumber(item)) {
				bitmapSetBit(f->pCache->isValueNone,rowId);
			}else if(f->flags & FIELD_FLAG_ACC){
				f->pCache->tivalue[rowId] = item->valueint;
				if(f->pCache->tivalue[f->lastTimestampPos] >= item->valueint){
					bitmapSetBit(f->pCache->isValueNone,rowId);
				}
			}else{
				f->pCache->tivalue[rowId] = item->valueint;
			}
		}else if(f->type == FIELD_TYPE_INT){
			if (!cJSON_IsNumber(item)) {
				bitmapSetBit(f->pCache->isValueNone,rowId);
			}else if(f->flags & FIELD_FLAG_ACC){
				f->pCache->ivalue[rowId] = item->valueint;
				if(f->pCache->ivalue[f->lastTimestampPos] >= item->valueint){
					bitmapSetBit(f->pCache->isValueNone,rowId);
				}
			}else{
				f->pCache->ivalue[rowId] = item->valueint;
			}
		}else if(f->type == FIELD_TYPE_BIG_INT){
			if (!cJSON_IsNumber(item)) {
				bitmapSetBit(f->pCache->isValueNone,rowId);
			}else if(f->flags & FIELD_FLAG_ACC){
				f->pCache->bivalue[rowId] = item->valueint;
				if(f->pCache->bivalue[f->lastTimestampPos] >= item->valueint){
					bitmapSetBit(f->pCache->isValueNone,rowId);
				}
			}else{
				f->pCache->bivalue[rowId] = item->valueint;
			}
		}
	}

	return C_OK;
}

timestamp_t getFileTimestamp(timestamp_t timestamp){
	return timestamp - timestamp % TS_FILE_PERIOD;
}

FILE *prepareFileHandle(FILE *cur_fp,const char *filename){
    char cwd[MAXPATHLEN]; /* Current working dir path for error messages. */
    int saved_errno;

	if(cur_fp != NULL){
	    /* Make sure data will not remain on the OS's output buffers */
		fflush(cur_fp);
		fsync(fileno(cur_fp));
/*	    if (reclaimFilePageCache(fileno(cur_fp), 0, 0) == -1) {
	        mosquitto_log_printf(MOSQ_LOG_NOTICE,"Unable to reclaim cache after saving RDB: %s", strerror(errno));
	    }*/
	    fclose(cur_fp);
	}

    FILE *fp = fopen(filename,"a+");
    if (!fp) {
        saved_errno = errno;
        char *str_err = strerror(errno);
        char *cwdp = getcwd(cwd,MAXPATHLEN);
        mosquitto_log_printf(MOSQ_LOG_WARNING,
            "Failed opening the tsdb file %s (in server root dir %s) "
            "for saving: %s",
            filename,
            cwdp ? cwdp : "unknown",
            str_err);
        errno = saved_errno;
        return NULL;
    }

	return fp;
}

void deviceSaveToDisk(device* d){
	raxIterator ri;
/*	if(c->id == CLIENT_ID_AOF){
	    raxStart(&ri, d->fields);
	    raxSeek(&ri, "^", NULL, 0);
	    while (raxNext(&ri)) {
	    	field *f = ri.data;
	    	memset(f->pCache->haveTimestamp,0,2*CACHE_BITMAP_SIZE);
	    }
	    raxStop(&ri);
	    return;
	}*/
	if(d->curTimestampPos < 0)
		return;

	int row_num = d->curTimestampPos + 1;
	timestamp_t endTimestamp = d->timestamps[row_num-1];
	timestamp_t fileTimestamp = getFileTimestamp(endTimestamp);

	char dbPathName[128];
	if(saveContext == NULL){
		saveContext = mosquitto_malloc(sizeof(TsdbSaveContext));
		saveContext->curFileTimestamp = 0;
		saveContext->curDataFp = NULL;
	}

	if(saveContext->curFileTimestamp != fileTimestamp){
		snprintf(dbPathName,128,"%s/%" PRIu64 ".data",iedb_dir,fileTimestamp);
		saveContext->curDataFp = prepareFileHandle(saveContext->curDataFp,dbPathName);
		saveContext->curFileTimestamp = fileTimestamp;
	}

	size_t index_size = sizeof(FINDEX)*(d->field_max_id+1);
	FINDEX* index = mosquitto_malloc(index_size);

	size_t output_size = MAX_FIELD_STRING_LEN*CACHE_ROW_NUM;
	char* output = mosquitto_malloc(output_size+8);

	fseek(saveContext->curDataFp, 0, SEEK_END);
	index[0].dataOffset = ftell(saveContext->curDataFp);
	index[0].dataLength = lzf_compress(d->timestamps,sizeof(timestamp_t)*CACHE_ROW_NUM,output,output_size);
	fwrite(output, index[0].dataLength,1,saveContext->curDataFp);
	size_t dataOffset = index[0].dataOffset + index[0].dataLength;

    raxStart(&ri, d->fields);
    raxSeek(&ri, "^", NULL, 0);
    while (raxNext(&ri)) {
    	field *f = ri.data;
    	assert(f->id <= d->field_max_id);
    	FINDEX* findex = &index[f->id];
    	findex->dataOffset = dataOffset;

    	size_t cache_size = getFieldCacheSize(f->type);
    	findex->dataLength = lzf_compress(f->pCache,cache_size,output,output_size);
    	fwrite(output, findex->dataLength,1,saveContext->curDataFp);
    	dataOffset += findex->dataLength;
    	memset(f->pCache->haveTimestamp,0,2*CACHE_BITMAP_SIZE);
    }
    raxStop(&ri);
    fwrite(index, index_size,1,saveContext->curDataFp);
    fflush(saveContext->curDataFp);
    fsync(fileno(saveContext->curDataFp));

    mosquitto_free(output);
    mosquitto_free(index);
    db_insert_tsindex(d->id,d->timestamps[0],d->timestamps[row_num-1],dataOffset,d->field_max_id,row_num);
}

int deviceAppendItem(device *d, timestamp_t ts, cJSON *pps, int no_create) {
    if(ts <= d->lastTimestamp){
    	errno = EDOM;
    	return C_ERR;
	}else{
		if(d->curTimestampPos >= CACHE_ROW_NUM-1){
			deviceSaveToDisk(d);
			d->curTimestampPos = 0;
		}else{
			d->curTimestampPos++;
		}
	}
    int rowId = d->curTimestampPos;

    d->timestamps[rowId] = ts;
    d->lastTimestamp = ts;

    int cur_size = 0;
    int field_size = cJSON_GetArraySize(pps);
    cJSON *aiter;
    cJSON_ArrayForEach(aiter, pps){
    	if(cur_size >= field_size)
    		return 1;
    	cur_size++;

    	char* field_name = aiter->string;
    	field *f = raxFind(d->fields,(unsigned char*)field_name,strlen(field_name));
    	if (f == raxNotFound){
    		if(no_create)
    		   continue;

        	FIELD_TYPE ftype;
        	if(cJSON_IsBool(aiter)){
        		ftype = FIELD_TYPE_BOOL;
        	}else if(cJSON_IsNumber(aiter)){
        		ftype = FIELD_TYPE_DOUBLE;
        	}else if(cJSON_IsString(aiter)){
        		ftype = FIELD_TYPE_STRING;
        	}else{
        		continue;
        	}

    		f = fieldNew(++d->field_max_id,ftype,ts);
    		raxInsert(d->fields,(unsigned char*)field_name,strlen(field_name),f,NULL);
    		db_insert_field(field_name,d->field_max_id,d->id,ts,ftype);
    	}

		if(addSample(d,f,rowId,aiter) == C_OK){
			f->lastTimestampPos = rowId;
			f->lastTimestamp = ts;
			if(f->rules){
				double value = 0;
				if(f->type == FIELD_TYPE_DOUBLE){
					value = f->pCache->dvalue[rowId];
				}else if(f->type == FIELD_TYPE_BOOL){
					if(bitmapTestBit(f->pCache->bvalue,rowId)){
						value = 1;
					}else{
						value = 0;
					}
				}else if(f->type == FIELD_TYPE_TING_INT){
					value = f->pCache->tivalue[rowId];
				}else if(f->type == FIELD_TYPE_SMALL_INT){
					value = f->pCache->sivalue[rowId];
				}else if(f->type == FIELD_TYPE_INT){
					value = f->pCache->ivalue[rowId];
				}else if(f->type == FIELD_TYPE_BIG_INT){
					value = f->pCache->bivalue[rowId];
				}else if(f->type == FIELD_TYPE_FLOAT){
					value = f->pCache->fvalue[rowId];
				}

				CompactionRule *rule = f->rules;
				while (rule != NULL) {
					double aggVal;
					if(handleCompaction(d,rule,ts,value,&aggVal)){
						cJSON_AddNumberToObject(pps,rule->func,aggVal);
					}
					rule = rule->nextRule;
				}
			}
		}

    }

    return C_OK;
}

/* -----------------------------------------------------------------------
 * device commands implementation
 * ----------------------------------------------------------------------- */
int genericParseTimestamp(char* buf, timestamp_t *ms, int strict) {
    if (strict && (buf[0] == '-' || buf[0] == '+') && buf[1] == '\0')
        goto invalid;

    /* Handle the "-" and "+" special cases. */
    if (buf[0] == '-' && buf[1] == '\0') {
        *ms = 0;
        return C_OK;
    } else if (buf[0] == '+' && buf[1] == '\0') {
        *ms = UINT64_MAX;
        return C_OK;
    }
    unsigned long long tmp;
    if (string2ull(buf,&tmp) == 0) goto invalid;
    *ms = (timestamp_t)tmp;

    return C_OK;

invalid:
    return C_ERR;
}

int tsAddGenericCommand(char* dn, timestamp_t t,cJSON *pps) {
/*	timestamp_t t;
    if (tm[0] == '*' && tm[1] == '\0') {
    	t = mstime();
    }else{
    	if (genericParseTimestamp(tm,&t,1) != C_OK){
    		return -1;
    	}
    }*/

    if (t == UINT64_MAX) {
        return -1;
    }

    /* Lookup the device at key. */
    device *d = raxFind(devices,(unsigned char*)dn,strlen(dn));
    if (d == raxNotFound){
    	d = deviceNew(++device_max_id);
    	raxInsert(devices,(unsigned char*)dn,strlen(dn),d,NULL);
    	db_insert_device(dn,device_max_id);
    }

    /* Append using the low level function and return the ID. */
    errno = 0;
    return deviceAppendItem(d,t,pps,0);
}

int mqtt_message_callback(int event, void *event_data, void *userdata){
	struct mosquitto_evt_message *ed = event_data;
	size_t topic_len = strlen(ed->topic);
	const char* username = mosquitto_client_username(ed->client);
	size_t name_len = strlen(username)/2;
	if(topic_len == (name_len+20)){
		if(strncmp(ed->topic,"/edge/property/",15)){
			return MOSQ_ERR_SUCCESS;
		}
		if(strncmp(username,ed->topic+15,name_len)){
			return MOSQ_ERR_SUCCESS;
		}
		if(strncmp(ed->topic + 15 + name_len,"/post",5)){
			return MOSQ_ERR_SUCCESS;
		}
	}else if(topic_len == (name_len+28)){
		if(strncmp(ed->topic,"/edge/property/history/",23)){
			return MOSQ_ERR_SUCCESS;
		}
		if(strncmp(username,ed->topic+23,name_len)){
			return MOSQ_ERR_SUCCESS;
		}
		if(strncmp(ed->topic + 23 + name_len,"/post",5)){
			return MOSQ_ERR_SUCCESS;
		}
	}else{
		return MOSQ_ERR_SUCCESS;
	}

	cJSON *tree = cJSON_ParseWithLength(ed->payload, ed->payloadlen);
	if(tree == NULL || !cJSON_IsArray(tree)){
		return MOSQ_ERR_SUCCESS;
	}

	int modified = 0;
	char *dn;
	cJSON *aiter;

	cJSON_ArrayForEach(aiter, tree){
		if(cJSON_IsObject(aiter)){
			if(json_get_string(aiter, "dn", &dn, false) == MOSQ_ERR_SUCCESS){
				cJSON *tm = cJSON_GetObjectItem(aiter, "time");
				cJSON *pps = cJSON_GetObjectItem(aiter, "properties");
				if(pps != NULL && cJSON_IsObject(pps) && tm != NULL && cJSON_IsNumber(tm)){
					if(tsAddGenericCommand(dn,(timestamp_t)(tm->valuedouble*1000),pps) == 1)
						modified = 1;
				}
			}
		}
	}
	if(modified){
		char *new_payload = cJSON_PrintUnformatted(tree);
		ed->payload = new_payload;
		ed->payloadlen = strlen(new_payload);
	}
	cJSON_Delete(tree);

	return MOSQ_ERR_SUCCESS;
}

int select_device_callback(void *data, int argc, char **argv, char **azColName){
	rax *devices = (rax*)data;
	char* name = argv[0];
	unsigned long long id = 0;
	string2ull(argv[1],&id);

	if(id == 0 || name == NULL)
	   return 0;
	device* d = deviceNew(id);
	raxInsert(devices,(unsigned char*)name,strlen(name),d,NULL);
	if(id > device_max_id)
		device_max_id = id;
	return 0;
}

int select_field_callback(void *data, int argc, char **argv, char **azColName){
	device *d = (device*)data;
	char* name = argv[0];
	unsigned long long id = 0;
	FIELD_TYPE type = FIELD_TYPE_MAX_TYPES;
	unsigned long long ctime = 0;

	string2ull(argv[1],&id);
	type = atoi(argv[2]);
	string2ull(argv[3],&ctime);

	if(id == 0 || name == NULL || type == FIELD_TYPE_MAX_TYPES)
	   return 0;

	field* f = fieldNew(id,type,ctime);
	raxInsert(d->fields,(unsigned char*)name,strlen(name),f,NULL);
	if(id > d->field_max_id)
		d->field_max_id = id;
	return 0;
}

int select_rule_callback(void *data, int argc, char **argv, char **azColName){
	char* dname = argv[0];
	char* func = argv[1];
	if(dname == NULL || func == NULL)
		return 0;

    device* d = raxFind(devices,(unsigned char*)dname,strlen(dname));
    if(d == raxNotFound){
    	return 0;
    }

	TS_AGG_TYPES_T aggType;
	char *next1 = strchr(func,'(');
	if(next1){
		aggType = StringLenAggTypeToEnum(func,next1 - (char*)func);
	}else{
		return 0;
	}
	if(aggType == TS_AGG_INVALID){
		return 0;
	}

	char* next2 = strchr(next1+1,',');
	if(next2 == NULL){
		return 0;
	}

	char* next3 = strchr(next2+1,',');
	if(next3 == NULL){
		return 0;
	}

	char* next4 = strchr(next3+1,')');
	if(next4 == NULL){
		return 0;
	}

	field* src_f = raxFind(d->fields,(unsigned char*)next1+1,next2-next1-1);
	if(src_f == raxNotFound){
		return 0;
	}

	long long bucketDuration;
	if(string2ll(next2+1,next3-next2-1,&bucketDuration) == 0){
		return 0;
	}
	long long timestampAlignment;
	if(string2ll(next3+1,next4-next3-1,&timestampAlignment) == 0){
		return 0;
	}
	if(timestampAlignment < 60000 || timestampAlignment > bucketDuration){
		return 0;
	}

	addRule(dname,src_f,func,aggType,bucketDuration,timestampAlignment);

	return 0;
}

int replyFieldsAgg(cJSON *j_row,timestamp_t curTimestamp,QFIELD* pFieldHeadTmp,int rowId,ORDER_TYPE otype){
	long fieldlen = 0;
	cJSON *j_fields;

	while(pFieldHeadTmp){
		if(pFieldHeadTmp->haveCache == 0){
			pFieldHeadTmp = pFieldHeadTmp->next;
			continue;
		}
		if(pFieldHeadTmp->curCache && bitmapTestBit(pFieldHeadTmp->curCache->haveTimestamp,rowId)){
			if(bitmapTestBit(pFieldHeadTmp->curCache->isValueNone,rowId)){
				pFieldHeadTmp = pFieldHeadTmp->next;
				continue;
			}

			CompactionRule *rule = pFieldHeadTmp->rule;
			if (rule != NULL) {
				timestamp_t startCurrentTimeBucket = rule->startCurrentTimeBucket;
				double aggVal;
				double curVal;
		    	if(pFieldHeadTmp->pField->type == FIELD_TYPE_BOOL){
		    		curVal = bitmapTestBit(pFieldHeadTmp->curCache->bvalue,rowId)?1:0;
		    	}else if(pFieldHeadTmp->pField->type == FIELD_TYPE_DOUBLE){
		    		curVal = pFieldHeadTmp->curCache->dvalue[rowId];
		    	}else if(pFieldHeadTmp->pField->type == FIELD_TYPE_TING_INT){
		    		curVal = pFieldHeadTmp->curCache->tivalue[rowId];
		    	}else if(pFieldHeadTmp->pField->type == FIELD_TYPE_SMALL_INT){
		    		curVal = pFieldHeadTmp->curCache->sivalue[rowId];
		    	}else if(pFieldHeadTmp->pField->type == FIELD_TYPE_INT){
		    		curVal = pFieldHeadTmp->curCache->ivalue[rowId];
		    	}else if(pFieldHeadTmp->pField->type == FIELD_TYPE_BIG_INT){
		    		curVal = pFieldHeadTmp->curCache->bivalue[rowId];
		    	}else if(pFieldHeadTmp->pField->type == FIELD_TYPE_FLOAT){
		    		curVal = pFieldHeadTmp->curCache->fvalue[rowId];
		    	}
				if(handleQueryCompaction(rule,curTimestamp,curVal,&aggVal,otype)){
					if(fieldlen == 0){
						cJSON_AddNumberToObject(j_row,"Timestamp",startCurrentTimeBucket/1000);
						j_fields = cJSON_CreateObject();
						cJSON_AddItemToObject(j_row,"Fields",j_fields);
					}

					cJSON_AddNumberToObject(j_fields,pFieldHeadTmp->name,aggVal);
					fieldlen++;
				}
			}
		}
		pFieldHeadTmp = pFieldHeadTmp->next;
	}
	if(fieldlen > 0){
		return 1;
	}

	return 0;
}

long long replyFieldsQuery(cJSON *j_row,timestamp_t curTimestamp,QFIELD* pFieldHeadTmp,int rowId,QUERY_TYPE qtype){
	long long field_len = 0;
	cJSON *j_fields;

	while(pFieldHeadTmp){
		if(pFieldHeadTmp->curCache == NULL || pFieldHeadTmp->haveCache == 0){
			pFieldHeadTmp = pFieldHeadTmp->next;
			continue;
		}

		if(bitmapTestBit(pFieldHeadTmp->curCache->haveTimestamp,rowId)){

			if(pFieldHeadTmp->isFirstRow && qtype == QUERY_NORMAL_FILTER_FIRST_NONE && bitmapTestBit(pFieldHeadTmp->curCache->isValueNone,rowId)){
				pFieldHeadTmp = pFieldHeadTmp->next;
				continue;
			}
			pFieldHeadTmp->isFirstRow = 0;

			if(field_len == 0){
				cJSON_AddNumberToObject(j_row,"Timestamp",curTimestamp/1000);
				j_fields = cJSON_CreateObject();
				cJSON_AddItemToObject(j_row,"Fields",j_fields);
			}

			if(bitmapTestBit(pFieldHeadTmp->curCache->isValueNone,rowId)){
				cJSON_AddNullToObject(j_fields,pFieldHeadTmp->name);
			}else{
            	if(pFieldHeadTmp->pField->type == FIELD_TYPE_BOOL){
					if(bitmapTestBit(pFieldHeadTmp->curCache->bvalue,rowId)){
						cJSON_AddTrueToObject(j_fields,pFieldHeadTmp->name);
					}else{
						cJSON_AddFalseToObject(j_fields,pFieldHeadTmp->name);
					}
            	}else if(pFieldHeadTmp->pField->type == FIELD_TYPE_DOUBLE){
            		cJSON_AddNumberToObject(j_fields,pFieldHeadTmp->name,pFieldHeadTmp->curCache->dvalue[rowId]);
            	}else if(pFieldHeadTmp->pField->type == FIELD_TYPE_STRING){
            		cJSON_AddStringToObject(j_fields,pFieldHeadTmp->name,pFieldHeadTmp->curCache->strvalue + rowId*MAX_FIELD_STRING_LEN);
            	}else if(pFieldHeadTmp->pField->type == FIELD_TYPE_TING_INT){
            		cJSON_AddNumberToObject(j_fields,pFieldHeadTmp->name,pFieldHeadTmp->curCache->tivalue[rowId]);
            	}else if(pFieldHeadTmp->pField->type == FIELD_TYPE_SMALL_INT){
            		cJSON_AddNumberToObject(j_fields,pFieldHeadTmp->name,pFieldHeadTmp->curCache->sivalue[rowId]);
            	}else if(pFieldHeadTmp->pField->type == FIELD_TYPE_INT){
            		cJSON_AddNumberToObject(j_fields,pFieldHeadTmp->name,pFieldHeadTmp->curCache->ivalue[rowId]);
            	}else if(pFieldHeadTmp->pField->type == FIELD_TYPE_BIG_INT){
            		cJSON_AddNumberToObject(j_fields,pFieldHeadTmp->name,pFieldHeadTmp->curCache->bivalue[rowId]);
            	}else if(pFieldHeadTmp->pField->type == FIELD_TYPE_FLOAT){
            		cJSON_AddNumberToObject(j_fields,pFieldHeadTmp->name,pFieldHeadTmp->curCache->fvalue[rowId]);
            	}else{
            		cJSON_AddNullToObject(j_fields,pFieldHeadTmp->name);
            	}
			}
			field_len++;
		}

		pFieldHeadTmp = pFieldHeadTmp->next;
	}

	return field_len;
}

size_t queryCahce(queryContext* qc,timestamp_t* curTimestamps,size_t curTotalSamples){//2018-06-01 08:00:00.000
	int start,end,step;
	if(qc->otype == ORDER_ASC){
		start = 0;
		end = curTotalSamples;
		step = 1;
	}else{
		start = curTotalSamples-1;
		end = -1;
		step = -1;
	}

	for(int i = start; i != end; i+=step){
		if(curTimestamps[i] < qc->startQueryTime){
			if(qc->otype == ORDER_DESC)
				break;
			else
				continue;
		}
		if(curTimestamps[i] > qc->endQueryTime){
			if(qc->otype == ORDER_ASC)
				break;
			else
				continue;
		}

		if(qc->qtype == QUERY_NORMAL || qc->qtype == QUERY_NORMAL_FILTER_FIRST_NONE){
			cJSON *j_row = cJSON_CreateObject();
			if(replyFieldsQuery(j_row,curTimestamps[i],qc->pFieldHead,i,qc->qtype) > 0){
				cJSON_AddItemToArray(qc->j_device,j_row);
				qc->samplelen++;
			}else{
				cJSON_Delete(j_row);
			}
		}else if(qc->qtype == QUERY_AGG){
			cJSON *j_row = cJSON_CreateObject();
			if(replyFieldsAgg(j_row,curTimestamps[i],qc->pFieldHead,i,qc->otype)){
				qc->samplelen++;
				cJSON_AddItemToArray(qc->j_device,j_row);
			}else{
				cJSON_Delete(j_row);
			}

		}

		if(qc->count != 0 && qc->samplelen >= qc->count)
			break;
	}

	return qc->samplelen;
}

int select_index_callback(void *data, long long start,long long end,long long pos,long long max_fid,long long row_num){
	queryContext* qc = data;
	assert(max_fid <= qc->d->field_max_id);

	QFIELD* pFieldHeadTmp;
	int timestamp_output_size = sizeof(timestamp_t)*CACHE_ROW_NUM;
	int buffer_size = MAX_FIELD_STRING_LEN*CACHE_ROW_NUM;
	if(qc->input == NULL){
		qc->timestamp_output = mosquitto_malloc(timestamp_output_size);
		qc->input = mosquitto_malloc(buffer_size);
		qc->index = mosquitto_malloc(sizeof(FINDEX)*(qc->d->field_max_id+1));

		pFieldHeadTmp = qc->pFieldHead;
		while(pFieldHeadTmp){
			size_t cache_size = getFieldCacheSize(pFieldHeadTmp->pField->type);
			pFieldHeadTmp->curCache = mosquitto_malloc(cache_size);
			pFieldHeadTmp = pFieldHeadTmp->next;
		}
	}

	pFieldHeadTmp = qc->pFieldHead;
	while(pFieldHeadTmp){
		pFieldHeadTmp->haveCache = 0;
		pFieldHeadTmp = pFieldHeadTmp->next;
	}

	if(qc->fileTimestamp != getFileTimestamp(end)){
		if(qc->dataFp != NULL){
			//reclaimFilePageCache(fileno(dataFp), 0, 0);
			fclose(qc->dataFp);
			qc->dataFp = NULL;
		}

		char dbPathName[128];
		snprintf(dbPathName,128,"%s/%" PRIu64 ".data",iedb_dir,getFileTimestamp(end));

		qc->dataFp = fopen(dbPathName, "r");
		if(qc->dataFp == NULL){
			//db_delete_index(qc->d->id,start);
			return 0;
		}
		qc->fileTimestamp = getFileTimestamp(end);
	}
	fseek(qc->dataFp, pos,SEEK_SET);
	if(fread(qc->index,1,sizeof(FINDEX)*(max_fid+1),qc->dataFp) != sizeof(FINDEX)*(max_fid+1))
		return 0;

	fseek(qc->dataFp, qc->index->dataOffset,SEEK_SET);
	if(fread(qc->input,1,qc->index->dataLength,qc->dataFp) != qc->index->dataLength)
		return 0;
	lzf_decompress(qc->input, qc->index->dataLength,qc->timestamp_output,timestamp_output_size);

	pFieldHeadTmp = qc->pFieldHead;
	while(pFieldHeadTmp){
		if(pFieldHeadTmp->pField->id <= max_fid /*&& pFieldHeadTmp->pField->ctime < end*/){
			FINDEX* findex = qc->index + pFieldHeadTmp->pField->id;
			fseek(qc->dataFp, findex->dataOffset,SEEK_SET);
			if(fread(qc->input,1,findex->dataLength,qc->dataFp) != (size_t)findex->dataLength)
				break;

			size_t cache_size = getFieldCacheSize(pFieldHeadTmp->pField->type);
			lzf_decompress(qc->input, findex->dataLength,pFieldHeadTmp->curCache,cache_size);
			pFieldHeadTmp->haveCache = 1;
		}
		pFieldHeadTmp = pFieldHeadTmp->next;
	}

	queryCahce(qc,qc->timestamp_output,row_num);
	pFieldHeadTmp = qc->pFieldHead;
	while(pFieldHeadTmp){
		pFieldHeadTmp->haveCache = 0;
		pFieldHeadTmp = pFieldHeadTmp->next;
	}

	if(qc->count > 0 && qc->samplelen >= qc->count){
		return -1;
	}

	return 0;
}

void deviceReplyWithRange(queryContext* qc){
	if(qc->otype == ORDER_DESC){
		QFIELD* pFieldHeadTmp = qc->pFieldHead;
		while(pFieldHeadTmp){
			if(pFieldHeadTmp->pField->lastTimestamp >= qc->d->timestamps[0]){
				pFieldHeadTmp->curCache = pFieldHeadTmp->pField->pCache;
				pFieldHeadTmp->haveCache = 1;
			}
			pFieldHeadTmp = pFieldHeadTmp->next;
		}
		queryCahce(qc,qc->d->timestamps,qc->d->curTimestampPos+1);
		if(qc->count > 0 && qc->samplelen >= qc->count){
			return;
		}
	}

	db_select_tsindex(qc,select_index_callback);

	if(qc->dataFp != NULL){
		//reclaimFilePageCache(fileno(dataFp), 0, 0);
		fclose(qc->dataFp);
	}

	if(qc->input != NULL){
		mosquitto_free(qc->timestamp_output);
		mosquitto_free(qc->input);
		mosquitto_free(qc->index);

		QFIELD* pFieldHeadTmp = qc->pFieldHead;
		while(pFieldHeadTmp){
			mosquitto_free(pFieldHeadTmp->curCache);
			pFieldHeadTmp->curCache = NULL;
			pFieldHeadTmp = pFieldHeadTmp->next;
		}
	}

	if(qc->otype == ORDER_ASC &&  ((qc->count > 0 && qc->samplelen < qc->count) || qc->count < 1)){
		QFIELD* pFieldHeadTmp = qc->pFieldHead;
		while(pFieldHeadTmp){
			if(pFieldHeadTmp->pField->lastTimestamp >= qc->d->timestamps[0]){
				pFieldHeadTmp->curCache = pFieldHeadTmp->pField->pCache;
				pFieldHeadTmp->haveCache = 1;
			}
			pFieldHeadTmp = pFieldHeadTmp->next;
		}
		queryCahce(qc,qc->d->timestamps,qc->d->curTimestampPos+1);
	}
}

/* TSRANGE/TSREVRANGE actual implementation.
 * The 'start' and 'end' IDs are parsed as follows:
 *   Incomplete 'start' has its sequence set to 0, and 'end' to UINT64_MAX.
 *   "-" and "+"" mean the minimal and maximal ID values, respectively.
 *   The "(" prefix means an open (exclusive) range, so TSRANGE  (1-0 (2-0
 *   will match anything from 1-1 and 1-UINT64_MAX.
 */
void tsQuery(char** argv,cJSON *j_device,timestamp_t startQueryTime,timestamp_t endQueryTime,size_t count, ORDER_TYPE order,QUERY_TYPE qtype,int device_id,int last_field_id) {
    device* d = raxFind(devices,(unsigned char*)argv[device_id],strlen(argv[device_id]));
    if(d == raxNotFound){
    	return;
    }

    QFIELD* pFieldHead = NULL;
    QFIELD* pFieldPre = NULL;
    if(device_id >= last_field_id){
        raxIterator ri;
        raxStart(&ri, d->fields);
        raxSeek(&ri, "^", NULL, 0);
        while (raxNext(&ri)) {
        	field *f = ri.data;
    		QFIELD* pFieldTemp = mosquitto_calloc(1,sizeof(QFIELD));
    		pFieldTemp->pField = f;
    		pFieldTemp->name = mosquitto_calloc(1,ri.key_len+1);
    		memcpy(pFieldTemp->name,ri.key,ri.key_len);
    		pFieldTemp->curCache = NULL;
    		pFieldTemp->isFirstRow = 1;
    		pFieldTemp->haveCache = 0;
    		if(pFieldHead == NULL){
    			pFieldHead = pFieldTemp;
    		}else{
    			pFieldPre->next = pFieldTemp;
    		}
    		pFieldPre = pFieldTemp;
        }
        raxStop(&ri);
    }else{
        for(int j = device_id+1; j<= last_field_id;j++){
        	if(qtype == QUERY_NORMAL){
            	field* f = raxFind(d->fields,(unsigned char*)argv[j],strlen(argv[j]));
            	if(f != raxNotFound){
            		QFIELD* pFieldTemp = mosquitto_calloc(1,sizeof(QFIELD));
            		pFieldTemp->name = argv[j];
            		pFieldTemp->pField = f;
            		pFieldTemp->curCache = NULL;
            		pFieldTemp->isFirstRow = 1;
            		pFieldTemp->haveCache = 0;
            		if(pFieldHead == NULL){
            			pFieldHead = pFieldTemp;
            		}else{
            			pFieldPre->next = pFieldTemp;
            		}
            		pFieldPre = pFieldTemp;
            	}
        	}else if(qtype == QUERY_AGG){
        		TS_AGG_TYPES_T t;
        		char *next1 = strchr(argv[j],'(');
        		if(next1){
        			t = StringLenAggTypeToEnum(argv[j],next1 - (char*)argv[j]);
        		}else{
        			continue;
        		}
        		if(t == TS_AGG_INVALID){
        			continue;
        		}

        		char* next2 = strchr(next1+1,',');
        		if(next2 == NULL)
        			continue;
        		char* next3 = strchr(next2+1,',');
        		if(next3 == NULL)
        			continue;
        		char* next4 = strchr(next3+1,')');
        		if(next4 == NULL)
        		    continue;

        		field* f = raxFind(d->fields,(unsigned char*)next1+1,next2-next1-1);
        		if(f == raxNotFound || f->type == FIELD_TYPE_STRING)
        			continue;

        		long long bucketDuration;
        		if(string2ll(next2+1,next3-next2-1,&bucketDuration) == 0)
        			continue;
        		bucketDuration *= 1000;
        		long long timestampAlignment;
        		if(string2ll(next3+1,next4-next3-1,&timestampAlignment) == 0)
        			continue;
        		timestampAlignment *= 1000;
        		if(timestampAlignment < 60000 || timestampAlignment < bucketDuration){
        			continue;
        		}

        		QFIELD* pFieldTemp = mosquitto_calloc(1,sizeof(QFIELD));
				pFieldTemp->name = argv[j];
				pFieldTemp->pField = f;
				pFieldTemp->curCache = NULL;
				pFieldTemp->rule = NewRule(argv[j],t, bucketDuration, timestampAlignment);
				if(pFieldHead == NULL){
					pFieldHead = pFieldTemp;
				}else{
					pFieldPre->next = pFieldTemp;
				}
				pFieldPre = pFieldTemp;
        	}
        }
    }
	queryContext qc = {
	        .j_device = j_device,
	        .d = d,
			.startQueryTime = startQueryTime,
			.endQueryTime = endQueryTime,
			.pFieldHead = pFieldHead,
	        .count = count,
			.otype = order,
			.qtype = qtype,
			.input = NULL,
			.timestamp_output = NULL,
			.dataFp = NULL,
			.fileTimestamp = 0,
			.samplelen = 0
	    };
    deviceReplyWithRange(&qc);

err:
	while(pFieldHead){
		QFIELD* curField = pFieldHead;
		pFieldHead = pFieldHead->next;
		if(device_id >= last_field_id)
			mosquitto_free(curField->name);
		if(curField->rule)
			FreeCompactionRule(curField->rule);
		mosquitto_free(curField);
	}
}

int tsQueryGenericCommand(cJSON** j_responses,int argc,char** argv, ORDER_TYPE order,QUERY_TYPE qtype) {
    timestamp_t startQueryTime, endQueryTime;
    size_t count = 0;

    if (argc < 3) {
        /* If IDLE was provided we must have at least 'start end count' */
        return HTTP_STATUS_BAD_REQUEST;
    }

    char *startarg = order == ORDER_DESC ? argv[1] : argv[0];
    char *endarg = order == ORDER_DESC ? argv[0] : argv[1];

    /* Parse start and end IDs. */
    if (genericParseTimestamp(startarg,&startQueryTime,1) != C_OK){
        return HTTP_STATUS_BAD_REQUEST;
    }
    if (genericParseTimestamp(endarg,&endQueryTime,1) != C_OK){
        return HTTP_STATUS_BAD_REQUEST;
    }
    startQueryTime *= 1000;
    endQueryTime *= 1000;

	cJSON *j_devices, *j_device;

	j_devices = cJSON_CreateObject();
	if(j_devices == NULL) return HTTP_STATUS_INTERNAL_SERVER_ERROR;

    int device_id = 2;
    int j = 2;
    while(j < argc){
    	int additional = argc-j-1;
    	if (strcasecmp(argv[j],"DEVICE") == 0){
    		j_device = cJSON_CreateArray();
    		if(j_device == NULL){
    			cJSON_Delete(j_devices);
    			return HTTP_STATUS_INTERNAL_SERVER_ERROR;
    		}
    		cJSON_AddItemToObject(j_devices,argv[device_id], j_device);

    		tsQuery(argv,j_device,startQueryTime, endQueryTime,count,order,qtype,device_id,j-1);
    		device_id = ++j;
    	}else if (strcasecmp(argv[j],"FILTER_FIRST_NONE") == 0 && qtype == QUERY_NORMAL){
			qtype = QUERY_NORMAL_FILTER_FIRST_NONE;
			device_id = j+1;
		}else if (strcasecmp(argv[j],"COUNT") == 0 && additional >= 1) {
        	long long count_value;
            if (getLongLong(argv[j+1],&count_value)!= C_OK)
            	return HTTP_STATUS_BAD_REQUEST;
            if (count_value < 0)
            	count = 0;
            else
            	count = (size_t)count_value;
            j++;
            device_id = j+1;
        }

    	j++;
    }
    if(device_id < argc){
		j_device = cJSON_CreateArray();
		if(j_device == NULL){
			cJSON_Delete(j_devices);
			return HTTP_STATUS_INTERNAL_SERVER_ERROR;
		}
		cJSON_AddItemToObject(j_devices,argv[device_id], j_device);
    	tsQuery(argv,j_device,startQueryTime, endQueryTime,count,order,qtype,device_id,j-1);
    }

    *j_responses = j_devices;
    return 0;
}


/* TSQUERY key start end [COUNT <n>] [FIELDS f1 f2] [FILTER_FIRST_NONE]*/
int tsQueryCommand(int argc,char** argv,cJSON** j_responses) {
    return tsQueryGenericCommand(j_responses,argc,argv,ORDER_ASC,QUERY_NORMAL);
}

/* TSREVQUERY key end start [COUNT <n>] [FIELDS f1 f2] [FILTER_FIRST_NONE]*/
int tsRevQueryCommand(int argc,char** argv,cJSON** j_responses) {
	return tsQueryGenericCommand(j_responses,argc,argv,ORDER_DESC,QUERY_NORMAL);
}

/* TSAGGQUERY key start end [COUNT <n>] [FIELDS max(f1,1000) min(f2,2000)] */
int tsAggQueryCommand(int argc,char** argv,cJSON** j_responses) {
	return tsQueryGenericCommand(j_responses,argc,argv,ORDER_ASC,QUERY_AGG);
}

/* TSAGGREVQUERY key end start [COUNT <n>] [FIELDS max(f1,1000) ave(f2,6000)] */
int tsAggRevQueryCommand(int argc,char** argv,cJSON** j_responses) {
	return tsQueryGenericCommand(j_responses,argc,argv,ORDER_DESC,QUERY_AGG);
}

//////////////////////////////////////////////////////////////////////////////////////////////////////////
int addReplyCacheFL(cJSON *j_fields,field *f,cache *pCache,timestamp_t* curTimestamps,size_t curTotalSamples,timestamp_t startQueryTime,timestamp_t endQueryTime,ORDER_TYPE otype){
	int start,end,step;
	if(otype == ORDER_ASC){
		start = 0;
		end = curTotalSamples;
		step = 1;
	}else{
		start = curTotalSamples-1;
		end = -1;
		step = -1;
	}
	for(int rowId = start; rowId != end; rowId+=step){
		if(curTimestamps[rowId] < startQueryTime){
			if(otype == ORDER_ASC)
				continue;
			else
				break;
		}
		if(curTimestamps[rowId] > endQueryTime){
			if(otype == ORDER_ASC)
				break;
			else
				continue;
		}

		if(bitmapTestBit(pCache->haveTimestamp,rowId) == 0)
			continue;
		if(bitmapTestBit(pCache->isValueNone,rowId))
			continue;

		cJSON* j_field = cJSON_CreateObject();
		cJSON_AddItemToArray(j_fields,j_field);
		cJSON_AddNumberToObject(j_field,"Timestamp",curTimestamps[rowId]/1000);

    	if(f->type == FIELD_TYPE_BOOL){
			if(bitmapTestBit(pCache->bvalue,rowId)){
				cJSON_AddTrueToObject(j_field,"Value");
			}else{
				cJSON_AddFalseToObject(j_field,"Value");
			}
    	}else if(f->type == FIELD_TYPE_DOUBLE){
    		cJSON_AddNumberToObject(j_field,"Value",pCache->dvalue[rowId]);
    	}else if(f->type == FIELD_TYPE_STRING){
    		cJSON_AddStringToObject(j_field,"Value",pCache->strvalue + rowId*MAX_FIELD_STRING_LEN);
    	}else if(f->type == FIELD_TYPE_TING_INT){
    		cJSON_AddNumberToObject(j_field,"Value",pCache->tivalue[rowId]);
    	}else if(f->type == FIELD_TYPE_SMALL_INT){
    		cJSON_AddNumberToObject(j_field,"Value",pCache->sivalue[rowId]);
    	}else if(f->type == FIELD_TYPE_INT){
    		cJSON_AddNumberToObject(j_field,"Value",pCache->ivalue[rowId]);
    	}else if(f->type == FIELD_TYPE_BIG_INT){
    		cJSON_AddNumberToObject(j_field,"Value",pCache->bivalue[rowId]);
    	}else if(f->type == FIELD_TYPE_FLOAT){
    		cJSON_AddNumberToObject(j_field,"Value",pCache->fvalue[rowId]);
    	}else{
    		cJSON_AddNullToObject(j_field,"Value");
    	}
		return 1;
	}
	return 0;
}

int select_index_callback_fl(void *data, unsigned long long start,unsigned long long end,unsigned long long pos,unsigned long long max_fid,unsigned long long row_num){
	queryContext* qc = data;

	int timestamp_output_size = sizeof(timestamp_t)*CACHE_ROW_NUM;
	int buffer_size = MAX_FIELD_STRING_LEN*CACHE_ROW_NUM;
	if(qc->input == NULL){
		qc->timestamp_output = mosquitto_malloc(timestamp_output_size);
		qc->input = mosquitto_malloc(buffer_size);
		qc->index = mosquitto_malloc(sizeof(FINDEX)*(qc->d->field_max_id+1));
	}

	if(qc->fileTimestamp != getFileTimestamp(end)){
		if(qc->dataFp != NULL){
			//reclaimFilePageCache(fileno(dataFp), 0, 0);
			fclose(qc->dataFp);
			qc->dataFp = NULL;
		}

		char dbPathName[128];
		snprintf(dbPathName,128,"%s/%" PRIu64 ".data",iedb_dir,getFileTimestamp(end));

		qc->dataFp = fopen(dbPathName, "r");
		if(qc->dataFp == NULL){
			return 0;
		}
		qc->fileTimestamp = getFileTimestamp(end);
	}
	fseek(qc->dataFp, pos,SEEK_SET);
	if(fread(qc->index,1,sizeof(FINDEX)*(max_fid+1),qc->dataFp) != sizeof(FINDEX)*(max_fid+1))
		return 0;

	fseek(qc->dataFp, qc->index->dataOffset,SEEK_SET);
	if(fread(qc->input,1,qc->index->dataLength,qc->dataFp) != qc->index->dataLength)
		return 0;
	lzf_decompress(qc->input, qc->index->dataLength,qc->timestamp_output,timestamp_output_size);

	int allFound = 1;
	FLQFIELD* pFieldHeadTmp = qc->pFieldHead;
	size_t max_cache_size = getFieldCacheSize(pFieldHeadTmp->pField->type);
	cache* curCache = mosquitto_malloc(max_cache_size);
	while(pFieldHeadTmp){
		field* f = pFieldHeadTmp->pField;
		int size = cJSON_GetArraySize(pFieldHeadTmp->j_field);
		if((qc->otype == ORDER_DESC && size == 2) || (qc->otype == ORDER_ASC && size == 1)){
			pFieldHeadTmp = pFieldHeadTmp->next;
			continue;
		}
		if(f->id <= max_fid /*&& f->ctime < end*/){
			FINDEX* findex = qc->index + f->id;
			fseek(qc->dataFp, findex->dataOffset,SEEK_SET);
			if(fread(qc->input,1,findex->dataLength,qc->dataFp) != (size_t)findex->dataLength)
				break;

			size_t cache_size = getFieldCacheSize(f->type);
			if(cache_size > max_cache_size){
				max_cache_size = cache_size;
				curCache = mosquitto_realloc(curCache,cache_size);
			}
			lzf_decompress(qc->input, findex->dataLength,curCache,cache_size);

			if(addReplyCacheFL(pFieldHeadTmp->j_field,f,curCache,qc->timestamp_output,row_num,qc->startQueryTime,qc->endQueryTime,qc->otype) == 0){
				allFound = 0;
			}
		}
		pFieldHeadTmp = pFieldHeadTmp->next;
	}
	mosquitto_free(curCache);

	if(allFound == 0){
		return 0;
	}

	return -1;
}

void addReplyFiledFL(device *d,FLQFIELD* pFieldHead,timestamp_t startQueryTime,timestamp_t endQueryTime,ORDER_TYPE otype){
	if(otype == ORDER_DESC){
		int allFound = 1;
		FLQFIELD* pFieldHeadTmp = pFieldHead;
		while(pFieldHeadTmp){
			field* f = pFieldHeadTmp->pField;
			if(addReplyCacheFL(pFieldHeadTmp->j_field,f,f->pCache,d->timestamps,d->curTimestampPos+1,startQueryTime,endQueryTime,otype) == 0)
				allFound = 0;
			pFieldHeadTmp = pFieldHeadTmp->next;
		}
		if(allFound)
			return;
	}

	queryContext qc = {
	        .pFieldHead = pFieldHead,
	        .d = d,
			.startQueryTime = startQueryTime,
			.endQueryTime = endQueryTime,
			.otype = otype,
			.input = NULL,
			.timestamp_output = NULL,
			.dataFp = NULL,
			.fileTimestamp = 0,
			.index = NULL
	    };
	db_select_tsindex(&qc,select_index_callback_fl);
	if(qc.input != NULL){
		mosquitto_free(qc.input);
		mosquitto_free(qc.timestamp_output);
		mosquitto_free(qc.index);
	}

	if(qc.dataFp != NULL){
		//reclaimFilePageCache(fileno(dataFp), 0, 0);
		fclose(qc.dataFp);
	}

	if(otype == ORDER_ASC){
		FLQFIELD* pFieldHeadTmp = pFieldHead;
		while(pFieldHeadTmp){
			int size = cJSON_GetArraySize(pFieldHeadTmp->j_field);
			if(size == 1){
				pFieldHeadTmp = pFieldHeadTmp->next;
				continue;
			}

			field* f = pFieldHeadTmp->pField;
			addReplyCacheFL(pFieldHeadTmp->j_field,f,f->pCache,d->timestamps,d->curTimestampPos+1,startQueryTime,endQueryTime,otype);
			pFieldHeadTmp = pFieldHeadTmp->next;
		}
	}
}

void tsFLQuery(char** argv,cJSON *j_device,timestamp_t startQueryTime,timestamp_t endQueryTime,int device_id,int last_field_id){
    device* d = raxFind(devices,(unsigned char*)argv[device_id],strlen(argv[device_id]));
    if(d == raxNotFound){
    	return;
    }

    FLQFIELD* pFieldHead = NULL;
    FLQFIELD* pFieldPre = NULL;
    if(device_id >= last_field_id){
        raxIterator ri;
        raxStart(&ri, d->fields);
        raxSeek(&ri, "^", NULL, 0);
        while (raxNext(&ri)) {
        	char* fname = mosquitto_calloc(1,ri.key_len+1);
        	memcpy(fname,ri.key,ri.key_len);
        	cJSON* j_field = cJSON_AddArrayToObject(j_device,fname);
        	mosquitto_free(fname);

        	field *f = ri.data;
        	FLQFIELD* pFieldTemp = mosquitto_calloc(1,sizeof(FLQFIELD));
    		pFieldTemp->pField = f;
    		pFieldTemp->j_field = j_field;
    		if(pFieldHead == NULL){
    			pFieldHead = pFieldTemp;
    		}else{
    			pFieldPre->next = pFieldTemp;
    		}
    		pFieldPre = pFieldTemp;
        }
        raxStop(&ri);
    }else{
        for(int j = device_id+1; j<= last_field_id;j++){
        	cJSON* j_field = cJSON_AddArrayToObject(j_device,argv[j]);
        	field* f = raxFind(d->fields,(unsigned char*)argv[j],strlen(argv[j]));
        	if(f != raxNotFound){
            	FLQFIELD* pFieldTemp = mosquitto_calloc(1,sizeof(FLQFIELD));
        		pFieldTemp->pField = f;
        		pFieldTemp->j_field = j_field;
        		if(pFieldHead == NULL){
        			pFieldHead = pFieldTemp;
        		}else{
        			pFieldPre->next = pFieldTemp;
        		}
        		pFieldPre = pFieldTemp;
        	}
        }
    }

    addReplyFiledFL(d,pFieldHead,startQueryTime,endQueryTime,ORDER_ASC);
    addReplyFiledFL(d,pFieldHead,startQueryTime,endQueryTime,ORDER_DESC);

	while(pFieldHead){
		FLQFIELD* curField = pFieldHead;
		pFieldHead = pFieldHead->next;
		mosquitto_free(curField);
	}
}

/* TSFLQUERY start end d1 f1 f2 [DEVICE d2] [DEVICE d3 f4 f5 f6]*/
int tsFLQueryCommand(int argc,char** argv,cJSON** j_responses) {
    timestamp_t startQueryTime, endQueryTime;

    if (argc < 3) {
        /* If IDLE was provided we must have at least 'start end count' */
    	return HTTP_STATUS_BAD_REQUEST;
    }

    /* Parse start and end IDs. */
    if (genericParseTimestamp(argv[0],&startQueryTime,1) != C_OK){
    	return HTTP_STATUS_BAD_REQUEST;
    }
    if (genericParseTimestamp(argv[1],&endQueryTime,1) != C_OK){
    	return HTTP_STATUS_BAD_REQUEST;
    }
    startQueryTime *= 1000;
    endQueryTime *= 1000;

	cJSON *j_devices, *j_device;
	j_devices = cJSON_CreateObject();

    int device_id = 2;
    int j = 3;
    while(j < argc){
    	if (strcasecmp(argv[j],"DEVICE") == 0){
    		j_device = cJSON_CreateObject();
    		if(j_device == NULL){
    			cJSON_Delete(j_devices);
    			return HTTP_STATUS_INTERNAL_SERVER_ERROR;
    		}
    		cJSON_AddItemToObject(j_devices,argv[device_id], j_device);

    		tsFLQuery(argv,j_device,startQueryTime, endQueryTime,device_id,j-1);
    		device_id = ++j;
    	}

    	j++;
    }
    if(device_id < argc){
		j_device = cJSON_CreateObject();
		if(j_device == NULL){
			cJSON_Delete(j_devices);
			return HTTP_STATUS_INTERNAL_SERVER_ERROR;
		}
		cJSON_AddItemToObject(j_devices,argv[device_id], j_device);
    	tsFLQuery(argv,j_device,startQueryTime, endQueryTime,device_id,j-1);
    }

    *j_responses = j_devices;
    return 0;
}

void tsLastQuery(char** argv,cJSON *j_device,int device_id,int last_field_id){
    device* d = raxFind(devices,(unsigned char*)argv[device_id],strlen(argv[device_id]));
    if(d == raxNotFound){
    	return;
    }
    if(device_id >= last_field_id){
        raxIterator ri;
        raxStart(&ri, d->fields);
        raxSeek(&ri, "^", NULL, 0);
        while (raxNext(&ri)) {
        	field *f = ri.data;
        	char* fname = mosquitto_calloc(1,ri.key_len+1);
        	memcpy(fname,ri.key,ri.key_len);

        	if(f->flags & FIELD_FLAG_LAST_NONE){
        		cJSON_AddNullToObject(j_device,fname);
        	}else{
            	if(f->type == FIELD_TYPE_BOOL){
            		if(bitmapTestBit(f->pCache->bvalue,f->lastTimestampPos)){
            			cJSON_AddTrueToObject(j_device,fname);
            		}else{
            			cJSON_AddFalseToObject(j_device,fname);
            		}
            	}else if(f->type == FIELD_TYPE_DOUBLE){
            		cJSON_AddNumberToObject(j_device,fname,f->pCache->dvalue[f->lastTimestampPos]);
            	}else if(f->type == FIELD_TYPE_STRING){
            		cJSON_AddStringToObject(j_device,fname,f->pCache->strvalue + f->lastTimestampPos*MAX_FIELD_STRING_LEN);
            	}else if(f->type == FIELD_TYPE_TING_INT){
            		cJSON_AddNumberToObject(j_device,fname,f->pCache->tivalue[f->lastTimestampPos]);
            	}else if(f->type == FIELD_TYPE_SMALL_INT){
            		cJSON_AddNumberToObject(j_device,fname,f->pCache->sivalue[f->lastTimestampPos]);
            	}else if(f->type == FIELD_TYPE_INT){
            		cJSON_AddNumberToObject(j_device,fname,f->pCache->ivalue[f->lastTimestampPos]);
            	}else if(f->type == FIELD_TYPE_BIG_INT){
            		cJSON_AddNumberToObject(j_device,fname,f->pCache->bivalue[f->lastTimestampPos]);
            	}else if(f->type == FIELD_TYPE_FLOAT){
            		cJSON_AddNumberToObject(j_device,fname,f->pCache->fvalue[f->lastTimestampPos]);
            	}else{
            		cJSON_AddNullToObject(j_device,fname);
            	}
        	}
        	mosquitto_free(fname);
        }
        raxStop(&ri);
    }else{
        for(int j = device_id+1; j<= last_field_id;j++){
        	char* fname = argv[j];
        	field* f = raxFind(d->fields,(unsigned char*)fname,strlen(fname));
        	if(f != raxNotFound){
            	if(f->flags & FIELD_FLAG_LAST_NONE){
            		cJSON_AddNullToObject(j_device,fname);
            	}else{
                	if(f->type == FIELD_TYPE_BOOL){
                		if(bitmapTestBit(f->pCache->bvalue,f->lastTimestampPos)){
                			cJSON_AddTrueToObject(j_device,fname);
                		}else{
                			cJSON_AddFalseToObject(j_device,fname);
                		}
                	}else if(f->type == FIELD_TYPE_DOUBLE){
                		cJSON_AddNumberToObject(j_device,fname,f->pCache->dvalue[f->lastTimestampPos]);
                	}else if(f->type == FIELD_TYPE_STRING){
                		cJSON_AddStringToObject(j_device,fname,f->pCache->strvalue + f->lastTimestampPos*MAX_FIELD_STRING_LEN);
                	}else if(f->type == FIELD_TYPE_TING_INT){
                		cJSON_AddNumberToObject(j_device,fname,f->pCache->tivalue[f->lastTimestampPos]);
                	}else if(f->type == FIELD_TYPE_SMALL_INT){
                		cJSON_AddNumberToObject(j_device,fname,f->pCache->sivalue[f->lastTimestampPos]);
                	}else if(f->type == FIELD_TYPE_INT){
                		cJSON_AddNumberToObject(j_device,fname,f->pCache->ivalue[f->lastTimestampPos]);
                	}else if(f->type == FIELD_TYPE_BIG_INT){
                		cJSON_AddNumberToObject(j_device,fname,f->pCache->bivalue[f->lastTimestampPos]);
                	}else if(f->type == FIELD_TYPE_FLOAT){
                		cJSON_AddNumberToObject(j_device,fname,f->pCache->fvalue[f->lastTimestampPos]);
                	}else{
                		cJSON_AddNullToObject(j_device,fname);
                	}
            	}
        	}
        }
    }
}

/* TSLASTQUERY d1 f1 f2 [DEVICE d2] [DEVICE d3 f4 f5 f6]*/
int tsLastQueryCommand(int argc,char** argv,cJSON** j_responses) {
    timestamp_t startQueryTime, endQueryTime;

    if (argc < 1) {
        /* If IDLE was provided we must have at least 'start end count' */
    	return HTTP_STATUS_BAD_REQUEST;
    }

	cJSON *j_devices, *j_device;
	j_devices = cJSON_CreateObject();

    int device_id = 0;
    int j = 1;
    while(j < argc){
    	if (strcasecmp(argv[j],"DEVICE") == 0){
    		j_device = cJSON_CreateObject();
    		if(j_device == NULL){
    			cJSON_Delete(j_devices);
    			return HTTP_STATUS_INTERNAL_SERVER_ERROR;
    		}
    		cJSON_AddItemToObject(j_devices,argv[device_id], j_device);

    		tsLastQuery(argv,j_device,device_id,j-1);
    		device_id = ++j;
    	}

    	j++;
    }
    if(device_id < argc){
		j_device = cJSON_CreateObject();
		if(j_device == NULL){
			cJSON_Delete(j_devices);
			return HTTP_STATUS_INTERNAL_SERVER_ERROR;
		}
		cJSON_AddItemToObject(j_devices,argv[device_id], j_device);
    	tsLastQuery(argv,j_device,device_id,j-1);
    }

    *j_responses = j_devices;
    return 0;
}

int getLongLong(char *o, unsigned long *value) {
    char *eptr;

    /* Use strtoul() because we need an *unsigned* long, so
     * getLongLongFromObject() does not cover the whole cursor space. */
    errno = 0;
    *value = strtoul(o, &eptr, 10);
    if (isspace((o)[0]) || eptr[0] != '\0' || errno == ERANGE){

        return C_ERR;
    }
    return C_OK;
}

int tsScanCommand(int argc,char** argv,cJSON** j_responses) {
    unsigned long dcursor;
    unsigned long fcursor;
    unsigned long count;
    unsigned long sampled = 0;
    if (argc < 5) {
        return HTTP_STATUS_BAD_REQUEST;
    }

    char* dpattern = argv[0];
    int all_devices = (dpattern[0] == '*' && strlen(dpattern) == 1);
    char* fpattern = argv[1];
    int all_fields = (fpattern[0] == '*' && strlen(fpattern) == 1);

    if (getLongLong(argv[2],&dcursor) == C_ERR) return HTTP_STATUS_BAD_REQUEST;
    if (getLongLong(argv[3],&fcursor) == C_ERR) return HTTP_STATUS_BAD_REQUEST;
    if (getLongLong(argv[4], &count)== C_ERR) return HTTP_STATUS_BAD_REQUEST;
    if (count < 1) {
        return HTTP_STATUS_BAD_REQUEST;
    }

    cJSON* j_res = cJSON_CreateObject();
    cJSON* j_devices = cJSON_CreateArray();
    cJSON_AddItemToObject(j_res,"Result",j_devices);

    cJSON* j_device;
    cJSON* j_fields;
    raxIterator ri;
    raxStart(&ri, devices);
    raxSeek(&ri, "^", NULL, 0);
    while (raxNext(&ri)) {
        if (all_devices || stringmatchlen(dpattern,strlen(dpattern),ri.key,ri.key_len,0)) {
        	unsigned long numCurFields = 0;
        	unsigned long replyFieldlen = 0;

        	device *d = ri.data;
        	uint64_t totalFields = raxSize(d->fields);

            raxIterator rif;
            raxStart(&rif, d->fields);
            raxSeek(&rif, "^", NULL, 0);
            while (raxNext(&rif)) {
            	if ((!all_fields) && stringmatchlen(fpattern,strlen(fpattern),rif.key,rif.key_len,0) == 0) {
            		totalFields--;
            		continue;
            	}

            	if(numCurFields < fcursor){
            		numCurFields++;
            		continue;
            	}else if(numCurFields == fcursor){
                	j_device = cJSON_CreateObject();
                	cJSON_AddItemToArray(j_devices,j_device);
            		char* dname = mosquitto_calloc(1,ri.key_len+1);
            		memcpy(dname,ri.key,ri.key_len);
            		cJSON_AddStringToObject(j_device,"Device",dname);
            		mosquitto_free(dname);
            	    j_fields = cJSON_CreateArray();
            	    cJSON_AddItemToObject(j_device,"Fields",j_fields);
            	}
            	numCurFields++;
            	replyFieldlen++;

            	field *f = rif.data;
            	cJSON* j_field = cJSON_CreateObject();
            	cJSON_AddItemToArray(j_fields,j_field);

        		char* fname = mosquitto_calloc(1,rif.key_len+1);
        		memcpy(fname,rif.key,rif.key_len);
        		cJSON_AddStringToObject(j_field,"Field",fname);
        		mosquitto_free(fname);

        		cJSON_AddNumberToObject(j_field,"Timestamp",f->lastTimestamp/1000);
            	if(f->flags & FIELD_FLAG_LAST_NONE){
            		cJSON_AddNullToObject(j_field,"Value");
            	}else{
                	if(f->type == FIELD_TYPE_BOOL){
                		if(bitmapTestBit(f->pCache->bvalue,f->lastTimestampPos)){
                			cJSON_AddTrueToObject(j_field,"Value");
                		}else{
                			cJSON_AddFalseToObject(j_field,"Value");
                		}
                	}else if(f->type == FIELD_TYPE_DOUBLE){
                		cJSON_AddNumberToObject(j_field,"Value",f->pCache->dvalue[f->lastTimestampPos]);
                	}else if(f->type == FIELD_TYPE_STRING){
                		cJSON_AddStringToObject(j_field,"Value",f->pCache->strvalue + f->lastTimestampPos*MAX_FIELD_STRING_LEN);
                	}else if(f->type == FIELD_TYPE_TING_INT){
                		cJSON_AddNumberToObject(j_field,"Value",f->pCache->tivalue[f->lastTimestampPos]);
                	}else if(f->type == FIELD_TYPE_SMALL_INT){
                		cJSON_AddNumberToObject(j_field,"Value",f->pCache->sivalue[f->lastTimestampPos]);
                	}else if(f->type == FIELD_TYPE_INT){
                		cJSON_AddNumberToObject(j_field,"Value",f->pCache->ivalue[f->lastTimestampPos]);
                	}else if(f->type == FIELD_TYPE_BIG_INT){
                		cJSON_AddNumberToObject(j_field,"Value",f->pCache->bivalue[f->lastTimestampPos]);
                	}else if(f->type == FIELD_TYPE_FLOAT){
                		cJSON_AddNumberToObject(j_field,"Value",f->pCache->fvalue[f->lastTimestampPos]);
                	}else{
                		cJSON_AddNullToObject(j_field,"Value");
                	}
            	}

            	sampled++;
            	if(sampled >= count){
            		break;
            	}
            }
            raxStop(&ri);

            if(replyFieldlen != 0){
            	dcursor++;
            }
            if(totalFields == numCurFields){
            	fcursor = 0;
            }else{
            	fcursor = numCurFields;
            }
        }
    }
    raxStop(&ri);
    cJSON_AddNumberToObject(j_res,"dcursor",dcursor);
    cJSON_AddNumberToObject(j_res,"fcursor",fcursor);

    *j_responses = j_res;
    return 0;
}

int deleteRule(char* dname,field* src_f,char* func,TS_AGG_TYPES_T aggType,long long bucketDuration,long long timestampAlignment){
	CompactionRule *pre = NULL;
	CompactionRule *last = src_f->rules;
	while (last != NULL){
		if(last->bucketDuration == (timestamp_t)bucketDuration && last->aggType == aggType && last->timestampAlignment == (timestamp_t)timestampAlignment){
			if(pre){
				pre->nextRule = last->nextRule;
			}else{
				src_f->rules = last->nextRule;
			}

			FreeCompactionRule(last);
			return 0;
		}
		pre = last;
		last = last->nextRule;
	}
	return -1;
}

int addRule(char *dname,field* src_f,char* func,TS_AGG_TYPES_T aggType,long long bucketDuration,long long timestampAlignment){
    if (src_f->rules == NULL) {
    	src_f->rules = NewRule(func,aggType, bucketDuration, timestampAlignment);
    } else {
    	CompactionRule *last = src_f->rules;
        while (last->nextRule != NULL){
    		if(last->aggType == aggType && last->bucketDuration == bucketDuration && last->timestampAlignment == timestampAlignment)
    			return -1;
            last = last->nextRule;
        }
        last->nextRule = NewRule(func,aggType, bucketDuration, timestampAlignment);
    }
    return 0;
}

int tsRuleGenericCommand(cJSON** j_responses,int argc,char** argv,int add) {
    if (argc < 2) {
        return HTTP_STATUS_BAD_REQUEST;
    }

    device* d = raxFind(devices,(unsigned char*)argv[0],strlen(argv[0]));
    if(d == raxNotFound){
    	return HTTP_STATUS_NOT_FOUND;
    }

    cJSON* j_device = cJSON_CreateObject();
    for (int j = 1; j < argc; j++){
		TS_AGG_TYPES_T aggType;
		char *next1 = strchr(argv[j],'(');
		if(next1){
			aggType = StringLenAggTypeToEnum(argv[j],next1 - (char*)argv[j]);
		}else{
			cJSON_AddStringToObject(j_device,argv[j],"err");
			continue;
		}
		if(aggType == TS_AGG_INVALID){
			cJSON_AddStringToObject(j_device,argv[j],"err");
			continue;
		}

		char* next2 = strchr(next1+1,',');
		if(next2 == NULL){
			cJSON_AddStringToObject(j_device,argv[j],"err");
			continue;
		}

		char* next3 = strchr(next2+1,',');
		if(next3 == NULL){
			cJSON_AddStringToObject(j_device,argv[j],"err");
			continue;
		}

		char* next4 = strchr(next3+1,')');
		if(next4 == NULL){
			cJSON_AddStringToObject(j_device,argv[j],"err");
		    continue;
		}

		field* src_f = raxFind(d->fields,(unsigned char*)next1+1,next2-next1-1);
		if(src_f == raxNotFound){
			cJSON_AddStringToObject(j_device,argv[j],"err");
			continue;
		}

		long long bucketDuration;
		if(string2ll(next2+1,next3-next2-1,&bucketDuration) == 0){
			cJSON_AddStringToObject(j_device,argv[j],"err");
			continue;
		}
		bucketDuration *= 1000;
		long long timestampAlignment;
		if(string2ll(next3+1,next4-next3-1,&timestampAlignment) == 0){
			cJSON_AddStringToObject(j_device,argv[j],"err");
			continue;
		}
		timestampAlignment *= 1000;
		if(timestampAlignment < 60000 || timestampAlignment > bucketDuration){
			cJSON_AddStringToObject(j_device,argv[j],"err");
			continue;
		}

		if(add){
			if(addRule(argv[0],src_f,argv[j],aggType,bucketDuration,timestampAlignment) != 0)
				cJSON_AddStringToObject(j_device,argv[j],"err");
			else
				db_insert_rule(argv[0],argv[j]);
		}else{
			if(deleteRule(argv[0],src_f,argv[j],aggType,bucketDuration,timestampAlignment) != 0)
				cJSON_AddStringToObject(j_device,argv[j],"err");
			else
				db_delete_rule(argv[0],argv[j]);
		}
		cJSON_AddStringToObject(j_device,argv[j],"ok");
    }

    *j_responses = j_device;
    return 0;
}

/* TSADDRULE key max(f1,1000) ave(f2,6000) */
int tsAddRuleCommand(int argc,char** argv,cJSON** j_responses) {
	return tsRuleGenericCommand(j_responses,argc,argv,1);
}
/* TSDELRULE key max(f1,1000) ave(f2,6000) */
int tsDelRuleCommand(int argc,char** argv,cJSON** j_responses) {
	return tsRuleGenericCommand(j_responses,argc,argv,0);
}

void DeleteOldFile(void){
	mosquitto_log_printf(MOSQ_LOG_NOTICE,"ts deleteOldFile start!!!");

	int64_t maxDelFileId = 0;
	DIR *dir = NULL;
	if((dir = opendir(iedb_dir)) == NULL) {
		mosquitto_log_printf(MOSQ_LOG_WARNING, "Can't open ts dir %s: %s",iedb_dir, strerror(errno));
		return;
	}

	int64_t curFileId = getFileTimestamp(mstime());

	struct dirent *file;
	char dbPathName[128];

	int64_t selectedTimeStamp[11];
	int curFileNum = 0;
	int maxFileNum = 10;
	uint64_t cur_space = 0;
	while((file = readdir(dir))) {
		if (file->d_type != DT_REG)
			continue;

		int len = strlen(file->d_name);
		if(len < 10 || strcmp(file->d_name + (len - 5),".data") != 0)
			continue;

		char *endptr = NULL;
		int64_t endFileId = strtoll(file->d_name, &endptr, 10);
		if(*endptr != '.')
			continue;
		int64_t tmpFileId = ts_retention_hours;
		tmpFileId = endFileId + tmpFileId*TS_FILE_PERIOD;
		if(ts_retention_hours == 0 || tmpFileId > curFileId ){
			if(ts_retention_mbs > 0){
				int j = curFileNum;
				while(j){
					if(endFileId < selectedTimeStamp[j-1]){
						if(j < maxFileNum)
							selectedTimeStamp[j] = selectedTimeStamp[j-1];
					}else{
						break;
					}
					j--;
				}

				selectedTimeStamp[j] = endFileId;
				if(curFileNum < maxFileNum)
					curFileNum++;
			}

			struct stat st;
			snprintf(dbPathName,128,"%s/%" PRIu64 ".data",iedb_dir,endFileId);
			stat(dbPathName, &st);
			cur_space += st.st_size;
			continue;
		}

		if(lastDeleteFileId < endFileId)
			lastDeleteFileId = endFileId;
		snprintf(dbPathName,128,"%s/%" PRIu64 ".data",iedb_dir,endFileId);
		unlink(dbPathName);
		if(endFileId > maxDelFileId)
			maxDelFileId = endFileId;

		mosquitto_log_printf(MOSQ_LOG_NOTICE,"ts delete Old File for ts_retention_hours: %s!!!",dbPathName);
	}
	closedir(dir);

	uint64_t dest_space = ts_retention_mbs;
	dest_space *= 1024*1024;
	for(int i = 0; i < curFileNum; i++){
		if(cur_space < dest_space)
			break;

		struct stat st;
		snprintf(dbPathName,128,"%s/%" PRIu64 ".data",iedb_dir,selectedTimeStamp[i]);
		stat(dbPathName, &st);
		cur_space -= st.st_size;
		unlink(dbPathName);
		if(selectedTimeStamp[i] > maxDelFileId)
			maxDelFileId = selectedTimeStamp[i];

		mosquitto_log_printf(MOSQ_LOG_NOTICE,"ts delete Old File for ts_retention_mbs : %s!!!",dbPathName);

		if(lastDeleteFileId < selectedTimeStamp[i])
			lastDeleteFileId = selectedTimeStamp[i];
	}
	if(maxDelFileId > 0)
		db_delete_tsindex(maxDelFileId+TS_FILE_PERIOD);

}

int mqtt_tick_callback(int event, void *event_data, void *userdata){
	static time_t pre_now_s = 0;
	struct mosquitto_evt_tick *event_tick = event_data;

	if (!(event_tick->now_s%3600)){
		if(pre_now_s != event_tick->now_s){
			pre_now_s = event_tick->now_s;
			DeleteOldFile();
		}
	}

	return MOSQ_ERR_SUCCESS;
}

int tsAllDevicesCommand(int argc,char** argv,cJSON** j_responses){
    if (argc != 0) {
        return HTTP_STATUS_BAD_REQUEST;
    }
    cJSON* j_devices = cJSON_CreateArray();

	raxIterator ri;
    raxStart(&ri, devices);
    raxSeek(&ri, "^", NULL, 0);
    while (raxNext(&ri)) {
    	char* buf = mosquitto_calloc(1,ri.key_len+1);
    	memcpy(buf,ri.key,ri.key_len);
    	cJSON* j_d = cJSON_CreateString(buf);
    	mosquitto_free(buf);
    	cJSON_AddItemToArray(j_devices,j_d);
    }
    raxStop(&ri);
    *j_responses = j_devices;
    return 0;
}

int tsAllFieldsCommand(int argc,char** argv,cJSON** j_responses){
    if (argc != 1) {
        return HTTP_STATUS_BAD_REQUEST;
    }
    device* d = raxFind(devices,(unsigned char*)argv[0],strlen(argv[0]));
    if(d == raxNotFound){
    	return HTTP_STATUS_NOT_FOUND;
    }

    cJSON* j_fields = cJSON_CreateArray();

	raxIterator ri;
    raxStart(&ri, d->fields);
    raxSeek(&ri, "^", NULL, 0);
    while (raxNext(&ri)) {
    	field* f = ri.data;
    	cJSON* j_d = cJSON_CreateObject();

    	char* buf = mosquitto_calloc(1,ri.key_len+1);
    	memcpy(buf,ri.key,ri.key_len);
    	cJSON_AddStringToObject(j_d,"name",buf);
    	mosquitto_free(buf);

    	if(f->type == FIELD_TYPE_BOOL){
    		cJSON_AddStringToObject(j_d,"type","BOOL");
    	}else if(f->type == FIELD_TYPE_DOUBLE){
    		cJSON_AddStringToObject(j_d,"type","DOUBLE");
    	}else if(f->type == FIELD_TYPE_STRING){
    		cJSON_AddStringToObject(j_d,"type","STRING");
    	}else if(f->type == FIELD_TYPE_TING_INT){
    		cJSON_AddStringToObject(j_d,"type","TING_INT");
    	}else if(f->type == FIELD_TYPE_SMALL_INT){
    		cJSON_AddStringToObject(j_d,"type","SMALL_INT");
    	}else if(f->type == FIELD_TYPE_INT){
    		cJSON_AddStringToObject(j_d,"type","INT");
    	}else if(f->type == FIELD_TYPE_BIG_INT){
    		cJSON_AddStringToObject(j_d,"type","BIG_INT");
    	}else if(f->type == FIELD_TYPE_FLOAT){
    		cJSON_AddStringToObject(j_d,"type","FLOAT");
    	}else{
    		cJSON_AddNullToObject(j_d,"type");
    	}

    	cJSON_AddItemToArray(j_fields,j_d);
    }
    raxStop(&ri);
    *j_responses = j_fields;
    return 0;
}

int tsAllDevicesFiledsCommand(int argc,char** argv,cJSON** j_responses){
    if (argc != 0) {
        return HTTP_STATUS_BAD_REQUEST;
    }
    cJSON* j_devices = cJSON_CreateObject();

	raxIterator rid;
    raxStart(&rid, devices);
    raxSeek(&rid, "^", NULL, 0);
    while (raxNext(&rid)) {
    	device* d = rid.data;

    	cJSON* j_fields = cJSON_CreateArray();
    	char* buf = mosquitto_calloc(1,rid.key_len+1);
    	memcpy(buf,rid.key,rid.key_len);
    	cJSON_AddItemToObject(j_devices,buf,j_fields);
    	mosquitto_free(buf);

    	raxIterator rif;
        raxStart(&rif, d->fields);
        raxSeek(&rif, "^", NULL, 0);
        while (raxNext(&rif)) {
        	field* f = rif.data;
        	cJSON* j_d = cJSON_CreateObject();

        	buf = mosquitto_calloc(1,rif.key_len+1);
        	memcpy(buf,rif.key,rif.key_len);
        	cJSON_AddStringToObject(j_d,"name",buf);
        	mosquitto_free(buf);

        	if(f->type == FIELD_TYPE_BOOL){
        		cJSON_AddStringToObject(j_d,"type","BOOL");
        	}else if(f->type == FIELD_TYPE_DOUBLE){
        		cJSON_AddStringToObject(j_d,"type","DOUBLE");
        	}else if(f->type == FIELD_TYPE_STRING){
        		cJSON_AddStringToObject(j_d,"type","STRING");
        	}else if(f->type == FIELD_TYPE_TING_INT){
        		cJSON_AddStringToObject(j_d,"type","TING_INT");
        	}else if(f->type == FIELD_TYPE_SMALL_INT){
        		cJSON_AddStringToObject(j_d,"type","SMALL_INT");
        	}else if(f->type == FIELD_TYPE_INT){
        		cJSON_AddStringToObject(j_d,"type","INT");
        	}else if(f->type == FIELD_TYPE_BIG_INT){
        		cJSON_AddStringToObject(j_d,"type","BIG_INT");
        	}else if(f->type == FIELD_TYPE_FLOAT){
        		cJSON_AddStringToObject(j_d,"type","FLOAT");
        	}else{
        		cJSON_AddNullToObject(j_d,"type");
        	}

        	cJSON_AddItemToArray(j_fields,j_d);
        }
        raxStop(&rif);
    }
    raxStop(&rid);
    *j_responses = j_devices;
    return 0;
}

int addReplyCacheDiff(DIFFQFIELD *pFieldHead,cache *pCache,timestamp_t* curTimestamps,size_t curTotalSamples,timestamp_t startQueryTime,timestamp_t endQueryTime,ORDER_TYPE otype){
	field *f = pFieldHead->pField;
	int start,end,step;
	if(otype == ORDER_ASC){
		start = 0;
		end = curTotalSamples;
		step = 1;
	}else{
		start = curTotalSamples-1;
		end = -1;
		step = -1;
	}
	for(int rowId = start; rowId != end; rowId+=step){
		if(curTimestamps[rowId] < startQueryTime){
			if(otype == ORDER_ASC)
				continue;
			else
				break;
		}
		if(curTimestamps[rowId] > endQueryTime){
			if(otype == ORDER_ASC)
				break;
			else
				continue;
		}

		if(bitmapTestBit(pCache->haveTimestamp,rowId) == 0)
			continue;
		if(bitmapTestBit(pCache->isValueNone,rowId))
			continue;

    	if(f->type == FIELD_TYPE_DOUBLE){
    		if(otype == ORDER_ASC){
    			pFieldHead->first_val = pCache->dvalue[rowId];
    			pFieldHead->first_found = 1;
    		}else{
    			pFieldHead->last_val = pCache->dvalue[rowId];
    			pFieldHead->last_found = 1;
    		}
    	}else if(f->type == FIELD_TYPE_TING_INT){
    		if(otype == ORDER_ASC){
    			pFieldHead->first_val = pCache->tivalue[rowId];
    			pFieldHead->first_found = 1;
    		}else{
    			pFieldHead->last_val = pCache->tivalue[rowId];
    			pFieldHead->last_found = 1;
    		}
    	}else if(f->type == FIELD_TYPE_SMALL_INT){
    		if(otype == ORDER_ASC){
    			pFieldHead->first_val = pCache->sivalue[rowId];
    			pFieldHead->first_found = 1;
    		}else{
    			pFieldHead->last_val = pCache->sivalue[rowId];
    			pFieldHead->last_found = 1;
    		}
    	}else if(f->type == FIELD_TYPE_INT){
    		if(otype == ORDER_ASC){
    			pFieldHead->first_val = pCache->ivalue[rowId];
    			pFieldHead->first_found = 1;
    		}else{
    			pFieldHead->last_val = pCache->ivalue[rowId];
    			pFieldHead->last_found = 1;
    		}
    	}else if(f->type == FIELD_TYPE_BIG_INT){
    		if(otype == ORDER_ASC){
    			pFieldHead->first_val = pCache->bivalue[rowId];
    			pFieldHead->first_found = 1;
    		}else{
    			pFieldHead->last_val = pCache->bivalue[rowId];
    			pFieldHead->last_found = 1;
    		}
    	}else if(f->type == FIELD_TYPE_FLOAT){
    		if(otype == ORDER_ASC){
    			pFieldHead->first_val = pCache->fvalue[rowId];
    			pFieldHead->first_found = 1;
    		}else{
    			pFieldHead->last_val = pCache->fvalue[rowId];
    			pFieldHead->last_found = 1;
    		}
    	}
		return 1;
	}
	return 0;
}

int select_index_callback_diff(void *data, unsigned long long start,unsigned long long end,unsigned long long pos,unsigned long long max_fid,unsigned long long row_num){
	queryContext* qc = data;

	int timestamp_output_size = sizeof(timestamp_t)*CACHE_ROW_NUM;
	int buffer_size = MAX_FIELD_STRING_LEN*CACHE_ROW_NUM;
	if(qc->input == NULL){
		qc->timestamp_output = mosquitto_malloc(timestamp_output_size);
		qc->input = mosquitto_malloc(buffer_size);
		qc->index = mosquitto_malloc(sizeof(FINDEX)*(qc->d->field_max_id+1));
	}

	if(qc->fileTimestamp != getFileTimestamp(end)){
		if(qc->dataFp != NULL){
			//reclaimFilePageCache(fileno(dataFp), 0, 0);
			fclose(qc->dataFp);
			qc->dataFp = NULL;
		}

		char dbPathName[128];
		snprintf(dbPathName,128,"%s/%" PRIu64 ".data",iedb_dir,getFileTimestamp(end));

		qc->dataFp = fopen(dbPathName, "r");
		if(qc->dataFp == NULL){
			return 0;
		}
		qc->fileTimestamp = getFileTimestamp(end);
	}
	fseek(qc->dataFp, pos,SEEK_SET);
	if(fread(qc->index,1,sizeof(FINDEX)*(max_fid+1),qc->dataFp) != sizeof(FINDEX)*(max_fid+1))
		return 0;

	fseek(qc->dataFp, qc->index->dataOffset,SEEK_SET);
	if(fread(qc->input,1,qc->index->dataLength,qc->dataFp) != qc->index->dataLength)
		return 0;
	lzf_decompress(qc->input, qc->index->dataLength,qc->timestamp_output,timestamp_output_size);

	int allFound = 1;
	DIFFQFIELD* pFieldHeadTmp = qc->pFieldHead;
	size_t max_cache_size = getFieldCacheSize(pFieldHeadTmp->pField->type);
	cache* curCache = mosquitto_malloc(max_cache_size);
	while(pFieldHeadTmp){
		if(qc->otype == ORDER_ASC && pFieldHeadTmp->first_found){
			pFieldHeadTmp = pFieldHeadTmp->next;
			continue;
		}
		if(qc->otype == ORDER_DESC && pFieldHeadTmp->last_found){
			pFieldHeadTmp = pFieldHeadTmp->next;
			continue;
		}
		field* f = pFieldHeadTmp->pField;
		if(f->id <= max_fid /*&& f->ctime < end*/){
			FINDEX* findex = qc->index + f->id;
			fseek(qc->dataFp, findex->dataOffset,SEEK_SET);
			if(fread(qc->input,1,findex->dataLength,qc->dataFp) != (size_t)findex->dataLength)
				break;

			size_t cache_size = getFieldCacheSize(f->type);
			if(cache_size > max_cache_size){
				max_cache_size = cache_size;
				curCache = mosquitto_realloc(curCache,cache_size);
			}
			lzf_decompress(qc->input, findex->dataLength,curCache,cache_size);

			if(addReplyCacheDiff(pFieldHeadTmp,curCache,qc->timestamp_output,row_num,qc->startQueryTime,qc->endQueryTime,qc->otype) == 0){
				allFound = 0;
			}
		}
		pFieldHeadTmp = pFieldHeadTmp->next;
	}
	mosquitto_free(curCache);

	if(allFound == 0){
		return 0;
	}

	return -1;
}

void getFirstFieldValue(device *d,DIFFQFIELD* pFieldHead,timestamp_t startQueryTime,timestamp_t endQueryTime,ORDER_TYPE otype){
	DIFFQFIELD* pFieldHeadTmp = pFieldHead;
	while(pFieldHeadTmp){
		if(otype == ORDER_DESC)
			pFieldHeadTmp->last_found = 0;
		else
			pFieldHeadTmp->first_found = 0;
		pFieldHeadTmp = pFieldHeadTmp->next;
	}

	if(otype == ORDER_DESC){
		int allFound = 1;
		pFieldHeadTmp = pFieldHead;
		while(pFieldHeadTmp){
			field* f = pFieldHeadTmp->pField;
			if(addReplyCacheDiff(pFieldHeadTmp,f->pCache,d->timestamps,d->curTimestampPos+1,startQueryTime,endQueryTime,otype) == 0)
				allFound = 0;
			pFieldHeadTmp = pFieldHeadTmp->next;
		}
		if(allFound)
			return;
	}

	queryContext qc = {
	        .pFieldHead = pFieldHead,
	        .d = d,
			.startQueryTime = startQueryTime,
			.endQueryTime = endQueryTime,
			.otype = otype,
			.input = NULL,
			.timestamp_output = NULL,
			.dataFp = NULL,
			.fileTimestamp = 0,
			.index = NULL
	    };
	db_select_tsindex(&qc,select_index_callback_diff);
	if(qc.input != NULL){
		mosquitto_free(qc.input);
		mosquitto_free(qc.timestamp_output);
		mosquitto_free(qc.index);
	}

	if(qc.dataFp != NULL){
		//reclaimFilePageCache(fileno(dataFp), 0, 0);
		fclose(qc.dataFp);
	}

	if(otype == ORDER_ASC){
		DIFFQFIELD* pFieldHeadTmp = pFieldHead;
		while(pFieldHeadTmp){
			if(pFieldHeadTmp->first_found){
				pFieldHeadTmp = pFieldHeadTmp->next;
				continue;
			}

			field* f = pFieldHeadTmp->pField;
			addReplyCacheDiff(pFieldHeadTmp,f->pCache,d->timestamps,d->curTimestampPos+1,startQueryTime,endQueryTime,otype);
			pFieldHeadTmp = pFieldHeadTmp->next;
		}
	}
}

void tsDiffQuery(char** argv,cJSON *j_devices,timestamp_t period, timestamp_t startQueryTime,timestamp_t endQueryTime,int device_id,int last_field_id){
	device* d = raxFind(devices,(unsigned char*)argv[device_id],strlen(argv[device_id]));
    if(d == raxNotFound){
    	return;
    }

    cJSON *j_device = cJSON_CreateArray();
    cJSON_AddItemToObject(j_devices,argv[device_id],j_device);

    DIFFQFIELD* pFieldHead = NULL;
    DIFFQFIELD* pFieldPre = NULL;
    if(device_id >= last_field_id){
        raxIterator ri;
        raxStart(&ri, d->fields);
        raxSeek(&ri, "^", NULL, 0);
        while (raxNext(&ri)) {
        	field *f = ri.data;
        	if(f->type != FIELD_TYPE_DOUBLE &&
        			f->type != FIELD_TYPE_TING_INT &&
					f->type != FIELD_TYPE_SMALL_INT &&
        			f->type != FIELD_TYPE_INT &&
					f->type != FIELD_TYPE_BIG_INT &&
					f->type != FIELD_TYPE_FLOAT){
        		continue;
        	}
        	//char* fname = mosquitto_calloc(1,ri.key_len+1);
        	//memcpy(fname,ri.key,ri.key_len);

        	DIFFQFIELD* pFieldTemp = mosquitto_calloc(1,sizeof(DIFFQFIELD));
    		pFieldTemp->pField = f;
    		pFieldTemp->name = ri.key;
    		if(pFieldHead == NULL){
    			pFieldHead = pFieldTemp;
    		}else{
    			pFieldPre->next = pFieldTemp;
    		}
    		pFieldPre = pFieldTemp;
        }
        raxStop(&ri);
    }else{
        for(int j = device_id+1; j<= last_field_id;j++){
        	field* f = raxFind(d->fields,(unsigned char*)argv[j],strlen(argv[j]));
        	if(f != raxNotFound){
            	if(f->type != FIELD_TYPE_DOUBLE &&
            			f->type != FIELD_TYPE_TING_INT &&
    					f->type != FIELD_TYPE_SMALL_INT &&
            			f->type != FIELD_TYPE_INT &&
    					f->type != FIELD_TYPE_BIG_INT &&
    					f->type != FIELD_TYPE_FLOAT){
            		continue;
            	}

        		DIFFQFIELD* pFieldTemp = mosquitto_calloc(1,sizeof(DIFFQFIELD));
        		pFieldTemp->pField = f;
        		pFieldTemp->name = argv[j];
        		if(pFieldHead == NULL){
        			pFieldHead = pFieldTemp;
        		}else{
        			pFieldPre->next = pFieldTemp;
        		}
        		pFieldPre = pFieldTemp;
        	}
        }
    }

    if(pFieldHead == NULL)
    	return;

    timestamp_t startSubQueryTime = startQueryTime;
    timestamp_t endSubQueryTime = startSubQueryTime + period;

    getFirstFieldValue(d,pFieldHead,startSubQueryTime,endSubQueryTime,ORDER_ASC);
    while(endSubQueryTime < endQueryTime){
    	getFirstFieldValue(d,pFieldHead,startSubQueryTime,endSubQueryTime,ORDER_DESC);

        cJSON * j_row = NULL;
		DIFFQFIELD* pFieldHeadTmp = pFieldHead;
		while(pFieldHeadTmp){
			if(!pFieldHeadTmp->last_found){
				pFieldHeadTmp = pFieldHeadTmp->next;
				continue;
			}
			if(!pFieldHeadTmp->first_found){
				getFirstFieldValue(d,pFieldHead,startSubQueryTime,endSubQueryTime,ORDER_ASC);
			}
			if(j_row == NULL){
				j_row = cJSON_CreateObject();
				cJSON_AddNumberToObject(j_row,"timestamp",endSubQueryTime/1000);
				cJSON_AddItemToArray(j_device,j_row);
			}
			cJSON_AddNumberToObject(j_row,pFieldHeadTmp->name,pFieldHeadTmp->last_val-pFieldHeadTmp->first_val);
			pFieldHeadTmp->first_val = pFieldHeadTmp->last_val;
			pFieldHeadTmp->first_found = 1;
			pFieldHeadTmp = pFieldHeadTmp->next;
		}

		startSubQueryTime = startSubQueryTime + period;
		endSubQueryTime = endSubQueryTime + period;
    }

	while(pFieldHead){
		DIFFQFIELD* curField = pFieldHead;
		pFieldHead = pFieldHead->next;
		mosquitto_free(curField);
	}
}

/* TSDIFFQUERY period start end d1 f1 f2 [DEVICE d2] [DEVICE d3 f4 f5 f6]*/
int tsDiffQueryCommand(int argc,char** argv,cJSON** j_responses) {
    timestamp_t startQueryTime, endQueryTime,period;

    if (argc < 4) {
    	return HTTP_STATUS_BAD_REQUEST;
    }

    /* Parse start and end IDs. */
    if (genericParseTimestamp(argv[0],&period,1) != C_OK){
        return HTTP_STATUS_BAD_REQUEST;
    }
    if (genericParseTimestamp(argv[1],&startQueryTime,1) != C_OK){
    	return HTTP_STATUS_BAD_REQUEST;
    }
    if (genericParseTimestamp(argv[2],&endQueryTime,1) != C_OK){
    	return HTTP_STATUS_BAD_REQUEST;
    }
    period *= 60*1000;
    startQueryTime *= 1000;
    endQueryTime *= 1000;
    timestamp_t now = mstime();
    if(now < endQueryTime)
    	endQueryTime = now;

    while(now - (ts_retention_hours+2)*60*60*1000 > startQueryTime){
    	startQueryTime += period;
    }

    //startQueryTime = startQueryTime - startQueryTime%period;
    //endQueryTime = endQueryTime - endQueryTime%period;

    cJSON *j_devices = cJSON_CreateObject();

    int device_id = 3;
    int j = 4;
    while(j < argc){
    	if (strcasecmp(argv[j],"DEVICE") == 0){
    		tsDiffQuery(argv,j_devices,period,startQueryTime, endQueryTime,device_id,j-1);
    		device_id = ++j;
    	}

    	j++;
    }
    if(device_id < argc){
    	tsDiffQuery(argv,j_devices,period,startQueryTime, endQueryTime,device_id,j-1);
    }

    *j_responses = j_devices;
    return 0;
}

const char *fast_double_parser_c_parse_number(const char *p, double *outDouble){
	char *eptr;
	double d = strtod(p,&eptr);
	if (eptr[0] != '\0' || errno == ERANGE || isnan(d)){
		return NULL;
	}
	*outDouble = d;
	return eptr;
}

int parseTsdata(device* d,char* uplpad_buf){
	timestamp_t lastTimestamp = 0;
	deviceSaveToDisk(d);
	d->curTimestampPos = -1;
	if(d->lastTimestamp > 0){
		lastTimestamp = d->lastTimestamp;
		d->lastTimestamp = 0;
	}

	size_t buf_len = strlen(uplpad_buf);
	char* start = uplpad_buf;
	char* end = strchr(start,'\r');
	if(end){
		*end = 0;
	}

	int fields_num = 0;
	char* tmp;
	while(1){
		fields_num++;
		tmp = strchr(start,',');
		if(!tmp)
			break;
		start = tmp+1;
	}

	char **all_names = mosquitto_calloc(fields_num,sizeof(char *));
	field **all_fields = mosquitto_calloc(fields_num,sizeof(field *));

	int i = 0;
	char* field_start = uplpad_buf;
	char* field_end;
	while(1){
		all_names[i++] = field_start;
		field_end = strchr(field_start,',');
		if(field_end)
			*field_end = 0;
		else
			break;

		field_start = field_end+1;
	}

	while(1){
		start = end + 2;
		if(start-uplpad_buf > buf_len-1)
			break;
		end = strchr(start,'\r');
		if(end){
			*end = 0;
		}

		int rowId;
		int i = 0;
		char* ts;
		char* field_start = start;
		while(1){
			field *f;
			field_end = strchr(field_start,',');
			if(field_end){
				*field_end = 0;
			}

			if(i == 0){
			    unsigned long long ts;
			    if (string2ull(field_start,&ts) == 0)
			    	break;
			    ts *= 1000;
			    if(ts <= d->lastTimestamp){
			    	break;
				}else{
					if(d->curTimestampPos >= CACHE_ROW_NUM-1){
						deviceSaveToDisk(d);
						d->curTimestampPos = 0;
					}else{
						d->curTimestampPos++;
					}
				}
			    rowId = d->curTimestampPos;

			    d->timestamps[rowId] = ts;
			    d->lastTimestamp = ts;
			}else{
				char* fvalue = field_start;
				if(!all_fields[i]){
					f = raxFind(d->fields,(unsigned char*)all_names[i],strlen(all_names[i]));
					if (f == raxNotFound && strlen(fvalue) > 0){
			    		FIELD_TYPE ftype = FIELD_TYPE_STRING;
			    		if(strcasecmp(fvalue,"true") == 0 || strcasecmp(fvalue,"false") == 0){
			    			ftype = FIELD_TYPE_BOOL;
			    		}else{
			    			double sample;
			    			if ((fast_double_parser_c_parse_number(fvalue, &sample) != NULL)) {
			    				ftype = FIELD_TYPE_DOUBLE;
			    			}
			    		}

			    		f = fieldNew(++d->field_max_id,ftype,ts);
			    		raxInsert(d->fields,(unsigned char*)all_names[i],strlen(all_names[i]),f,NULL);
			    		db_insert_field(all_names[i],d->field_max_id,d->id,ts,ftype);
			    		all_fields[i] = f;
					}
				}else{
					f = all_fields[i];
				}

				if(strlen(fvalue) > 0){
					bitmapSetBit(f->pCache->haveTimestamp,rowId);
					f->lastTimestampPos = rowId;
					f->lastTimestamp = ts;
					if(f->type == FIELD_TYPE_BOOL){
						if(strcasecmp(fvalue,"true") == 0){
							f->flags &= ~FIELD_FLAG_LAST_NONE;
							bitmapSetBit(f->pCache->bvalue,rowId);
						}
						else if(strcasecmp(fvalue,"false") == 0){
							f->flags &= ~FIELD_FLAG_LAST_NONE;
							bitmapClearBit(f->pCache->bvalue,rowId);
						}
						else{
							bitmapSetBit(f->pCache->isValueNone,rowId);
							f->flags |= FIELD_FLAG_LAST_NONE;
						}
					}else if(f->type == FIELD_TYPE_DOUBLE){
		    			double sample;
		    			if ((fast_double_parser_c_parse_number(fvalue, &sample) != NULL)) {
		    				f->pCache->dvalue[rowId] = sample;
		    				if(f->flags & FIELD_FLAG_ACC){
		    					if(f->pCache->dvalue[f->lastTimestampPos] >= sample){
		    						bitmapSetBit(f->pCache->isValueNone,rowId);
		    					}
		    				}
		    			}else{
		    				bitmapSetBit(f->pCache->isValueNone,rowId);
		    			}
					}else if(f->type == FIELD_TYPE_FLOAT){
		    			double sample;
		    			if ((fast_double_parser_c_parse_number(fvalue, &sample) != NULL)) {
		    				f->pCache->fvalue[rowId] = sample;
		    				if(f->flags & FIELD_FLAG_ACC){
		    					if(f->pCache->fvalue[f->lastTimestampPos] >= sample){
		    						bitmapSetBit(f->pCache->isValueNone,rowId);
		    					}
		    				}
		    			}else{
		    				bitmapSetBit(f->pCache->isValueNone,rowId);
		    			}
					}else if(f->type == FIELD_TYPE_STRING){
						size_t len = strlen(fvalue);
						if(len >= MAX_FIELD_STRING_LEN)
							len = MAX_FIELD_STRING_LEN-1;
						memcpy(f->pCache->strvalue + MAX_FIELD_STRING_LEN*rowId,fvalue,len);
						f->pCache->strvalue[MAX_FIELD_STRING_LEN*rowId+len] = '\0';
					}else if(f->type == FIELD_TYPE_SMALL_INT){
						char* eptr;
				        long long int v = strtoll(fvalue, &eptr, 10);
				        if (eptr != '\0'){
				        	bitmapSetBit(f->pCache->isValueNone,rowId);
				        }else if(f->flags & FIELD_FLAG_ACC){
							f->pCache->sivalue[rowId] = (short)v;
							if(f->pCache->sivalue[f->lastTimestampPos] >= (short)v){
								bitmapSetBit(f->pCache->isValueNone,rowId);
							}
						}else{
							f->pCache->sivalue[rowId] = (short)v;
						}
					}else if(f->type == FIELD_TYPE_TING_INT){
						char* eptr;
				        long long int v = strtoll(fvalue, &eptr, 10);
				        if (eptr != '\0'){
				        	bitmapSetBit(f->pCache->isValueNone,rowId);
				        }else if(f->flags & FIELD_FLAG_ACC){
							f->pCache->tivalue[rowId] = (char)v;
							if(f->pCache->tivalue[f->lastTimestampPos] >= (char)v){
								bitmapSetBit(f->pCache->isValueNone,rowId);
							}
						}else{
							f->pCache->tivalue[rowId] = (char)v;
						}
					}else if(f->type == FIELD_TYPE_INT){
						char* eptr;
				        long long int v = strtoll(fvalue, &eptr, 10);
				        if (eptr != '\0'){
				        	bitmapSetBit(f->pCache->isValueNone,rowId);
				        }else if(f->flags & FIELD_FLAG_ACC){
							f->pCache->ivalue[rowId] = (int)v;
							if(f->pCache->ivalue[f->lastTimestampPos] >= (int)v){
								bitmapSetBit(f->pCache->isValueNone,rowId);
							}
						}else{
							f->pCache->ivalue[rowId] = (int)v;
						}
					}else if(f->type == FIELD_TYPE_BIG_INT){
						char* eptr;
				        long long int v = strtoll(fvalue, &eptr, 10);
				        if (eptr != '\0'){
				        	bitmapSetBit(f->pCache->isValueNone,rowId);
				        }else if(f->flags & FIELD_FLAG_ACC){
							f->pCache->bivalue[rowId] = (int64_t)v;
							if(f->pCache->bivalue[f->lastTimestampPos] >= (int64_t)v){
								bitmapSetBit(f->pCache->isValueNone,rowId);
							}
						}else{
							f->pCache->bivalue[rowId] = (int64_t)v;
						}
					}
				}
			}

			if(!field_end)
				break;

			field_start = field_end+1;
			i++;
		}
	}

	deviceSaveToDisk(d);
	d->curTimestampPos = -1;
	if(lastTimestamp > 0){
		d->lastTimestamp = lastTimestamp;
	}

	mosquitto_free(all_fields);
	mosquitto_free(all_names);
	return 0;
}

int newdeviceCommand(int argc,char** argv,cJSON** j_responses){
    if (argc < 1) {
    	return HTTP_STATUS_BAD_REQUEST;
    }

    cJSON *j_devices = cJSON_CreateObject();
	device* d = raxFind(devices,(unsigned char*)argv[0],strlen(argv[0]));
    if(d == raxNotFound){
    	d = deviceNew(++device_max_id);
    	raxInsert(devices,(unsigned char*)argv[0],strlen(argv[0]),d,NULL);
    	db_insert_device(argv[0],device_max_id);
    	cJSON_AddStringToObject(j_devices,argv[0],"New device successfully!");
    }else{
    	cJSON_AddStringToObject(j_devices,argv[0],"Device exists!");
    }

    *j_responses = j_devices;
    return 0;
}

int deldeviceCommand(int argc,char** argv,cJSON** j_responses){
    if (argc < 1) {
    	return HTTP_STATUS_BAD_REQUEST;
    }

    cJSON *j_devices = cJSON_CreateObject();
	device* d = raxFind(devices,(unsigned char*)argv[0],strlen(argv[0]));
    if(d != raxNotFound){
    	db_delete_tsindex_by_device(d->id);
    	db_delete_device(d->id);
    	raxRemove(devices,(unsigned char*)argv[0],strlen(argv[0]),NULL);
    	freeDevice(d);

    	cJSON_AddStringToObject(j_devices,argv[0],"Del device successfully!");
    }else{
    	cJSON_AddStringToObject(j_devices,argv[0],"Device not exists!");
    }

    *j_responses = j_devices;
    return 0;
}

int uploadCommand(int argc,char** argv,cJSON** j_responses){
    if (argc < 3) {
    	return HTTP_STATUS_BAD_REQUEST;
    }
	device* d = raxFind(devices,(unsigned char*)argv[1],strlen(argv[1]));
    if(d == raxNotFound){
    	d = deviceNew(++device_max_id);
    	raxInsert(devices,(unsigned char*)argv[1],strlen(argv[1]),d,NULL);
    	db_insert_device(argv[1],device_max_id);
    }

	cJSON *j_devices = cJSON_CreateObject();
	if(strcasecmp(argv[0],"tsdata") == 0){
		if(parseTsdata(d,argv[2]) == 0)
			cJSON_AddStringToObject(j_devices,argv[1],"Upload successfully!");
		else{
			cJSON_AddStringToObject(j_devices,argv[1],"parse fail!");
		}
	}

    *j_responses = j_devices;
    return 0;
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////
int addReplyCacheBool(BOOLQFIELD *f,cache *pCache,timestamp_t* curTimestamps,size_t curTotalSamples,timestamp_t startQueryTime,timestamp_t endQueryTime){
	int start,end,step;
	start = 0;
	end = curTotalSamples;
	step = 1;
	for(int rowId = start; rowId != end; rowId+=step){
		if(curTimestamps[rowId] < startQueryTime){
			continue;
		}
		if(curTimestamps[rowId] > endQueryTime){
				break;
		}

		if(bitmapTestBit(pCache->haveTimestamp,rowId) == 0)
			continue;
		if(bitmapTestBit(pCache->isValueNone,rowId))
			continue;

		if(bitmapTestBit(pCache->bvalue,rowId)){
			if(f->onStartTime == 0){
				f->onStartTime = curTimestamps[rowId];
			}
		}else{
			if(f->onStartTime > 0){
				f->value += curTimestamps[rowId] - f->onStartTime;
				f->onStartTime = 0;
			}
		}
	}
	return 0;
}

int select_index_callback_bool(void *data, unsigned long long start,unsigned long long end,unsigned long long pos,unsigned long long max_fid,unsigned long long row_num){
	queryContext* qc = data;

	int timestamp_output_size = sizeof(timestamp_t)*CACHE_ROW_NUM;
	int buffer_size = MAX_FIELD_STRING_LEN*CACHE_ROW_NUM;
	if(qc->input == NULL){
		qc->timestamp_output = mosquitto_malloc(timestamp_output_size);
		qc->input = mosquitto_malloc(buffer_size);
		qc->index = mosquitto_malloc(sizeof(FINDEX)*(qc->d->field_max_id+1));
	}

	if(qc->fileTimestamp != getFileTimestamp(end)){
		if(qc->dataFp != NULL){
			//reclaimFilePageCache(fileno(dataFp), 0, 0);
			fclose(qc->dataFp);
			qc->dataFp = NULL;
		}

		char dbPathName[128];
		snprintf(dbPathName,128,"%s/%" PRIu64 ".data",iedb_dir,getFileTimestamp(end));

		qc->dataFp = fopen(dbPathName, "r");
		if(qc->dataFp == NULL){
			return 0;
		}
		qc->fileTimestamp = getFileTimestamp(end);
	}
	fseek(qc->dataFp, pos,SEEK_SET);
	if(fread(qc->index,1,sizeof(FINDEX)*(max_fid+1),qc->dataFp) != sizeof(FINDEX)*(max_fid+1))
		return 0;

	fseek(qc->dataFp, qc->index->dataOffset,SEEK_SET);
	if(fread(qc->input,1,qc->index->dataLength,qc->dataFp) != qc->index->dataLength)
		return 0;
	lzf_decompress(qc->input, qc->index->dataLength,qc->timestamp_output,timestamp_output_size);

	BOOLQFIELD* pFieldHeadTmp = qc->pFieldHead;
	size_t max_cache_size = getFieldCacheSize(pFieldHeadTmp->pField->type);
	cache* curCache = mosquitto_malloc(max_cache_size);
	while(pFieldHeadTmp){
		field* f = pFieldHeadTmp->pField;
		if(f->id <= max_fid /*&& f->ctime < end*/){
			FINDEX* findex = qc->index + f->id;
			fseek(qc->dataFp, findex->dataOffset,SEEK_SET);
			if(fread(qc->input,1,findex->dataLength,qc->dataFp) != (size_t)findex->dataLength)
				break;

			size_t cache_size = getFieldCacheSize(f->type);
			if(cache_size > max_cache_size){
				max_cache_size = cache_size;
				curCache = mosquitto_realloc(curCache,cache_size);
			}
			lzf_decompress(qc->input, findex->dataLength,curCache,cache_size);

			addReplyCacheBool(pFieldHeadTmp,curCache,qc->timestamp_output,row_num,qc->startQueryTime,qc->endQueryTime);
		}
		pFieldHeadTmp = pFieldHeadTmp->next;
	}
	mosquitto_free(curCache);
	return 0;
}

void addReplyFiledBOOL(device *d,BOOLQFIELD* pFieldHead,timestamp_t startQueryTime,timestamp_t endQueryTime){
	queryContext qc = {
	        .pFieldHead = pFieldHead,
	        .d = d,
			.startQueryTime = startQueryTime,
			.endQueryTime = endQueryTime,
			.otype = ORDER_ASC,
			.input = NULL,
			.timestamp_output = NULL,
			.dataFp = NULL,
			.fileTimestamp = 0,
			.index = NULL
	    };
	db_select_tsindex(&qc,select_index_callback_bool);
	if(qc.input != NULL){
		mosquitto_free(qc.input);
		mosquitto_free(qc.timestamp_output);
		mosquitto_free(qc.index);
	}

	if(qc.dataFp != NULL){
		//reclaimFilePageCache(fileno(dataFp), 0, 0);
		fclose(qc.dataFp);
	}


	BOOLQFIELD* pFieldHeadTmp = pFieldHead;
	while(pFieldHeadTmp){
		field* f = pFieldHeadTmp->pField;
		addReplyCacheBool(pFieldHeadTmp,f->pCache,d->timestamps,d->curTimestampPos+1,startQueryTime,endQueryTime);
		pFieldHeadTmp = pFieldHeadTmp->next;
	}
}
void tsBoolQuery(char** argv,cJSON *j_device,timestamp_t startQueryTime,timestamp_t endQueryTime,int device_id,int last_field_id){
    device* d = raxFind(devices,(unsigned char*)argv[device_id],strlen(argv[device_id]));
    if(d == raxNotFound){
    	return;
    }

    BOOLQFIELD* pFieldHead = NULL;
    BOOLQFIELD* pFieldPre = NULL;
    if(device_id >= last_field_id){
        raxIterator ri;
        raxStart(&ri, d->fields);
        raxSeek(&ri, "^", NULL, 0);
        while (raxNext(&ri)) {
        	field *f = ri.data;
        	if(f->type != FIELD_TYPE_BOOL)
        		continue;

        	char* fname = mosquitto_calloc(1,ri.key_len+1);
        	memcpy(fname,ri.key,ri.key_len);

        	BOOLQFIELD* pFieldTemp = mosquitto_calloc(1,sizeof(BOOLQFIELD));
    		pFieldTemp->pField = f;
    		pFieldTemp->value = 0;
    		pFieldTemp->onStartTime = 0;
    		pFieldTemp->name = fname;
    		if(pFieldHead == NULL){
    			pFieldHead = pFieldTemp;
    		}else{
    			pFieldPre->next = pFieldTemp;
    		}
    		pFieldPre = pFieldTemp;
        }
        raxStop(&ri);
    }else{
        for(int j = device_id+1; j<= last_field_id;j++){
        	field* f = raxFind(d->fields,(unsigned char*)argv[j],strlen(argv[j]));
        	if(f != raxNotFound && f->type == FIELD_TYPE_BOOL){
        		BOOLQFIELD* pFieldTemp = mosquitto_calloc(1,sizeof(BOOLQFIELD));
        		pFieldTemp->pField = f;
        		pFieldTemp->value = 0;
        		pFieldTemp->onStartTime = 0;
        		pFieldTemp->name = argv[j];
        		if(pFieldHead == NULL){
        			pFieldHead = pFieldTemp;
        		}else{
        			pFieldPre->next = pFieldTemp;
        		}
        		pFieldPre = pFieldTemp;
        	}
        }
    }

    if(pFieldHead){
    	addReplyFiledBOOL(d,pFieldHead,startQueryTime,endQueryTime);
        timestamp_t now = mstime();
        if(endQueryTime > now)
        	endQueryTime = now;
    	while(pFieldHead){
    		BOOLQFIELD* curField = pFieldHead;
    		if(curField->onStartTime > 0)
    			curField->value += endQueryTime - curField->onStartTime;
    		curField->value /= 1000;
    		cJSON_AddNumberToObject(j_device,curField->name,curField->value);
    		if(device_id >= last_field_id){
    			mosquitto_free(curField->name);
    		}
    		pFieldHead = pFieldHead->next;
    		mosquitto_free(curField);
    	}
    }

}


/* TSBOOLQUERY start end d1 f1 f2 [DEVICE d2] [DEVICE d3 f4 f5 f6]*/
int tsBoolQueryCommand(int argc,char** argv,cJSON** j_responses) {
    timestamp_t startQueryTime, endQueryTime;

    if (argc < 3) {
        /* If IDLE was provided we must have at least 'start end count' */
    	return HTTP_STATUS_BAD_REQUEST;
    }

    /* Parse start and end IDs. */
    if (genericParseTimestamp(argv[0],&startQueryTime,1) != C_OK){
    	return HTTP_STATUS_BAD_REQUEST;
    }
    if (genericParseTimestamp(argv[1],&endQueryTime,1) != C_OK){
    	return HTTP_STATUS_BAD_REQUEST;
    }
    startQueryTime *= 1000;
    endQueryTime *= 1000;

	cJSON *j_devices, *j_device;
	j_devices = cJSON_CreateObject();

    int device_id = 2;
    int j = 3;
    while(j < argc){
    	if (strcasecmp(argv[j],"DEVICE") == 0){
    		j_device = cJSON_CreateObject();
    		if(j_device == NULL){
    			cJSON_Delete(j_devices);
    			return HTTP_STATUS_INTERNAL_SERVER_ERROR;
    		}
    		cJSON_AddItemToObject(j_devices,argv[device_id], j_device);

    		tsBoolQuery(argv,j_device,startQueryTime, endQueryTime,device_id,j-1);
    		device_id = ++j;
    	}

    	j++;
    }
    if(device_id < argc){
		j_device = cJSON_CreateObject();
		if(j_device == NULL){
			cJSON_Delete(j_devices);
			return HTTP_STATUS_INTERNAL_SERVER_ERROR;
		}
		cJSON_AddItemToObject(j_devices,argv[device_id], j_device);
    	tsBoolQuery(argv,j_device,startQueryTime, endQueryTime,device_id,j-1);
    }

    *j_responses = j_devices;
    return 0;
}
