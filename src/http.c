/*
 * http.c
 *
 *  Created on: 2025年1月8日
 *      Author: a
 */
#include "http.h"

#include "../plugins/iedb/rax.h"
#include "mosquitto_internal.h"
#include "mosquitto_broker_internal.h"
#include <openssl/err.h>
#include <openssl/aes.h>
#include <openssl/evp.h>
#include <openssl/bio.h>
#include <openssl/buffer.h>
#include <openssl/rand.h>
#include <openssl/md5.h>

#if defined(_WIN32)
#include <intrin.h>
#else
#include <cpuid.h>
#endif

void GetCPUID(int info[4], int function_id) {
#if defined(_WIN32)
    __cpuid(info, function_id);
#else
    __cpuid(function_id, info[0], info[1], info[2], info[3]);
#endif
}

static char cpuid_str[128] = {0};
static time_t start_timestamp = 0;
static time_t end_timestamp = 0;

static struct router *router_list = NULL;

int base64_encode(unsigned char *in, int in_len, char **encoded)
{
	BIO *bmem, *b64;
	BUF_MEM *bptr = NULL;

	if(in_len < 0) return 1;

	b64 = BIO_new(BIO_f_base64());
	if(b64 == NULL) return 1;

	BIO_set_flags(b64, BIO_FLAGS_BASE64_NO_NL);
	bmem = BIO_new(BIO_s_mem());
	if(bmem == NULL){
		BIO_free_all(b64);
		return 1;
	}
	b64 = BIO_push(b64, bmem);
	BIO_write(b64, in, in_len);
	if(BIO_flush(b64) != 1){
		BIO_free_all(b64);
		return 1;
	}
	BIO_get_mem_ptr(b64, &bptr);
	*encoded = mosquitto_malloc(bptr->length+1);
	if(!(*encoded)){
		BIO_free_all(b64);
		return 1;
	}
	memcpy(*encoded, bptr->data, bptr->length);
	(*encoded)[bptr->length] = '\0';
	BIO_free_all(b64);

	return 0;
}


int base64_decode(char *in, unsigned char **decoded, int *decoded_len)
{
	BIO *bmem, *b64;
	size_t slen;

	slen = strlen(in);

	b64 = BIO_new(BIO_f_base64());
	if(!b64){
		return 1;
	}
	BIO_set_flags(b64, BIO_FLAGS_BASE64_NO_NL);

	bmem = BIO_new(BIO_s_mem());
	if(!bmem){
		BIO_free_all(b64);
		return 1;
	}
	b64 = BIO_push(b64, bmem);
	BIO_write(bmem, in, (int)slen);

	if(BIO_flush(bmem) != 1){
		BIO_free_all(b64);
		return 1;
	}
	*decoded = mosquitto_calloc(slen, 1);
	if(!(*decoded)){
		BIO_free_all(b64);
		return 1;
	}
	*decoded_len =  BIO_read(b64, *decoded, (int)slen);
	BIO_free_all(b64);

	if(*decoded_len <= 0){
		mosquitto_free(*decoded);
		*decoded = NULL;
		*decoded_len = 0;
		return 1;
	}

	return 0;
}

void cpuid(void){
	//int eax, ebx, ecx, edx;
	//eax = 1; // processor serial number
	//asm volatile("cpuid": "=a" (eax),"=b" (ebx),"=c" (ecx),"=d" (edx): "0" (eax), "2" (ecx));
    //snprintf(cpuid_str,64,"%08X%08XubuntuCpuId", edx, eax);
	int info[4];
	GetCPUID(info,1);
	snprintf(cpuid_str,64,"%08X%08XubuntuCpuId", info[3], info[0]);

    MD5_CTX ctx;
    unsigned char digest[MD5_DIGEST_LENGTH];
    if (!MD5_Init(&ctx)) {
        fprintf(stderr, "MD5 init fail\n");
        return;
    }
    MD5_Update(&ctx, cpuid_str, strlen(cpuid_str));
    MD5_Final(digest, &ctx);

    const char hex_chars[] = "0123456789abcdef";
    for (int i = 0; i < MD5_DIGEST_LENGTH; i++) {
    	cpuid_str[i * 2] = hex_chars[(digest[i] >> 4) & 0xF];
    	cpuid_str[i * 2 + 1] = hex_chars[digest[i] & 0xF];
    }
    cpuid_str[MD5_DIGEST_LENGTH * 2] = '\0';
    return;
}

int cpuidCommand(int argc,char** argv,cJSON** j_responses){
    if (argc > 0) {
    	return HTTP_STATUS_BAD_REQUEST;
    }

    if(cpuid_str[0] == 0)
    	cpuid();

    cJSON *j_tree = cJSON_CreateObject();
    cJSON_AddStringToObject(j_tree,"cpuid",cpuid_str);

    char licence_filepath[256];
    #ifdef WIN32
    	snprintf(licence_filepath, 255, "%s\\licence", db.config->persistence_location);
    #else
    	snprintf(licence_filepath, 255, "%s/licence", db.config->persistence_location);
    #endif

	FILE *fptr = fopen(licence_filepath, "r");
	if(fptr == NULL){
		cJSON_AddNullToObject(j_tree,"licence");
	}else{
		char buf[256];
		size_t rlen = fread(buf, 1, 255, fptr);
		fclose(fptr);
		if(rlen == 0){
			cJSON_AddStringToObject(j_tree,"licence","");
		}else{
			buf[rlen] = 0;
			cJSON_AddStringToObject(j_tree,"licence",buf);
		}
	}

    *j_responses = j_tree;
	return 0;
}

void handleErrors(void) {
    ERR_print_errors_fp(stderr);
}

unsigned char *aes_cbc_decrypt(const unsigned char *ciphertext, int ciphertext_len,
                               const unsigned char *key, const unsigned char *iv,
                               int *plaintext_len) {
    EVP_CIPHER_CTX *ctx;
    unsigned char *plaintext;
    int len;
    int ret = 0;

    /* 创建并初始化上下文 */
    if(!(ctx = EVP_CIPHER_CTX_new())){
        handleErrors();
        return NULL;
    }

    /* 初始化解密操作 */
    if(1 != EVP_DecryptInit_ex(ctx, EVP_aes_128_cbc(), NULL, key, iv)){
        handleErrors();
        return NULL;
    }

    /* 分配输出缓冲区 */
    plaintext = mosquitto_malloc(ciphertext_len + EVP_CIPHER_CTX_block_size(ctx));
    if(!plaintext){
        handleErrors();
        return NULL;
    }

    /* 执行解密更新操作 */
    if(1 != EVP_DecryptUpdate(ctx, plaintext, &len, ciphertext, ciphertext_len)) {
    	mosquitto_free(plaintext);
        handleErrors();
        return NULL;
    }
    *plaintext_len = len;

    /* 完成解密过程 */
    if(1 != EVP_DecryptFinal_ex(ctx, plaintext + len, &len)) {
    	mosquitto_free(plaintext);
        handleErrors();
        return NULL;
    }
    *plaintext_len += len;

    /* 清理上下文 */
    EVP_CIPHER_CTX_free(ctx);

    return plaintext;
}

int parser_licence(char* lic,size_t lic_len ){
	unsigned char key[] = "IoTEdgeDB_secret";
	unsigned char iv[]  = "IoTEdgeDB_secret";

    int decrypted_len;
    unsigned char *decrypted = NULL;

    unsigned char* ciphertext;
    int ciphertext_len;
    if(base64_decode(lic, &ciphertext, &ciphertext_len)){
    	log__printf(NULL, MOSQ_LOG_WARNING, "parser_licence base64_decode failed!\n");
    	return 1;
    }

    /* 执行解密 */
    decrypted = aes_cbc_decrypt(ciphertext, ciphertext_len, key, iv, &decrypted_len);
    mosquitto_free(ciphertext);
    if(decrypted) {
        /* 添加字符串终止符 */
        decrypted[decrypted_len] = '\0';
        log__printf(NULL, MOSQ_LOG_WARNING, "parser_licence decrypted:%s\n",decrypted);
        if(cpuid_str[0] == 0)
        	cpuid();
        char* pos = strchr(decrypted,'/');
        int cpuid_len = pos - (char*)decrypted;
        if(pos == NULL || cpuid_len != strlen(cpuid_str) || strncmp(cpuid_str,decrypted,cpuid_len)){
        	mosquitto_free(decrypted);
        	log__printf(NULL, MOSQ_LOG_WARNING, "cpuid mismatch!\n");
        	return 1;
        }

        char* start = pos+1;
        pos = strchr(start,'/');
        if(pos == NULL){
        	mosquitto_free(decrypted);
        	log__printf(NULL, MOSQ_LOG_WARNING, "start timestamp missed!\n");
        	return 1;
        }
        errno = 0;
        char *endptr = NULL;
        unsigned long long value;
        value = strtoull(start,&endptr,10);
        if (errno == EINVAL || errno == ERANGE){
        	mosquitto_free(decrypted);
        	log__printf(NULL, MOSQ_LOG_WARNING, "start timestamp failed!\n");
            return 1;
        }
        start_timestamp = value;

        char* end = pos+1;
        errno = 0;
        endptr = NULL;
        value = strtoull(end,&endptr,10);
        if (errno == EINVAL || errno == ERANGE || !(*end != '\0' && *endptr == '\0')){
        	mosquitto_free(decrypted);
        	log__printf(NULL, MOSQ_LOG_WARNING, "end timestamp failed!\n");
            return 1;
        }
        end_timestamp = value;

        mosquitto_free(decrypted);
    } else {
    	log__printf(NULL, MOSQ_LOG_WARNING, "Decryption failed!\n");
    	return 1;
    }

    return 0;
}

int licenceCommand(int argc,char** argv,cJSON** j_responses){
    if (argc != 1) {
    	return HTTP_STATUS_BAD_REQUEST;
    }

    cJSON *j_tree = cJSON_CreateObject();
    if(parser_licence(argv[0],strlen(argv[0])) == 0){
    	char buf[64];
    	struct tm *ti = localtime(&start_timestamp);
    	strftime(buf, 64, "%Y-%m-%dT%H:%M:%S", ti);
    	cJSON_AddStringToObject(j_tree,"start",buf);

    	ti = localtime(&end_timestamp);
    	strftime(buf, 64, "%Y-%m-%dT%H:%M:%S", ti);
    	cJSON_AddStringToObject(j_tree,"end",buf);

        char licence_filepath[256];
        #ifdef WIN32
        	snprintf(licence_filepath, 255, "%s\\licence", db.config->persistence_location);
        #else
        	snprintf(licence_filepath, 255, "%s/licence", db.config->persistence_location);
        #endif
		FILE *fptr = fopen(licence_filepath, "w");
		if(fptr == NULL){
			log__printf(NULL, MOSQ_LOG_WARNING, "Warning: failed to open licence file.");
			return HTTP_STATUS_INTERNAL_SERVER_ERROR;
		}
		fwrite(argv[0], 1, strlen(argv[0]), fptr);
		fclose(fptr);
    }else{
    	cJSON_AddStringToObject(j_tree,"info","failure");
    }

    *j_responses = j_tree;
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

int check_licence(void){
	if(end_timestamp == 0){
        char licence_filepath[256];
        #ifdef WIN32
        	snprintf(licence_filepath, 255, "%s\\licence", db.config->persistence_location);
        #else
        	snprintf(licence_filepath, 255, "%s/licence", db.config->persistence_location);
        #endif
		FILE *fptr = fopen(licence_filepath, "r");
		if(fptr == NULL){
			log__printf(NULL, MOSQ_LOG_WARNING, "Warning: failed to open licence file.");
			return 1;
		}

		char* buf = mosquitto_calloc(256,1);
		size_t rlen = fread(buf, 1, 256, fptr);
		fclose(fptr);
		if(rlen == 0){
			mosquitto_free(buf);
			log__printf(NULL, MOSQ_LOG_WARNING, "Warning: licence file is empty.");
			return 1;
		}
		if(parser_licence(buf,rlen)){
			mosquitto_free(buf);
			return 1;
		}

		mosquitto_free(buf);
	}

	if(db.now_real_s < start_timestamp || db.now_real_s > end_timestamp){
		log__printf(NULL, MOSQ_LOG_WARNING, "check_licence fail: now:%lld,start:%lld,end:%lld.",db.now_real_s,start_timestamp,end_timestamp);
		return 1;
	}
	return 0;
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

	if(strcmp(cmd_name,"cpuid") && strcmp(cmd_name,"licence") && check_licence()){
		return HTTP_STATUS_UNAUTHORIZED;
	}
	log__printf(NULL, MOSQ_LOG_NOTICE, "cmd_name:%s,cmd_len:%d",cmd_name, cmd_len);

	struct router *r = NULL;
	HASH_FIND(hh, router_list, cmd_name, cmd_len, r);
	if(r == NULL)
		return HTTP_STATUS_NOT_FOUND;
	if((!slash) && body==NULL) {
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
	return 0;
}

int http_init(void){
	url_register("cpuid",cpuidCommand);
	url_register("licence",licenceCommand);
	return 0;
}
