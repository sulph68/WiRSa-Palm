// The part of mbedtls/ctr_drbg.h that relay_proto.cpp uses.
#pragma once
#include <stddef.h>
typedef struct { unsigned int state; } mbedtls_ctr_drbg_context;
void mbedtls_ctr_drbg_init(mbedtls_ctr_drbg_context *ctx);
int mbedtls_ctr_drbg_seed(mbedtls_ctr_drbg_context *ctx, int (*f_entropy)(void *, unsigned char *, size_t), void *p_entropy,
                          const unsigned char *custom, size_t len);
int mbedtls_ctr_drbg_random(void *p_rng, unsigned char *output, size_t output_len);
