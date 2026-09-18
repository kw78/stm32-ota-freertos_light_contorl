/*
 * SHA-256 / HMAC-SHA256 —— FIPS 180-4 / RFC 2104 参考实现的紧凑改写
 * （P7 信任链；bootloader 与 App 共用，host 测试同源编译）
 */

#include "sha256.h"
#include <string.h>

static const uint32_t K[64] = {
    0x428A2F98, 0x71374491, 0xB5C0FBCF, 0xE9B5DBA5, 0x3956C25B, 0x59F111F1,
    0x923F82A4, 0xAB1C5ED5, 0xD807AA98, 0x12835B01, 0x243185BE, 0x550C7DC3,
    0x72BE5D74, 0x80DEB1FE, 0x9BDC06A7, 0xC19BF174, 0xE49B69C1, 0xEFBE4786,
    0x0FC19DC6, 0x240CA1CC, 0x2DE92C6F, 0x4A7484AA, 0x5CB0A9DC, 0x76F988DA,
    0x983E5152, 0xA831C66D, 0xB00327C8, 0xBF597FC7, 0xC6E00BF3, 0xD5A79147,
    0x06CA6351, 0x14292967, 0x27B70A85, 0x2E1B2138, 0x4D2C6DFC, 0x53380D13,
    0x650A7354, 0x766A0ABB, 0x81C2C92E, 0x92722C85, 0xA2BFE8A1, 0xA81A664B,
    0xC24B8B70, 0xC76C51A3, 0xD192E819, 0xD6990624, 0xF40E3585, 0x106AA070,
    0x19A4C116, 0x1E376C08, 0x2748774C, 0x34B0BCB5, 0x391C0CB3, 0x4ED8AA4A,
    0x5B9CCA4F, 0x682E6FF3, 0x748F82EE, 0x78A5636F, 0x84C87814, 0x8CC70208,
    0x90BEFFFA, 0xA4506CEB, 0xBEF9A3F7, 0xC67178F2,
};

static uint32_t ror(uint32_t x, unsigned n) { return (x >> n) | (x << (32 - n)); }

static void sha256_block(uint32_t h[8], const uint8_t p[64])
{
    uint32_t w[64];
    for (int i = 0; i < 16; i++)
        w[i] = ((uint32_t)p[4 * i] << 24) | ((uint32_t)p[4 * i + 1] << 16)
             | ((uint32_t)p[4 * i + 2] << 8) | p[4 * i + 3];
    for (int i = 16; i < 64; i++) {
        uint32_t s0 = ror(w[i - 15], 7) ^ ror(w[i - 15], 18) ^ (w[i - 15] >> 3);
        uint32_t s1 = ror(w[i - 2], 17) ^ ror(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    uint32_t a = h[0], b = h[1], c = h[2], d = h[3];
    uint32_t e = h[4], f = h[5], g = h[6], hh = h[7];
    for (int i = 0; i < 64; i++) {
        uint32_t S1 = ror(e, 6) ^ ror(e, 11) ^ ror(e, 25);
        uint32_t ch = (e & f) ^ (~e & g);
        uint32_t t1 = hh + S1 + ch + K[i] + w[i];
        uint32_t S0 = ror(a, 2) ^ ror(a, 13) ^ ror(a, 22);
        uint32_t mj = (a & b) ^ (a & c) ^ (b & c);
        uint32_t t2 = S0 + mj;
        hh = g; g = f; f = e; e = d + t1;
        d = c; c = b; b = a; a = t1 + t2;
    }
    h[0] += a; h[1] += b; h[2] += c; h[3] += d;
    h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
}

void sha256_init(Sha256 *c)
{
    static const uint32_t IV[8] = {0x6A09E667, 0xBB67AE85, 0x3C6EF372,
                                   0xA54FF53A, 0x510E527F, 0x9B05688C,
                                   0x1F83D9AB, 0x5BE0CD19};
    memcpy(c->h, IV, sizeof(IV));
    c->len = 0;
    c->buflen = 0;
}

void sha256_update(Sha256 *c, const uint8_t *p, size_t n)
{
    c->len += n;
    while (n) {
        size_t take = 64 - c->buflen;
        if (take > n) take = n;
        memcpy(c->buf + c->buflen, p, take);
        c->buflen += (uint8_t)take;
        p += take;
        n -= take;
        if (c->buflen == 64) {
            sha256_block(c->h, c->buf);
            c->buflen = 0;
        }
    }
}

void sha256_finish(Sha256 *c, uint8_t out[32])
{
    uint64_t bits = c->len * 8;
    uint8_t pad = 0x80;
    sha256_update(c, &pad, 1);
    uint8_t z = 0;
    while (c->buflen != 56)
        sha256_update(c, &z, 1);
    uint8_t lenb[8];
    for (int i = 0; i < 8; i++)
        lenb[i] = (uint8_t)(bits >> (56 - 8 * i));
    sha256_update(c, lenb, 8);       /* len 计数已在开头累加，此处 padding 不影响 */
    for (int i = 0; i < 8; i++) {
        out[4 * i]     = (uint8_t)(c->h[i] >> 24);
        out[4 * i + 1] = (uint8_t)(c->h[i] >> 16);
        out[4 * i + 2] = (uint8_t)(c->h[i] >> 8);
        out[4 * i + 3] = (uint8_t)c->h[i];
    }
}

/* ---- HMAC（RFC 2104）---- */

static void hmac_k0(const uint8_t *key, size_t keylen, uint8_t k0[64])
{
    if (keylen > 64) {               /* 规范：长密钥先哈希 */
        Sha256 t;
        sha256_init(&t);
        sha256_update(&t, key, keylen);
        sha256_finish(&t, k0);
        memset(k0 + 32, 0, 32);
    } else {
        memcpy(k0, key, keylen);
        memset(k0 + keylen, 0, 64 - keylen);
    }
}

void hmac_sha256_begin(HmacSha256 *h, const uint8_t *key, size_t keylen)
{
    hmac_k0(key, keylen, h->k0);
    h->keylen = keylen;
    sha256_init(&h->inner);
    uint8_t ip[64];
    for (int i = 0; i < 64; i++) ip[i] = h->k0[i] ^ 0x36;
    sha256_update(&h->inner, ip, 64);
}

void hmac_sha256_end(HmacSha256 *h, uint8_t out[32])
{
    uint8_t inner_digest[32];
    sha256_finish(&h->inner, inner_digest);
    Sha256 outer;
    sha256_init(&outer);
    uint8_t op[64];
    for (int i = 0; i < 64; i++) op[i] = h->k0[i] ^ 0x5C;
    sha256_update(&outer, op, 64);
    sha256_update(&outer, inner_digest, 32);
    sha256_finish(&outer, out);
}

void hmac_sha256(const uint8_t *key, size_t keylen,
                 const uint8_t *msg, size_t msglen, uint8_t out[32])
{
    HmacSha256 h;
    hmac_sha256_begin(&h, key, keylen);
    sha256_update(&h.inner, msg, msglen);
    hmac_sha256_end(&h, out);
}
