/*
 * db.h
 *
 *  Created on: 2025Äê1ÔÂ17ÈÕ
 *      Author: a
 */

#ifndef PLUGINS_IEDB_DB_H_
#define PLUGINS_IEDB_DB_H_

typedef int (selectIndexCallback)(void *privdata, long long start,long long end,long long pos,long long max_fid,long long row_num);

int db_open(void);
int db_close(void);

#endif /* PLUGINS_IEDB_DB_H_ */
