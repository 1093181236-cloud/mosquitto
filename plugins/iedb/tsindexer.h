/*
 * device.h
 *
 *  Created on: 2023Äê9ÔÂ4ÈÕ
 *      Author: a
 */
#ifndef INDEXER_H
#define INDEXER_H

#include "sds.h"

#include <sys/types.h>
#include <stdbool.h>

/* Prototypes of exported APIs. */
struct client;

typedef struct
{
    sds key;
    sds value;
} Label;

typedef enum
{
    EQ,
    NEQ,
    // Contains a label
    CONTAINS,
    // Not Contains a label
    NCONTAINS,
    LIST_MATCH,    // List of matching predicates
    LIST_NOTMATCH, // List of non-matching predicates
    // REQ,
    // NREQ
} PredicateType;

typedef struct QueryPredicate
{
    PredicateType type;
    sds key;
    sds *valuesList;
    size_t valueListCount;
} QueryPredicate;

typedef struct QueryPredicateList
{
    QueryPredicate *list;
    size_t count;
    size_t ref;
} QueryPredicateList;

int parsePredicate(const char *label_value_pair,
                   size_t label_value_pair_size,
                   QueryPredicate *retQuery,
                   const char *separator);
void QueryPredicate_Free(QueryPredicate *predicate, size_t count);
void QueryPredicateList_Free(QueryPredicateList *list);

void IndexInit(void);
void FreeLabels(void *value, size_t labelsCount);
void IndexMetric(sds ts_key, Label *labels, size_t labels_count);
void RemoveIndexedMetric(sds ts_key);
void RemoveAllIndexedMetrics(void);
void RemoveAllIndexedMetrics_generic(rax *_labelsIndex,rax **_tsLabelIndex);
int IsKeyIndexed(sds ts_key);
rax *QueryIndex(QueryPredicate *index_predicate,size_t predicate_count);

int CountPredicateType(QueryPredicateList *queries, PredicateType type);
#endif
