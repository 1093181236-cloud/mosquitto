/*
 * Copyright 2018-2019 Redis Labs Ltd. and Contributors
 *
 * This file is available under the Redis Labs Source Available License Agreement
 */
#include "tscompaction.h"
#include "tsrule.h"
#include "mosquitto.h"
#include "mosquitto_broker.h"

#include <ctype.h>
#include <float.h>
#include <math.h> // sqrt
#include <string.h>
#include <assert.h>
#include <stdlib.h>

#ifdef _DEBUG
#include "valgrind/valgrind.h"
#endif

#if __GNUC__ >= 3
#define likely(x) __builtin_expect(!!(x), 1)
#define unlikely(x) __builtin_expect(!!(x), 0)
#else
#define likely(x) (x)
#define unlikely(x) (x)
#endif
#define min(a,b) (((a)<(b))?(a):(b))

#define _DOUBLE_MIN (((double)-1.0) * DBL_MAX)
#define TSDB_OK 0
#define TSDB_ERROR -1
#define DC 0
#define TRUE 1
#define FALSE 0

typedef struct MaxMinContext
{
    double minValue;
    double maxValue;
} MaxMinContext;

static __attribute__((always_inline)) inline void _AssignIfGreater(double *__restrict__ value, double *__restrict__ newValues)
{
    if(*newValues > *value) {
        *value = *newValues;
    }
}


typedef struct SingleValueContext
{
    double value;
    char isResetted;
} SingleValueContext;

typedef struct AvgContext
{
    double val;
    double cnt;
    bool isOverflow;
} AvgContext;

typedef struct WeightData
{
    double weightSum;
    timestamp_t prevPrevTS;
    timestamp_t prevTS;
    double prevValue;
    timestamp_t bucketStartTS;
    timestamp_t bucketEndTS;
    bool is_first_bucket;
    bool is_last_ts_handled;
    int64_t iteration;
    double prevWeight;
    double weight_sum;
} WeightData;

typedef struct TwaContext
{
    AvgContext avgContext;
    WeightData weightData;
    bool reverse;
} TwaContext;

typedef struct StdContext
{
    double sum;
    double sum_2; // sum of (values^2)
    u_int64_t cnt;
} StdContext;

void finalize_empty_with_NAN(double *value) {
    *value = NAN;
}

void finalize_empty_with_ZERO(double *value) {
    *value = 0;
}

void *SingleValueCreateContext(__attribute__((unused)) bool reverse) {
    SingleValueContext *context = (SingleValueContext *)mosquitto_malloc(sizeof(SingleValueContext));
    context->value = 0;
    context->isResetted = TRUE;
    return context;
}

void SingleValueReset(void *contextPtr) {
    SingleValueContext *context = (SingleValueContext *)contextPtr;
    context->value = 0;
    context->isResetted = TRUE;
}

void SingleValueFinalize(void *contextPtr, double *val) {
    SingleValueContext *context = (SingleValueContext *)contextPtr;
    *val = context->value;
}

void SingleValueWriteContext(__attribute__((unused)) void *contextPtr, __attribute__((unused)) void *io) {
    //SingleValueContext *context = (SingleValueContext *)contextPtr;
    //RedisModule_SaveDouble(io, context->value);
    //RedisModule_SaveUnsigned(io, context->isResetted);
}

int SingleValueReadContext(__attribute__((unused)) void *contextPtr, __attribute__((unused)) void *io, __attribute__((unused)) int encver) {
    //SingleValueContext *context = (SingleValueContext *)contextPtr;
    //context->value = LoadDouble_IOError(io, goto err);
    //context->isResetted = LoadUnsigned_IOError(io, goto err);
    return TSDB_OK;

}

static inline void _AvgInitContext(AvgContext *context) {
    context->cnt = 0;
    context->val = 0;
    context->isOverflow = false;
}

void *AvgCreateContext(__attribute__((unused)) bool reverse) {
    AvgContext *context = (AvgContext *)mosquitto_malloc(sizeof(AvgContext));
    _AvgInitContext(context);
    return context;
}

// Except valgrind it's equivalent to sizeof(long double) > 8
#if !defined(_DEBUG) && !defined(_VALGRIND)
bool hasLongDouble = sizeof(long double) > 8;
#else
bool hasLongDouble = false;
#endif

void AvgAddValue(void *contextPtr, double value, __attribute__((unused)) timestamp_t ts) {
    AvgContext *context = (AvgContext *)contextPtr;
    context->cnt++;

    // Test for overflow
    if (unlikely(((context->val < 0.0) == (value < 0.0) &&
                  (fabs(context->val) > (DBL_MAX - fabs(value)))) ||
                 context->isOverflow)) {
        // calculating: avg(t+1) = t*avg(t)/(t+1) + val/(t+1)

        long double ld_val = context->val;
        long double ld_value = value;
        if (likely(hasLongDouble)) { // better accuracy
            ld_val /= context->cnt;
            if (context->isOverflow) {
                ld_val *= (long double)(context->cnt - 1);
            }
        } else {
            if (context->isOverflow) {
                ld_val *= ((long double)(context->cnt - 1) / context->cnt);
            } else {
                ld_val /= context->cnt;
            }
        }
        ld_val += (ld_value / (long double)context->cnt);
        context->val = ld_val;
        context->isOverflow = true;
    } else { // No Overflow
        context->val += value;
    }
}

void AvgAddContext(void *rulePtr,int subContextpos){
	CompactionRule *r = (CompactionRule *)rulePtr;
	AvgContext *mcontext = (AvgContext*)r->aggContext;
	AvgContext *scontext = (AvgContext *)(r->subContext[subContextpos]);
    mcontext->cnt += scontext->cnt;
    double value = scontext->val;

    // Test for overflow
    if (unlikely(((mcontext->val < 0.0) == (value < 0.0) &&
                  (fabs(mcontext->val) > (DBL_MAX - fabs(value)))) ||
                 mcontext->isOverflow)) {
        // calculating: avg(t+scontext->cnt) = t*avg(t)/(t+scontext->cnt) + val/(t+scontext->cnt)

        long double ld_val = mcontext->val;
        long double ld_value = value;
        if (likely(hasLongDouble)) { // better accuracy
            ld_val /= mcontext->cnt;
            if (mcontext->isOverflow) {
                ld_val *= (long double)(mcontext->cnt - scontext->cnt);
            }
        } else {
            if (mcontext->isOverflow) {
                ld_val *= ((long double)(mcontext->cnt - scontext->cnt) / mcontext->cnt);
            } else {
                ld_val /= mcontext->cnt;
            }
        }
        ld_val += (ld_value / (long double)mcontext->cnt);
        mcontext->val = ld_val;
        mcontext->isOverflow = true;
    } else { // No Overflow
        mcontext->val += value;
    }
}

void AvgFinalize(void *contextPtr, double *value) {
    AvgContext *context = (AvgContext *)contextPtr;
    assert(context->cnt > 0);
    if (unlikely(context->isOverflow)) {
        *value = context->val;
    } else {
        *value = context->val / context->cnt;
    }
}

void AvgRemoveContext(void *rulePtr,int subContextpos){
	CompactionRule *r = (CompactionRule *)rulePtr;
	AvgContext *mcontext = (AvgContext*)r->aggContext;
	AvgContext *scontext = (AvgContext *)(r->subContext[subContextpos]);
    mcontext->cnt -= scontext->cnt;

    if (unlikely(mcontext->isOverflow)){
    	mcontext->val -= scontext->val / scontext->cnt;
    }else{
    	mcontext->val -= scontext->val;
    }
}

void AvgReset(void *contextPtr) {
    _AvgInitContext(contextPtr);
}

void AvgWriteContext(__attribute__((unused)) void *contextPtr, __attribute__((unused)) void *io) {
    //AvgContext *context = (AvgContext *)contextPtr;
//    RedisModule_SaveDouble(io, context->val);
//    RedisModule_SaveDouble(io, context->cnt);
//    RedisModule_SaveUnsigned(io, context->isOverflow);
}

int AvgReadContext(__attribute__((unused)) void *contextPtr, __attribute__((unused)) void *io, __attribute__((unused)) int encver) {
    //AvgContext *context = (AvgContext *)contextPtr;
    //context->val = LoadDouble_IOError(io, goto err);
    //context->cnt = LoadDouble_IOError(io, goto err);
    //context->isOverflow = false;
    //context->isOverflow = !!(LoadUnsigned_IOError(io, goto err));
    return TSDB_OK;

}

static inline void _TwainitContext(TwaContext *context, bool reverse) {
    _AvgInitContext(&context->avgContext);
    context->weightData.weightSum = 0;
    context->weightData.prevPrevTS = DC;    // arbitrary value
    context->weightData.prevTS = DC;        // arbitrary value
    context->weightData.prevValue = DC;     // arbitrary value
    context->weightData.bucketStartTS = DC; // arbitrary value
    context->weightData.bucketEndTS = DC;   // arbitrary value
    context->weightData.is_first_bucket = true;
    context->weightData.is_last_ts_handled = false;
    context->weightData.iteration = 0;
    context->weightData.prevWeight = 0;
    context->weightData.weight_sum = 0;
    context->reverse = reverse;
}

void *TwaCreateContext(bool reverse) {
    TwaContext *context = (TwaContext *)mosquitto_malloc(sizeof(TwaContext));
    _TwainitContext(context, reverse);
    return context;
}

static inline void _update_twaContext(TwaContext *wcontext,
                                      const double *value,
                                      const timestamp_t *ts,
                                      const double *weight) {
    wcontext->weightData.prevPrevTS = wcontext->weightData.prevTS;
    wcontext->weightData.prevValue = *value;
    wcontext->weightData.prevTS = *ts;
    if (weight) {
        wcontext->weightData.prevWeight = *weight;
    }
}

void TwaAddBucketParams(void *contextPtr, timestamp_t bucketStartTS, timestamp_t bucketEndTS) {
    TwaContext *context = (TwaContext *)contextPtr;
    if (context->reverse) {
        __SWAP(bucketStartTS, bucketEndTS);
    }
    context->weightData.bucketStartTS = bucketStartTS;
    context->weightData.bucketEndTS = bucketEndTS;
}

void TwaAddPrevBucketLastSample(void *contextPtr, double value, timestamp_t ts) {
    TwaContext *wcontext = (TwaContext *)contextPtr;
    _update_twaContext(wcontext, &value, &ts, NULL);
    wcontext->weightData.is_first_bucket = false;
}

void TwaAddValue(void *contextPtr, double value, timestamp_t ts) {
    TwaContext *wcontext = (TwaContext *)contextPtr;
    AvgContext *context = &wcontext->avgContext;
    const bool *rev = &wcontext->reverse;
    int64_t *iter = &wcontext->weightData.iteration;
    ++(*iter);
    const timestamp_t *prevTS = &wcontext->weightData.prevTS;
    const double *prev_value = &wcontext->weightData.prevValue;
    const timestamp_t time_delta = llabs((int64_t)(ts - (*prevTS)));
    const double half_time_delta = time_delta / 2.0;
    const bool *is_first_bucket = &wcontext->weightData.is_first_bucket;
    const timestamp_t *startTS = &wcontext->weightData.bucketStartTS;
    double weight = 0;

    // add prev value with it's weight

    if ((*iter) == 1) { // First sample in bucket
        if (!(*is_first_bucket)) {
            // on reverse it's the mirror of the regular case
            if ((half_time_delta <= llabs((int64_t)((*startTS) - (*prevTS))))) {
                // regular: prev_ts --- --- half_way --- bucket_start --- fisrt_ts
                // reverse: fisrt_ts --- bucket_start --- half_way --- --- prev_ts
                weight = llabs((int64_t)(ts - (*startTS)));
            } else {
                // regular: prev_ts --- bucket_start --- half_way --- --- fisrt_ts
                // reverse: fisrt_ts --- --- half_way --- bucket_start --- prev_ts
                weight = (*rev) ? ((*startTS) - (ts + half_time_delta))
                                : ((*prevTS) + half_time_delta - (*startTS));
                wcontext->weightData.weight_sum += weight;
                AvgAddValue(context, (*prev_value) * weight, DC);
                weight = half_time_delta;
            }
        }
        // else: cur sample is the first in the series,
        // assume the delta from the prev sample is same as the delta from next sample
        // will be handled on next sample

        _update_twaContext(wcontext, &value, &ts, &weight);
        return;
    } else if (unlikely((*iter) == 2 && (*is_first_bucket))) {
        // 2nd sample in bucket and first bucket in the series
        // extrapolate the weight of the 1st sample to be time_delta
        weight = min(half_time_delta, llabs((int64_t)((*prevTS) - (*startTS)))) + half_time_delta;
    } else {
        weight = half_time_delta + wcontext->weightData.prevWeight;
    }

    // add prev value with it's weight
    AvgAddValue(context, (*prev_value) * weight, DC);
    wcontext->weightData.weight_sum += weight;
    wcontext->weightData.prevWeight = 0;

    // save cur value first weight
    _update_twaContext(wcontext, &value, &ts, &half_time_delta);
}

void TwaAddNextBucketFirstSample(void *contextPtr, double value, timestamp_t ts) {
    TwaContext *wcontext = (TwaContext *)contextPtr;
    AvgContext *context = &wcontext->avgContext;
    const bool *rev = &wcontext->reverse;
    int64_t *iter = &wcontext->weightData.iteration;
    ++(*iter);
    const timestamp_t *prevTS = &wcontext->weightData.prevTS;
    const double *prev_value = &wcontext->weightData.prevValue;
    const timestamp_t time_delta = llabs((int64_t)(ts - (*prevTS)));
    const double half_time_delta = time_delta / 2.0;
    const bool *is_first_bucket = &wcontext->weightData.is_first_bucket;
    const timestamp_t *startTS = &wcontext->weightData.bucketStartTS;
    const timestamp_t *endTS = &wcontext->weightData.bucketEndTS;
    double weight = wcontext->weightData.prevWeight;

    if (unlikely((*iter) == 2 && (*is_first_bucket))) {
        // Only 1 sample in bucket and first in the series
        // extrapolate the 1st weight of the 1st sample to be time_delta
        weight = min(half_time_delta, llabs((int64_t)((*prevTS) - (*startTS))));
    }

    // add the 2nd weight of prev sample
    if (half_time_delta >= llabs((int64_t)((*endTS) - (*prevTS)))) {
        // regular: last_ts --- bucket_end --- half_way --- --- next_ts
        // reverse: next_ts --- --- half_way --- bucket_end --- last_ts
        weight += llabs((int64_t)((*endTS) - (*prevTS)));
        wcontext->weightData.weight_sum += weight;
        AvgAddValue(context, (*prev_value) * weight, DC);
    } else {
        // regular: last_ts --- --- half_way --- bucket_end --- next_ts
        // reverse: next_ts --- bucket_end --- half_way --- --- last_ts
        weight += half_time_delta;
        wcontext->weightData.weight_sum += weight;
        AvgAddValue(context, (*prev_value) * weight, DC);
        weight = (*rev) ? ((ts + half_time_delta) - (*endTS))
                        : ((*endTS) - ((*prevTS) + half_time_delta));
        wcontext->weightData.weight_sum += weight;
        AvgAddValue(context, value * weight, DC);
    }

    wcontext->weightData.prevWeight = 0;
    wcontext->weightData.is_last_ts_handled = true;
}

void TwaAddContext(void *rulePtr,int subContextpos){
	CompactionRule *r = (CompactionRule *)rulePtr;
	TwaContext *mcontext = (TwaContext*)r->aggContext;
	TwaContext *scontext = (TwaContext *)(r->subContext[subContextpos]);
	mcontext->weightData.weight_sum += scontext->weightData.weight_sum;
	AvgAddValue(mcontext, scontext->avgContext.val, DC);
	mcontext->avgContext.cnt += scontext->avgContext.cnt -1;
}
void TwaRemoveContext(void *rulePtr,int subContextpos){
	CompactionRule *r = (CompactionRule *)rulePtr;
	TwaContext *mcontext = (TwaContext*)r->aggContext;
	TwaContext *scontext = (TwaContext *)(r->subContext[subContextpos]);
	mcontext->weightData.weight_sum -= scontext->weightData.weight_sum;
	mcontext->avgContext.val -= scontext->avgContext.val;
	mcontext->avgContext.cnt -= scontext->avgContext.cnt;
}
void TwaFinalize(void *contextPtr, double *value) {
    TwaContext *wcontext = (TwaContext *)contextPtr;
    AvgContext *context = &wcontext->avgContext;
    int64_t *iter = &wcontext->weightData.iteration;
    ++(*iter);

    if (!wcontext->weightData.is_last_ts_handled) {
        const bool *is_first_bucket = &wcontext->weightData.is_first_bucket;
        if (unlikely((*iter) == 2 && (*is_first_bucket))) {
            // Only 1 sample in bucket and that's the only sample in the series
            // don't use weights at all
            wcontext->weightData.weight_sum = 0;
            AvgAddValue(context, wcontext->weightData.prevValue, DC);
        } else {
            // This is the last bucket in the series
            // extrapolate the weight of the prev sample to be the prev time_delta
            const timestamp_t *prevTS = &wcontext->weightData.prevTS;
            const timestamp_t *endTS = &wcontext->weightData.bucketEndTS;
            const timestamp_t time_delta =
                llabs((int64_t)((*prevTS) - wcontext->weightData.prevPrevTS));
            const double half_time_delta = time_delta / 2.0;
            double weight = min(half_time_delta, llabs((int64_t)((*endTS) - (*prevTS)))) +
                            wcontext->weightData.prevWeight;
            wcontext->weightData.weight_sum += weight;
            AvgAddValue(context, wcontext->weightData.prevValue * weight, DC);
        }
    }

    if (wcontext->weightData.weight_sum > 0) {
        // Normalizing the weighting for each time by dividing each weight by the mean of all
        // weights
        const double avg_weight = wcontext->weightData.weight_sum / context->cnt;
        context->val /= avg_weight;
    }

    AvgFinalize(context, value);
    return;
}

void TwaGetLastSample(void *contextPtr, Sample *sample) {
    TwaContext *wcontext = (TwaContext *)contextPtr;
    sample->timestamp = wcontext->weightData.prevTS;
    sample->value = wcontext->weightData.prevValue;
}

void TwaReset(void *contextPtr) {
    TwaContext *wcontext = (TwaContext *)contextPtr;
    _TwainitContext(contextPtr, wcontext->reverse);
}

void TwaWriteContext(void *contextPtr, void *io) {
    TwaContext *context = (TwaContext *)contextPtr;
    AvgWriteContext(&context->avgContext, io);
/*    RedisModule_SaveDouble(io, context->weightData.weightSum);
    RedisModule_SaveUnsigned(io, context->weightData.prevPrevTS);
    RedisModule_SaveUnsigned(io, context->weightData.prevTS);
    RedisModule_SaveDouble(io, context->weightData.prevValue);
    RedisModule_SaveUnsigned(io, context->weightData.bucketStartTS);
    RedisModule_SaveUnsigned(io, context->weightData.bucketEndTS);
    RedisModule_SaveUnsigned(io, context->weightData.is_first_bucket);
    RedisModule_SaveUnsigned(io, context->weightData.is_last_ts_handled);
    RedisModule_SaveUnsigned(io, context->weightData.iteration);
    RedisModule_SaveDouble(io, context->weightData.prevWeight);
    RedisModule_SaveDouble(io, context->weightData.weight_sum);
    RedisModule_SaveUnsigned(io, context->reverse);*/
}

int TwaReadContext(void *contextPtr, void *io, int encver) {
    TwaContext *context = (TwaContext *)contextPtr;
    if (AvgReadContext(&context->avgContext, io, encver) == TSDB_ERROR) {
        goto err;
    }
/*
    context->weightData.weightSum = LoadDouble_IOError(io, goto err);
    context->weightData.prevPrevTS = LoadUnsigned_IOError(io, goto err);
    context->weightData.prevTS = LoadUnsigned_IOError(io, goto err);
    context->weightData.prevValue = LoadDouble_IOError(io, goto err);
    context->weightData.bucketStartTS = LoadUnsigned_IOError(io, goto err);
    context->weightData.bucketEndTS = LoadUnsigned_IOError(io, goto err);
    context->weightData.is_first_bucket = LoadUnsigned_IOError(io, goto err);
    context->weightData.is_last_ts_handled = LoadUnsigned_IOError(io, goto err);
    context->weightData.iteration = LoadUnsigned_IOError(io, goto err);
    context->weightData.prevWeight = LoadDouble_IOError(io, goto err);
    context->weightData.weight_sum = LoadDouble_IOError(io, goto err);
    context->reverse = LoadUnsigned_IOError(io, goto err);*/
    return TSDB_OK;
err:
    return TSDB_ERROR;
}

void *StdCreateContext(__attribute__((unused)) bool reverse) {
    StdContext *context = (StdContext *)mosquitto_malloc(sizeof(StdContext));
    context->cnt = 0;
    context->sum = 0;
    context->sum_2 = 0;
    return context;
}

void StdAddValue(void *contextPtr, double value, __attribute__((unused)) timestamp_t ts) {
    StdContext *context = (StdContext *)contextPtr;
    ++context->cnt;
    context->sum += value;
    context->sum_2 += value * value;
}
void StdAddContext(void *rulePtr,int subContextpos){
	CompactionRule *r = (CompactionRule *)rulePtr;
	StdContext *mcontext = (StdContext*)r->aggContext;
	StdContext *scontext = (StdContext *)(r->subContext[subContextpos]);
    mcontext->cnt += scontext->cnt;
    mcontext->sum += scontext->sum;
    mcontext->sum_2 += scontext->sum_2;
}
void StdRemoveContext(void *rulePtr,int subContextpos){
	CompactionRule *r = (CompactionRule *)rulePtr;
	StdContext *mcontext = (StdContext*)r->aggContext;
	StdContext *scontext = (StdContext *)(r->subContext[subContextpos]);
    mcontext->cnt -= scontext->cnt;
    mcontext->sum -= scontext->sum;
    mcontext->sum_2 -= scontext->sum_2;
}

static inline double variance(double sum, double sum_2, double count) {
    if (count == 0) {
        return 0;
    }

    /*  var(X) = sum((x_i - E[X])^2)
     *  = sum(x_i^2) - 2 * sum(x_i) * E[X] + E^2[X] */
    return (sum_2 - 2 * sum * sum / count + pow(sum / count, 2) * count) / count;
}

void VarPopulationFinalize(void *contextPtr, double *value) {
    StdContext *context = (StdContext *)contextPtr;
    u_int64_t count = context->cnt;
    assert(count > 0);
    *value = variance(context->sum, context->sum_2, count);
}

void VarSamplesFinalize(void *contextPtr, double *value) {
    StdContext *context = (StdContext *)contextPtr;
    u_int64_t count = context->cnt;
    assert(count > 0);
    if (count == 1) {
        *value = 0;
    } else {
        *value = variance(context->sum, context->sum_2, count) * count / (count - 1);
    }
}

void StdPopulationFinalize(void *contextPtr, double *value) {
    double val;
    VarPopulationFinalize(contextPtr, &val);
    *value = sqrt(val);
}

void StdSamplesFinalize(void *contextPtr, double *value) {
    double val;
    VarSamplesFinalize(contextPtr, &val);
    *value = sqrt(val);
}

void StdReset(void *contextPtr) {
    StdContext *context = (StdContext *)contextPtr;
    context->cnt = 0;
    context->sum = 0;
    context->sum_2 = 0;
}

void StdWriteContext(__attribute__((unused)) void *contextPtr, __attribute__((unused)) void *io) {
//    StdContext *context = (StdContext *)contextPtr;
    //RedisModule_SaveDouble(io, context->sum);
    //RedisModule_SaveDouble(io, context->sum_2);
    //RedisModule_SaveUnsigned(io, context->cnt);
}

int StdReadContext(__attribute__((unused)) void *contextPtr, __attribute__((unused)) void *io, __attribute__((unused)) int encver) {
    //StdContext *context = (StdContext *)contextPtr;
    //context->sum = LoadDouble_IOError(io, goto err);
    //context->sum_2 = LoadDouble_IOError(io, goto err);
    //context->cnt = LoadUnsigned_IOError(io, goto err);
    return TSDB_OK;

}

void rm_free(void *ptr) {
	mosquitto_free(ptr);
}

// time weighted avg
static AggregationClass waggAvg = { .createContext = TwaCreateContext,
                                    .appendValue = TwaAddValue,
                                    .freeContext = rm_free,
                                    .finalize = TwaFinalize,
                                    .finalizeEmpty = finalize_empty_with_NAN,
                                    .writeContext = TwaWriteContext,
                                    .readContext = TwaReadContext,
									.addContext = TwaAddContext,
									.removeContext = TwaRemoveContext,
                                    .addBucketParams = TwaAddBucketParams,
                                    .addPrevBucketLastSample = TwaAddPrevBucketLastSample,
                                    .addNextBucketFirstSample = TwaAddNextBucketFirstSample,
                                    .getLastSample = TwaGetLastSample,
                                    .resetContext = TwaReset };

static AggregationClass aggAvg = { .createContext = AvgCreateContext,
                                   .appendValue = AvgAddValue,
                                   .appendValueVec = NULL, /* determined on run time */
                                   .freeContext = rm_free,
                                   .finalize = AvgFinalize,
                                   .finalizeEmpty = finalize_empty_with_NAN,
                                   .writeContext = AvgWriteContext,
                                   .readContext = AvgReadContext,
									.addContext = AvgAddContext,
									.removeContext = AvgRemoveContext,
                                   .addBucketParams = NULL,
                                   .addPrevBucketLastSample = NULL,
                                   .addNextBucketFirstSample = NULL,
                                   .getLastSample = NULL,
                                   .resetContext = AvgReset };

static AggregationClass aggStdP = { .createContext = StdCreateContext,
                                    .appendValue = StdAddValue,
                                    .appendValueVec = NULL, /* determined on run time */
                                    .freeContext = rm_free,
                                    .finalize = StdPopulationFinalize,
                                    .finalizeEmpty = finalize_empty_with_NAN,
                                    .writeContext = StdWriteContext,
                                    .readContext = StdReadContext,
									.addContext = StdAddContext,
									.removeContext = StdRemoveContext,
                                    .addBucketParams = NULL,
                                    .addPrevBucketLastSample = NULL,
                                    .addNextBucketFirstSample = NULL,
                                    .getLastSample = NULL,
                                    .resetContext = StdReset };

static AggregationClass aggStdS = { .createContext = StdCreateContext,
                                    .appendValue = StdAddValue,
                                    .appendValueVec = NULL, /* determined on run time */
                                    .freeContext = rm_free,
                                    .finalize = StdSamplesFinalize,
                                    .finalizeEmpty = finalize_empty_with_NAN,
                                    .writeContext = StdWriteContext,
                                    .readContext = StdReadContext,
									.addContext = StdAddContext,
									.removeContext = StdRemoveContext,
                                    .addBucketParams = NULL,
                                    .addPrevBucketLastSample = NULL,
                                    .addNextBucketFirstSample = NULL,
                                    .getLastSample = NULL,
                                    .resetContext = StdReset };

static AggregationClass aggVarP = { .createContext = StdCreateContext,
                                    .appendValue = StdAddValue,
                                    .appendValueVec = NULL, /* determined on run time */
                                    .freeContext = rm_free,
                                    .finalize = VarPopulationFinalize,
                                    .finalizeEmpty = finalize_empty_with_NAN,
                                    .writeContext = StdWriteContext,
                                    .readContext = StdReadContext,
									.addContext = StdAddContext,
									.removeContext = StdRemoveContext,
                                    .addBucketParams = NULL,
                                    .addPrevBucketLastSample = NULL,
                                    .addNextBucketFirstSample = NULL,
                                    .getLastSample = NULL,
                                    .resetContext = StdReset };

static AggregationClass aggVarS = { .createContext = StdCreateContext,
                                    .appendValue = StdAddValue,
                                    .appendValueVec = NULL, /* determined on run time */
                                    .freeContext = rm_free,
                                    .finalize = VarSamplesFinalize,
                                    .finalizeEmpty = finalize_empty_with_NAN,
                                    .writeContext = StdWriteContext,
                                    .readContext = StdReadContext,
									.addContext = StdAddContext,
									.removeContext = StdRemoveContext,
                                    .addBucketParams = NULL,
                                    .addPrevBucketLastSample = NULL,
                                    .addNextBucketFirstSample = NULL,
                                    .getLastSample = NULL,
                                    .resetContext = StdReset };

void *MaxMinCreateContext(__attribute__((unused)) bool reverse) {
    MaxMinContext *context = (MaxMinContext *)mosquitto_malloc(sizeof(MaxMinContext));
    context->minValue = DBL_MAX;
    context->maxValue = _DOUBLE_MIN;
    return context;
}

void MaxAppendValue(void *contextPtr, double value, __attribute__((unused)) timestamp_t ts) {
    _AssignIfGreater(&((MaxMinContext *)contextPtr)->maxValue, &value);
}

void MaxAppendValuesVec(void *__restrict__ context,
                        double *__restrict__ values,
                        size_t si,
                        size_t ei) {
    for (size_t i = si; i <= ei; ++i) {
        _AssignIfGreater(&((MaxMinContext *)context)->maxValue, &values[i]);
    }
}

void MinAppendValue(void *contextPtr, double value, __attribute__((unused)) timestamp_t ts) {
    MaxMinContext *context = (MaxMinContext *)contextPtr;
    if (value < context->minValue) {
        context->minValue = value;
    }
}

void MaxMinAppendValue(void *contextPtr, double value, __attribute__((unused)) timestamp_t ts) {
    MaxMinContext *context = (MaxMinContext *)contextPtr;
    if (value > context->maxValue) {
        context->maxValue = value;
    }
    if (value < context->minValue) {
        context->minValue = value;
    }
}

void MaxAddContext(void *rulePtr,int subContextpos){
	CompactionRule* r = (CompactionRule*)rulePtr;
	MaxMinContext *mcontext = (MaxMinContext *)r->aggContext;
	MaxMinContext *scontext = (MaxMinContext *)(r->subContext[subContextpos]);
    if (scontext->maxValue > mcontext->maxValue) {
    	mcontext->maxValue = scontext->maxValue;
    }
}
void MaxRemoveContext(void *rulePtr,int subContextpos){
	CompactionRule* r = (CompactionRule*)rulePtr;
	MaxMinContext *mcontext = (MaxMinContext *)r->aggContext;
	MaxMinContext *scontext = (MaxMinContext *)(r->subContext[subContextpos]);
    if (scontext->maxValue == mcontext->maxValue) {
    	mcontext->minValue = DBL_MAX;
    	mcontext->maxValue = _DOUBLE_MIN;
    	for(int i = 0; i < r->subContextLength; i++){
    		if(i == subContextpos)
    			continue;
    		scontext = (MaxMinContext *)(r->subContext[subContextpos]);
    		_AssignIfGreater(&mcontext->maxValue, &scontext->maxValue);
    	}
    }
}

void MinAddContext(void *rulePtr,int subContextpos){
	CompactionRule* r = (CompactionRule*)rulePtr;
	MaxMinContext *mcontext = (MaxMinContext *)r->aggContext;
	MaxMinContext *scontext = (MaxMinContext *)(r->subContext[subContextpos]);
    if (scontext->minValue < mcontext->minValue) {
    	mcontext->minValue = scontext->minValue;
    }
}
void MinRemoveContext(void *rulePtr,int subContextpos){
	CompactionRule* r = (CompactionRule*)rulePtr;
	MaxMinContext *mcontext = (MaxMinContext *)r->aggContext;
	MaxMinContext *scontext = (MaxMinContext *)(r->subContext[subContextpos]);
    if (scontext->minValue == mcontext->minValue) {
    	mcontext->minValue = DBL_MAX;
    	mcontext->maxValue = _DOUBLE_MIN;
    	for(int i = 0; i < r->subContextLength; i++){
    		if(i == subContextpos)
    			continue;
    		scontext = (MaxMinContext *)(r->subContext[subContextpos]);
    		if (scontext->minValue < mcontext->minValue) {
    		    mcontext->minValue = scontext->minValue;
    		}
    	}
    }
}

void MaxMinAddContext(void *rulePtr,int subContextpos){
	CompactionRule* r = (CompactionRule*)rulePtr;
	MaxMinContext *mcontext = (MaxMinContext *)r->aggContext;
	MaxMinContext *scontext = (MaxMinContext *)(r->subContext[subContextpos]);
    if (scontext->maxValue > mcontext->maxValue) {
    	mcontext->maxValue = scontext->maxValue;
    }
    if (scontext->minValue < mcontext->minValue) {
    	mcontext->minValue = scontext->minValue;
    }
}
void MaxMinRemoveContext(void *rulePtr,int subContextpos){
	CompactionRule* r = (CompactionRule*)rulePtr;
	MaxMinContext *mcontext = (MaxMinContext *)r->aggContext;
	MaxMinContext *scontext = (MaxMinContext *)(r->subContext[subContextpos]);
    if (scontext->minValue == mcontext->minValue || scontext->maxValue == mcontext->maxValue) {
    	mcontext->minValue = DBL_MAX;
    	mcontext->maxValue = _DOUBLE_MIN;
    	for(int i = 0; i < r->subContextLength; i++){
    		if(i == subContextpos)
    			continue;
    		scontext = (MaxMinContext *)(r->subContext[subContextpos]);
    	    if (scontext->maxValue > mcontext->maxValue) {
    	    	mcontext->maxValue = scontext->maxValue;
    	    }
    		if (scontext->minValue < mcontext->minValue) {
    		    mcontext->minValue = scontext->minValue;
    		}
    	}
    }
}

void MaxFinalize(void *contextPtr, double *value) {
    MaxMinContext *context = (MaxMinContext *)contextPtr;
    *value = context->maxValue;
}

void MinFinalize(void *contextPtr, double *value) {
    MaxMinContext *context = (MaxMinContext *)contextPtr;
    *value = context->minValue;
}

void RangeFinalize(void *contextPtr, double *value) {
    MaxMinContext *context = (MaxMinContext *)contextPtr;
    *value = context->maxValue - context->minValue;
}

void MaxMinReset(void *contextPtr) {
    MaxMinContext *context = (MaxMinContext *)contextPtr;
    context->minValue = DBL_MAX;
    context->maxValue = _DOUBLE_MIN;
}

void MaxMinWriteContext(__attribute__((unused)) void *contextPtr, __attribute__((unused)) void *io) {
    //MaxMinContext *context = (MaxMinContext *)contextPtr;
    //RedisModule_SaveDouble(io, context->maxValue);
    //RedisModule_SaveDouble(io, context->minValue);
}

int MaxMinReadContext(__attribute__((unused)) void *contextPtr, __attribute__((unused)) void *io, __attribute__((unused)) int encver) {
    //MaxMinContext *context = (MaxMinContext *)contextPtr;
    //context->maxValue = LoadDouble_IOError(io, goto err);
    //context->minValue = LoadDouble_IOError(io, goto err);
    return TSDB_OK;
}

void SumAppendValue(void *contextPtr, double value, __attribute__((unused)) timestamp_t ts) {
    SingleValueContext *context = (SingleValueContext *)contextPtr;
    context->value += value;
}
void SumAddContext(void *rulePtr,int subContextpos){
	CompactionRule* r = (CompactionRule*)rulePtr;
	SingleValueContext *mcontext = (SingleValueContext *)r->aggContext;
	SingleValueContext *scontext = (SingleValueContext *)(r->subContext[subContextpos]);
	mcontext->value += scontext->value;
}
void SumRemoveContext(void *rulePtr,int subContextpos){
	CompactionRule* r = (CompactionRule*)rulePtr;
	SingleValueContext *mcontext = (SingleValueContext *)r->aggContext;
	SingleValueContext *scontext = (SingleValueContext *)(r->subContext[subContextpos]);
	mcontext->value -= scontext->value;
}

void CountAppendValue(void *contextPtr,  __attribute__((unused)) double value, __attribute__((unused)) timestamp_t ts) {
    SingleValueContext *context = (SingleValueContext *)contextPtr;
    context->value++;
}

void CountFinalize(void *contextPtr, double *val) {
    SingleValueContext *context = (SingleValueContext *)contextPtr;
    *val = context->value;
}

void FirstAppendValue(void *contextPtr, double value, __attribute__((unused)) timestamp_t ts) {
    SingleValueContext *context = (SingleValueContext *)contextPtr;
    if (context->isResetted) {
        context->isResetted = FALSE;
        context->value = value;
    }
}
void FirstAddContext(void *rulePtr,int subContextpos){
	CompactionRule* r = (CompactionRule*)rulePtr;
	SingleValueContext *mcontext = (SingleValueContext *)r->aggContext;
	SingleValueContext *scontext = (SingleValueContext *)(r->subContext[subContextpos]);
    if (mcontext->isResetted) {
        mcontext->isResetted = FALSE;
        mcontext->value = scontext->value;
    }
}
void FirstRemoveContext(void *rulePtr,int subContextpos){
	CompactionRule* r = (CompactionRule*)rulePtr;
	SingleValueContext *mcontext = (SingleValueContext *)r->aggContext;
	subContextpos++;
	if(subContextpos == r->subContextLength)
		subContextpos = 0;
	SingleValueContext *scontext = (SingleValueContext *)(r->subContext[subContextpos]);

	if (scontext->isResetted == FALSE){
		mcontext->value = scontext->value;
		mcontext->isResetted = FALSE;
	}else{
		mcontext->isResetted = TRUE;
	}
}
void LastAddContext(void *rulePtr,int subContextpos){
	CompactionRule* r = (CompactionRule*)rulePtr;
	SingleValueContext *mcontext = (SingleValueContext *)r->aggContext;
	SingleValueContext *scontext = (SingleValueContext *)(r->subContext[subContextpos]);
	mcontext->value = scontext->value;
}
void LastRemoveContext(__attribute__((unused)) void *rulePtr,__attribute__((unused)) int subContextpos){
	return;
}

void LastAppendValue(void *contextPtr, double value, __attribute__((unused)) timestamp_t ts) {
    SingleValueContext *context = (SingleValueContext *)contextPtr;
    context->value = value;
}

AggregationClass aggMax = { .createContext = MaxMinCreateContext,
                            .appendValue = MaxAppendValue,
                            .appendValueVec = NULL, /* determined on run time */
                            .freeContext = rm_free,
                            .finalize = MaxFinalize,
                            .finalizeEmpty = finalize_empty_with_NAN,
                            .writeContext = MaxMinWriteContext,
                            .readContext = MaxMinReadContext,
							.addContext = MaxAddContext,
							.removeContext = MaxRemoveContext,
                            .addBucketParams = NULL,
                            .addPrevBucketLastSample = NULL,
                            .addNextBucketFirstSample = NULL,
                            .getLastSample = NULL,
                            .resetContext = MaxMinReset };

static AggregationClass aggMin = { .createContext = MaxMinCreateContext,
                                   .appendValue = MinAppendValue,
                                   .appendValueVec = NULL, /* determined on run time */
                                   .freeContext = rm_free,
                                   .finalize = MinFinalize,
                                   .finalizeEmpty = finalize_empty_with_NAN,
                                   .writeContext = MaxMinWriteContext,
                                   .readContext = MaxMinReadContext,
								   .addContext = MinAddContext,
								   .removeContext = MinRemoveContext,
                                   .addBucketParams = NULL,
                                   .addPrevBucketLastSample = NULL,
                                   .addNextBucketFirstSample = NULL,
                                   .getLastSample = NULL,
                                   .resetContext = MaxMinReset };

static AggregationClass aggSum = { .createContext = SingleValueCreateContext,
                                   .appendValue = SumAppendValue,
                                   .appendValueVec = NULL, /* determined on run time */
                                   .freeContext = rm_free,
                                   .finalize = SingleValueFinalize,
                                   .finalizeEmpty = finalize_empty_with_ZERO,
                                   .writeContext = SingleValueWriteContext,
                                   .readContext = SingleValueReadContext,
								   .addContext = SumAddContext,
								   .removeContext = SumRemoveContext,
                                   .addBucketParams = NULL,
                                   .addPrevBucketLastSample = NULL,
                                   .addNextBucketFirstSample = NULL,
                                   .getLastSample = NULL,
                                   .resetContext = SingleValueReset };


static AggregationClass aggCount = { .createContext = SingleValueCreateContext,
                                     .appendValue = CountAppendValue,
                                     .appendValueVec = NULL, /* determined on run time */
                                     .freeContext = rm_free,
                                     .finalize = CountFinalize,
                                     .finalizeEmpty = finalize_empty_with_ZERO,
                                     .writeContext = SingleValueWriteContext,
                                     .readContext = SingleValueReadContext,
									 .addContext = SumAddContext,
									 .removeContext = SumRemoveContext,
                                     .addBucketParams = NULL,
                                     .addPrevBucketLastSample = NULL,
                                     .addNextBucketFirstSample = NULL,
                                     .getLastSample = NULL,
                                     .resetContext = SingleValueReset };

static AggregationClass aggFirst = { .createContext = SingleValueCreateContext,
                                     .appendValue = FirstAppendValue,
                                     .appendValueVec = NULL, /* determined on run time */
                                     .freeContext = rm_free,
                                     .finalize = SingleValueFinalize,
                                     .finalizeEmpty = finalize_empty_with_NAN,
                                     .writeContext = SingleValueWriteContext,
                                     .readContext = SingleValueReadContext,
									 .addContext = FirstAddContext,
									 .removeContext = FirstRemoveContext,
                                     .addBucketParams = NULL,
                                     .addPrevBucketLastSample = NULL,
                                     .addNextBucketFirstSample = NULL,
                                     .getLastSample = NULL,
                                     .resetContext = SingleValueReset };

static AggregationClass aggLast = { .createContext = SingleValueCreateContext,
                                    .appendValue = LastAppendValue,
                                    .appendValueVec = NULL, /* determined on run time */
                                    .freeContext = rm_free,
                                    .finalize = SingleValueFinalize,
                                    .finalizeEmpty = finalize_empty_with_NAN,
                                    .writeContext = SingleValueWriteContext,
                                    .readContext = SingleValueReadContext,
									.addContext = LastAddContext,
									.removeContext = LastRemoveContext,
                                    .addBucketParams = NULL,
                                    .addPrevBucketLastSample = NULL,
                                    .addNextBucketFirstSample = NULL,
                                    .getLastSample = NULL,
                                    .resetContext = SingleValueReset };

static AggregationClass aggRange = { .createContext = MaxMinCreateContext,
                                     .appendValue = MaxMinAppendValue,
                                     .appendValueVec = NULL, /* determined on run time */
                                     .freeContext = rm_free,
                                     .finalize = RangeFinalize,
                                     .finalizeEmpty = finalize_empty_with_NAN,
                                     .writeContext = MaxMinWriteContext,
                                     .readContext = MaxMinReadContext,
									 .addContext = MaxMinAddContext,
									 .removeContext = MaxMinRemoveContext,
                                     .addBucketParams = NULL,
                                     .addPrevBucketLastSample = NULL,
                                     .addNextBucketFirstSample = NULL,
                                     .getLastSample = NULL,
                                     .resetContext = MaxMinReset };


void initGlobalCompactionFunctions(void) {
    //const X86Features *features = getArchitectureOptimization();
    //aggMax.appendValueVec = MaxAppendValuesVec;
/*
#if defined(__x86_64__)
    if (!features) {
        return;
         remove this comment to enable avx512
     } else if (features->avx512f) {
            aggMax.appendValueVec = MaxAppendValuesAVX512F;
            return;
        }
    } else if (features->avx2) {
        aggMax.appendValueVec = MaxAppendValuesAVX2;
        return;
    }
#endif // __x86_64__
*/
    return;
}


int StringAggTypeToEnum(const char *agg_type) {
    return StringLenAggTypeToEnum(agg_type, strlen(agg_type));
}

int RMStringLenAggTypeToEnum(const char * aggTypeStr) {
    size_t str_len = strlen(aggTypeStr);
    const char *aggTypeCStr = aggTypeStr;
    return StringLenAggTypeToEnum(aggTypeCStr, str_len);
}

int StringLenAggTypeToEnum(const char *agg_type, size_t len) {
    int result = TS_AGG_INVALID;
    char agg_type_lower[len];
    for (size_t i = 0; i < len; i++) {
        agg_type_lower[i] = tolower(agg_type[i]);
    }
    if (len == 3) {
        if (strncmp(agg_type_lower, "min", len) == 0 && len == 3) {
            result = TS_AGG_MIN;
        } else if (strncmp(agg_type_lower, "max", len) == 0) {
            result = TS_AGG_MAX;
        } else if (strncmp(agg_type_lower, "sum", len) == 0) {
            result = TS_AGG_SUM;
        } else if (strncmp(agg_type_lower, "avg", len) == 0) {
            result = TS_AGG_AVG;
        } else if (strncmp(agg_type_lower, "twa", len) == 0) {
            result = TS_AGG_TWA;
        }
    } else if (len == 4) {
        if (strncmp(agg_type_lower, "last", len) == 0) {
            result = TS_AGG_LAST;
        } else if (strncmp(agg_type_lower, "stdp", len) == 0) {
            result = TS_AGG_STD_P;
        } else if (strncmp(agg_type_lower, "stds", len) == 0) {
            result = TS_AGG_STD_S;
        } else if (strncmp(agg_type_lower, "varp", len) == 0) {
            result = TS_AGG_VAR_P;
        } else if (strncmp(agg_type_lower, "vars", len) == 0) {
            result = TS_AGG_VAR_S;
        }
    } else if (len == 5) {
        if (strncmp(agg_type_lower, "count", len) == 0) {
            result = TS_AGG_COUNT;
        } else if (strncmp(agg_type_lower, "range", len) == 0) {
            result = TS_AGG_RANGE;
        } else if (strncmp(agg_type_lower, "first", len) == 0) {
            result = TS_AGG_FIRST;
        }
    }
    return result;
}

const char *AggTypeEnumToString(TS_AGG_TYPES_T aggType) {
    switch (aggType) {
        case TS_AGG_MIN:
            return "MIN";
        case TS_AGG_MAX:
            return "MAX";
        case TS_AGG_SUM:
            return "SUM";
        case TS_AGG_AVG:
            return "AVG";
        case TS_AGG_TWA:
            return "TWA";
        case TS_AGG_STD_P:
            return "STDP";
        case TS_AGG_STD_S:
            return "STDS";
        case TS_AGG_VAR_P:
            return "VARP";
        case TS_AGG_VAR_S:
            return "VARS";
        case TS_AGG_COUNT:
            return "COUNT";
        case TS_AGG_FIRST:
            return "FIRST";
        case TS_AGG_LAST:
            return "LAST";
        case TS_AGG_RANGE:
            return "RANGE";
        case TS_AGG_NONE:
        case TS_AGG_INVALID:
        case TS_AGG_TYPES_MAX:
            break;
    }
    return "Unknown";
}

const char *AggTypeEnumToStringLowerCase(TS_AGG_TYPES_T aggType) {
    switch (aggType) {
        case TS_AGG_MIN:
            return "min";
        case TS_AGG_MAX:
            return "max";
        case TS_AGG_SUM:
            return "sum";
        case TS_AGG_AVG:
            return "avg";
        case TS_AGG_TWA:
            return "twa";
        case TS_AGG_STD_P:
            return "stdp";
        case TS_AGG_STD_S:
            return "stds";
        case TS_AGG_VAR_P:
            return "varp";
        case TS_AGG_VAR_S:
            return "vars";
        case TS_AGG_COUNT:
            return "count";
        case TS_AGG_FIRST:
            return "first";
        case TS_AGG_LAST:
            return "last";
        case TS_AGG_RANGE:
            return "range";
        case TS_AGG_NONE:
        case TS_AGG_INVALID:
        case TS_AGG_TYPES_MAX:
            break;
    }
    return "unknown";
}

AggregationClass *GetAggClass(TS_AGG_TYPES_T aggType) {
    switch (aggType) {
        case TS_AGG_MIN:
            return &aggMin;
        case TS_AGG_MAX:
            return &aggMax;
        case TS_AGG_AVG:
            return &aggAvg;
        case TS_AGG_TWA:
            return &waggAvg;
        case TS_AGG_STD_P:
            return &aggStdP;
        case TS_AGG_STD_S:
            return &aggStdS;
        case TS_AGG_VAR_P:
            return &aggVarP;
        case TS_AGG_VAR_S:
            return &aggVarS;
        case TS_AGG_SUM:
            return &aggSum;
        case TS_AGG_COUNT:
            return &aggCount;
        case TS_AGG_FIRST:
            return &aggFirst;
        case TS_AGG_LAST:
            return &aggLast;
        case TS_AGG_RANGE:
            return &aggRange;
        case TS_AGG_NONE:
        case TS_AGG_INVALID:
        case TS_AGG_TYPES_MAX:
            break;
    }
    return NULL;
}
