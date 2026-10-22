#pragma once
#include <stddef.h>
#include <stdint.h>
// Deterministic checksum stand-in: tests sequencing/policy, NOT SHA primitives.
// SDK build uses the actual mbedTLS implementation, never this header.
typedef struct {uint8_t sum[32]; size_t used;} mbedtls_sha256_context;
void mbedtls_sha256_init(mbedtls_sha256_context *);
void mbedtls_sha256_free(mbedtls_sha256_context *);
int mbedtls_sha256_starts(mbedtls_sha256_context *, int);
int mbedtls_sha256_update(mbedtls_sha256_context *, const unsigned char *, size_t);
int mbedtls_sha256_finish(mbedtls_sha256_context *, unsigned char[32]);
