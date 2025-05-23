/*
Copyright (c) 2020 Roger Light <roger@atchoo.org>

All rights reserved. This program and the accompanying materials
are made available under the terms of the Eclipse Public License 2.0
and Eclipse Distribution License v1.0 which accompany this distribution.

The Eclipse Public License is available at
   https://www.eclipse.org/legal/epl-2.0/
and the Eclipse Distribution License is available at
  http://www.eclipse.org/org/documents/edl-v10.php.

SPDX-License-Identifier: EPL-2.0 OR BSD-3-Clause

Contributors:
   Roger Light - initial implementation and documentation.
*/

#include "config.h"
#include "sha256.h"
#include "md5.h"
#include <stdio.h>
#include <string.h>

#include "mosquitto.h"
#include "mosquitto_broker.h"
#include "dynamic_security.h"

#define SHA256_BLOCK_SIZE 32
#define HASH_PASSWORD_LEN (SHA256_BLOCK_SIZE*2)

void handle_client_auth(char* sn,char* clientid);

static int memcmp_const(const void *a, const void *b, size_t len)
{
	size_t i;
	int rc = 0;

	if(!a || !b) return 1;

	for(i=0; i<len; i++){
		rc |= ((char *)a)[i] ^ ((char *)b)[i];
	}
	return rc;
}

int check_hmac_sha256(const char* clinet_id, const char* username, const char* password){
    /* This implements SHA256-HMAC. */
    unsigned char digest[SHA256_BLOCK_SIZE];
    unsigned char kxor[64];
    unsigned char key[64];

    SHA256_CTX ctx;
    char *cset = "0123456789abcdef";

    if(strlen(password) != HASH_PASSWORD_LEN)
    	return -1;

    char* p = strchr(clinet_id,'|');
    if(p == NULL)
    	return -1;
    int client_id_len = p - clinet_id;
    int timestamp_len = strlen(clinet_id) - client_id_len - 47;

    char msg[256];
    snprintf(msg,256,"clientId%.*sdevicename%sproductkey%stimestamp%.*s",client_id_len,clinet_id,username,username,timestamp_len,p+46);

    md5_byte_t md5_buf[16];
	md5_state_t pms;
	md5_init(&pms);
	md5_append(&pms, (const md5_byte_t *)username, (int)strlen(username));
	md5_append(&pms, (const md5_byte_t *)"Luomi@4321", 10);
	md5_finish(&pms, md5_buf);
    char secret[32];
    char *cset_upper = "0123456789ABCDEF";
    for (int j = 0; j < 16; j++) {
    	secret[j*2] = cset_upper[((md5_buf[j]&0xF0)>>4)];
    	secret[j*2+1] = cset_upper[(md5_buf[j]&0xF)];
    }
    size_t key_len = 32;

    memset(key,0,64);
    if(key_len > 64){
        sha256_init(&ctx);
        sha256_update(&ctx,secret,key_len);
        sha256_final(&ctx,key);
    }else{
    	memcpy(key,secret,key_len);
    }

    /* IKEY: key xored with 0x36. */
    memcpy(kxor,key,sizeof(key));
    for (unsigned int i = 0; i < sizeof(kxor); i++) kxor[i] ^= 0x36;

    /* Obtain HASH(IKEY||MESSAGE). */
    sha256_init(&ctx);
    sha256_update(&ctx,kxor,sizeof(kxor));
    sha256_update(&ctx,(unsigned char*)msg,strlen(msg));
    sha256_final(&ctx,digest);

    /* OKEY: key xored with 0x5c. */
    memcpy(kxor,key,sizeof(key));
    for (unsigned int i = 0; i < sizeof(kxor); i++) kxor[i] ^= 0x5C;

    /* Obtain HASH(OKEY || HASH(IKEY||MESSAGE)). */
    sha256_init(&ctx);
    sha256_update(&ctx,kxor,sizeof(kxor));
    sha256_update(&ctx,digest,SHA256_BLOCK_SIZE);
    sha256_final(&ctx,digest);

    char hex[HASH_PASSWORD_LEN];
    for (int j = 0; j < SHA256_BLOCK_SIZE; j++) {
        hex[j*2] = cset[((digest[j]&0xF0)>>4)];
        hex[j*2+1] = cset[(digest[j]&0xF)];
    }

    if(memcmp_const(password, hex, SHA256_BLOCK_SIZE)){
    	return -1;
    }

    return 0;
}


int mqtt_basic_auth_callback(int event, void *event_data, void *userdata)
{
	struct mosquitto_evt_basic_auth *ed = event_data;
	struct dynsec__client *client;
	const char *clientid;

	UNUSED(event);
	UNUSED(userdata);

	if(ed->username == NULL || ed->password == NULL) return MOSQ_ERR_AUTH;
    char* username = strchr(ed->username, '|');
    if(username == NULL) return MOSQ_ERR_AUTH;
    username += 1;
    clientid = mosquitto_client_id(ed->client);
	if(check_hmac_sha256(clientid,username,ed->password) == 0){
		handle_client_auth(username,clientid);
		return MOSQ_ERR_SUCCESS;
	}else{
		return MOSQ_ERR_AUTH;
	}
}
