/*
 * OTA 协议栈 host fuzz 台 —— 把真实 ota.c（解析器 + 分发器）原样编译到
 * 宿主机，用 ASan/UBSan + RAM 版 SPI Flash 模型轰炸。
 *
 * 做法不复制代码：harness 先定义独立 mock，再 #include "../../Core/Src/ota.c"——
 * 包括其中的 static 状态（ring buffer / 会话变量 / 解析器状态机），
 * 因此可以对设备内部状态做断言，而不仅看输入输出。
 *
 * 三层弹药：
 *   1. 定向回归：Bug #11/#13 家族的已知攻击面（超长 len / 巨大 fw_size /
 *      乱序 SEQ / CRC 半包超时后重同步 / 非法 GET_LOG）
 *   2. 结构化随机：合法帧 + 变异（位翻转/截断/拼接）
 *   3. 纯随机字节流
 *
 * oracle：
 *   O1 内存安全       —— ASan/UBSan（越界写栈/环形缓冲、未定义行为）；
 *                          W25 mock 额外断言 8MB 边界与扇区对齐
 *   O2 复位仅由合法 END 触发 —— NVIC_SystemReset 时断言会话数学自洽 +
 *                          槽 A 落盘数据 CRC32 == 申报 CRC32
 *   O3 擦除保护       —— START 申报非法大小后 erase 计数不得增长
 *   O4 重同步         —— 任意垃圾流之后，一帧合法 QUERY 必须被应答
 *
 * 用法：./fuzz [--seconds N] [--seed N]
 */

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "stm32f1xx_hal.h"       /* shim：HAL 最小替身 */

/* ---- 与被测代码无关的 mock（先定义） ---- */

static volatile uint32_t g_tick;
static uint8_t  tx_log[1 << 16];
static size_t   tx_len;
static uint32_t sysreset_count;
static uint32_t erase_count;

UART_HandleTypeDef huart1;         /* 类型来自 shim/main.h */

uint32_t HAL_GetTick(void) { return g_tick; }

void HAL_UART_Transmit(UART_HandleTypeDef *h, const uint8_t *data,
                       uint16_t len, int timeout)
{
    (void)h; (void)timeout;
    assert(data != NULL);
    assert(len <= 512);                       /* 响应包上限，越界即 bug */
    if (tx_len + len > sizeof(tx_log)) return;
    memcpy(tx_log + tx_len, data, len);
    tx_len += len;
}

/* ota.c 会调用但工程头文件未声明的 CMSIS 内联 */
void NVIC_SystemReset(void);

/* ---- 真实被测代码（含全部 static 状态） ---- */
#include "../../Core/Src/sha256.c"     /* ota.c v3 的签名验证依赖 */
#include "../../Core/Src/ota.c"

/* ---- 引用被测代码内部状态的 mock 实现 ---- */

static uint8_t w25_storage[8 * 1024 * 1024];

static void w25_bounds(uint32_t addr, uint32_t len, const char *who)
{
    if ((uint64_t)addr + len > sizeof(w25_storage)) {
        fprintf(stderr, "O1 violated: %s 越界 addr=0x%X len=%u\n", who, addr, len);
        abort();
    }
}

void W25_EraseSector(uint32_t addr)
{
    w25_bounds(addr, W25_SECTOR_SIZE, "EraseSector");
    assert((addr % W25_SECTOR_SIZE) == 0);
    memset(w25_storage + addr, 0xFF, W25_SECTOR_SIZE);
    erase_count++;
}

void W25_WritePage(uint32_t addr, const uint8_t *data, uint16_t len)
{
    w25_bounds(addr, len, "WritePage");
    assert(len <= 256);
    memcpy(w25_storage + addr, data, len);
}

void W25_Read(uint32_t addr, uint8_t *data, uint32_t len)
{
    w25_bounds(addr, len, "Read");
    memcpy(data, w25_storage + addr, len);
}

void NVIC_SystemReset(void)
{
    /* O2：只有完整的合法 END 能走到这里 */
    assert(ota_fw_size != 0 && ota_fw_size <= OTA_FW_MAX_SIZE);
    assert(ota_bytes_written == ota_fw_size);
    uint32_t crc = crc32_update(0xFFFFFFFF, w25_storage + OTA_FW_ADDR, ota_fw_size);
    if (~crc != ota_fw_crc32) {
        fprintf(stderr, "O2 violated: END 复位时槽 A CRC 不符\n");
        abort();
    }
    sysreset_count++;
}

void LightCtrl_SetEnable(uint8_t en, uint16_t target) { (void)en; (void)target; }
void Supervisor_IWDGFeed(void) {}
int  Log_Read(uint8_t which, uint16_t idx, LogRec_t *out)
{ (void)which; (void)idx; (void)out; return 0; }

/* supervisor.c 里的共享运行时状态（ota.h 声明 extern） */
volatile uint8_t g_ota_backup_busy = 0;
volatile uint8_t g_ota_session_active = 0;
OTA_Flag_t g_ota_flag_cache;

/* ---- 驱动 ---- */

static uint64_t rng_state;
static uint32_t rnd(void)
{
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 7;
    rng_state ^= rng_state << 17;
    return (uint32_t)(rng_state >> 32);
}

static int quiet_clock = 0;                   /* O4 验证帧用：帧内不跳时钟 */

static void feed(const uint8_t *data, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        OTA_RingBuf_Put(data[i]);
        if (!quiet_clock && rnd() % 32 == 0)
            g_tick += rnd() % 800;             /* 随机推进时钟：探索超时分支 */
        g_tick += 1;
        OTA_Process();
    }
}

static void session_reset(void)
{
    pkt_state = PKT_WAIT_HEADER;
    pkt_idx = 0;
    ota_fw_size = 0;
    ota_fw_crc32 = 0;
    ota_fw_version = 0;
    ota_fw_build_ts = 0;
    memset(ota_fw_hmac, 0, sizeof(ota_fw_hmac));
    ota_bytes_written = 0;
    g_ota_session_active = 0;
    memset(&g_ota_flag_cache, 0, sizeof(g_ota_flag_cache));   /* 防降级地板归零 */
}

/* v3 签名 START 载荷（与 ota_tool.start_payload 逐字节一致） */
static void build_signed_start(const uint8_t *img, uint32_t size, uint32_t ts,
                               uint8_t out[32], int corrupt_mac)
{
    uint8_t key[OTA_HMAC_KEY_LEN];
    assert(ota_hmac_key_from_hex(OTA_HMAC_KEY_HEX, key, sizeof(key)));
    OTA_ImageHdr_t h = {
        .magic = OTA_IMG_MAGIC, .version = 0xBEEF,
        .size = size, .build_ts = ts,
    };
    h.crc32 = ~crc32_update(0xFFFFFFFF, img, size);
    HmacSha256 hm;
    hmac_sha256_begin(&hm, key, sizeof(key));
    sha256_update(&hm.inner, (const uint8_t *)&h, 20);
    sha256_update(&hm.inner, img, size);
    uint8_t mac[32];
    hmac_sha256_end(&hm, mac);
    memcpy(out, &(uint32_t){size}, 4);
    memcpy(out + 4, &h.crc32, 4);
    memcpy(out + 8, &(uint32_t){0xBEEF}, 4);
    memcpy(out + 12, &ts, 4);
    memcpy(out + 16, mac, 16);
    if (corrupt_mac) out[16] ^= 0x01;      /* 破坏签名最低位 */
}

static size_t tx_find(uint8_t b)               /* TX 流里找字节（ACK/NACK） */
{
    for (size_t i = 0; i < tx_len; i++)
        if (tx_log[i] == b) return i;
    return (size_t)-1;
}

static uint8_t frame[7 + PKT_DATA_MAX];
static size_t frame_len;

static void build_frame(uint8_t cmd, const uint8_t *data, uint8_t len, uint16_t seq)
{
    uint8_t body[2 + 2 + PKT_DATA_MAX];
    body[0] = cmd; body[1] = len;
    body[2] = (uint8_t)seq; body[3] = (uint8_t)(seq >> 8);
    if (len) memcpy(body + 4, data, len);      /* len=0 时不得传 NULL 给 memcpy */
    uint16_t crc = crc16_compute(body, (uint16_t)(4 + len));
    frame[0] = PKT_HEADER;
    memcpy(frame + 1, body, 4 + len);
    frame[5 + len] = (uint8_t)(crc >> 8);
    frame[6 + len] = (uint8_t)crc;
    frame_len = 7 + len;
}

/* ---------- 定向回归（Bug #11/#13 攻击面） ---------- */

static int directed_cases(void)
{
    int fails = 0;
    quiet_clock = 1;               /* 定向用例确定性执行：帧内不注入时钟抖动 */
                                   /* （D1 的超时由用例自身显式注入） */

    /* D1: 帧内超时发生在 CRC 半包后，下一帧合法 QUERY 必须被应答（O4） */
    session_reset(); tx_len = 0;
    {
        uint8_t half[] = {PKT_HEADER, CMD_QUERY, 0, 0, 0, 0xAB};  /* 只给 CRC 高字节 */
        feed(half, sizeof(half));
        g_tick += OTA_PKT_TIMEOUT_MS + 1;
        build_frame(CMD_QUERY, NULL, 0, 0);
        feed(frame, frame_len);
        if (tx_len < 8) {                       /* QUERY 响应包 ≥ 8+12B */
            printf("D1 FAIL: 半包 CRC 超时后解析器未重同步（应答丢失）\n");
            fails++;
        }
    }

    /* D2: len > 64 的帧必须整包丢弃且可重同步 */
    session_reset(); tx_len = 0;
    {
        uint8_t bad[] = {PKT_HEADER, CMD_QUERY, 200, 0, 0};
        feed(bad, sizeof(bad));
        build_frame(CMD_QUERY, NULL, 0, 0);
        feed(frame, frame_len);
        if (tx_len < 8) { printf("D2 FAIL: 超长 len 后未重同步\n"); fails++; }
    }

    /* D3: START 申报超限大小 → NACK 且不得擦任何扇区（O3，Bug #13） */
    session_reset(); tx_len = 0;
    {
        uint32_t before = erase_count;
        uint8_t d[12] = {0xFF, 0xFF, 0xFF, 0x7F, 0, 0, 0, 0, 1, 0, 0, 0};
        build_frame(CMD_OTA_START, d, 12, 0);
        feed(frame, frame_len);
        if (tx_find(0x15) == (size_t)-1 || erase_count != before) {
            printf("D3 FAIL: 非法 fw_size 未被拒绝或已触发擦除\n");
            fails++;
        }
    }

    /* D4: 12B 旧格式 START 必须拒绝（v3 强制签名） */
    session_reset(); tx_len = 0;
    {
        uint8_t d[12] = {128, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
        build_frame(CMD_OTA_START, d, 12, 0);
        feed(frame, frame_len);
        if (tx_find(0x15) == (size_t)-1) { printf("D4 FAIL: 未签名 12B START 未 NACK\n"); fails++; }
    }

    /* D4b: 签名上传小镜像：乱序 SEQ NACK → 顺序补齐 → END 合法复位（O2） */
    session_reset(); tx_len = 0;
    {
        uint8_t img[128];
        for (int i = 0; i < 128; i++) img[i] = (uint8_t)i;
        uint8_t d[32];
        build_signed_start(img, 128, 1000, d, 0);
        build_frame(CMD_OTA_START, d, 32, 0);
        feed(frame, frame_len);
        if (tx_find(0x06) == (size_t)-1) { printf("D4b FAIL: 合法签名 START 未 ACK\n"); fails++; }
        build_frame(CMD_OTA_DATA, img + 64, 64, 5);          /* 乱序：先发 seq5 */
        feed(frame, frame_len);
        if (tx_find(0x15) == (size_t)-1) { printf("D4b FAIL: 乱序 SEQ 未 NACK\n"); fails++; }
        tx_len = 0;
        build_frame(CMD_OTA_DATA, img, 64, 0);
        feed(frame, frame_len);
        build_frame(CMD_OTA_DATA, img + 64, 64, 1);
        feed(frame, frame_len);
        uint32_t resets = sysreset_count;
        build_frame(CMD_OTA_END, NULL, 0, 0);
        feed(frame, frame_len);
        if (sysreset_count != resets + 1) {
            printf("D4b FAIL: 合法 END 未触发复位流程\n");
            fails++;
        }
    }

    /* D7: 防降级——build_ts 低于 flag.min_build 的 START 必须早拒 */
    session_reset(); tx_len = 0;
    {
        uint8_t img[64];
        for (int i = 0; i < 64; i++) img[i] = (uint8_t)(i ^ 0x5A);
        uint8_t d[32];
        build_signed_start(img, 64, 100, d, 0);              /* ts=100 */
        g_ota_flag_cache.min_build = 500;                    /* 地板 500 */
        build_frame(CMD_OTA_START, d, 32, 0);
        feed(frame, frame_len);
        if (tx_find(0x15) == (size_t)-1) { printf("D7 FAIL: 降级镜像 START 未 NACK\n"); fails++; }
    }

    /* D8: 签名错误——完整上传后 END 必须拒绝且不得复位（信任链核心 oracle） */
    session_reset(); tx_len = 0;
    {
        uint8_t img[64];
        for (int i = 0; i < 64; i++) img[i] = (uint8_t)(i * 3 + 1);
        uint8_t d[32];
        build_signed_start(img, 64, 2000, d, 1);             /* 破坏 1bit 签名 */
        build_frame(CMD_OTA_START, d, 32, 0);
        feed(frame, frame_len);
        build_frame(CMD_OTA_DATA, img, 64, 0);
        feed(frame, frame_len);
        uint32_t resets = sysreset_count;
        build_frame(CMD_OTA_END, NULL, 0, 0);
        feed(frame, frame_len);
        if (sysreset_count != resets || tx_find(0x15) == (size_t)-1) {
            printf("D8 FAIL: 坏签名 END 未拒绝（复位=%u）\n", sysreset_count);
            fails++;
        }
    }

    /* D5: GET_LOG 非法 which → NACK */
    session_reset(); tx_len = 0;
    {
        uint8_t d[3] = {9, 0, 0};
        build_frame(CMD_GET_LOG, d, 3, 0);
        feed(frame, frame_len);
        if (tx_find(0x15) == (size_t)-1) { printf("D5 FAIL: 非法 which 未 NACK\n"); fails++; }
    }

    /* D6: 满长响应包（len=64）不得越界——OTA_SendPacket 缓冲区曾少 1 字节 */
    session_reset(); tx_len = 0;
    {
        uint8_t data64[PKT_DATA_MAX];
        memset(data64, 0xA5, sizeof(data64));
        OTA_SendPacket(CMD_QUERY, data64, PKT_DATA_MAX);   /* ASan 监视下构造 */
        if (tx_len != 7 + PKT_DATA_MAX) {
            printf("D6 FAIL: 满长响应包长度异常 %zu\n", tx_len);
            fails++;
        }
    }

    quiet_clock = 0;
    return fails;
}

/* ---------- 结构化随机 + 纯随机 ---------- */

static const uint8_t CMDS[] = {CMD_OTA_START, CMD_OTA_DATA, CMD_OTA_END,
                               CMD_QUERY, CMD_GET_LOG, CMD_LIGHT_CTRL};

static void fuzz_one(uint8_t mode)
{
    session_reset();
    tx_len = 0;

    if (mode == 0) {                            /* 纯随机 */
        uint8_t buf[96];
        size_t n = rnd() % sizeof(buf) + 1;
        for (size_t i = 0; i < n; i++) buf[i] = (uint8_t)rnd();
        feed(buf, n);
    } else {                                    /* 合法帧 + 变异 */
        uint8_t data[PKT_DATA_MAX];
        uint8_t dlen = (uint8_t)(rnd() % 17);   /* 覆盖 0..16（START=12 类） */
        for (int i = 0; i < dlen; i++) data[i] = (uint8_t)rnd();
        build_frame(CMDS[rnd() % 6], data, dlen, (uint16_t)rnd());
        size_t cut = frame_len;
        if (rnd() % 2) cut = rnd() % frame_len;                 /* 截断 */
        uint8_t mut[128];
        memcpy(mut, frame, cut);
        size_t mlen = cut;
        if (mlen && rnd() % 2)                                   /* 位翻转 */
            mut[rnd() % mlen] ^= (uint8_t)(1u << (rnd() % 8));
        if (rnd() % 4 == 0 && mlen + 8 < sizeof(mut)) {          /* 拼接垃圾 */
            for (int i = 0; i < 8; i++) mut[mlen + i] = (uint8_t)rnd();
            mlen += 8;
        }
        feed(mut, mlen);
    }

    /* O4：垃圾流之后一帧合法 QUERY 必须能被应答（重同步能力）。
     * 模型：主机在垃圾流后静默 > 帧内超时，再以安静时钟发查询——
     * 帧内超时应把任何半包残留复位，随后 QUERY 必须得到应答 */
    g_tick += OTA_PKT_TIMEOUT_MS + 10;
    build_frame(CMD_QUERY, NULL, 0, 0);
    quiet_clock = 1;
    feed(frame, frame_len);
    quiet_clock = 0;
    if (tx_len < 8) {
        fprintf(stderr, "O4 violated: 随机流后 QUERY 无应答\n");
        abort();
    }
}

int main(int argc, char **argv)
{
    double seconds = 20.0;
    uint64_t seed = (uint64_t)time(NULL);
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--seconds") && i + 1 < argc) seconds = atof(argv[++i]);
        else if (!strcmp(argv[i], "--seed") && i + 1 < argc) seed = strtoull(argv[++i], NULL, 0);
    }
    if (!seed) seed = 1;
    rng_state = seed * 6364136223846793005ULL + 1442695040888963407ULL;
    rng_state ^= (uint64_t)time(NULL);

    printf("OTA fuzz 台 | seed=%llu seconds=%.0fs\n", (unsigned long long)seed, seconds);

    int fails = directed_cases();
    printf("定向回归: %s（8 项）\n", fails ? "FAIL" : "PASS");

    long iters = 0;
    clock_t t0 = clock();
    while ((double)(clock() - t0) / CLOCKS_PER_SEC < seconds) {
        for (int k = 0; k < 200; k++) {
            fuzz_one((uint8_t)(rnd() % 2));
            iters++;
        }
    }
    printf("随机轰炸: %ld 轮 | END 合法复位 %u 次 | 擦除 %u 次 | ASan/UBSan 无告警\n",
           iters, sysreset_count, erase_count);
    printf("oracle O1-O4 全程内建断言，任何违反即刻 abort\n");

    return fails ? 1 : 0;
}
