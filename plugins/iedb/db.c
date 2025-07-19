/*
 * db.c
 *
 *  Created on: 2025Äê1ÔÂ17ÈÕ
 *      Author: a
 */
#include <stddef.h>
#include <sqlite3.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>

#ifdef _WIN32
#include <io.h>  // Windows equivalent of unistd.h functions
#else
#include <unistd.h>
#endif

#include "mosquitto.h"
#include "mosquitto_broker.h"
#include "db.h"
#include "device.h"
#include "rax.h"

sqlite3 *sqlite3_db;
static sqlite3_stmt *pSelectTsIndexAscStmt;
static sqlite3_stmt *pSelectTsIndexDescStmt;
static sqlite3_stmt *pInsertTsIndexStmt;
static sqlite3_stmt *pDeleteTsIndexStmt;
static sqlite3_stmt *pInsertDeviceStmt;
static sqlite3_stmt *pInsertFieldStmt;
static sqlite3_stmt *pInsertRuleStmt;
static sqlite3_stmt *pDeleteRuleStmt;

extern char *iedb_dir;
rax* devices = NULL;
int select_device_callback(void *data, int argc, char **argv, char **azColName);
int select_field_callback(void *data, int argc, char **argv, char **azColName);
int select_rule_callback(void *data, int argc, char **argv, char **azColName);
int db_client_init(void);
int db_group_init(void);
int db_role_init(void);
int db_mqtt_client_init(void);
int db_tag_init(void);
void save_cache(void){
	char dbPathName[128];
	snprintf(dbPathName,128,"%s/cache.data",iedb_dir);
    FILE *fp = fopen(dbPathName,"w");
    if (!fp) {
        char *str_err = strerror(errno);
        mosquitto_log_printf(MOSQ_LOG_WARNING,"Failed opening cache file %s for saving: %s",dbPathName,str_err);
        return;
    }

	raxIterator ri;
    raxStart(&ri, devices);
    raxSeek(&ri, "^", NULL, 0);
    while (raxNext(&ri)) {
    	device *d = ri.data;
    	unsigned char head = 1;
    	fwrite(&head, 1,1,fp);
    	fwrite(&ri.key_len, sizeof(ri.key_len),1,fp);
    	fwrite(ri.key, ri.key_len,1,fp);
    	fwrite(&d->lastTimestamp, sizeof(d->lastTimestamp),1,fp);
    	fwrite(&d->curTimestampPos, sizeof(d->curTimestampPos),1,fp);
    	fwrite(d->timestamps, sizeof(d->timestamps),1,fp);

    	raxIterator ri2;
        raxStart(&ri2, d->fields);
        raxSeek(&ri2, "^", NULL, 0);
        while (raxNext(&ri2)) {
        	field *f = ri2.data;
        	head = 0;
        	fwrite(&head, 1,1,fp);
        	fwrite(&ri2.key_len, sizeof(ri2.key_len),1,fp);
        	fwrite(ri2.key, ri2.key_len,1,fp);
        	fwrite(&f->lastTimestamp, sizeof(f->lastTimestamp),1,fp);
        	fwrite(&f->lastTimestampPos, sizeof(f->lastTimestampPos),1,fp);
        	size_t cache_size = getFieldCacheSize(f->type);
        	fwrite(f->pCache, cache_size,1,fp);
        }
        raxStop(&ri2);
    }
    raxStop(&ri);

    fclose(fp);
}

void load_cache(void){
	char dbPathName[128];
	snprintf(dbPathName,128,"%s/cache.data",iedb_dir);
    FILE *fp = fopen(dbPathName,"r");
    if (!fp) {
        char *str_err = strerror(errno);
        mosquitto_log_printf(MOSQ_LOG_WARNING,"Failed opening cache file %s for reading: %s",dbPathName,str_err);
        return;
    }

    device *d = NULL;
	size_t key_len;
	unsigned char head;
    while(1){
    	fread(&head,1,1,fp);
		if(fread(&key_len, sizeof(key_len),1,fp) != 1)
			break;
		unsigned char *key = mosquitto_malloc(key_len);
		if(fread(key, key_len,1,fp) != 1){
			mosquitto_free(key);
		    break;
		}

    	if(head == 1){
    		d = raxFind(devices,key,key_len);
    		if(d == raxNotFound){
    			fseek(fp, sizeof(timestamp_t)+sizeof(int)+sizeof(timestamp_t)*CACHE_ROW_NUM, SEEK_CUR);
    		}else{
    			if(fread(&d->lastTimestamp, sizeof(d->lastTimestamp),1,fp) != 1){
    				mosquitto_free(key);
    			    break;
    			}
    			if(fread(&d->curTimestampPos, sizeof(d->curTimestampPos),1,fp) != 1){
    				mosquitto_free(key);
    			    break;
    			}
    			if(fread(d->timestamps, sizeof(d->timestamps),1,fp) != 1){
    				mosquitto_free(key);
    			    break;
    			}
    		}
    	}else{
    		if(d == raxNotFound){
    			mosquitto_free(key);
    			break;
    		}else{
    			field* f = raxFind(d->fields,key,key_len);
    			if(f == raxNotFound){
    				mosquitto_free(key);
    			    break;
    			}else{
    	        	if(fread(&f->lastTimestamp, sizeof(f->lastTimestamp),1,fp) != 1){
        				mosquitto_free(key);
        			    break;
        			}
    	        	if(fread(&f->lastTimestampPos, sizeof(f->lastTimestampPos),1,fp) != 1){
        				mosquitto_free(key);
        			    break;
        			}
    				size_t cache_size = getFieldCacheSize(f->type);
    				if(fread(f->pCache, cache_size,1,fp) != 1){
    				   mosquitto_free(key);
    				   break;
    				}
    			}
    		}
    	}
    	mosquitto_free(key);
    }
    fclose(fp);
    //unlink(dbPathName);
}

int db_create_device(void){
	char *zErrMsg = 0;
	int  rc;
	char* sql = "CREATE TABLE IF NOT EXISTS  device("  \
	         "id INTEGER PRIMARY KEY     NOT NULL," \
	         "name           TEXT    NOT NULL);";

	rc = sqlite3_exec(sqlite3_db, sql, NULL, 0, &zErrMsg);
	if( rc != SQLITE_OK ){
		mosquitto_log_printf(MOSQ_LOG_ERR, "SQL error: %s\n", zErrMsg);
		sqlite3_free(zErrMsg);
	}
	return rc;
}

int db_create_field(void){
	char *zErrMsg = 0;
	int  rc;
	char* sql = "CREATE TABLE IF NOT EXISTS field("  \
	         "id 			INTEGER      NOT NULL," \
			 "did 			INTEGER      NOT NULL," \
	         "name           TEXT    NOT NULL," \
			 "type           TINYINT    NOT NULL," \
			 "ctime           INTEGER    NOT NULL," \
			 "PRIMARY KEY (did,id)," \
	         "FOREIGN KEY(did) REFERENCES device(id))WITHOUT ROWID;";

	rc = sqlite3_exec(sqlite3_db, sql, NULL, 0, &zErrMsg);
	if( rc != SQLITE_OK ){
		mosquitto_log_printf(MOSQ_LOG_ERR, "SQL error: %s\n", zErrMsg);
		sqlite3_free(zErrMsg);
	}
	return rc;
}

int db_create_tsindex(void){
	char *zErrMsg = 0;
	int  rc;
	char* sql = "CREATE TABLE IF NOT EXISTS tsindex("  \
			 "did 			INTEGER      NOT NULL," \
	         "start         INTEGER    NOT NULL," \
			 "end           INTEGER    NOT NULL," \
			 "pos           INTEGER    NOT NULL," \
			 "max_fid       INTEGER    NOT NULL," \
			 "row_num       INTEGER    NOT NULL," \
			 "PRIMARY KEY (did,start)," \
	         "FOREIGN KEY(did) REFERENCES device(id))WITHOUT ROWID;";

	rc = sqlite3_exec(sqlite3_db, sql, NULL, 0, &zErrMsg);
	if( rc != SQLITE_OK ){
		mosquitto_log_printf(MOSQ_LOG_ERR, "SQL error: %s\n", zErrMsg);
		sqlite3_free(zErrMsg);
	}
	return rc;
}

int db_create_rule(void){
	char *zErrMsg = 0;
	int  rc;
	char* sql = "CREATE TABLE IF NOT EXISTS rule("  \
	         "id 			INTEGER   PRIMARY KEY   NOT NULL," \
			 "dname 		TEXT      NOT NULL," \
			 "func 			TEXT      NOT NULL);";

	rc = sqlite3_exec(sqlite3_db, sql, NULL, 0, &zErrMsg);
	if( rc != SQLITE_OK ){
		mosquitto_log_printf(MOSQ_LOG_ERR, "SQL error: %s\n", zErrMsg);
		sqlite3_free(zErrMsg);
	}
	return rc;
}

int db_open(void){
	char *zErrMsg = 0;
	char dbPathName[128];
	snprintf(dbPathName,127,"%s/iedb.db",iedb_dir);
	int rc = sqlite3_open(dbPathName, &sqlite3_db);
	if( rc ){
		mosquitto_log_printf(MOSQ_LOG_ERR, "Can't open database: %s\n", sqlite3_errmsg(sqlite3_db));
	    return -1;
	}

	rc = db_create_device();
	if( rc != SQLITE_OK )
		return -1;

	rc = db_create_field();
	if( rc != SQLITE_OK )
		return -1;

	rc = db_create_tsindex();
	if( rc != SQLITE_OK )
		return -1;

	rc = db_create_rule();
	if( rc != SQLITE_OK )
		return -1;

	devices = raxNew();
	rc = sqlite3_exec(sqlite3_db, "SELECT name,id FROM device", select_device_callback, devices, &zErrMsg);
	if( rc != SQLITE_OK ){
		mosquitto_log_printf(MOSQ_LOG_ERR, "SQL error: %s\n", zErrMsg);
		sqlite3_free(zErrMsg);
		return -1;
	}

	raxIterator ri;
    raxStart(&ri, devices);
    raxSeek(&ri, "^", NULL, 0);
    while (raxNext(&ri)) {
    	device *d = ri.data;
    	char sql[256];
    	snprintf(sql,255,"SELECT name,id,type,ctime FROM field WHERE did=%lld",d->id);
    	rc = sqlite3_exec(sqlite3_db, sql, select_field_callback, d, &zErrMsg);
    	if( rc != SQLITE_OK ){
    		mosquitto_log_printf(MOSQ_LOG_ERR, "SQL error: %s\n", zErrMsg);
    		sqlite3_free(zErrMsg);
    		return -1;
    	}
    }
    raxStop(&ri);

    load_cache();

	rc = sqlite3_exec(sqlite3_db, "SELECT dname,func FROM rule", select_rule_callback, devices, &zErrMsg);
	if( rc != SQLITE_OK ){
		mosquitto_log_printf(MOSQ_LOG_ERR, "SQL error: %s\n", zErrMsg);
		sqlite3_free(zErrMsg);
		return -1;
	}

	rc = sqlite3_prepare_v2(sqlite3_db,"SELECT start,end,pos,max_fid,row_num FROM tsindex WHERE did=?1 AND (start BETWEEN ?2 AND ?3 OR end BETWEEN ?2 AND ?3 OR ?2 BETWEEN start AND end OR ?3 BETWEEN start AND end) ORDER BY start ASC",-1, &pSelectTsIndexAscStmt, 0);
	if( rc != SQLITE_OK || pSelectTsIndexAscStmt == NULL){
		mosquitto_log_printf(MOSQ_LOG_ERR, "pSelectTsIndexStmt error: %s\n", sqlite3_errmsg(sqlite3_db));
    	return -1;
	}

	rc = sqlite3_prepare_v2(sqlite3_db,"SELECT start,end,pos,max_fid,row_num FROM tsindex WHERE did=?1 AND (start BETWEEN ?2 AND ?3 OR end BETWEEN ?2 AND ?3 OR ?2 BETWEEN start AND end OR ?3 BETWEEN start AND end) ORDER BY start DESC",-1, &pSelectTsIndexDescStmt, 0);
	if( rc != SQLITE_OK || pSelectTsIndexDescStmt == NULL){
		mosquitto_log_printf(MOSQ_LOG_ERR, "pSelectTsIndexStmt error: %s\n", sqlite3_errmsg(sqlite3_db));
    	return -1;
	}

	rc = sqlite3_prepare_v2(sqlite3_db,"INSERT INTO tsindex (did,start,end,pos,max_fid,row_num) VALUES (?1,?2,?3, ?4, ?5, ?6)",-1, &pInsertTsIndexStmt, 0);
	if( rc != SQLITE_OK  || pInsertTsIndexStmt == NULL){
		mosquitto_log_printf(MOSQ_LOG_ERR, "pInsertTsIndexStmt error: %s\n", sqlite3_errmsg(sqlite3_db));
    	return -1;
	}

	rc = sqlite3_prepare_v2(sqlite3_db,"DELETE FROM tsindex WHERE end<?1",-1, &pDeleteTsIndexStmt, 0);
	if( rc != SQLITE_OK  || pDeleteTsIndexStmt == NULL){
		mosquitto_log_printf(MOSQ_LOG_ERR, "pDeleteTsIndexStmt error: %s\n", sqlite3_errmsg(sqlite3_db));
    	return -1;
	}

	rc = sqlite3_prepare_v2(sqlite3_db,"INSERT INTO device (id,name) VALUES (?1, ?2)",-1, &pInsertDeviceStmt, 0);
	if( rc != SQLITE_OK  || pInsertDeviceStmt == NULL){
		mosquitto_log_printf(MOSQ_LOG_ERR, "pInsertDeviceStmt error: %s\n", sqlite3_errmsg(sqlite3_db));
    	return -1;
	}

	rc = sqlite3_prepare_v2(sqlite3_db,"INSERT INTO field (id,name,did,ctime,type) VALUES (?1,?2,?3,?4,?5)",-1, &pInsertFieldStmt, 0);
	if( rc != SQLITE_OK  || pInsertFieldStmt == NULL){
		mosquitto_log_printf(MOSQ_LOG_ERR, "pInsertFieldStmt error: %s\n", sqlite3_errmsg(sqlite3_db));
    	return -1;
	}

	rc = sqlite3_prepare_v2(sqlite3_db,"DELETE FROM rule WHERE dname=?1 AND func=?2",-1, &pDeleteRuleStmt, 0);
	if( rc != SQLITE_OK  || pDeleteRuleStmt == NULL){
		mosquitto_log_printf(MOSQ_LOG_ERR, "pDeleteRuleStmt error: %s\n", sqlite3_errmsg(sqlite3_db));
    	return -1;
	}

	rc = sqlite3_prepare_v2(sqlite3_db,"INSERT INTO rule (dname,func) VALUES (?1,?2)",-1, &pInsertRuleStmt, 0);
	if( rc != SQLITE_OK  || pInsertRuleStmt == NULL){
		mosquitto_log_printf(MOSQ_LOG_ERR, "pInsertRuleStmt error: %s\n", sqlite3_errmsg(sqlite3_db));
    	return -1;
	}

	rc = db_role_init();
	if(rc != SQLITE_OK)
		return -1;
	rc = db_group_init();
	if(rc != SQLITE_OK)
		return -1;
	rc = db_client_init();
	if(rc != SQLITE_OK)
		return -1;

	rc = db_mqtt_client_init();
	if(rc != SQLITE_OK)
		return -1;

	rc = db_tag_init();
	if(rc != SQLITE_OK)
		return -1;

	return 0;
}

int db_close(void){
	save_cache();
	raxFreeWithCallback(devices,freeDevice);
	sqlite3_finalize(pSelectTsIndexAscStmt);
	sqlite3_finalize(pSelectTsIndexDescStmt);
	sqlite3_finalize(pInsertTsIndexStmt);
	sqlite3_finalize(pDeleteTsIndexStmt);
	sqlite3_finalize(pInsertDeviceStmt);
	sqlite3_finalize(pInsertFieldStmt);
	sqlite3_finalize(pInsertRuleStmt);
	sqlite3_finalize(pDeleteRuleStmt);
	sqlite3_close(sqlite3_db);
	return 0;
}

int db_insert_device(char* device_name, long long id){
	sqlite3_bind_int64(pInsertDeviceStmt, 1, id);
	sqlite3_bind_text(pInsertDeviceStmt, 2, device_name, -1, SQLITE_STATIC);

	if(sqlite3_step(pInsertDeviceStmt) != SQLITE_DONE){
		mosquitto_log_printf(MOSQ_LOG_ERR, "SQL error: %s\n", sqlite3_errmsg(sqlite3_db));
	}
	sqlite3_reset(pInsertDeviceStmt);
	return 0;
}

int db_insert_field(char* field_name,long long id,long long did,timestamp_t ctime,int type){
	sqlite3_bind_int64(pInsertFieldStmt, 1, id);
	sqlite3_bind_text(pInsertFieldStmt, 2, field_name, -1, SQLITE_STATIC);
	sqlite3_bind_int64(pInsertFieldStmt, 3, did);
	sqlite3_bind_int64(pInsertFieldStmt, 4, ctime);
	sqlite3_bind_int64(pInsertFieldStmt, 5, type);

	if(sqlite3_step(pInsertFieldStmt) != SQLITE_DONE){
		mosquitto_log_printf(MOSQ_LOG_ERR, "SQL error: %s\n", sqlite3_errmsg(sqlite3_db));
	}
	sqlite3_reset(pInsertFieldStmt);

	return 0;
}
int db_insert_rule(char* dname,char* func){
	sqlite3_bind_text(pInsertDeviceStmt, 1, dname, -1, SQLITE_STATIC);
	sqlite3_bind_text(pInsertDeviceStmt, 2, func, -1, SQLITE_STATIC);

	if(sqlite3_step(pInsertRuleStmt) != SQLITE_DONE){
		mosquitto_log_printf(MOSQ_LOG_ERR, "SQL error: %s\n", sqlite3_errmsg(sqlite3_db));
	}
	sqlite3_reset(pInsertRuleStmt);
	return 0;
}


int db_delete_rule(char* dname,char* func){
	sqlite3_bind_text(pInsertDeviceStmt, 1, dname, -1, SQLITE_STATIC);
	sqlite3_bind_text(pInsertDeviceStmt, 2, func, -1, SQLITE_STATIC);

	if(sqlite3_step(pDeleteRuleStmt) != SQLITE_DONE){
		mosquitto_log_printf(MOSQ_LOG_ERR, "SQL error: %s\n", sqlite3_errmsg(sqlite3_db));
	}
	sqlite3_reset(pDeleteRuleStmt);
	return 0;
}

int db_delete_device(long long id){
	char *zErrMsg = 0;
	int  rc;
	char sql[128];
	snprintf(sql,255,"DELETE FROM device WHERE id=%lld",id);

	rc = sqlite3_exec(sqlite3_db, sql, NULL, NULL, &zErrMsg);
	if( rc != SQLITE_OK ){
		mosquitto_log_printf(MOSQ_LOG_ERR, "db_delete_device SQL error: %s\n", zErrMsg);
		sqlite3_free(zErrMsg);
	}
	return rc;
}

int db_insert_tsindex(long long did,timestamp_t start,timestamp_t end,size_t pos,long long field_max_id,long long row_num){
	sqlite3_bind_int64(pInsertTsIndexStmt, 1, did);
	sqlite3_bind_int64(pInsertTsIndexStmt, 2, start);
	sqlite3_bind_int64(pInsertTsIndexStmt, 3, end);
	sqlite3_bind_int64(pInsertTsIndexStmt, 4, pos);
	sqlite3_bind_int64(pInsertTsIndexStmt, 5, field_max_id);
	sqlite3_bind_int64(pInsertTsIndexStmt, 6, row_num);

	if(sqlite3_step(pInsertTsIndexStmt) != SQLITE_DONE){
		mosquitto_log_printf(MOSQ_LOG_ERR, "SQL error: %s\n", sqlite3_errmsg(sqlite3_db));
	}
	sqlite3_reset(pInsertTsIndexStmt);
	return 0;
}

int db_delete_tsindex(timestamp_t end){
	sqlite3_bind_int64(pDeleteTsIndexStmt, 1, end);

	if(sqlite3_step(pDeleteTsIndexStmt) != SQLITE_DONE){
		mosquitto_log_printf(MOSQ_LOG_ERR, "SQL error: %s\n", sqlite3_errmsg(sqlite3_db));
	}
	sqlite3_reset(pDeleteTsIndexStmt);
	return 0;
}

int db_delete_tsindex_by_device(long long did){
	char *zErrMsg = 0;
	int  rc;
	char sql[128];
	snprintf(sql,255,"DELETE FROM tsindex WHERE did=%lld",did);

	rc = sqlite3_exec(sqlite3_db, sql, NULL, NULL, &zErrMsg);
	if( rc != SQLITE_OK ){
		mosquitto_log_printf(MOSQ_LOG_ERR, "db_delete_tsindex_by_device SQL error: %s\n", zErrMsg);
		sqlite3_free(zErrMsg);
	}
	return rc;
}

int handle_block_asc(queryContext* qc,selectIndexCallback* cb,sqlite3_stmt *pStmt,long long start,long long end,long long pos,long long max_fid,long long row_num,long long src_start,long long src_end){
	long long dest_start,dest_end;
	long long src_start2,src_end2;
	if(sqlite3_step(pStmt)==SQLITE_ROW){
		long long next_start,next_end,next_pos,next_max_fid,next_row_num;
		next_start = sqlite3_column_int64(pStmt,0);
		next_end = sqlite3_column_int64(pStmt,1);
		next_pos = sqlite3_column_int64(pStmt,2);
		next_max_fid = sqlite3_column_int64(pStmt,3);
		next_row_num = sqlite3_column_int64(pStmt,4);

		if(next_start >= src_end){//no overlap
			dest_start = src_start;
			dest_end = src_end;
			int rc = cb(qc,start,end,pos,max_fid,row_num,dest_start,dest_end);
			if(rc == 0){
				src_start2 = next_start;
				src_end2 = next_end;
				handle_block_asc(qc,cb,pStmt,next_start,next_end,next_pos,next_max_fid,next_row_num,src_start2,src_end2);
			}
		}else if(next_end < src_end){//overlap
			dest_start = src_start;
			dest_end = src_end;
			int rc = cb(qc,start,end,pos,max_fid,row_num,dest_start,dest_end);
			if(rc == 0){
				src_start2 = next_start;
				src_end2 = next_end;
				handle_block_asc(qc,cb,pStmt,next_start,next_end,next_pos,next_max_fid,next_row_num,src_start2,src_end2);
			}
		}else{//contain
			dest_start = src_start;
			dest_end = next_start;
			int rc = cb(qc,start,end,pos,max_fid,row_num,dest_start,dest_end);
			if(rc == 0){
				dest_start = next_start;
				dest_end = next_end;
				rc = cb(qc,next_start,next_end,next_pos,next_max_fid,next_row_num,dest_start,dest_end);
				if(rc == 0){
					src_start2 = next_end;
					src_end2 = src_end;
					handle_block_asc(qc,cb,pStmt,start,end,pos,max_fid,row_num,src_start2,src_end2);
				}
			}
		}
	}else{
		dest_start = src_start;
		dest_end = src_end;
		cb(qc,start,end,pos,max_fid,row_num,dest_start,dest_end);
	}
	return 0;
}

int handle_block_desc(queryContext* qc,selectIndexCallback* cb,sqlite3_stmt *pStmt,long long start,long long end,long long pos,long long max_fid,long long row_num,long long src_start,long long src_end){
	long long dest_start,dest_end;
	long long src_start2,src_end2;
	if(sqlite3_step(pStmt)==SQLITE_ROW){
		long long next_start,next_end,next_pos,next_max_fid,next_row_num;
		next_start = sqlite3_column_int64(pStmt,0);
		next_end = sqlite3_column_int64(pStmt,1);
		next_pos = sqlite3_column_int64(pStmt,2);
		next_max_fid = sqlite3_column_int64(pStmt,3);
		next_row_num = sqlite3_column_int64(pStmt,4);

		if(next_end <= src_start){//no overlap
			dest_start = src_start;
			dest_end = src_end;
			int rc = cb(qc,start,end,pos,max_fid,row_num,dest_start,dest_end);
			if(rc == 0){
				src_start2 = next_start;
				src_end2 = next_end;
				handle_block_desc(qc,cb,pStmt,next_start,next_end,next_pos,next_max_fid,next_row_num,src_start2,src_end2);
			}
		}else if(next_start < src_start){//overlap
			dest_start = src_start;
			dest_end = src_end;
			int rc = cb(qc,start,end,pos,max_fid,row_num,dest_start,dest_end);
			if(rc == 0){
				src_start2 = next_start;
				src_end2 = next_end;
				handle_block_desc(qc,cb,pStmt,next_start,next_end,next_pos,next_max_fid,next_row_num,src_start2,src_end2);
			}
		}else{//contain
			dest_start = next_end;
			dest_end = src_end;
			int rc = cb(qc,start,end,pos,max_fid,row_num,dest_start,dest_end);
			if(rc == 0){
				dest_start = next_start;
				dest_end = next_end;
				rc = cb(qc,next_start,next_end,next_pos,next_max_fid,next_row_num,dest_start,dest_end);
				if(rc == 0){
					src_start2 = src_start;
					src_end2 = next_start;
					handle_block_desc(qc,cb,pStmt,start,end,pos,max_fid,row_num,src_start2,src_end2);
				}
			}
		}
	}else{
		dest_start = src_start;
		dest_end = src_end;
		cb(qc,start,end,pos,max_fid,row_num,dest_start,dest_end);
	}
	return 0;
}

int db_select_tsindex(queryContext* qc,selectIndexCallback* cb){
	static sqlite3_stmt *pStmt;
	if(qc->otype == ORDER_ASC)
		pStmt = pSelectTsIndexAscStmt;
	else
		pStmt = pSelectTsIndexDescStmt;

	sqlite3_bind_int64(pStmt, 1, qc->d->id);
	sqlite3_bind_int64(pStmt, 2, qc->startQueryTime);
	sqlite3_bind_int64(pStmt, 3, qc->endQueryTime);
	if( sqlite3_step(pStmt)==SQLITE_ROW ){
		long long start,end,pos,max_fid,row_num;
		start = sqlite3_column_int64(pStmt,0);
		end = sqlite3_column_int64(pStmt,1);
		pos = sqlite3_column_int64(pStmt,2);
		max_fid = sqlite3_column_int64(pStmt,3);
		row_num = sqlite3_column_int64(pStmt,4);

		if(qc->otype == ORDER_ASC)
			handle_block_asc(qc,cb,pStmt,start,end,pos,max_fid,row_num,start,end);
		else
			handle_block_desc(qc,cb,pStmt,start,end,pos,max_fid,row_num,start,end);
	}
	sqlite3_reset(pStmt);

	return 0;
}
