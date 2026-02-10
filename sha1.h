/*
 * SHA-1 in C
 * By Steve Reid <sreid@sea-to-sky.net>
 * 100% Public Domain
 *
 * -----------------
 * Modified 07/2002 by Ralph Giles <giles@ghostscript.com>
 * Still 100% public domain
 * modified for use with stdint types, code cleanup.
 *
 * HMAC-SHA1 implementation added, also public domain.
 */

#ifndef SHA1_H
#define SHA1_H

#include <stdint.h>
#include <stddef.h>

#define SHA1_DIGEST_SIZE 20
#define SHA1_BLOCK_SIZE  64

typedef struct {
    uint32_t state[5];
    uint32_t count[2];
    unsigned char buffer[64];
} SHA1_CTX;

void sha1_init(SHA1_CTX *context);
void sha1_update(SHA1_CTX *context, const unsigned char *data, size_t len);
void sha1_final(SHA1_CTX *context, unsigned char digest[SHA1_DIGEST_SIZE]);

void hmac_sha1(const unsigned char *key, size_t key_len,
               const unsigned char *data, size_t data_len,
               unsigned char out[SHA1_DIGEST_SIZE]);

#endif /* SHA1_H */
