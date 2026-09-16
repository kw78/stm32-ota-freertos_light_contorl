#include "supervisor.h"
#include "w25d64.h"
#include "cmsis_os.h"
#include "main.h"
#include <string.h>
#include <stdio.h>

/* ---- 共享运行时状态（ota.c 引用；bootloader 不链接本文件） ---- */
volatile uint8_t g_ota_backup_busy = 0;
volatile uint8_t g_ota_session_active = 0;
OTA_Flag_t g_ota_flag_cache;

extern UART_HandleTypeDef huart1;
extern volatile uint32_t seconds_today;     /* main.c：TIM3 1Hz 统计计数，健康探针 */

/* ---- IWDG：直接寄存器操作（工程未启用 HAL IWDG 驱动，寄存器本身就几个） ----
 * LSI 30~60kHz（典型 40kHz），预分频 /256：
 *   早期长超时 RLR=4095 → 17.5~35s：覆盖上电初始化 + OTA 复位后 Bootloader 搬运窗口
 *   运行期收紧 RLR=1250 → 5.3~10.7s（典型 8s），健康时每 2s 喂一次
 * IWDG 一旦启动只能靠复位停止——这是特性不是缺陷：跑挂的固件无处可逃
 */
#define IWDG_KEY_ENABLE   0xCCCCu
#define IWDG_KEY_FEED     0xAAAAu
#define IWDG_KEY_UNLOCK   0x5555u
#define IWDG_PRESC_256    6u
#define IWDG_RLR_LONG     4095u
#define IWDG_RLR_RUN      1250u

static void iwdg_reload_config(uint16_t rlr)
{
    IWDG->KR  = IWDG_KEY_UNLOCK;
    IWDG->PR  = IWDG_PRESC_256;
    IWDG->RLR = rlr;
    IWDG->KR  = IWDG_KEY_FEED;
}

void Supervisor_IWDGEarlyArm(void)
{
    iwdg_reload_config(IWDG_RLR_LONG);
    IWDG->KR = IWDG_KEY_ENABLE;
}

void Supervisor_IWDGFeed(void)
{
    IWDG->KR = IWDG_KEY_FEED;
}

/* ---- 参数 ---- */
#define SUPERVISOR_PERIOD_MS   2000
#define CONFIRM_DELAY_MS       10000
#define SUPERVISOR_STACK_SIZE  (512 * 4)

static void log_line(const char *s)
{
    HAL_UART_Transmit(&huart1, (const uint8_t *)s, (uint16_t)strlen(s), 100);
}

/* 健康判定：
 *   a) HAL tick 在走 → TIM4（HAL 时基）中断活着
 *   b) seconds_today 在涨 → TIM3 统计中断 + 主循环状态机路径活着
 *   （本任务能被调度本身就证明 FreeRTOS 内核活着）
 * 任何一项不动 → 返回 0，本轮不喂狗
 */
static int health_check(void)
{
    static uint32_t last_tick = 0;
    static uint32_t last_sec = 0;
    uint32_t now = HAL_GetTick();
    uint32_t sec = seconds_today;
    int ok = 1;

    if (now - last_tick < SUPERVISOR_PERIOD_MS / 2) ok = 0;   /* tick 停了 */
    if (sec == last_sec) ok = 0;                              /* 秒计数停了 */

    last_tick = now;
    last_sec = sec;
    return ok;
}

/* 把当前控制块写回 SPI Flash 并同步缓存 */
static void flag_commit(OTA_Flag_t *f)
{
    W25_EraseSector(OTA_FLAG_ADDR);
    W25_WritePage(OTA_FLAG_ADDR, (const uint8_t *)f, sizeof(*f));
    memcpy(&g_ota_flag_cache, f, sizeof(*f));
}

/* 启动时加载控制块：v2 直接用；v1 旧标志或杂数据 → 一次性迁移为干净的 v2 IDLE */
static void flag_boot_init(void)
{
    OTA_Flag_t f;
    W25_Read(OTA_FLAG_ADDR, (uint8_t *)&f, sizeof(f));

    if (f.magic == OTA_FLAG_MAGIC && f.ver == OTA_FLAG_VER) {
        memcpy(&g_ota_flag_cache, &f, sizeof(f));
        return;
    }

    /* v1 旧标志（'OTA1'）或擦除态/损坏数据：全部归一化为 v2 IDLE。
     * 注：v1 bootloader 看到 v2 magic 会直接跳过升级逻辑，双方向兼容 */
    memset(&f, 0, sizeof(f));
    f.magic = OTA_FLAG_MAGIC;
    f.ver   = OTA_FLAG_VER;
    f.state = OTA_STATE_IDLE;
    flag_commit(&f);
}

/* 备份槽 A → 槽 B 金固件。返回 0 成功。
 * 长操作（擦 14 扇区 + 写 56KB + 校验），过程中按块喂狗。 */
static int golden_backup(void)
{
    OTA_ImageHdr_t hdr;
    W25_Read(OTA_HDR_A_ADDR, (uint8_t *)&hdr, sizeof(hdr));
    if (hdr.magic != OTA_IMG_MAGIC || hdr.size == 0 || hdr.size > OTA_FW_MAX_SIZE)
        return -1;

    /* 备份前再校验一次槽 A（Bootloader 搬运前校验过，这里防搬运期间的位翻转） */
    uint8_t buf[256];
    uint32_t pos = 0, crc = 0xFFFFFFFF;
    while (pos < hdr.size) {
        uint32_t chunk = (hdr.size - pos > 256) ? 256 : (hdr.size - pos);
        W25_Read(OTA_FW_ADDR + pos, buf, chunk);
        crc = crc32_update(crc, buf, chunk);
        pos += chunk;
    }
    if ((~crc) != hdr.crc32)
        return -1;

    /* 擦槽 B（镜像头扇区 + 数据扇区），逐扇区喂狗防看门狗超时 */
    W25_EraseSector(OTA_HDR_B_ADDR);
    Supervisor_IWDGFeed();
    for (uint32_t addr = OTA_GOLDEN_ADDR;
         addr < OTA_GOLDEN_ADDR + hdr.size;
         addr += W25_SECTOR_SIZE) {
        W25_EraseSector(addr);
        Supervisor_IWDGFeed();
    }

    /* 逐块拷贝 A → B */
    for (pos = 0; pos < hdr.size; pos += 256) {
        uint16_t chunk = (uint16_t)((hdr.size - pos > 256) ? 256 : (hdr.size - pos));
        W25_Read(OTA_FW_ADDR + pos, buf, chunk);
        W25_WritePage(OTA_GOLDEN_ADDR + pos, buf, chunk);
        if ((pos & 0x7FF) == 0)          /* 每 2KB 喂一次 */
            Supervisor_IWDGFeed();
    }

    /* 回读槽 B 校验，确认金固件真正落盘 */
    crc = 0xFFFFFFFF;
    for (pos = 0; pos < hdr.size; pos += 256) {
        uint32_t chunk = (hdr.size - pos > 256) ? 256 : (hdr.size - pos);
        W25_Read(OTA_GOLDEN_ADDR + pos, buf, chunk);
        crc = crc32_update(crc, buf, chunk);
    }
    if ((~crc) != hdr.crc32)
        return -2;

    /* 写槽 B 镜像头 */
    W25_WritePage(OTA_HDR_B_ADDR, (const uint8_t *)&hdr, sizeof(hdr));
    return 0;
}

void StartTaskSupervisor(void *argument)
{
    (void)argument;

    flag_boot_init();

    uint32_t healthy_since = 0;          /* 0 = 尚未开始连续健康计时 */
    uint8_t  confirmed = 0;              /* 本次开机只确认一次 */

    /* 运行期收紧看门狗：从 26s 早期窗口收紧到 8s（LSI 典型值） */
    iwdg_reload_config(IWDG_RLR_RUN);
    Supervisor_IWDGFeed();

    for (;;) {
        osDelay(SUPERVISOR_PERIOD_MS);

        if (health_check()) {
            Supervisor_IWDGFeed();
            if (healthy_since == 0)
                healthy_since = HAL_GetTick();
        } else {
            healthy_since = 0;           /* 失守，健康计时清零重新来 */
        }

        /* TESTING 固件健康跑满观察期 → 确认启动（两阶段提交的第二阶段） */
        if (!confirmed &&
            healthy_since != 0 &&
            HAL_GetTick() - healthy_since >= CONFIRM_DELAY_MS &&
            g_ota_flag_cache.state == OTA_STATE_TESTING &&
            !g_ota_session_active) {     /* 传输进行中不打扰 */

            g_ota_backup_busy = 1;       /* 挡住新的 OTA START/END（见优先级分析） */
            int ret = golden_backup();
            g_ota_backup_busy = 0;
            Supervisor_IWDGFeed();

            if (ret == 0) {
                OTA_Flag_t f;
                memcpy(&f, &g_ota_flag_cache, sizeof(f));
                f.state = OTA_STATE_IDLE;
                f.golden_valid = 1;
                f.retry_cnt = 0;
                flag_commit(&f);
                confirmed = 1;
                char msg[48];
                int n = snprintf(msg, sizeof(msg),
                    "OTA confirmed, golden=v%08lX\r\n",
                    (unsigned long)APP_VERSION);
                HAL_UART_Transmit(&huart1, (uint8_t *)msg, (uint16_t)n, 100);
            } else {
                /* 备份失败：保持 TESTING（还能靠回滚计数保护），下轮重试 */
                log_line("golden backup FAIL\r\n");
            }
        }
    }
}
