/*
 * rule.c
 *
 *  Created on: 2023Äê9ÔÂ4ÈÕ
 *      Author: a
 */

#include <inttypes.h>
#include <string.h>
#include "mosquitto_broker.h"
#include "tsrule.h"
#include "device.h"
#include "util.h"

void FreeCompactionRule(void *value) {
    CompactionRule *rule = (CompactionRule *)value;
    ((AggregationClass *)rule->aggClass)->freeContext(rule->aggContext);
    for(int i = 0; i < rule->subContextLength; i++)
    	((AggregationClass *)rule->aggClass)->freeContext(rule->subContext[i]);
    mosquitto_free(rule->subContext);
    mosquitto_free(rule->func);
    mosquitto_free(rule);
}

CompactionRule *NewRule(char*func,int aggType,
		timestamp_t bucketDuration,
		timestamp_t timestampAlignment) {
    if (bucketDuration == 0ULL) {
        return NULL;
    }

    CompactionRule *rule = (CompactionRule *)mosquitto_malloc(sizeof(CompactionRule));
    rule->func = mosquitto_strdup(func);
    rule->aggClass = GetAggClass(aggType);
    rule->aggType = aggType;
    rule->aggContext = rule->aggClass->createContext(false);
    rule->bucketDuration = bucketDuration;
    rule->timestampAlignment = timestampAlignment;
    rule->startCurrentTimeBucket = 0;
    rule->nextRule = NULL;
    rule->subContextLength = 0;
    if(timestampAlignment > 0 && timestampAlignment <= bucketDuration){
    	rule->curSubContextPos = 0;
    	rule->subContextLength = bucketDuration/timestampAlignment + (bucketDuration%timestampAlignment?1:0);
    	rule->subContext = (void **)mosquitto_malloc(rule->subContextLength*sizeof(void*));
    	for(int i = 0; i < rule->subContextLength; i++)
    		rule->subContext[i] = rule->aggClass->createContext(false);
    }

    return rule;
}

int addAggSample(void *dp,void *fp,double value){
	device *d = (device*)dp;
	field *f = (field*)fp;
	int rowId = d->curTimestampPos;
	f->pCache->dvalue[rowId] = value;
	bitmapSetBit(f->pCache->haveTimestamp,rowId);
	f->lastTimestamp = d->lastTimestamp;
	f->lastTimestampPos = rowId;

	return 0;
}

// Calculate the begining of aggregation bucket
static inline timestamp_t CalcBucketStart(timestamp_t ts,
                                          timestamp_t bucketDuration,
                                          timestamp_t timestampAlignment) {
    const int64_t timestamp_diff = ts - timestampAlignment;
    return ts - modulo(timestamp_diff, (int64_t)bucketDuration);
}
static inline timestamp_t BucketStartNormalize(timestamp_t bucketTS) {
    return max(0, (int64_t)bucketTS);
}

int handleQueryCompaction(CompactionRule *rule,timestamp_t timestamp,double value,double* aggVal,int otype) {
	int ret = 0;
    //timestamp_t currentTimestamp = CalcBucketStart(timestamp, rule->bucketDuration, rule->timestampAlignment);
	timestamp_t currentTimestamp = timestamp;
    timestamp_t currentTimestampNormalized = BucketStartNormalize(currentTimestamp);

    if (rule->startCurrentTimeBucket == 0) {
        // first sample, lets init the startCurrentTimeBucket
        rule->startCurrentTimeBucket = currentTimestampNormalized;
        rule->startTime = currentTimestampNormalized;

        if (rule->aggClass->addBucketParams) {
            rule->aggClass->addBucketParams(rule->aggContext,
            		timestamp,
					timestamp + rule->bucketDuration);
        }
    }

    int tmp = ((timestamp - rule->startTime) % rule->bucketDuration);
    int curSubContextPos = tmp /rule->timestampAlignment;
    if(rule->curSubContextPos != curSubContextPos){
    	rule->aggClass->addContext(rule, rule->curSubContextPos);
    }

    int append = 1;
    if ((otype == ORDER_ASC && timestamp >= (rule->startCurrentTimeBucket + rule->bucketDuration)) || (otype == ORDER_DESC && timestamp <= (rule->startCurrentTimeBucket - rule->bucketDuration))) {
    	if(timestamp == (rule->startCurrentTimeBucket + rule->bucketDuration)){
    		append = 0;
    		rule->aggClass->appendValue(rule->subContext[rule->curSubContextPos], value, timestamp);
    	}

    	ret = 1;
        if (rule->aggClass->addNextBucketFirstSample) {
            rule->aggClass->addNextBucketFirstSample(rule->aggContext, value, timestamp);
        }

        if(rule->curSubContextPos == curSubContextPos){
        	rule->aggClass->addContext(rule, rule->curSubContextPos);
        }
        rule->aggClass->finalize(rule->aggContext, aggVal);
//        if(rule->curSubContextPos == curSubContextPos){
//        	rule->aggClass->removeContext(rule, rule->curSubContextPos);
//        }

        Sample last_sample;
        if (rule->aggClass->addPrevBucketLastSample) {
            rule->aggClass->getLastSample(rule->aggContext, &last_sample);
        }
        //rule->aggClass->resetContext(rule->aggContext);
        if (rule->aggClass->addBucketParams) {
            rule->aggClass->addBucketParams(rule->aggContext,
            		timestamp,
					timestamp + rule->bucketDuration);
        }

        if (rule->aggClass->addPrevBucketLastSample) {
            rule->aggClass->addPrevBucketLastSample(
                rule->aggContext, last_sample.value, last_sample.timestamp);
        }
        //rule->startCurrentTimeBucket = timestamp;
        while((otype == ORDER_ASC && timestamp >= (rule->startCurrentTimeBucket + rule->bucketDuration)) || (otype == ORDER_DESC && timestamp <= (rule->startCurrentTimeBucket - rule->bucketDuration))){
            int pos = ((rule->startCurrentTimeBucket - rule->startTime) % rule->bucketDuration) /rule->timestampAlignment;
            rule->aggClass->removeContext(rule, pos);
            rule->aggClass->resetContext(rule->subContext[pos]);
            if(otype == ORDER_ASC)
            	rule->startCurrentTimeBucket += rule->timestampAlignment;
            else
            	rule->startCurrentTimeBucket -= rule->timestampAlignment;
        }
    }

    if(append){
        if(rule->curSubContextPos != curSubContextPos){
        	//rule->aggClass->removeContext(rule, curSubContextPos);
        	//rule->aggClass->resetContext(rule->subContext[curSubContextPos]);
        	rule->curSubContextPos = curSubContextPos;
        }
        //rule->aggClass->appendValue(rule->aggContext, value, timestamp);
        rule->aggClass->appendValue(rule->subContext[rule->curSubContextPos], value, timestamp);
    }

    return ret;
}

int handleCompaction(void *dp,CompactionRule *rule,timestamp_t timestamp,double value,double* aggVal) {
	int ret = 0;
	device *d = (device*)dp;
    //timestamp_t currentTimestamp = CalcBucketStart(timestamp, rule->bucketDuration, rule->timestampAlignment);
	timestamp_t currentTimestamp = timestamp;
    timestamp_t currentTimestampNormalized = BucketStartNormalize(currentTimestamp);

    if (rule->startCurrentTimeBucket == 0) {
        // first sample, lets init the startCurrentTimeBucket
        rule->startCurrentTimeBucket = currentTimestampNormalized;
        rule->startTime = currentTimestampNormalized;

        if (rule->aggClass->addBucketParams) {
            rule->aggClass->addBucketParams(rule->subContext[rule->curSubContextPos],
                                            currentTimestampNormalized,
                                            currentTimestamp + rule->timestampAlignment);
        }
    }

    int tmp = ((timestamp - rule->startTime) % rule->bucketDuration);
    int curSubContextPos = tmp /rule->timestampAlignment;
    if(rule->curSubContextPos != curSubContextPos){
    	rule->aggClass->addContext(rule, rule->curSubContextPos);
    }

    if ((timestamp - rule->startCurrentTimeBucket) > rule->bucketDuration) {
    	ret = 1;
/*        if (rule->aggClass->addNextBucketFirstSample) {
            rule->aggClass->addNextBucketFirstSample(rule->aggContext, value, timestamp);
        }*/
        if(rule->curSubContextPos == curSubContextPos){
        	rule->aggClass->addContext(rule, rule->curSubContextPos);
        }

        rule->aggClass->finalize(rule->aggContext, aggVal);
        //addAggSample(d,rule->aggField, aggVal);

        if(rule->curSubContextPos == curSubContextPos){
        	//rule->aggClass->removeContext(rule, rule->curSubContextPos);
        }
        //rule->aggClass->resetContext(rule->aggContext);
        //rule->startCurrentTimeBucket = currentTimestampNormalized;
        while ((timestamp - rule->startCurrentTimeBucket) > rule->bucketDuration){
            int pos = ((rule->startCurrentTimeBucket - rule->startTime) % rule->bucketDuration) /rule->timestampAlignment;
            rule->aggClass->removeContext(rule, pos);
            rule->aggClass->resetContext(rule->subContext[pos]);
            rule->startCurrentTimeBucket += rule->timestampAlignment;
        }
    }
    if(rule->curSubContextPos != curSubContextPos){
    	//rule->aggClass->removeContext(rule, curSubContextPos);
    	Sample last_sample;
    	if (rule->aggClass->addPrevBucketLastSample) {
    		rule->aggClass->getLastSample(rule->subContext[rule->curSubContextPos], &last_sample);
    	}
    	//rule->aggClass->resetContext(rule->subContext[curSubContextPos]);
    	if (rule->aggClass->addBucketParams) {
    		rule->aggClass->addBucketParams(rule->subContext[curSubContextPos],currentTimestampNormalized,currentTimestamp + rule->timestampAlignment);
    	}
    	if (rule->aggClass->addPrevBucketLastSample) {
    		rule->aggClass->addPrevBucketLastSample(rule->subContext[curSubContextPos], last_sample.value, last_sample.timestamp);
    	}

    	rule->curSubContextPos = curSubContextPos;
    }    

    //rule->aggClass->appendValue(rule->aggContext, value, timestamp);
    rule->aggClass->appendValue(rule->subContext[rule->curSubContextPos], value, timestamp);
    return ret;
}




