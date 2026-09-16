#include "blackbox.h"
#include "ota.h"
#include "w25d64.h"
#include "main.h"
#include <string.h>

/* ---- 环形区布局 ---- */
#define REC_SIZE          32u
#define RING_BB_ADDR      OTA_BLACKBOX_ADDR          /* 4KB / 128 槽 */
#define RING_BB_CAP       (4096u / REC_SIZE)
#define RING_ST_ADDR      OTA_CFG_ADDR               /* 12KB / 384 槽 */
#define RING_ST_CAP       (12u * 1024u / REC_SIZE)

typedef struct {
    uint32_t base;
    uint16_t cap;
    uint16_t next;          /* 下一个写入槽位 */
    uint16_t last_seq;      /* 最近一条有效记录的序号 */
} LogRing_t;

static LogRing_t ring_bb = { RING_BB_ADDR, RING_BB_CAP, 0, 0 };
static LogRing_t ring_st = { RING_ST_ADDR, RING_ST_CAP, 0, 0 };

/* 崩溃现场邮箱：noinit 段，复位不初始化 */
CrashMailbox_t g_crash_mailbox __attribute__((section(".noinit"), used));

/* CRC16 增量计算（Modbus 反射多项式，可跨多段续算） */
static uint16_t crc16_seg(uint16_t crc, const uint8_t *d, uint16_t len)
{
    for (uint16_t i = 0; i < len; i++) {
        crc ^= d[i];
        for (int j = 0; j < 8; j++)
            crc = (crc & 1u) ? (uint16_t)((crc >> 1) ^ 0xA001u) : (uint16_t)(crc >> 1);
    }
    return crc;
}

/* 记录 CRC：覆盖 kind+seq 与 f[6] 两段（crc16 字段位于两者之间，自身不参与） */
static uint16_t rec_crc16(const LogRec_t *r)
{
    uint16_t crc = crc16_seg(0xFFFF, (const uint8_t *)&r->kind, 4);
    return crc16_seg(crc, (const uint8_t *)r->f, 24);
}

/* 扫描环形区：找最高序号的有效记录，确定写指针。
 * 上电调用一次，512 槽 × 32B 的 SPI 读约两百毫秒，可接受 */
static void ring_scan(LogRing_t *r)
{
    LogRec_t rec;
    r->next = 0;
    r->last_seq = 0;
    uint16_t best_idx = 0xFFFF;
    uint16_t best_seq = 0;

    for (uint16_t i = 0; i < r->cap; i++) {
        W25_Read(r->base + (uint32_t)i * REC_SIZE, (uint8_t *)&rec, sizeof(rec));
        if (rec.magic != LOG_REC_MAGIC || rec_crc16(&rec) != rec.crc16)
            continue;
        if (best_idx == 0xFFFF) {
            best_idx = i;
            best_seq = rec.seq;
        } else {
            /* 序号单调递增但 uint16 会回绕：差值 < 0x8000 视为更新 */
            uint16_t diff = (uint16_t)(rec.seq - best_seq);
            if (diff != 0 && diff < 0x8000u) {
                best_seq = rec.seq;
                best_idx = i;
            }
        }
    }
    if (best_idx != 0xFFFF) {
        r->last_seq = best_seq;
        r->next = (uint16_t)((best_idx + 1u) % r->cap);
    }
}

void Log_Init(void)
{
    ring_scan(&ring_bb);
    ring_scan(&ring_st);
}

/* 槽位是否为擦除态（全 0xFF，可直接编程） */
static int slot_erased(const LogRing_t *r, uint16_t idx)
{
    LogRec_t rec;
    W25_Read(r->base + (uint32_t)idx * REC_SIZE, (uint8_t *)&rec, sizeof(rec));
    return rec.magic == 0xFFFFu;
}

int Log_Write(uint8_t which, uint16_t kind, const uint32_t f[6])
{
    LogRing_t *r = (which == LOG_BLACKBOX) ? &ring_bb : &ring_st;

    /* 写指针位置不是擦除态 → 环已写满，整扇区擦除回卷（旧记录让位） */
    if (!slot_erased(r, r->next)) {
        W25_EraseSector(r->base);
        r->next = 0;
    }

    LogRec_t rec;
    rec.magic = LOG_REC_MAGIC;
    rec.kind  = kind;
    rec.seq   = (uint16_t)(r->last_seq + 1u);
    memcpy(rec.f, f, sizeof(rec.f));
    rec.crc16 = rec_crc16(&rec);

    W25_WritePage(r->base + (uint32_t)r->next * REC_SIZE,
                  (const uint8_t *)&rec, sizeof(rec));

    /* 回读确认（失败不算写入成功，下次开机重扫能自愈指针） */
    LogRec_t chk;
    W25_Read(r->base + (uint32_t)r->next * REC_SIZE, (uint8_t *)&chk, sizeof(chk));
    if (chk.magic != LOG_REC_MAGIC || chk.crc16 != rec.crc16)
        return -1;

    r->last_seq = rec.seq;
    r->next = (uint16_t)((r->next + 1u) % r->cap);
    return 0;
}

int Log_Read(uint8_t which, uint16_t idx, LogRec_t *out)
{
    const LogRing_t *r = (which == LOG_BLACKBOX) ? &ring_bb : &ring_st;
    if (idx >= r->cap)
        return 0;
    W25_Read(r->base + (uint32_t)idx * REC_SIZE, (uint8_t *)out, sizeof(*out));
    return (out->magic == LOG_REC_MAGIC && rec_crc16(out) == out->crc16) ? 1 : 0;
}

/* ---- HardFault 捕获 ----
 * 入口在 stm32f1xx_it.c：naked 汇编判断 msp/psp 取异常栈帧指针后跳到这里。
 * 中断上下文只写 RAM 邮箱（不碰 SPI/UART——故障现场外设状态不可信），
 * 然后关中断死等 IWDG 复位：复位原因保持为看门狗 → Bootloader 的回滚
 * 计数才会认定"固件跑挂"；现场由 noinit 邮箱带过复位，supervisor 转录。
 */
void HardFault_Capture(uint32_t *frame)
{
    g_crash_mailbox.cfsr = SCB->CFSR;
    g_crash_mailbox.hfsr = SCB->HFSR;
    g_crash_mailbox.bfar = SCB->BFAR;
    g_crash_mailbox.pc   = frame[6];
    g_crash_mailbox.lr   = frame[5];
    g_crash_mailbox.uptime = HAL_GetTick();
    g_crash_mailbox.magic = CRASH_MAGIC;

    __disable_irq();
    for (;;) { }              /* 等 IWDG（最长 26s 早期窗口 / 8s 运行期） */
}
