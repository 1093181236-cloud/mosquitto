#ifndef DEVICE_H
#define DEVICE_H

#include <stdio.h>
#include <stddef.h>

#include "tsrule.h"
#include "tscompaction.h"

#include <cjson/cJSON.h>
#include "rax.h"

#define C_OK                    0
#define C_ERR                   -1

//#define timestamp_t uint64_t
#define modulo(x, N) ((x % N + N) % N)

#define BITS_PER_BYTE 			8

#define FIELD_FLAG_NONE 		0
#define FIELD_FLAG_ACC 			(1<<0)
#define FIELD_FLAG_LAST_NONE 	(1<<1)

#define CACHE_ROW_NUM			512
#define CACHE_BITMAP_SIZE		CACHE_ROW_NUM/BITS_PER_BYTE

#define TS_FILE_PERIOD		3600000
#define MAX_NAME_SIZE			32

#define MAX_FIELD_STRING_LEN 	64

typedef enum {
	FLAG_OP_INVALID = -1,
	FLAG_OP_RESET = 0,
	FLAG_OP_SET = 1
}FLAG_OP_TYPE;
typedef enum {
	FIELD_TYPE_BOOL,
	FIELD_TYPE_TING_INT,
	FIELD_TYPE_SMALL_INT,
	FIELD_TYPE_INT,
	FIELD_TYPE_BIG_INT,
	FIELD_TYPE_FLOAT,
	FIELD_TYPE_DOUBLE,
	FIELD_TYPE_STRING,
	FIELD_TYPE_MAX_TYPES
} FIELD_TYPE;

typedef struct _cache{
	unsigned char haveTimestamp[CACHE_BITMAP_SIZE];
	unsigned char isValueNone[CACHE_BITMAP_SIZE];
	union{
		unsigned char bvalue[CACHE_BITMAP_SIZE];
		char tivalue[CACHE_ROW_NUM];
		short sivalue[CACHE_ROW_NUM];
		int ivalue[CACHE_ROW_NUM];
		int64_t bivalue[CACHE_ROW_NUM];
		float fvalue[CACHE_ROW_NUM];
		double dvalue[CACHE_ROW_NUM];
		char strvalue[CACHE_ROW_NUM*MAX_FIELD_STRING_LEN];
	};
}cache;

typedef struct _field{
	long long id;
	FIELD_TYPE type;
	unsigned char flags;

	CompactionRule *rules;
	size_t lastTimestampPos;
	timestamp_t lastTimestamp;
	timestamp_t ctime;

	cache* pCache;
}field;

typedef struct device {
	long long id;
    rax *fields;
    long long field_max_id;
    char* mqtt_username;

    timestamp_t lastTimestamp;
    int curTimestampPos;
    timestamp_t timestamps[CACHE_ROW_NUM];
} device;

typedef struct _FINDEX{
	size_t dataOffset;
	size_t dataLength;
}FINDEX;

typedef struct _tsdbSaveContext{
	timestamp_t curFileTimestamp;
	FILE* curDataFp;
}TsdbSaveContext;

typedef enum {
	ORDER_ASC = 1,
	ORDER_DESC = -1
}ORDER_TYPE;

typedef enum
{
    QUERY_NORMAL = 0,
	QUERY_NORMAL_FILTER_FIRST_NONE = 1,
	QUERY_AGG = 2
} QUERY_TYPE;

typedef struct _QFIELD{
	void* name;
	field* pField;
	struct _QFIELD* next;
	CompactionRule *rule;
	cache* curCache;
	int isFirstRow;
	int haveCache;
}QFIELD,*PQFIELD;

typedef struct _TSINDEX{
	long long start;
	long long end;
	long long pos;
	long long max_fid;
	long long row_num;
	struct _TSINDEX *prev, *next;
}TSINDEX,*PTSINDEX;

typedef struct _TSINDEX2{
	TSINDEX* ti;
	long long start;
	long long end;

	struct _TSINDEX2 *prev, *next;
}TSINDEX2,*PTSINDEX2;

typedef struct _queryContext{
	cJSON *j_device;
	device *d;
	timestamp_t startQueryTime;
	timestamp_t endQueryTime;
	void* pFieldHead;
	size_t count;
	size_t samplelen;
	ORDER_TYPE otype;
	QUERY_TYPE qtype;
	timestamp_t* timestamp_output;
	char*input;
	FILE* dataFp;
	timestamp_t fileTimestamp;
	FINDEX* index;
}queryContext;

typedef struct _FLQFIELD{
	cJSON *j_field;
	field* pField;
	struct _FLQFIELD* next;
}FLQFIELD,*PFLQFIELD;

typedef struct _BOOLQFIELD{
	char* name;
	timestamp_t onStartTime;
	timestamp_t value;
	field* pField;
	struct _BOOLQFIELD* next;
}BOOLQFIELD,*PBOOLQFIELD;

typedef struct _DIFFQFIELD{
	int first_found;
	int last_found;
	char* name;
	double first_val;
	double last_val;
	field* pField;
	struct _DIFFQFIELD* next;
}DIFFQFIELD,*PDIFFQFIELD;

device *deviceNew(long long id);
void freeDevice(void *p);
void fieldFree(void* p);
size_t getFieldCacheSize(FIELD_TYPE type);
//field* fieldNew(long long id,FIELD_TYPE type,timestamp_t ctime);

void DeleteOldFile(void);

#endif
