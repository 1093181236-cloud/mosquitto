/*
 * rule.h
 *
 *  Created on: 2023Äê9ÔÂ4ÈÕ
 *      Author: a
 */

#ifndef RULE_H_
#define RULE_H_

#include "tscompaction.h"

typedef struct CompactionRule{
	char* func;
    timestamp_t bucketDuration;
    timestamp_t timestampAlignment;
    AggregationClass *aggClass;
    TS_AGG_TYPES_T aggType;
    void *aggContext;
    struct CompactionRule *nextRule;
    timestamp_t startTime;
    timestamp_t startCurrentTimeBucket; // Beware that the first bucket is alway starting in 0 no
                                        // matter the alignment
    void **subContext;
    int subContextLength;
    int curSubContextPos;
} CompactionRule;


CompactionRule *NewRule(char*func,int aggType,
		timestamp_t bucketDuration,
		timestamp_t timestampAlignment);

void FreeCompactionRule(void *value);

int handleCompaction(void *dp,CompactionRule *rule,timestamp_t timestamp,double value,double* aggVal);
int handleQueryCompaction(CompactionRule *rule,timestamp_t timestamp,double value,double* aggVal,int otype);



#endif
