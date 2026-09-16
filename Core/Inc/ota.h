#ifndef OTA_H
#define OTA_H
#include "main.h"

/*
 * =====================================================================
 *  OTA v2 分区表与数据结构 —— Bootloader 契约（一次定稿，保持 ABI 稳定）
 *
 *  W25D64 (8MB) 布局：
 *    0x000000  4KB   控制块 flag（本文件 OTA_Flag_t）
 *    0x001000  4KB   槽 A 镜像头（OTA_ImageHdr_t）
 *    0x002000  56KB  槽 A 固件暂存区（升级写入，Bootloader 搬运源）
 *    0x010000  4KB   槽 B 镜像头
 *    0x011000  56KB  槽 B 金固件（最近一次确认启动成功的镜像，回滚源）
 *    0x020000  4KB   黑匣子环形区（P2 预留）
 *    0x024000  12KB  配置 / 光照统计（P2 预留）
 *
 *  升级状态机（bootloader + supervisor 共同维护）：
 *    IDLE --(OTA END 写 flag)--> PENDING
 *    PENDING --(bootloader 校验+搬运成功)--> TESTING
 *    TESTING --(App 10s 健康自检通过，备份金固件)--> IDLE
 *    TESTING --(连续 4 次 IWDG 复位)--> 回滚槽 B --> IDLE
 * =====================================================================
 */

/* ---- SPI Flash 分区地址（v2 契约，勿改动——改动即破坏已烧录 bootloader）---- */
#define OTA_FLAG_ADDR       0x00000000
#define OTA_HDR_A_ADDR      0x00001000
#define OTA_FW_ADDR         0x00002000
#define OTA_HDR_B_ADDR      0x00010000
#define OTA_GOLDEN_ADDR     0x00011000
#define OTA_BLACKBOX_ADDR   0x00020000  /* P2 预留 */
#define OTA_CFG_ADDR        0x00024000  /* P2 预留 */
#define OTA_FW_MAX_SIZE     (56 * 1024)

/* ---- 控制块（flag）---- */
#define OTA_FLAG_MAGIC      0x4F544132U  /* 'OTA2' */
#define OTA_FLAG_MAGIC_V1   0x4F544131U  /* 'OTA1' 旧版标志，仅用于一次性迁移识别 */
#define OTA_FLAG_VER        2

#define OTA_STATE_IDLE      0x00        /* 无升级活动 */
#define OTA_STATE_PENDING   0x01        /* 槽 A 有完整固件，等待 Bootloader 搬运 */
#define OTA_STATE_TESTING   0x02        /* 候选固件已运行，等待 App 确认 */

#define OTA_ROLLBACK_LIMIT  3           /* IWDG 复位次数超过此值则回滚 */

typedef struct {
    uint32_t magic;                     /* OTA_FLAG_MAGIC */
    uint16_t ver;                       /* OTA_FLAG_VER */
    uint8_t  state;                     /* OTA_STATE_xxx */
    uint8_t  golden_valid;              /* 槽 B 金固件是否可用 */
    uint16_t retry_cnt;                 /* TESTING 期间 IWDG 复位计数 */
    uint16_t reserved;
} OTA_Flag_t;                           /* 固定 12 字节（4KB 扇区内单页写） */

/* ---- 镜像头（每个槽一个，描述槽内固件）---- */
#define OTA_IMG_MAGIC       0x32474D49U  /* 'IMG2'（小端读出） */

typedef struct {
    uint32_t magic;                     /* OTA_IMG_MAGIC */
    uint32_t version;                   /* 构建版本（git short hash） */
    uint32_t size;                      /* 固件字节数 */
    uint32_t crc32;                     /* 固件 CRC32（与上位机 binascii.crc32 一致） */
} OTA_ImageHdr_t;                       /* 16 字节 */

/* ---- 上位机协议 v2 ----
 * 帧: | 0xAA | CMD | LEN | SEQ_LO | SEQ_HI | DATA(LEN) | CRC16_HI | CRC16_LO |
 * CRC16 覆盖 CMD + LEN + SEQ + DATA。
 * SEQ 仅对 DATA 有意义（写入偏移 = SEQ * 64），重传同 SEQ 幂等（直接 ACK）。
 * v1 帧无 SEQ 字段；ota_tool.py 会先 QUERY 探测，老固件无响应则自动降级 v1。
 */
#define PKT_HEADER          0xAA
#define CMD_OTA_START       0x01        /* DATA: fw_size(4) crc32(4) version(4)，共 12B */
#define CMD_OTA_DATA        0x02        /* DATA: ≤64B 固件数据，SEQ = 偏移/64 */
#define CMD_OTA_END         0x03        /* DATA: 无 */
#define CMD_QUERY           0x10        /* DATA: 无；响应 12B 状态（见下） */
#define CMD_GET_LOG         0x12        /* DATA: which(1B)+idx(2B)；响应 32B 记录或空(结束) */
#define OTA_CHUNK_SIZE      64

/* QUERY 响应（12B, 小端） */
typedef struct {
    uint8_t  proto;                     /* 协议版本 = 2 */
    uint8_t  state;                     /* OTA_STATE_xxx */
    uint8_t  golden;                    /* golden_valid */
    uint8_t  retry;                     /* retry_cnt */
    uint32_t version;                   /* 当前固件版本（APP_VERSION） */
    uint32_t uptime_sec;                /* 开机秒数 */
} OTA_QueryResp_t;

/* ---- 跨模块共享的运行时状态（定义在 supervisor.c，bootloader 不链接）---- */
extern volatile uint8_t g_ota_backup_busy;    /* 金固件备份进行中，OTA START/END 应 NACK */
extern volatile uint8_t g_ota_session_active; /* OTA 会话进行中，supervisor 推迟确认动作 */
extern OTA_Flag_t g_ota_flag_cache;           /* 控制块缓存（supervisor 启动时加载/更新，QUERY 用） */

/* 固件版本注入（CMake 生成；bootloader 编译本文件时无注入，取默认值） */
#ifndef APP_VERSION
#define APP_VERSION 0xFFFFFFFFu
#endif

/* CRC 函数声明 */
uint16_t crc16_compute(const uint8_t *data, uint16_t len);
uint32_t crc32_compute(const uint8_t *data, uint32_t len);
uint32_t crc32_update(uint32_t crc, const uint8_t *data, uint32_t len);

#endif
