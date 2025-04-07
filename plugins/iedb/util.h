/*
 * util.h
 *
 *  Created on: 2025Äê1ÔÂ16ÈÕ
 *      Author: a
 */

#ifndef PLUGINS_IEDB_UTIL_H_
#define PLUGINS_IEDB_UTIL_H_

#include <stddef.h>

#define min(a, b) ((a) < (b) ? (a) : (b))
#define max(a, b) ((a) > (b) ? (a) : (b))

#define LONG_STR_SIZE      21

int string2ull(const char *s, unsigned long long *value);
int string2ll(const char *s, size_t slen, long long *value);
int stringmatchlen(const char *pattern, int patternLen,
        const char *string, int stringLen, int nocase);
int stringmatch(const char *pattern, const char *string, int nocase);

void bitmapClearBit(unsigned char *bitmap, int pos);
void bitmapSetBit(unsigned char *bitmap, int pos);
int bitmapTestBit(unsigned char *bitmap, int pos);
long long mstime(void);

#endif /* PLUGINS_IEDB_UTIL_H_ */
