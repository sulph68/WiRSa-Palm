// The part of mbedtls/rsa.h that relay_proto.cpp uses; the test supplies a stand-in (see relay_proto_test.cpp).
#pragma once
#include <stddef.h>
typedef struct { int unused; } mbedtls_rsa_context;
#define MBEDTLS_RSA_PKCS_V15 0
#define MBEDTLS_RSA_PRIVATE 1
void mbedtls_rsa_init(mbedtls_rsa_context *ctx, int padding, int hash_id);
void mbedtls_rsa_free(mbedtls_rsa_context *ctx);
int mbedtls_rsa_gen_key(mbedtls_rsa_context *ctx, int (*f_rng)(void *, unsigned char *, size_t), void *p_rng, unsigned int nbits, int exponent);
size_t mbedtls_rsa_get_len(const mbedtls_rsa_context *ctx);
int mbedtls_rsa_export_raw(const mbedtls_rsa_context *ctx, unsigned char *N, size_t N_len, unsigned char *P, size_t P_len,
                           unsigned char *Q, size_t Q_len, unsigned char *D, size_t D_len, unsigned char *E, size_t E_len);
int mbedtls_rsa_pkcs1_decrypt(mbedtls_rsa_context *ctx, int (*f_rng)(void *, unsigned char *, size_t), void *p_rng, int mode,
                              size_t *olen, const unsigned char *input, unsigned char *output, size_t output_max_len);
