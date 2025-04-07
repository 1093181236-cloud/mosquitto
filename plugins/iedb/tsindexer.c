/*
 * indexer.c
 *
 *  Created on: 2023Äê9ÔÂ4ÈÕ
 *      Author: a
 */
#include "server.h"
#include <assert.h>
#include <limits.h>
#include <string.h>

#include "tsindexer.h"

rax *labelsIndex;  // maps label to it's ts keys.
rax *tsLabelIndex; // maps ts_key to it's dict in labelsIndex
//extern bool isTrimming;

#define KV_PREFIX "__index_%s=%s"
#define K_PREFIX "__key_index_%s"

typedef enum
{
    Indexer_Add = 0x1,
    Indexer_Remove = 0x2,
} INDEXER_OPERATION_T;


void *MY_DictGetC(rax *d, void *key, size_t keylen, int *nokey) {
    void *res = raxFind(d,key,keylen);
    if (nokey) *nokey = (res == raxNotFound);
    return (res == raxNotFound) ? NULL : res;
}

void *MY_DictGet(rax *d, sds key, int *nokey) {
    return MY_DictGetC(d,key,sdslen(key),nokey);
}

int MY_DictDelC(rax *d, void *key, size_t keylen, void *oldval) {
    int retval = raxRemove(d,key,keylen,oldval);
    return retval ? C_OK : C_ERR;
}

int MY_DictDel(rax *d, sds key, void *oldval) {
    return MY_DictDelC(d,key,sdslen(key),oldval);
}

void IndexInit(void) {
    labelsIndex = raxNew();
    tsLabelIndex = raxNew();
}

void FreeLabels(void *value, size_t labelsCount) {
    Label *labels = (Label *)value;
    for (size_t i = 0; i < labelsCount; ++i) {
        sdsfree(labels[i].key);
        sdsfree(labels[i].value);
    }
    zfree(labels);
}

static int parseValueList(char *token, size_t *count, sds **values) {
    char *iter_ptr;
    if (token == NULL) {
        return C_ERR;
    }
    size_t token_len = strlen(token);

    if (token[token_len - 1] == ')') {
        token[token_len - 1] = '\0'; // remove closing parentheses
    } else {
        return C_ERR;
    }

    int filterCount = 0;
    for (int i = 0; token[i] != '\0'; i++) {
        if (token[i] == ',') {
            filterCount++;
        }
    }
    if (token_len <= 1) {
        // when the token is <=1 it means that we have an empty list
        *count = 0;
        *values = NULL;
        return C_OK;
    } else {
        *count = filterCount + 1;
    }
    *values = zcalloc((*count)*sizeof(sds));

    char *subToken = strtok_r(token, ",", &iter_ptr);
    for (size_t i = 0; i < *count; i++) {
        if (subToken == NULL) {
            return C_ERR;
        }
        (*values)[i] = sdsnew(subToken);
        subToken = strtok_r(NULL, ",", &iter_ptr);
    }
    return C_OK;
}

int parsePredicate(const char *label_value_pair,
                   size_t label_value_pair_size,
                   QueryPredicate *retQuery,
                   const char *separator) {
    char *token;
    char *iter_ptr;
    char *labelstr = zmalloc((label_value_pair_size + 1) * sizeof(char));
    labelstr[label_value_pair_size] = '\0';
    strncpy(labelstr, label_value_pair, label_value_pair_size);

    // Extract key
    token = strtok_r(labelstr, separator, &iter_ptr);
    if (token == NULL) {
        zfree(labelstr);
        return C_ERR;
    }
    retQuery->key = sdsnew(token);

    // Extract value
    token = strtok_r(NULL, separator, &iter_ptr);
    if (strstr(separator, "=(") != NULL) {
        if (parseValueList(token, &retQuery->valueListCount, &retQuery->valuesList) == C_ERR) {
        	sdsfree(retQuery->key);
            retQuery->key = NULL;
            zfree(labelstr);
            return C_ERR;
        }
    } else if (token != NULL) {
        retQuery->valueListCount = 1;
        retQuery->valuesList = zmalloc(sizeof(sds));
        retQuery->valuesList[0] = sdsnew(token);
    } else {
        retQuery->valuesList = NULL;
        retQuery->valueListCount = 0;
    }
    zfree(labelstr);
    return C_OK;
}

int CountPredicateType(QueryPredicateList *queries, PredicateType type) {
    int count = 0;
    for (size_t i = 0; i < queries->count; i++) {
        if (queries->list[i].type == type) {
            count++;
        }
    }
    return count;
}

static inline void labelsIndexRemoveTsKey(rax *leaf,
                                          sds key,
                                          sds ts_key,
                                          rax *_labelsIndex) {
    MY_DictDel(leaf, ts_key, NULL);
    if (raxSize(leaf) == 0) {
    	raxFree(leaf);
        MY_DictDel(_labelsIndex, key, NULL);
    }
}

int MY_DictSetC(rax *d, void *key, size_t keylen, void *ptr) {
    int retval = raxTryInsert(d,key,keylen,ptr,NULL);
    return (retval == 1) ? C_OK : C_ERR;
}
int MY_DictSet(rax *d, sds key, void *ptr) {
    return MY_DictSetC(d,key,sdslen(key),ptr);
}

void labelIndexUnderKey(INDEXER_OPERATION_T op,
                        sds key,
                        sds ts_key,
                        rax *_labelsIndex,
                        rax *_tsLabelIndex) {
    int nokey = 0;
    rax *leaf = MY_DictGet(_labelsIndex, key, &nokey);
    if (nokey) {
        leaf = raxNew();
        MY_DictSet(_labelsIndex, key, leaf);
    }

    rax *ts_leaf = MY_DictGet(_tsLabelIndex, ts_key, &nokey);
    if (nokey) {
        ts_leaf =  raxNew();
        MY_DictSet(_tsLabelIndex, ts_key, ts_leaf);
    }

    if (op & Indexer_Add) {
        MY_DictSet(leaf, ts_key, NULL);
        MY_DictSet(ts_leaf, key, NULL);
    } else if (op & Indexer_Remove) {
        labelsIndexRemoveTsKey(leaf, key, ts_key, _labelsIndex);
    }
}

void IndexMetric(sds ts_key, Label *labels, size_t labels_count) {
    const char *key_string, *value_string;
    for (size_t i = 0; i < labels_count; i++) {
        key_string = labels[i].key;
        value_string = labels[i].value;
        sds indexed_key_value = sdscatprintf(sdsempty(), KV_PREFIX, key_string, value_string);
        sds indexed_key = sdscatprintf(sdsempty(), K_PREFIX, key_string);

        labelIndexUnderKey(Indexer_Add, indexed_key_value, ts_key, labelsIndex, tsLabelIndex);
        labelIndexUnderKey(Indexer_Add, indexed_key, ts_key, labelsIndex, tsLabelIndex);

        sdsfree(indexed_key_value);
        sdsfree(indexed_key);
    }
}

raxIterator *MY_DictIteratorStartC(rax *d, const char *op, void *key, size_t keylen) {
	raxIterator *ri = zmalloc(sizeof(*ri));
    raxStart(ri,d);
    raxSeek(ri,op,key,keylen);
    return ri;
}

raxIterator *MY_DictIteratorStart(rax *d, const char *op, sds key) {
    return MY_DictIteratorStartC(d,op,key,sdslen(key));
}
void *MY_DictNextC(raxIterator *ri, size_t *keylen, void **dataptr) {
    if (!raxNext(ri)) return NULL;
    if (keylen) *keylen = ri->key_len;
    if (dataptr) *dataptr = ri->data;
    return ri->key;
}
void *MY_DictPrevC(raxIterator *ri, size_t *keylen, void **dataptr) {
    if (!raxPrev(ri)) return NULL;
    if (keylen) *keylen = ri->key_len;
    if (dataptr) *dataptr = ri->data;
    return ri->key;
}
sds MY_DictNext(raxIterator *ri, void **dataptr) {
    size_t keylen;
    void *key = MY_DictNextC(ri,&keylen,dataptr);
    if (key == NULL) return NULL;
    return sdsnewlen(key,keylen);
}
sds MY_DictPrev(raxIterator *ri, void **dataptr) {
    size_t keylen;
    void *key = MY_DictPrevC(ri,&keylen,dataptr);
    if (key == NULL) return NULL;
    return sdsnewlen(key,keylen);
}
void MY_DictIteratorStop(raxIterator *ri) {
    raxStop(ri);
    zfree(ri);
}

// Removes the ts from the label index and from the inverse index, if exist.
// del_key should be false if caller wants to avoid iterator invalidation.
void RemoveIndexedMetric_generic(sds ts_key,
                                 rax *_labelsIndex,
                                 rax *_tsLabelIndex,
                                 bool del_key) {
    int nokey = 0;
    rax *ts_leaf = MY_DictGet(_tsLabelIndex, ts_key, &nokey);
    if (nokey) { // series has no labels or already been removed from index
        return;
    }

    raxIterator *iter = MY_DictIteratorStartC(ts_leaf, "^", NULL, 0);
    sds currentLabelKey;
    while ((currentLabelKey = MY_DictNext(iter, NULL)) != NULL) {
        labelIndexUnderKey(Indexer_Remove, currentLabelKey, ts_key, _labelsIndex, _tsLabelIndex);
        sdsfree(currentLabelKey);
    }
    MY_DictIteratorStop(iter);
    raxFree(ts_leaf);
    if (del_key) {
        MY_DictDel(_tsLabelIndex, ts_key, NULL);
    }
}

// Removes the ts from the label index and from the inverse index, if exist.
void RemoveIndexedMetric(sds ts_key) {
    RemoveIndexedMetric_generic(ts_key, labelsIndex, tsLabelIndex, true);
}

// Removes all indexed metrics
void RemoveAllIndexedMetrics_generic(rax *_labelsIndex,
                                     rax **_tsLabelIndex) {
	raxIterator *iter = MY_DictIteratorStartC(*_tsLabelIndex, "^", NULL, 0);
    sds currentTSKey;
    while ((currentTSKey = MY_DictNext(iter, NULL)) != NULL) {
        RemoveIndexedMetric_generic(currentTSKey, _labelsIndex, *_tsLabelIndex, false);
        sdsfree(currentTSKey);
    }
    MY_DictIteratorStop(iter);
    raxFree(*_tsLabelIndex);
    *_tsLabelIndex = raxNew();
}

void RemoveAllIndexedMetrics(void) {
    RemoveAllIndexedMetrics_generic(labelsIndex, &tsLabelIndex);
}

int IsKeyIndexed(sds ts_key) {
    int nokey;
    MY_DictGet(tsLabelIndex, ts_key, &nokey);
    return !nokey;
}

void _union(rax *dest, rax *src) {
    /*
     * Copy all elements from src to dest
     */
	raxIterator *iter = MY_DictIteratorStartC(src, "^", NULL, 0);
    sds currentKey;
    while ((currentKey = MY_DictNext(iter, NULL)) != NULL) {
        MY_DictSet(dest, currentKey, (void *)1);
        sdsfree(currentKey);
    }
    MY_DictIteratorStop(iter);
}

void _intersect(rax *left, rax *right) {
	raxIterator *iter = MY_DictIteratorStartC(left, "^", NULL, 0);
	unsigned char *currentKey;
    size_t currentKeyLen;
    while ((currentKey = MY_DictNextC(iter, &currentKeyLen, NULL)) != NULL) {
        int doesNotExist = 0;
        MY_DictGetC(right, currentKey, currentKeyLen, &doesNotExist);
        if (doesNotExist == 0) {
            continue;
        }
        MY_DictDelC(left, currentKey, currentKeyLen, NULL);
        raxSeek(iter, ">", currentKey, currentKeyLen);
    }
    MY_DictIteratorStop(iter);
}

void _difference(rax *left, rax *right) {
    if (raxSize(right) == 0) {
        // the right leaf is empty, this means that the diff is basically no-op since the left will
        // remain intact.
        return;
    }

    // iterating over the left dict (which is always smaller) will allow us to have less data to
    // iterate over
    raxIterator *iter = MY_DictIteratorStartC(left, "^", NULL, 0);

    unsigned char *currentKey;
    size_t currentKeyLen;
    while ((currentKey = MY_DictNextC(iter, &currentKeyLen, NULL)) != NULL) {
        int doesNotExist = 0;
        MY_DictGetC(right, currentKey, currentKeyLen, &doesNotExist);
        if (doesNotExist == 1) {
            continue;
        }
        MY_DictDelC(left, currentKey, currentKeyLen, NULL);
        raxSeek(iter, ">", currentKey, currentKeyLen);
    }
    MY_DictIteratorStop(iter);
}

rax *GetPredicateKeysDict(QueryPredicate *predicate,bool *isCloned) {
    /*
     * Return the dictionary of all the keys that match the predicate.
     */
    rax *currentLeaf = NULL;
    *isCloned = false;
    sds index_key;
    //size_t _s;
    sds key = predicate->key;
    sds value;

    int nokey;

    if (predicate->type == NCONTAINS || predicate->type == CONTAINS) {
        index_key = sdscatprintf(sdsempty(), K_PREFIX, predicate->key);
        currentLeaf = MY_DictGet(labelsIndex, index_key, &nokey);
        sdsfree(index_key);
    } else { // one or more entries
        rax *singleEntryLeaf;
        int unioned_count = 0;
        for (size_t i = 0; i < predicate->valueListCount; i++) {
            value = predicate->valuesList[i];
            index_key = sdscatprintf(sdsempty(), KV_PREFIX, key, value);
            singleEntryLeaf = MY_DictGet(labelsIndex, index_key, &nokey);
            sdsfree(index_key);
            if (singleEntryLeaf != NULL) {
                // if there's only 1 item left to fetch from the index we can just return it
                if (unioned_count == 0 && predicate->valueListCount - i == 1) {
                    return singleEntryLeaf;
                }
                if (currentLeaf == NULL) {
                    currentLeaf = raxNew();
                    *isCloned = true;
                }
                _union(currentLeaf, singleEntryLeaf);
                unioned_count++;
            }
        }
    }
    return currentLeaf;
}

rax *QueryIndexPredicate(QueryPredicate *predicate,rax *prevResults) {
    rax *localResult = raxNew();
    rax *currentLeaf;
    bool isCloned;
    currentLeaf = GetPredicateKeysDict(predicate, &isCloned);

    if (currentLeaf != NULL) {
        // Copy everything to new dict only in case this is the first predicate.
        // In the next iteration, when prevResults is no longer NULL, there is
        // no need to copy again. We can work on currentLeaf, since only the left dict is being
        // changed during intersection / difference.
        if (prevResults == NULL) {
        	raxIterator *iter = MY_DictIteratorStartC(currentLeaf, "^", NULL, 0);
            sds currentKey;
            while ((currentKey = MY_DictNext(iter, NULL)) != NULL) {
                MY_DictSet(localResult, currentKey, (void *)1);
                sdsfree(currentKey);
            }
            MY_DictIteratorStop(iter);
        } else {
        	raxFree(localResult);
            localResult = currentLeaf;
        }
    }

    rax *result = NULL;
    if (prevResults != NULL) {
        if (predicate->type == EQ || predicate->type == CONTAINS) {
            _intersect(prevResults, localResult);
        } else if (predicate->type == LIST_MATCH) {
            _intersect(prevResults, localResult);
        } else if (predicate->type == LIST_NOTMATCH) {
            _difference(prevResults, localResult);
        } else if (predicate->type == NCONTAINS) {
            _difference(prevResults, localResult);
        } else if (predicate->type == NEQ) {
            _difference(prevResults, localResult);
        }
        result = prevResults;
    } else if (predicate->type == EQ || predicate->type == CONTAINS ||
               predicate->type == LIST_MATCH) {
        result = localResult;
    } else {
        result = prevResults; // always NULL
    }

    if (result != localResult && localResult != currentLeaf && localResult != NULL) {
    	raxFree(localResult);
    }
    if (isCloned && currentLeaf != result) {
    	raxFree(currentLeaf);
    }
    return result;
}

void PromoteSmallestPredicateToFront( QueryPredicate *index_predicate,
                                     size_t predicate_count) {
    /*
     * Find the predicate that has the minimal amount of keys that match to it, and move it to the
     * beginning of the predicate list so we will start our calculation from the smallest predicate.
     * This is an optimization, so we will copy the smallest dict possible.
     */
    if (predicate_count > 1) {
        int minIndex = 0;
        unsigned int minDictSize = UINT_MAX;
        bool isCloned;
        for (size_t i = 0; i < predicate_count; i++) {
            rax *currentPredicateKeys = GetPredicateKeysDict( &index_predicate[i], &isCloned);
            unsigned int currentDictSize =
                (currentPredicateKeys != NULL) ? raxSize(currentPredicateKeys) : 0;
            if (currentDictSize < minDictSize) {
                minIndex = i;
                minDictSize = currentDictSize;
            }
            if (currentPredicateKeys != NULL && isCloned) {
                raxFree(currentPredicateKeys);
            }
        }

        // switch between the minimal predicate and the predicate in the first place
        if (minIndex != 0) {
            QueryPredicate temp = index_predicate[minIndex];
            index_predicate[minIndex] = index_predicate[0];
            index_predicate[0] = temp;
        }
    }
}

rax *QueryIndex(QueryPredicate *index_predicate,size_t predicate_count) {
    rax *result = NULL;

    PromoteSmallestPredicateToFront(index_predicate, predicate_count);

    // EQ or Contains
    for (size_t i = 0; i < predicate_count; i++) {
        if (index_predicate[i].type == EQ || index_predicate[i].type == CONTAINS ||
            index_predicate[i].type == LIST_MATCH) {
            result = QueryIndexPredicate(&index_predicate[i], result);
            if (result == NULL) {
                return raxNew();
            }
        }
    }

    // The next two types of queries are reducers so we run them after the matcher
    // NCONTAINS or NEQ
    for (size_t i = 0; i < predicate_count; i++) {
        if (index_predicate[i].type == NCONTAINS || index_predicate[i].type == NEQ ||
            index_predicate[i].type == LIST_NOTMATCH) {
            result = QueryIndexPredicate(&index_predicate[i], result);
            if (result == NULL) {
                return raxNew();
            }
        }
    }

    if (result == NULL) {
        return raxNew();
    }

/*    if (unlikely(isTrimming)) {
        RedisModuleDictIter *iter = RedisModule_DictIteratorStartC(result, "^", NULL, 0);
        RedisModuleString *currentKey;
        int slot, firstSlot, lastSlot;
        RedisModule_ShardingGetSlotRange(&firstSlot, &lastSlot);
        while ((currentKey = RedisModule_DictNext(NULL, iter, NULL)) != NULL) {
            slot = RedisModule_ShardingGetKeySlot(currentKey);
            if (firstSlot > slot || lastSlot < slot) {
                RedisModule_DictDel(result, currentKey, NULL);
                RedisModule_DictIteratorReseek(iter, ">", currentKey);
            }
            RedisModule_FreeString(NULL, currentKey);
        }
        RedisModule_DictIteratorStop(iter);
    }*/

    return result;
}

void QueryPredicate_Free(QueryPredicate *predicate_list, size_t count) {
    for (size_t predicate_index = 0; predicate_index < count; predicate_index++) {
        QueryPredicate *predicate = &predicate_list[predicate_index];
        if (predicate->valuesList != NULL) {
            for (size_t i = 0; i < predicate->valueListCount; i++) {
                if (predicate->valuesList[i] != NULL) {
                    sdsfree(predicate->valuesList[i]);
                }
            }
        }
        sdsfree(predicate->key);
        zfree(predicate->valuesList);
    }
}

void QueryPredicateList_Free(QueryPredicateList *list) {
    if (list->ref > 1) {
        list->ref--;
        return;
    }
    assert(list->ref == 1);

    for (size_t i = 0; i < list->count; i++) {
        QueryPredicate_Free(&list->list[i], 1);
    }
    zfree(list->list);
    zfree(list);
}
