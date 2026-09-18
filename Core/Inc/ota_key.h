#ifndef OTA_KEY_H
#define OTA_KEY_H

#include <stdint.h>

/*
 * P7 信任链：HMAC 签名密钥（32 字节，十六进制字符串形式）
 *
 * 默认值是【公开的开发密钥】——CI/演示开箱即用，不提供任何真实防护。
 * 生产/自有密钥：python3 tools/ota_tool.py genkey 生成后，用
 *   cmake -DOTA_HMAC_KEY_HEX=<64位hex> ...
 * 注入固件构建，并给上位机 --key 指定同一密钥文件。
 *
 * 威胁模型（README「信任链」一节同文）：
 *   防得住 —— OTA 通道上的未签名镜像 / 篡改镜像 / 版本回滚（含金固件槽伪造）
 *   防不住 —— 物理攻击（SWD 全开，密钥可从 Flash 读出）；对称密钥的分发
 *            （上位机与固件持有同一密钥）
 */
#ifndef OTA_HMAC_KEY_HEX
#define OTA_HMAC_KEY_HEX \
    "676363746573742d70372d686d61632d6465762d6b6579232323232323232323"
#endif

#define OTA_HMAC_KEY_LEN 32

/* 十六进制密钥串 → 字节（手写 nibble 解析：sscanf 会把整个 scanf 家族
 * 拖进 bootloader，~1KB flash 不可接受） */
static inline int ota_key_hex_nibble(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static inline int ota_hmac_key_from_hex(const char *hex, uint8_t *out, int outlen)
{
    for (int i = 0; i < outlen; i++) {
        int a = ota_key_hex_nibble(hex[2 * i]);
        int b = ota_key_hex_nibble(hex[2 * i + 1]);
        if (a < 0 || b < 0) return 0;
        out[i] = (uint8_t)((a << 4) | b);
    }
    return 1;
}

#endif
