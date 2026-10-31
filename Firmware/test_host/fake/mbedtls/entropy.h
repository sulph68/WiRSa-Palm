// The part of mbedtls/entropy.h that relay_proto.cpp uses.
#pragma once
#include <stddef.h>
typedef struct { int unused; } mbedtls_entropy_context;
void mbedtls_entropy_init(mbedtls_entropy_context *ctx);
int mbedtls_entropy_func(void *data, unsigned char *output, size_t len);
