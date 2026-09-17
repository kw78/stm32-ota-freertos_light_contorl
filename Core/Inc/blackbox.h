#ifndef BLACKBOX_H
#define BLACKBOX_H

#include <stdint.h>

/*
 * 黑匣子：掉电留存的设备事件日志（SPI Flash 环形区）+ 崩溃现场邮箱（RAM noinit）
 *
 * 记录类型：
 *   REC_BOOT   每次开机的"黑匣子开机信"（版本/复位原因/升级状态/运行时长）
 *   REC_FAULT  HardFault 现场（PC/LR/CFSR/HFSR/BFAR + 崩溃时 uptime）
 *   REC_STATS  光照统计快照（每 30min 落盘一次，断电只丢最后一段）
 *
 * 两条环形区：黑匣子事件 128 槽 @0x020000，统计快照 384 槽 @0x024000。
 * 写满回卷时整扇区擦除（统计区 8 天一圈，10 万次擦写寿命 ≈ 数千年，无压力）。
 */

/* 记录（32B 定长，跨端结构与 ota_tool.py 解码保持一致） */
#define LOG_REC_MAGIC   0xBB01u

#define REC_BOOT        1       /* f: fw_version, reset_csr, state/golden<<8|retry, 0, 0, 0     */
#define REC_FAULT       2       /* f: cfsr, hfsr, bfar, pc, lr, uptime_at_crash                  */
#define REC_STATS       3       /* f: uptime, dark_s, dim_s, ideal_s, glare_s, fw_version       */
#define REC_PVD         4       /* f: dip_count, uptime_at_dip, pvdo_now, 0, 0, 0              */

typedef struct {
    uint16_t magic;              /* LOG_REC_MAGIC */
    uint16_t kind;               /* REC_xxx */
    uint16_t seq;                /* 环内单调递增序号（回卷后继续累加） */
    uint16_t crc16;              /* 两段计算：kind+seq 与 f[6]（crc 字段自身不参与） */
    uint32_t f[6];
} LogRec_t;                      /* 严格 32 字节（8B 头 + 24B 载荷） */

/* 环形区选择 */
#define LOG_BLACKBOX    1
#define LOG_STATS       2

/*
 * 崩溃现场邮箱：.noinit RAM 段，HardFault 中断里只写 RAM（不碰外设），
 * 下次开机由 supervisor 转录成 REC_FAULT 落盘。STM32F1 的 SRAM 内容在
 * 系统/看门狗复位后保留（仅上电复位清零），这是整个机制成立的前提。
 */
#define CRASH_MAGIC     0xC0FFEE01u

typedef struct {
    uint32_t magic;              /* CRASH_MAGIC = 有现场待转录 */
    uint32_t cfsr;
    uint32_t hfsr;
    uint32_t bfar;
    uint32_t pc;                 /* 崩溃点（异常栈帧里的 PC） */
    uint32_t lr;
    uint32_t uptime;             /* 崩溃时的开机毫秒数 */
} CrashMailbox_t;                /* 28 字节 */

/* 环形区操作（supervisor 上下文调用；内部直接操作 SPI Flash） */
void     Log_Init(void);                          /* 上电扫描两个环，定位写指针 */
int      Log_Write(uint8_t which, uint16_t kind, const uint32_t f[6]);
int      Log_Read(uint8_t which, uint16_t idx, LogRec_t *out);   /* 1=有效记录 */

/* HardFault 捕获入口（stm32f1xx_it.c 的 naked 汇编跳到这里，frame=异常栈帧） */
void     HardFault_Capture(uint32_t *frame);

/* 崩溃现场邮箱实体（定义在 blackbox.c 的 .noinit 段，复位不初始化） */
extern CrashMailbox_t g_crash_mailbox;

#endif
