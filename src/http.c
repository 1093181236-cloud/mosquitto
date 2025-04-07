/*
 * http.c
 *
 *  Created on: 2025Äê1ÔÂ8ÈÕ
 *      Author: a
 */
#include "http.h"

#include "../plugins/iedb/rax.h"
#include "mosquitto_internal.h"
#include "mosquitto_broker_internal.h"

static struct router *router_list = NULL;
int test_form1(struct lws *wsi,int argc,char** argv,cJSON** tree){
	unsigned char buf[4096];

	size_t buflen = (size_t)snprintf((char *)buf, 4096, "HTTP/1.0 200 OK\r\n"
															"Server: mosquitto\r\n"
															"Content-Length: %u\r\n\r\nhello word",
															(unsigned int)10);
	if (lws_write(wsi, buf, buflen, LWS_WRITE_HTTP) < 0){
		return -1;
	}

	return 0;
}

static char *decode_uri(const char *uri, size_t length, size_t *out_len, int always_decode_plus) {
	char c;
	size_t i, j;
	int in_query = always_decode_plus;

	char *ret = mosquitto_calloc(1,length+4);

	for (i = j = 0; i < length; i++) {
		c = uri[i];
		if (c == '?') {
			in_query = 1;
		} else if (c == '+' && in_query) {
			c = ' ';
		} else if (c == '%' && isxdigit((unsigned char)uri[i+1]) &&
		    isxdigit((unsigned char)uri[i+2])) {
			char tmp[] = { uri[i+1], uri[i+2], '\0' };
			c = (char)strtol(tmp, NULL, 16);
			i += 2;
		}
		ret[j++] = c;
	}
	*out_len = (size_t)j;

	return ret;
}

int run_url(const char *uri,const char *body,cJSON** j_responses){
	uri++;
	size_t uri_len = strlen(uri);
	char *qmark = memchr(uri, '?', uri_len);
	char *slash;
	const char *p, *cmd_name = uri;
	int cmd_len;
	int param_count = 0;

	if(qmark) {
		uri_len = qmark - uri;
	}
	for(p = uri; p && p < uri + uri_len; param_count++) {
		p = memchr(p+1, '/', uri_len - (p+1-uri));
	}
	if(param_count == 0) {
		return HTTP_STATUS_BAD_REQUEST;
	}

	const char *ext;
	int ext_len = -1;
	for(ext = uri + uri_len - 1; ext != uri && *ext != '/'; --ext) {
		if(*ext == '.') {
			ext++;
			ext_len = uri + uri_len - ext;
			uri_len = uri_len - ext_len - 1;
			break;
		}
	}

	slash = memchr(uri, '/', uri_len);
	if(slash) {
		cmd_len = slash - uri;
	} else {
		cmd_len = uri_len;
	}

	struct router *r = NULL;
	HASH_FIND(hh, router_list, cmd_name, cmd_len, r);
	if(r == NULL)
		return HTTP_STATUS_NOT_FOUND;
	if(!slash) {
		return r->cb(0,NULL,j_responses);
	}

	int argc = 0;
	if(body)
		param_count++;
	char** argv = mosquitto_malloc(sizeof(char*)*param_count);
	p = cmd_name + cmd_len + 1;
	while(p < uri + uri_len) {
		const char *arg = p;
		int arg_len;
		char *next = memchr(arg, '/', uri_len - (arg-uri));
		if(!next || next > uri + uri_len) { /* last argument */
			p = uri + uri_len;
			arg_len = p - arg;
		} else { /* found a slash */
			arg_len = next - arg;
			p = next + 1;
		}

		/* record argument */
		size_t param_len;
		char* param = decode_uri(arg, arg_len, &param_len, 1);
		param[param_len] = 0;
	    argv[argc] = param;
	    argc++;
	}
	if(body){
		argv[argc] = body;
		argc++;
	}
	int res = r->cb(argc,argv,j_responses);
	if(body)
		argc--;
	for(int i = 0; i < argc; i++)
		mosquitto_free(argv[i]);
	mosquitto_free(argv);
	return res;
}

int url_register(char* url,void* cb){
	struct router* r = mosquitto_calloc(1, sizeof(struct router));
	r->cb = (urlCallback)cb;
	HASH_ADD_KEYPTR(hh, router_list, url, strlen(url), r);
	return 0;
}

int url_unregister(char* url){
	struct router* r;

	HASH_FIND(hh, router_list, url, strlen(url), r);
	if(r){
		HASH_DELETE(hh, router_list, r);
		mosquitto_free(r);
	}
}

int http_init(void){
	url_register("test",test_form1);
	return 0;
}
