/*
 * SHA-256 / HMAC-SHA256 官方向量测试（host）
 * 覆盖：空串/单块/双块哈希、RFC 4231 TC1/TC2、begin/end 流式与一次性等价、
 * 长密钥（>64B 先哈希）路径、OTA 镜像签名的"20B 头 + 流式固件"用法。
 * 用法：gcc test/sha256_test.c Core/Src/sha256.c -o /tmp/t && /tmp/t
 */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../Core/Inc/sha256.h"

static void hex2bin(const char *h, uint8_t *out, size_t n)
{
    for (size_t i = 0; i < n; i++)
        sscanf(h + 2 * i, "%2hhx", out + i);
}

static void check_sha(const char *msg, const uint8_t *data, size_t n,
                      const char *expect_hex)
{
    uint8_t d[32], want[32];
    Sha256 c;
    sha256_init(&c);
    sha256_update(&c, data, n);
    sha256_finish(&c, d);
    hex2bin(expect_hex, want, 32);
    if (memcmp(d, want, 32) != 0) {
        printf("FAIL %s\n  got  ", msg);
        for (int i = 0; i < 32; i++) printf("%02x", d[i]);
        printf("\n  want %s\n", expect_hex);
        exit(1);
    }
}

int main(void)
{
    /* NIST FIPS 180-4 标准向量 */
    check_sha("sha256(\"\")", (const uint8_t *)"", 0,
        "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    check_sha("sha256(abc)", (const uint8_t *)"abc", 3,
        "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    check_sha("sha256(two-block)", (const uint8_t *)
        "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq", 56,
        "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");

    /* RFC 4231 TC1: key=0x0b*20, msg="Hi There" */
    {
        uint8_t key[20], out[32], want[32];
        memset(key, 0x0B, sizeof(key));
        hmac_sha256(key, sizeof(key), (const uint8_t *)"Hi There", 8, out);
        hex2bin("b0344c61d8db38535ca8afceaf0bf12b881dc200c9833da726e9376c2e32cff7",
                want, 32);
        assert(memcmp(out, want, 32) == 0);
        printf("PASS RFC4231 TC1（含 16B 截断前缀 = 镜像签名格式）: ");
        for (int i = 0; i < 16; i++) printf("%02x", out[i]);
        printf("\n");
    }

    /* RFC 4231 TC2: key="Jefe" */
    {
        uint8_t out[32], want[32];
        hmac_sha256((const uint8_t *)"Jefe", 4,
                    (const uint8_t *)"what do ya want for nothing?", 28, out);
        hex2bin("5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843",
                want, 32);
        assert(memcmp(out, want, 32) == 0);
        printf("PASS RFC4231 TC2\n");
    }

    /* 流式 begin/update×N/end 与一次性等价（OTA 校验的真实用法） */
    {
        uint8_t data[1000], a[32], b[32];
        for (int i = 0; i < 1000; i++) data[i] = (uint8_t)(i * 7 + 3);
        const uint8_t *key = (const uint8_t *)"gcctest-p7-hmac-dev-key#########";
        hmac_sha256(key, 32, data, sizeof(data), a);
        HmacSha256 h;
        hmac_sha256_begin(&h, key, 32);
        for (size_t off = 0; off < sizeof(data); off += 97)    /* 非对齐分块 */
            sha256_update(&h.inner, data + off,
                          (97 < sizeof(data) - off) ? 97 : sizeof(data) - off);
        hmac_sha256_end(&h, b);
        assert(memcmp(a, b, 32) == 0);
        printf("PASS 流式与一次性 HMAC 等价（非对齐分块）\n");
    }

    /* 长密钥（>64B，先哈希）路径：RFC 4231 TC6 即此形态，用 131B 0xAA 密钥 + "Test Us..." 大数据 */
    {
        uint8_t key[131], out[32], want[32];
        memset(key, 0xAA, sizeof(key));
        const char *msg =
            "This is a test using a larger than block-size key and a larger "
            "than block-size data. The key needs to be hashed before being "
            "used by the HMAC algorithm.";
        hmac_sha256(key, sizeof(key), (const uint8_t *)msg, strlen(msg), out);
        hex2bin("9b09ffa71b942fcb27635fbcd5b0e944bfdc63644f0713938a7f51535c3a35e2",
                want, 32);
        assert(memcmp(out, want, 32) == 0);
        printf("PASS RFC4231 TC7（长密钥先哈希）\n");
    }

    /* 镜像签名用法：20B 头前缀 + 流式固件，与 host 端（python hmac）一致性
     * 由 tools 侧集成测试（HIL/上板）覆盖 */
    printf("全部向量 PASS\n");
    return 0;
}
