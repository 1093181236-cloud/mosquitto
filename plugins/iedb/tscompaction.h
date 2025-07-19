/*
 * Copyright 2018-2019 Redis Labs Ltd. and Contributors
 *
 * This file is available under the Redis Labs Source Available License Agreement
 */
#ifndef COMPACTION_H
#define COMPACTION_H

#include <stdint.h>
#include <sys/types.h>
#include <stdbool.h>

#define timestamp_t uint64_t
//#define api_timestamp_t u_int64_t


typedef struct Sample
{
    timestamp_t timestamp;
    double value;
} Sample;

typedef enum {
    TS_AGG_INVALID = -1,
    TS_AGG_NONE = 0,
    TS_AGG_MIN,
    TS_AGG_MAX,
    TS_AGG_SUM,
    TS_AGG_AVG,
    TS_AGG_COUNT,
    TS_AGG_FIRST,
    TS_AGG_LAST,
    TS_AGG_RANGE,
    TS_AGG_STD_P,
    TS_AGG_STD_S,
    TS_AGG_VAR_P,
    TS_AGG_VAR_S,
    TS_AGG_TWA,
    TS_AGG_TYPES_MAX // 13
} TS_AGG_TYPES_T;


typedef struct AggregationClass
{
    void *(*createContext)(bool reverse);
    void (*freeContext)(void *context);
    void (*appendValue)(void *context, double value, timestamp_t ts);
    void (*appendValueVec)(void * context,
                           double * values,
                           size_t si,
                           size_t ei);
    void (*resetContext)(void *context);
    void (*writeContext)(void *context, void *io);
    int (*readContext)(void *context, void *io, int encver);
    void (*addContext)(void *rulePtr,int subContextpos);
    void (*removeContext)(void *rulePtr,int subContextpos);
    void (*addBucketParams)(void *contextPtr, timestamp_t bucketStartTS, timestamp_t bucketEndTS);
    void (*addPrevBucketLastSample)(void *contextPtr,
                                    double value,
                                    timestamp_t ts); // Should be called before appending any sample
    void (*addNextBucketFirstSample)(
        void *contextPtr,
        double value,
        timestamp_t ts); // Should be called after appended all the rest of the samples.
    void (*getLastSample)(void *contextPtr,
                          Sample *sample); // Returns the last sample appended to the context
    void (*finalize)(void *context, double *value);
    void (*finalizeEmpty)(double *value); // assigns empty value to value
} AggregationClass;

AggregationClass *GetAggClass(TS_AGG_TYPES_T aggType);
int StringAggTypeToEnum(const char *agg_type);
int RMStringLenAggTypeToEnum(const char* aggTypeStr);
int StringLenAggTypeToEnum(const char *agg_type, size_t len);
const char *AggTypeEnumToString(TS_AGG_TYPES_T aggType);
const char *AggTypeEnumToStringLowerCase(TS_AGG_TYPES_T aggType);
void initGlobalCompactionFunctions(void);

#endif
