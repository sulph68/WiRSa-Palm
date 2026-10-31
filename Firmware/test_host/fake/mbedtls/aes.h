// The part of mbedtls/aes.h that relay_proto.cpp uses; the test supplies the functions.
#pragma once
#include <stddef.h>
#include <stdint.h>
typedef struct { uint8_t rk[176]; } mbedtls_aes_context;
#define MBEDTLS_AES_ENCRYPT 1
void mbedtls_aes_init(mbedtls_aes_context *ctx);
void mbedtls_aes_free(mbedtls_aes_context *ctx);
int mbedtls_aes_setkey_enc(mbedtls_aes_context *ctx, const unsigned char *key, unsigned int keybits);
int mbedtls_aes_crypt_ecb(mbedtls_aes_context *ctx, int mode, const unsigned char in[16], unsigned char out[16]);
int mbedtls_aes_crypt_ctr(mbedtls_aes_context *ctx, size_t length, size_t *nc_off, unsigned char nonce_counter[16],
                          unsigned char stream_block[16], const unsigned char *input, unsigned char *output);
