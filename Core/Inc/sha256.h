#ifndef SHA256_H
#define SHA256_H

#include <stdint.h>
#include <stddef.h>

/*
 * 紧凑 SHA-256 / HMAC-SHA256（P7 信任链，bootloader 与 App 共用）
 *
 * 纯 C、无 HAL 依赖 —— 可直接在 host（fuzz/向量测试）编译。
 * 镜像签名 = HMAC-SHA256 截断 16 字节（128bit），覆盖镜像头前 20 字节
 * （magic/version/size/crc32/build_ts）+ 固件全部字节，见 ota.h v3 契约。
 */

typedef struct {
    uint32_t h[8];
    uint64_t len;            /* 已吸入的总字节数 */
    uint8_t  buf[64];
    uint8_t  buflen;
} Sha256;

void sha256_init(Sha256 *c);
void sha256_update(Sha256 *c, const uint8_t *p, size_t n);
void sha256_finish(Sha256 *c, uint8_t out[32]);

/* HMAC：密钥 ≤ 64 字节（长了按规范先哈希）。
 * 镜像校验是"20B 头前缀 + 流式固件"两段结构，故提供 begin/end 分离接口；
 * 一次性场景直接用 hmac_sha256()。 */
typedef struct {
    Sha256  inner;
    uint8_t k0[64];          /* 已处理成 64B 的密钥（ipad/opad 各自异或时用） */
    size_t  keylen;
} HmacSha256;

void hmac_sha256_begin(HmacSha256 *h, const uint8_t *key, size_t keylen);
/* 喂消息：直接调 sha256_update(&h->inner, ...) */
void hmac_sha256_end(HmacSha256 *h, uint8_t out[32]);

void hmac_sha256(const uint8_t *key, size_t keylen,
                 const uint8_t *msg, size_t msglen, uint8_t out[32]);

#endif
