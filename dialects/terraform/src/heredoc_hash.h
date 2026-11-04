#ifndef HCL_HEREDOC_HASH_H
#define HCL_HEREDOC_HASH_H

/*
 * Private SHA-256 for bounded heredoc identifiers. Adapted from sha-256.c/.h:
 * https://github.com/amosnier/sha-2/tree/565f65009bdd98267361b17d50cddd7c9beb3e6c
 * Algorithm unchanged; declarations made private and prefixed, comments reduced.
 * SPDX-License-Identifier: 0BSD
 *
 * Copyright 2021 Alain Mosnier
 *
 * Permission to use, copy, modify, and/or distribute this software for any
 * purpose with or without fee is hereby granted.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES WITH
 * REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF MERCHANTABILITY AND
 * FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY SPECIAL, DIRECT,
 * INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM
 * LOSS OF USE, DATA OR PROFITS, WHETHER IN AN ACTION OF CONTRACT, NEGLIGENCE OR
 * OTHER TORTIOUS ACTION, ARISING OUT OF OR IN CONNECTION WITH THE USE OR
 * PERFORMANCE OF THIS SOFTWARE.
 */

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define HCL_HEREDOC_HASH_SIZE 32
#define HCL_HEREDOC_HASH_CHUNK_SIZE 64

struct HclHeredocSha256 {
    uint8_t *hash;
    uint8_t chunk[HCL_HEREDOC_HASH_CHUNK_SIZE];
    uint8_t *chunk_pos;
    size_t space_left;
    uint64_t total_len;
    uint32_t h[8];
};

#define HCL_HEREDOC_HASH_LENGTH_SIZE 8

static inline uint32_t hcl_heredoc_hash_rotate(uint32_t value, unsigned int count)
{

    return value >> count | value << (32 - count);
}

static inline void hcl_heredoc_hash_chunk(uint32_t *h, const uint8_t *p)
{
    unsigned i, j;
    uint32_t ah[8];

    for (i = 0; i < 8; i++)
        ah[i] = h[i];

    // A rolling schedule keeps only sixteen words on the stack.
    uint32_t w[16];

    for (i = 0; i < 4; i++) {
        for (j = 0; j < 16; j++) {
            if (i == 0) {
                w[j] =
                    (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | (uint32_t)p[3];
                p += 4;
            } else {

                const uint32_t s0 = hcl_heredoc_hash_rotate(w[(j + 1) & 0xf], 7) ^ hcl_heredoc_hash_rotate(w[(j + 1) & 0xf], 18) ^
                            (w[(j + 1) & 0xf] >> 3);
                const uint32_t s1 = hcl_heredoc_hash_rotate(w[(j + 14) & 0xf], 17) ^
                            hcl_heredoc_hash_rotate(w[(j + 14) & 0xf], 19) ^ (w[(j + 14) & 0xf] >> 10);
                w[j] = w[j] + s0 + w[(j + 9) & 0xf] + s1;
            }
            const uint32_t s1 = hcl_heredoc_hash_rotate(ah[4], 6) ^ hcl_heredoc_hash_rotate(ah[4], 11) ^ hcl_heredoc_hash_rotate(ah[4], 25);
            const uint32_t ch = (ah[4] & ah[5]) ^ (~ah[4] & ah[6]);

            static const uint32_t k[] = {
                0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4,
                0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe,
                0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f,
                0x4a7484aa, 0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7,
                0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc,
                0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b,
                0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116,
                0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
                0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7,
                0xc67178f2};

            const uint32_t temp1 = ah[7] + s1 + ch + k[i << 4 | j] + w[j];
            const uint32_t s0 = hcl_heredoc_hash_rotate(ah[0], 2) ^ hcl_heredoc_hash_rotate(ah[0], 13) ^ hcl_heredoc_hash_rotate(ah[0], 22);
            const uint32_t maj = (ah[0] & ah[1]) ^ (ah[0] & ah[2]) ^ (ah[1] & ah[2]);
            const uint32_t temp2 = s0 + maj;

            ah[7] = ah[6];
            ah[6] = ah[5];
            ah[5] = ah[4];
            ah[4] = ah[3] + temp1;
            ah[3] = ah[2];
            ah[2] = ah[1];
            ah[1] = ah[0];
            ah[0] = temp1 + temp2;
        }
    }

    for (i = 0; i < 8; i++)
        h[i] += ah[i];
}

static void hcl_heredoc_hash_init(struct HclHeredocSha256 *sha_256, uint8_t hash[HCL_HEREDOC_HASH_SIZE])
{
    sha_256->hash = hash;
    sha_256->chunk_pos = sha_256->chunk;
    sha_256->space_left = HCL_HEREDOC_HASH_CHUNK_SIZE;
    sha_256->total_len = 0;

    sha_256->h[0] = 0x6a09e667;
    sha_256->h[1] = 0xbb67ae85;
    sha_256->h[2] = 0x3c6ef372;
    sha_256->h[3] = 0xa54ff53a;
    sha_256->h[4] = 0x510e527f;
    sha_256->h[5] = 0x9b05688c;
    sha_256->h[6] = 0x1f83d9ab;
    sha_256->h[7] = 0x5be0cd19;
}

static void hcl_heredoc_hash_write(struct HclHeredocSha256 *sha_256, const void *data, size_t len)
{
    sha_256->total_len += len;

    const uint8_t *p = (const uint8_t *)data;

    while (len > 0) {

        if (sha_256->space_left == HCL_HEREDOC_HASH_CHUNK_SIZE && len >= HCL_HEREDOC_HASH_CHUNK_SIZE) {
            hcl_heredoc_hash_chunk(sha_256->h, p);
            len -= HCL_HEREDOC_HASH_CHUNK_SIZE;
            p += HCL_HEREDOC_HASH_CHUNK_SIZE;
            continue;
        }

        const size_t consumed_len = len < sha_256->space_left ? len : sha_256->space_left;
        memcpy(sha_256->chunk_pos, p, consumed_len);
        sha_256->space_left -= consumed_len;
        len -= consumed_len;
        p += consumed_len;
        if (sha_256->space_left == 0) {
            hcl_heredoc_hash_chunk(sha_256->h, sha_256->chunk);
            sha_256->chunk_pos = sha_256->chunk;
            sha_256->space_left = HCL_HEREDOC_HASH_CHUNK_SIZE;
        } else {
            sha_256->chunk_pos += consumed_len;
        }
    }
}

static uint8_t *hcl_heredoc_hash_close(struct HclHeredocSha256 *sha_256)
{
    uint8_t *pos = sha_256->chunk_pos;
    size_t space_left = sha_256->space_left;
    uint32_t *const h = sha_256->h;

    *pos++ = 0x80;
    --space_left;

    if (space_left < HCL_HEREDOC_HASH_LENGTH_SIZE) {
        memset(pos, 0x00, space_left);
        hcl_heredoc_hash_chunk(h, sha_256->chunk);
        pos = sha_256->chunk;
        space_left = HCL_HEREDOC_HASH_CHUNK_SIZE;
    }
    const size_t left = space_left - HCL_HEREDOC_HASH_LENGTH_SIZE;
    memset(pos, 0x00, left);
    pos += left;
    uint64_t len = sha_256->total_len;
    pos[7] = (uint8_t)(len << 3);
    len >>= 5;
    int i;
    for (i = 6; i >= 0; --i) {
        pos[i] = (uint8_t)len;
        len >>= 8;
    }
    hcl_heredoc_hash_chunk(h, sha_256->chunk);

    int j;
    uint8_t *const hash = sha_256->hash;
    for (i = 0, j = 0; i < 8; i++) {
        hash[j++] = (uint8_t)(h[i] >> 24);
        hash[j++] = (uint8_t)(h[i] >> 16);
        hash[j++] = (uint8_t)(h[i] >> 8);
        hash[j++] = (uint8_t)h[i];
    }
    return sha_256->hash;
}

static void hcl_heredoc_hash(uint8_t hash[HCL_HEREDOC_HASH_SIZE], const void *input, size_t len)
{
    struct HclHeredocSha256 sha_256;
    hcl_heredoc_hash_init(&sha_256, hash);
    hcl_heredoc_hash_write(&sha_256, input, len);
    (void)hcl_heredoc_hash_close(&sha_256);
}

#endif
