extern "C" {
#include "tweetnacl/tweetnacl.h"
#include "nacl_box.h"
#include "randombytes.h"
#include "reencrypt_u.h"
#include "sgx_urts.h"
#include "serialize.h"
}
#include <stdio.h>
#include <stdlib.h>

#ifdef _MSC_VER
#include <tchar.h>
#define ENCLAVE_FILE _T("reencrypt.signed.dll")
#else
#define ENCLAVE_FILE "reencrypt.signed.so"
#endif

#define HEXPRINT(buffer, bufferlen) do { \
	for(uint32_t i=0; i<bufferlen; printf("%02x", buffer[i++])); \
}while(0)


typedef struct context
{
	sgx_enclave_id_t eid;
	// client nacl keypair
	unsigned char pk[crypto_box_PUBLICKEYBYTES], sk[crypto_box_SECRETKEYBYTES];
	// enclave nacl public key
	unsigned char enclavepk[crypto_box_PUBLICKEYBYTES];
} context;

uint8_t create_enclave(context &ctx)
{
	sgx_status_t ret = SGX_SUCCESS;
	sgx_launch_token_t token = {0};
	int token_updated = 0;

	if(sgx_create_enclave(ENCLAVE_FILE, SGX_DEBUG_FLAG, &token, &token_updated, &ctx.eid, NULL) != SGX_SUCCESS)
	{
		return 1;
	}
	return 0;
}

uint32_t destroy_enclave(const context &ctx)
{
	return (sgx_destroy_enclave(ctx.eid) == SGX_SUCCESS ? 0 : 1);
}


int register_key(const context &ctx, uint8_t *key,
				 size_t keylen, uint64_t expiration_date,
				 client_id *clients, size_t nclients,
				 policy_type keys_from_policy, key_id *keys_from,
				 size_t nkeys_from, policy_type keys_to_policy,
				 key_id *keys_to, size_t nkeys_to, key_id new_key) {
	uint8_t *keyblob=NULL;
	size_t keybloblen;
	uint8_t *request_p = NULL;
	size_t request_plen;
	// new key id
	uint8_t *keytmp = NULL;
	size_t keytmplen;
	// to parse output
	size_t keyresponselen;
	uint8_t *keyresponse = NULL;

	// nacl nonce
	unsigned char nonce[crypto_box_NONCEBYTES];
	int error;

	struct keydata_t *k=(struct keydata_t*)malloc(sizeof(struct keydata_t));
	memset(k, 0, sizeof(struct keydata_t));

	memcpy(k->key, key, keylen);
	k->expiration_date=expiration_date;

	k->n_authorized_clients=nclients;
	k->authorized_clients=(client_id *) malloc(nclients * sizeof(client_id));
	if(k->authorized_clients == NULL)
		goto err;
	for(size_t i=0; i<nclients; ++i) {
		memcpy(&k->authorized_clients[i], clients[i],
			   sizeof(client_id));
	}

	k->policy_from = keys_from_policy;
	if(keys_from_policy == POLICY_LIST) {
		k->n_keys_from = nkeys_from;
		k->keys_from = (key_id *)malloc(nkeys_from * sizeof(key_id));
		if(k->keys_from == NULL)
			goto err;
		for(size_t i=0; i<nkeys_from; ++i) {
			memcpy(&k->keys_from[i], keys_from[i], sizeof(key_id));
		}
	}

	k->policy_to = keys_to_policy;
	if(keys_to_policy == POLICY_LIST) {
		k->n_keys_to = nkeys_to;
		k->keys_to = (key_id *)malloc(nkeys_to * sizeof(key_id));
		if(k->keys_to == NULL)
			goto err;
		for(size_t i=0; i<nkeys_to; ++i) {
			memcpy(&k->keys_to[i], keys_to[i], sizeof(key_id));
		}
	}
	// the key structure is ready - serialize it
	key_serialize(k, &keyblob, &keybloblen);
	keyresponselen=0x64;
	keyresponse=(uint8_t*)malloc(keyresponselen);
	// generate a nonce and encrypt register_key
	randombytes(nonce, crypto_box_NONCEBYTES);
	box(ctx.enclavepk, ctx.sk, nonce, keyblob, keybloblen, &request_p, &request_plen);
	register_key(ctx.eid, &error, (client_id*)ctx.pk, request_p, request_plen,keyresponse, &keyresponselen);
	if(unbox(ctx.enclavepk, ctx.sk, keyresponse, keyresponselen, &keytmp, &keytmplen))
	{
		goto err;
	}
	// output the response
	memcpy(new_key, keytmp, sizeof(key_id));
	free(keytmp);
	free(request_p);
	free(keyblob);
	free(keyresponse);
	return 0;
err:
	free(keytmp);
	free(request_p);
	free(keyblob);
	free(keyresponse);
	return 1;
}


int reencrypt(const context &ctx, key_id key_in, key_id key_out,
			  uint8_t *c, uint32_t clen, uint8_t **c2, uint32_t *c2len)
{
	// temp structure to store reencrypt requests
	struct request_t {
		key_id key_in;
		key_id key_out;
		uint8_t c[];
	};
	struct request_t *request = (struct request_t *)malloc(512);
	// to store boxed request
	uint8_t *request_b = NULL;
	size_t request_blen;
	// to store the boxed response
	size_t response_blen = 512;
	uint8_t response_b[512];
	// to store the unboxed response
	uint8_t *response = NULL;
	size_t responselen;
	// nacl nonce
	unsigned char nonce[crypto_box_NONCEBYTES];
	// reencrypt status
	int error;
	// construct request
	memcpy(request->key_in, key_in, sizeof(key_id));
	memcpy(request->key_out, key_out, sizeof(key_id));
	memcpy(request->c, c, clen);
	// get a random nonce
	randombytes(nonce, crypto_box_NONCEBYTES);
	// and box the request
	if(box(ctx.enclavepk, ctx.sk, nonce, (uint8_t*) request, 2*sizeof(key_id) + clen, &request_b, &request_blen))
	{
		goto err;
	}
	if(reencrypt(ctx.eid, &error, (client_id*)ctx.pk, request_b, request_blen, response_b, &response_blen) != SGX_SUCCESS){
		goto err;
	}
	if(error)
	{
		goto err;
	}
	// the reencrypt request was successfully processed 
	// unbox
	if(unbox(ctx.enclavepk, ctx.sk, response_b, response_blen, &response, &responselen)) {
		goto err;
	}
	// output result
	*c2 = response;
	*c2len = responselen;
	free(request);
	return 0;
err:
	free(request);
	return 1;
}

int main(int argc, char* argv[])
{
	context ctx;
	// key used to encrypt the test payload
	uint8_t key1[16] = {
		0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
		0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f
	};
	// test payload
	// ciphertext for key1:"test"
	const uint32_t payloadlen=32;
	uint8_t payload[payloadlen] = {
		0xac, 0x3c, 0x75, 0x6d, 0x0a, 0x71, 0x7b, 0x5d,
		0x98, 0x77, 0xf1, 0x6b, 0x7a, 0xab, 0x79, 0x5b,
		0x2f, 0x64, 0xe2, 0x57, 0xe1, 0xda, 0xf5, 0x84,
		0xda, 0x2e, 0x5d, 0xf7, 0xf1, 0x2d, 0xdb, 0x36
	};
	// test key_id
	key_id new_key;

	// register key test values
	client_id clients[2];

	uint8_t *response = NULL;
	uint32_t responselen;
	int error;
	struct keydata_t *k=(struct keydata_t*)malloc(sizeof(struct keydata_t));

	// create enclave
	if(!create_enclave(ctx)){
		printf("Enclave create success!\n");
	}
	else{
		goto err;
	}
	// generate client nacl keypair
	crypto_box_keypair(ctx.pk, ctx.sk); 
	printf("Client keypair created!\n");
	// try to unseal enclave keypair
	unseal_keypair(ctx.eid, &error, ctx.enclavepk);
	if(!error){
		printf("unseal success!\n");
	}
	if(error) { // wasn't able to unseal then  generate a new pair
		generate_keypair(ctx.eid, &error, ctx.enclavepk);
		if(!error){
			printf("Enclave keypair created!\n");
		}
		else{
			goto err;
		}
		// seal the new keypair
		seal_keypair(ctx.eid, &error);
		if(!error){
			printf("Enclave seal_keypair success!\n");
		}
		else{
			goto err;
		}
	}
	// authorize our client:Simulation of certification
	memcpy(&clients[0], ctx.pk, sizeof(client_id));
	// register key:generate key id
	if(!register_key(ctx, key1, 16, (uint64_t)2462838400,
					clients, 1, POLICY_ALL, NULL, 0,
					POLICY_ALL, NULL, 0, new_key)){
		printf("key_id created!\n");
	}
	else{
		goto err;
	}

	/*	new_key = {
		0xE9, 0x99, 0xDE, 0xD9, 0x5B, 0x72, 0xD9, 0x1E,
		0x29, 0x55, 0x52, 0xB6, 0xB1, 0x35, 0xF0, 0x17};*/

	// reencrypt test payload from new_key(key1)to new_key(key2)
	if(!reencrypt(ctx, new_key, new_key, payload, payloadlen, &response, &responselen)){
		// printf("plaintext:");
		printf("Reencrypt cipher:");
		HEXPRINT(response,responselen);
		printf("\n");
		printf("Reencrypt success!\n");
	}
	else{
		goto err;
	}

	sgx_destroy_enclave(ctx.eid);
	return 0;
err:
	sgx_destroy_enclave(ctx.eid);
	return 1;
}

