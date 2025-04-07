#ifndef HTTP_H
#define HTTP_H

#include "uthash.h"
#include <cjson/cJSON.h>
typedef int (*urlCallback)(int argc,char** argv,cJSON** tree);

struct router{
	UT_hash_handle hh;
	urlCallback cb;
};

int http_init(void);
int run_url(const char *url,const char *body,cJSON** j_responses);


#endif
